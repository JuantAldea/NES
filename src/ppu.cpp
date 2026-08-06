#include <algorithm>
#include <cstring>

#include "../include/ppu.h"
#include "../include/bus.h"
/*
1uint8_t& PPU::get_register(const RegisterMMap reg)
{
    assert((addr >= PPUCTRL && addr <= PPUCTRL) || addr == OAMDMA);
    switch (reg) {
    case PPUCTRL:
        return registers.PPUCTRL;
    case PPUMASK:
        return registers.PPUMASK;
    case PPUSTATUS:
        return registers.PPUSTATUS;
    case OAMADDR:
        return registers.OAMADDR;
    case OAMDATA:
        return registers.OAMDATA;
    case PPUSCROLL:
        return registers.PPUSCROLL;
    case PPUADDR:
        return registers.PPUADDR;
    case PPUDATA:
        return registers.PPUDATA;
    case OAMDMA:
        return registers.OAMDMA;
    default:
        break;
    }
}
*/

// https://wiki.nesdev.com/w/index.php/PPU_sprite_evaluation

// Drives some or all of the eight bits of the PPU's data bus latch.
//
// Only the masked bits take the new value and have their decay timers
// refreshed; the rest keep ageing. Modelling this per bit rather than per byte
// is what makes a PPUSTATUS read (which drives bits 5-7) leave bits 0-4 to
// carry on decaying from whenever they were last driven.
void PPU::drive_open_bus(const uint8_t value, const uint8_t mask)
{
    open_bus = static_cast<uint8_t>((open_bus & ~mask) | (value & mask));

    for (int bit = 0; bit < 8; ++bit) {
        if (mask & (1u << bit)) {
            open_bus_refreshed_at[bit] = total_cycles;
        }
    }

    refresh_open_bus_next_decay();
}

// The soonest cycle at which any currently-set bit could expire.
//
// decay_open_bus runs on every one of the ~150 million PPU ticks a test ROM
// takes, so it must not scan eight bits each time. Recomputing the earliest
// deadline here - on the rare drive - lets the hot path be two comparisons.
void PPU::refresh_open_bus_next_decay()
{
    uint64_t soonest = UINT64_MAX;

    for (int bit = 0; bit < 8; ++bit) {
        if (open_bus & (1u << bit)) {
            soonest = std::min(soonest, open_bus_refreshed_at[bit] + open_bus_decay_cycles);
        }
    }

    open_bus_next_decay = soonest;
}

// Clears any bit that has gone long enough without being driven.
void PPU::decay_open_bus()
{
    // The overwhelmingly common case: nothing is due to expire yet. An all-zero
    // latch has no deadline at all (UINT64_MAX), so it falls out here too.
    if (total_cycles < open_bus_next_decay) {
        return;
    }

    for (int bit = 0; bit < 8; ++bit) {
        if ((open_bus & (1u << bit)) == 0) {
            continue;
        }
        if (total_cycles - open_bus_refreshed_at[bit] >= open_bus_decay_cycles) {
            open_bus &= static_cast<uint8_t>(~(1u << bit));
        }
    }

    refresh_open_bus_next_decay();
}

// --- the v/t/x scroll operations ----------------------------------------
//
// Transcribed from NESdev's "PPU scrolling". v is not a plain counter: coarse X
// occupies its low five bits and coarse Y bits 5-9, so "move one tile right"
// and "move one line down" are increments of subfields that carry into the
// nametable select rather than into each other.

// Dot 8, 16, ... 256, and again at 328 and 336.
//
// Coarse X counts 0-31 across a 32-tile nametable, so its carry is not a carry
// into coarse Y - it flips the HORIZONTAL nametable bit, which is what makes a
// scroll run off the right of one screen and onto the left of the next.
void PPU::increment_coarse_x()
{
    if ((registers.PPUADDR & 0x001F) == 31) {
        registers.PPUADDR = static_cast<uint16_t>(registers.PPUADDR & ~0x001F);
        registers.PPUADDR = static_cast<uint16_t>(registers.PPUADDR ^ 0x0400);
    } else {
        registers.PPUADDR = static_cast<uint16_t>(registers.PPUADDR + 1);
    }
}

// Dot 256.
//
// Fine Y counts the eight rows within a tile and carries into coarse Y. Coarse
// Y is where the subtlety is: a nametable is 30 tiles tall but the field is
// five bits, so it wraps at 30, not at 32. Wrapping at 29->0 flips the VERTICAL
// nametable bit; the 31->0 case exists because software can write a coarse Y of
// 30 or 31 through $2006, and that runs through the attribute table instead of
// the tiles, wrapping WITHOUT switching nametable.
void PPU::increment_y()
{
    if ((registers.PPUADDR & 0x7000) != 0x7000) {
        registers.PPUADDR = static_cast<uint16_t>(registers.PPUADDR + 0x1000);
        return;
    }

    registers.PPUADDR = static_cast<uint16_t>(registers.PPUADDR & ~0x7000);

    int y = (registers.PPUADDR & 0x03E0) >> 5;
    if (y == 29) {
        y = 0;
        registers.PPUADDR = static_cast<uint16_t>(registers.PPUADDR ^ 0x0800);
    } else if (y == 31) {
        y = 0;
    } else {
        y += 1;
    }

    registers.PPUADDR = static_cast<uint16_t>((registers.PPUADDR & ~0x03E0) | (y << 5));
}

// Dot 257: v: ....A.. ...BCDEF <- t: ....A.. ...BCDEF
//
// Coarse X and the horizontal nametable bit only. Everything the line just
// drawn did to v's X half is undone, so the next line starts from the same
// scroll position - which is what makes the scroll a per-frame value that a
// mid-frame $2005 write can change (split scrolling).
void PPU::copy_horizontal_from_t()
{
    constexpr uint16_t horizontal_bits = 0x041F;
    registers.PPUADDR = static_cast<uint16_t>((registers.PPUADDR & ~horizontal_bits) | (temp_addr & horizontal_bits));
}

// Pre-render dots 280-304: v: GHIA.BC DEF..... <- t: GHIA.BC DEF.....
//
// Fine Y, coarse Y and the vertical nametable bit. Repeated over 25 dots on
// hardware; doing it once would be equivalent here, but the range is what the
// spec says and a mid-range $2005 write should not survive it.
void PPU::copy_vertical_from_t()
{
    constexpr uint16_t vertical_bits = 0x7BE0;
    registers.PPUADDR = static_cast<uint16_t>((registers.PPUADDR & ~vertical_bits) | (temp_addr & vertical_bits));
}

// A $2007 access while rendering is enabled and the PPU is on a visible or
// pre-render scanline does NOT add the $2000 increment. v is the rendering
// address, and the access is spliced into the fetch pipeline, so it performs
// the two increments the pipeline would have: coarse X and Y.
//
// This is blargg's vram_access subtest 4, "$2007 should not increment when
// rendering is on".
void PPU::advance_vram_address()
{
    const bool on_a_rendering_scanline = scanline < post_render_scanline || scanline == pre_render_scanline;

    if (rendering_enabled() && on_a_rendering_scanline) {
        increment_coarse_x();
        increment_y();
        return;
    }

    // Masked to 15 bits, not the bus's 14. v carries fine Y in bits 12-14, so
    // folding it to the bus width here would clear the top fine-Y bit on every
    // $2007 access made while rendering is off - silently dragging a fine Y of
    // 4-7 down to 0-3 for the rest of the frame.
    registers.PPUADDR = (registers.PPUADDR + vram_step) & vram_register_mask;
}

void PPU::clock()
{
    ++total_cycles;

    decay_open_bus();

    // OAM DMA is NOT driven from here. It steals CPU cycles, so it advances at
    // the CPU's rate; Bus::clock runs it. Ticking it once per dot made the
    // transfer three times too short.

    //341 clocks/scanline
    // external PPU memory accessed every two clocks = 170 reads
    //+ 1 spare cycle

    switch (scanline) {
        case 0 ... 239:
            process_visible_scanline();
            break;
        case post_render_scanline:
            // idle
            break;
        case vblank_start_scanline:
            // The vblank flag is set on the second dot of scanline 241.
            if (cycle == 1) {
                // ...unless the CPU read $2002 on this very dot. The read's
                // clear and the PPU's set race, and the clear wins: the flag
                // is not set at all, and stays clear for the whole of this
                // vblank. This is 02-vbl_set_time's row 04 ("- -") and
                // 06-suppression's ("flag never set, no NMI").
                if (!suppress_vblank_flag_set) {
                    set_vblank();
                }
                suppress_vblank_flag_set = false;
            }
            break;
        case 242 ... 260:
            // TODO is it idle as well?
            break;
        case pre_render_scanline:
            if (cycle == 1) {
               clear_vblank();
               clear_sprite0_hit();
               clear_sprite_overflow();
            }
            // The pre-render line runs the same fetch pipeline as a visible
            // one - that is how the first two tiles of scanline 0 are already
            // in the shift registers when it starts - but it outputs no
            // pixels. It is also the only line that copies t's vertical half
            // into v.
            process_visible_scanline();
            break;
        default:
            std::cerr << "Scanline out of range: " << scanline << std::endl;
            break;
    }

    advance_dot();

    // Anything above may have moved the vblank flag, and /NMI follows it.
    update_nmi_line();
    /*

    - Sprite DMA is 6144 clock cycles long (or in CPU clock cycles, 6144/12).
    256 individual transfers are made from CPU memory to a temp register inside
    the CPU, then from the CPU's temp reg, to $2004.

    - One scanline is EXACTLY 1364 cycles long. In comparison to the CPU's
    speed, one scanline is 1364/12 CPU cycles long.

    - One frame is EXACTLY 357368 cycles long, or EXACTLY 262 scanlines long.

    */
}

void PPU::advance_dot()
{
    // The PPU commits to the shortened frame one dot BEFORE it acts on it, and
    // the difference is measurable: 10-even_odd_timing toggles PPUMASK a dot
    // either side of here and counts the clocks that result.
    //
    // Sampling rendering_enabled() at the jump itself would be one dot late.
    // The CPU's bus access is placed just ahead of its dot's tick (see
    // Bus::clock), so a $2001 write aligned to dot 339 would still be visible
    // by the end of that dot - and the ROM says it must not be. Latching at
    // the end of dot 338 is what excludes it. The window is exactly one dot
    // wide in each direction: latching at 337 or earlier makes the ROM report
    // "clock is skipped too soon", at 339 or later "too late".
    if (scanline == pre_render_scanline && cycle == odd_frame_skip_decision_dot) {
        odd_frame_skip_armed = rendering_enabled();
    }

    // On odd frames the final dot of the pre-render line, (261, 340), is
    // skipped while rendering is enabled, making those frames one dot short.
    const bool skips_last_dot =
        scanline == pre_render_scanline && cycle == dots_per_scanline - 2 && (frame % 2) == 1 && odd_frame_skip_armed;

    if (skips_last_dot) {
        cycle = 0;
        scanline = 0;
        ++frame;
        return;
    }

    ++cycle;
    if (cycle < dots_per_scanline) {
        return;
    }

    cycle = 0;
    ++scanline;
    if (scanline < scanlines_per_frame) {
        return;
    }

    scanline = 0;
    ++frame;
}

// Every dot, the shift registers move one pixel left. The pattern registers are
// 16 bits because the tile being fetched is two tiles ahead of the tile being
// drawn: the high byte is the tile on screen now, the low byte the one being
// assembled.
//
// The attribute registers are 8 bits' worth of the SAME two bits repeated,
// because a tile's palette is per-tile but has to be selected by the same fine-X
// tap as the pattern bits. They are held in uint16_t so that all four shift
// identically.
void PPU::shift_background_registers()
{
    bg_pattern_shift_low = static_cast<uint16_t>(bg_pattern_shift_low << 1);
    bg_pattern_shift_high = static_cast<uint16_t>(bg_pattern_shift_high << 1);
    bg_attribute_shift_low = static_cast<uint16_t>(bg_attribute_shift_low << 1);
    bg_attribute_shift_high = static_cast<uint16_t>(bg_attribute_shift_high << 1);
}

// On the eight-dot boundary the four latched bytes drop into the low half of
// the shift registers, behind the eight pixels still being drawn from the high
// half.
void PPU::reload_background_shifters()
{
    bg_pattern_shift_low = static_cast<uint16_t>((bg_pattern_shift_low & 0xFF00) | pattern_low_latch);
    bg_pattern_shift_high = static_cast<uint16_t>((bg_pattern_shift_high & 0xFF00) | pattern_high_latch);

    // Smear each attribute bit across the whole byte: every pixel of this tile
    // gets the same palette, but has to read it out of a per-pixel register.
    bg_attribute_shift_low =
        static_cast<uint16_t>((bg_attribute_shift_low & 0xFF00) | ((attribute_latch & 0x01) ? 0x00FF : 0x0000));
    bg_attribute_shift_high =
        static_cast<uint16_t>((bg_attribute_shift_high & 0xFF00) | ((attribute_latch & 0x02) ? 0x00FF : 0x0000));
}

// One of the four two-dot memory accesses that make up a tile fetch. Which one
// depends on the dot's position in the eight-dot pattern; the fetch is modelled
// as happening on the second dot of each pair, which is when hardware latches
// the byte.
//
//   dots 1-2  nametable byte     -> which tile
//   dots 3-4  attribute byte     -> which palette
//   dots 5-6  pattern table low  -> bit 0 of each pixel
//   dots 7-8  pattern table high -> bit 1 of each pixel
void PPU::fetch_background_byte()
{
    switch ((cycle - 1) % 8) {
    case 0:
        nametable_latch = ppu_bus_read(tile_address());
        break;
    case 2: {
        // One attribute byte covers a 32x32-pixel block: four tiles by four
        // tiles, two bits per 16x16 quadrant, packed bottom-right, bottom-left,
        // top-right, top-left from the high bits down.
        //
        // Which quadrant is bit 1 of each coarse counter - v bit 6 for coarse
        // Y, v bit 1 for coarse X - so the shift is 4 for the bottom half and
        // 2 for the right half.
        uint8_t attribute = ppu_bus_read(attribute_address());
        if (registers.PPUADDR & 0x0040) {
            attribute = static_cast<uint8_t>(attribute >> 4);
        }
        if (registers.PPUADDR & 0x0002) {
            attribute = static_cast<uint8_t>(attribute >> 2);
        }
        attribute_latch = attribute & 0x03;
        break;
    }
    case 4:
        pattern_low_latch = ppu_bus_read(background_pattern_address());
        break;
    case 6:
        // The two bitplanes of a tile are eight bytes apart, not interleaved.
        pattern_high_latch = ppu_bus_read(static_cast<uint16_t>(background_pattern_address() + 8));
        break;
    case 7:
        // Dots 8, 16, ... 256, and 328 and 336 in the prefetch region.
        increment_coarse_x();
        break;
    default:
        // The first dot of each pair: hardware puts the address on the bus,
        // and there is nothing to model.
        break;
    }
}

// Produces the pixel for dot `cycle` of a visible scanline and writes it into
// the framebuffer.
//
// The framebuffer holds a 6-bit palette INDEX, not a colour: the greyscale mask
// is applied here because it is part of the palette lookup, but colour emphasis
// is not, because it is a property of the video signal rather than of the pixel.
void PPU::render_pixel()
{
    const int x = cycle - 1;

    uint8_t pixel = 0;
    uint8_t palette = 0;

    // The leftmost eight pixels can be blanked independently of the background
    // as a whole (PPUMASK bit 1), which games use to hide the column of garbage
    // a mid-frame scroll change leaves there.
    const bool background_visible = show_background && (x >= 8 || show_bg_in_leftmost);

    if (background_visible) {
        // Fine X taps the shift registers up to seven pixels further along:
        // this is the whole of sub-tile horizontal scrolling.
        const uint16_t tap = static_cast<uint16_t>(0x8000 >> fine_x);
        pixel = static_cast<uint8_t>(((bg_pattern_shift_low & tap) ? 0x01 : 0x00) |
                                     ((bg_pattern_shift_high & tap) ? 0x02 : 0x00));
        palette = static_cast<uint8_t>(((bg_attribute_shift_low & tap) ? 0x01 : 0x00) |
                                       ((bg_attribute_shift_high & tap) ? 0x02 : 0x00));
    }

    // Colour 0 of every palette is not stored: it reads through to the backdrop
    // at $3F00. A transparent pixel and a disabled background are therefore the
    // same pixel.
    const uint16_t palette_addr =
        pixel == 0 ? 0x3F00 : static_cast<uint16_t>(0x3F00 + (palette << 2) + pixel);

    // Greyscale (PPUMASK bit 0) forces the low four bits of the index, which
    // collapses every hue onto the grey column of the NES palette.
    const uint8_t colour = static_cast<uint8_t>(ppu_bus_read(palette_addr) & (greyscale ? 0x30 : 0x3F));
    framebuffer[scanline * screen_width + x] = colour;

    // Sprite 0 hit needs the background's OPACITY at this dot, and this is the
    // only place it is known. `pixel` is already through the background's
    // left-clip, so a hit under a hidden left column cannot get this far.
    if (pixel != 0) {
        check_sprite0_hit(x);
    }
}

// Reverses the eight bits of a pattern byte, which is what horizontal flip does
// to a sprite's row.
static uint8_t reverse_bits(uint8_t byte)
{
    byte = static_cast<uint8_t>(((byte & 0xF0) >> 4) | ((byte & 0x0F) << 4));
    byte = static_cast<uint8_t>(((byte & 0xCC) >> 2) | ((byte & 0x33) << 2));
    byte = static_cast<uint8_t>(((byte & 0xAA) >> 1) | ((byte & 0x55) << 1));
    return byte;
}

// Works out which of sprite 0's eight columns are opaque on the scanline about
// to be drawn, and where they start.
//
// OAM byte 0 is the Y one LESS than the first line the sprite appears on -
// sprites are delayed a scanline by the way hardware evaluates them - so a
// sprite with Y = 0 first appears on scanline 1, and one with Y = 239 never
// appears at all.
void PPU::evaluate_sprite0_for_scanline(const int target_scanline)
{
    sprite0_on_this_scanline = false;

    // "Sprite 0" is not OAM[0]. Hardware sprite evaluation begins at whatever
    // OAMADDR holds when it starts, and the sprite that can raise the hit flag
    // is the FIRST one evaluated - so it is the sprite at OAMADDR, and a
    // non-zero OAMADDR shifts which sprite that is. Games are expected to set
    // OAMADDR to 0 before an OAM DMA precisely because of this.
    //
    // Aligned down to a four-byte boundary: OAM is read as sprite records, so
    // an OAMADDR pointing mid-record starts the record there. This models the
    // common case rather than the misaligned one.
    //
    // The value comes from the latch taken at dot 65, not from OAMADDR now:
    // hardware clears OAMADDR across dots 257-320, so by the time this runs the
    // value that chose the starting sprite has already been wiped.
    const uint8_t base = sprite_eval_oamaddr;

    const int height = big_sprites ? 16 : 8;
    // OAM holds Y minus one: a sprite with Y = n first appears on scanline
    // n + 1, which is why a Y of 255 can never be drawn.
    const int row = target_scanline - (OAM_memory[base] + 1);
    if (row < 0 || row >= height) {
        return;
    }

    const uint8_t tile = OAM_memory[base + 1];
    const uint8_t attributes = OAM_memory[base + 2];
    const bool flip_horizontally = attributes & 0x40;
    const bool flip_vertically = attributes & 0x80;

    // Vertical flip mirrors within the WHOLE sprite, so for an 8x16 it swaps
    // the two tiles as well as the rows inside them - which the arithmetic
    // below gets for free by flipping the row index before splitting it.
    const int pattern_row = flip_vertically ? (height - 1 - row) : row;

    uint16_t address;
    if (big_sprites) {
        // An 8x16 sprite ignores PPUCTRL's sprite pattern table select: bit 0
        // of the tile number chooses the table, and the rest is the index of
        // the TOP tile, whose bottom half is the tile after it.
        const uint16_t table = (tile & 0x01) ? 0x1000 : 0x0000;
        const uint16_t top_tile = static_cast<uint16_t>(tile & 0xFE);
        address = static_cast<uint16_t>(table + ((top_tile + (pattern_row >= 8 ? 1 : 0)) << 4) + (pattern_row & 0x07));
    } else {
        address = static_cast<uint16_t>(sprite_pattern_8x8_table_addr + (tile << 4) + pattern_row);
    }

    // A sprite pixel is opaque when its two bitplane bits are not both zero,
    // which is the OR of the two bytes taken bit by bit. Nothing here needs the
    // colour, only the opacity.
    uint8_t opaque = static_cast<uint8_t>(ppu_bus_read(address) | ppu_bus_read(static_cast<uint16_t>(address + 8)));
    if (flip_horizontally) {
        opaque = reverse_bits(opaque);
    }

    sprite0_opaque_columns = opaque;
    sprite0_left_x = OAM_memory[base + 3];
    sprite0_on_this_scanline = opaque != 0;
}

// Called with the screen X of a dot whose BACKGROUND pixel is opaque.
//
// NESdev, PPUSTATUS bit 6: "Sprite 0 hit cannot detect collision at X=255, nor
// anywhere where either sprites or backgrounds are disabled via PPUMASK. This
// includes X=0..7 when the leftmost 8 pixels are hidden." The background half
// of that is already accounted for - render_pixel only calls this when the
// background pixel survived its own left-clip - so what is left is the sprite
// half and X=255.
void PPU::check_sprite0_hit(const int x)
{
    if (!sprite0_on_this_scanline || !show_sprites) {
        return;
    }

    // X=255 is excluded by the hardware itself: the pixel pipeline has nowhere
    // to carry the comparison to.
    if (x == 255) {
        return;
    }

    if (x < 8 && !show_sprites_in_leftmost) {
        return;
    }

    const int column = x - sprite0_left_x;
    if (column < 0 || column >= 8) {
        return;
    }

    if ((sprite0_opaque_columns >> (7 - column)) & 0x01) {
        // "Immediately set when any opaque pixel of sprite 0 overlaps any
        // opaque pixel of background, REGARDLESS OF SPRITE PRIORITY" - so the
        // attribute's priority bit is not consulted, and neither is whatever
        // else may be drawn over the top.
        set_sprite0_hit();
    }
}

// The background pipeline, for the 240 visible scanlines and for the pre-render
// line, which runs the same fetches but draws nothing.
void PPU::process_visible_scanline()
{
    const bool pre_render = (scanline == pre_render_scanline);

    // Sprite evaluation for the NEXT line runs during THIS line's dots 65-256,
    // and the sprite pattern fetches that follow it occupy 257-320. Resolving
    // sprite 0 at dot 257 puts it at the end of that window: late enough that
    // the line's own pixels (dots 1-256) are finished, so a single set of
    // sprite-0 state can be overwritten here without double buffering, and
    // early enough that OAM writes made after this point cannot reach the line
    // that is about to be drawn.
    //
    // It used to run at dot 0 of the line being drawn, which had two
    // consequences. OAM rewritten between the two points was picked up a line
    // early. And because it sat inside the rendering_enabled() guard, enabling
    // rendering part-way through a frame left the previous evaluation in place,
    // so a line whose evaluation never happened could still report a hit -
    // hardware has no secondary OAM for such a line and cannot.
    //
    // Hence the else: no evaluation window means no sprite on the next line.
    if (cycle == 257) {
        if (rendering_enabled()) {
            // The pre-render line evaluates for line 0; every visible line for
            // the one after it. Line 239 evaluates for the post-render line,
            // which draws nothing, so the result is simply unused.
            evaluate_sprite0_for_scanline(pre_render ? 0 : scanline + 1);
        } else {
            sprite0_on_this_scanline = false;
        }
    }

    if (rendering_enabled()) {
        // Sprite evaluation starts here and reads OAM from OAMADDR onwards, so
        // this is the moment that decides which sprite is "sprite 0". Latched
        // because the clear below destroys it before the evaluation is
        // resolved at dot 257.
        if (cycle == 65) {
            sprite_eval_oamaddr = static_cast<uint8_t>(registers.OAMADDR & 0xFC);
        }

        // Hardware holds OAMADDR at 0 for the whole of the sprite-fetch
        // window. This is what makes OAMADDR effectively 0 at the start of
        // every evaluation, and therefore why sprite 0 is normally OAM[0]:
        // the pre-render line clears it too, so even a value written during
        // vblank is gone before line 0 is evaluated.
        //
        // It also means a game only sees the "sprite 0 moves" behaviour by
        // writing $2003 mid-screen, between dots 321 and 64 of the next line.
        if (cycle >= 257 && cycle <= 320) {
            registers.OAMADDR = 0;
        }

        // Dots 1-256 fetch the tiles for THIS line (two tiles ahead of the
        // pixel being drawn), and 321-336 the first two tiles of the NEXT one.
        // Both regions shift; the gap between them, where hardware is fetching
        // sprite patterns, does not.
        //
        // Shifting, reloading and fetching are on three different dot ranges,
        // and collapsing them into one was a real bug: with the fetch region
        // starting at dot 2, case 0 of fetch_background_byte never ran at dot
        // 1, so the nametable byte for the tile covering pixels 16-23 was
        // never read on this line. The picture still came out right, because
        // the dot-338/340 reads on the previous line leave the right value in
        // nametable_latch and v does not move in between - but the attribute
        // and pattern fetches at dots 3, 5 and 7 read v LIVE, so a $2006 write
        // in that window desynchronised the tile index from its own pattern
        // row and the switch showed up eight pixels late.

        // The shift belongs to the END of a dot: the pixel drawn on dot 1 comes
        // from what the previous line's prefetch left in the registers.
        if ((cycle >= 2 && cycle <= 257) || (cycle >= 322 && cycle <= 337)) {
            shift_background_registers();
        }

        // The reload lands on the dot after each eight-dot group completes -
        // 9, 17, ... 257, then 329 and 337 - which is what keeps the pipeline
        // exactly two tiles ahead of the pixel being drawn.
        if (((cycle >= 9 && cycle <= 257) || cycle == 329 || cycle == 337) && ((cycle - 1) % 8) == 0) {
            reload_background_shifters();
        }

        // Dots 1-256 fetch the tiles for THIS line, 321-336 the first two of
        // the NEXT one.
        if ((cycle >= 1 && cycle <= 256) || (cycle >= 321 && cycle <= 336)) {
            fetch_background_byte();
        }

        if (cycle == 256) {
            // The last coarse X increment of the line has already happened
            // (dot 256 is a multiple of eight); this is the one that moves down
            // a pixel row.
            increment_y();
        }

        if (cycle == 257) {
            // v's X half is restored from t, undoing the 32 coarse X
            // increments the line just made.
            copy_horizontal_from_t();
        }

        if (pre_render && cycle >= 280 && cycle <= 304) {
            copy_vertical_from_t();
        }

        if (cycle == 338 || cycle == 340) {
            // Two more nametable reads, whose values are discarded. They matter
            // to MMC3's scanline counter, not to the picture.
            nametable_latch = ppu_bus_read(tile_address());
        }
    }

    // Dots 1-256 of a visible line each produce one pixel. This runs even with
    // rendering disabled, because the PPU still drives the backdrop colour out
    // of the palette then.
    if (!pre_render && cycle >= 1 && cycle <= 256) {
        render_pixel();
    }
}

// Which of the 2KB of internal nametable RAM a $2000-$2FFF address lands in.
//
// The console has room for two 1KB screens but the address space has four. The
// cartridge decides how the four map onto the two by how it wires the PPU's
// A10/A11 lines, and the iNES header records which way round:
//
//   horizontal   $2000 $2400 -> screen 0      vertical   $2000 $2800 -> screen 0
//                $2800 $2C00 -> screen 1                 $2400 $2C00 -> screen 1
//
// Horizontal mirroring duplicates side by side, so a game scrolling VERTICALLY
// gets two distinct screens - the naming refers to how the duplication runs,
// not to the scrolling it suits.
uint16_t PPU::nametable_offset(const uint16_t addr) const
{
    const uint16_t index = (addr - 0x2000) & 0x0FFF;
    const uint16_t screen = index / 0x0400;
    const uint16_t within = index & 0x03FF;

    const bool horizontal = bus->rom.horizontal_mirroring;
    const uint16_t bank = horizontal ? (screen >> 1) : (screen & 1);

    return static_cast<uint16_t>(bank * 0x0400 + within);
}

// Palette RAM is 32 bytes mirrored every $20 through to $3FFF. On top of that,
// $3F10/$3F14/$3F18/$3F1C are not entries of their own: they are aliases of
// $3F00/$3F04/$3F08/$3F0C. Those four are the backdrop colour and the unused
// entry of each sprite palette, and hardware wires them to the same cells - so
// a write to $3F10 is observable at $3F00.
uint16_t PPU::palette_offset(const uint16_t addr)
{
    uint16_t index = addr & 0x001F;

    if ((index & 0x13) == 0x10) {
        index &= ~0x10;
    }

    return index;
}

uint8_t PPU::ppu_bus_read(const uint16_t addr)
{
    const uint16_t a = addr & vram_addr_mask;

    if (a < 0x2000) {
        // Pattern tables live on the cartridge: CHR-ROM if it brought any,
        // otherwise the console's CHR-RAM.
        // Routed through chr_read rather than indexed directly: on CNROM the
        // visible 8KB is whichever bank the cartridge last latched, so
        // indexing chr_rom would pin it to bank 0 forever.
        if (bus->rom.has_chr_rom()) {
            return bus->rom.chr_read(a);
        }
        return chr_ram[a];
    }

    if (a < 0x3F00) {
        // $3000-$3EFF mirrors $2000-$2EFF, which the & 0x0FFF inside
        // nametable_offset already folds away.
        return nametable_ram[nametable_offset(a)];
    }

    return palette_ram[palette_offset(a)];
}

void PPU::ppu_bus_write(const uint16_t addr, const uint8_t data)
{
    const uint16_t a = addr & vram_addr_mask;

    if (a < 0x2000) {
        // CHR-ROM is not writable; CHR-RAM is. A cartridge with CHR-ROM
        // silently discards the write, which is what the hardware does. That
        // holds for CNROM too: the bank register is written through the CPU
        // bus at $8000-$FFFF, never through this one.
        if (bus->rom.has_chr_rom()) {
            return;
        }
        chr_ram[a] = data;
        return;
    }

    if (a < 0x3F00) {
        nametable_ram[nametable_offset(a)] = data;
        return;
    }

    // Palette RAM is six bits wide. Masking on write means the stored value is
    // always a valid index into the 64-entry colour table, which matters most
    // for the rendering that will read these directly.
    palette_ram[palette_offset(a)] = data & 0x3F;
}

// 513 cycles, or 514 when the write to $4014 landed on an odd CPU cycle: the
// transfer is 256 read/write pairs plus a halt cycle, and needs one more to
// realign when it would otherwise start on the wrong phase.
//
// The parity is the CPU's, not the PPU's. Taking it from the dot counter made
// it effectively arbitrary.
//
// It also has to come from Bus::cpu_cycles rather than CPU::total_cycles. The
// two agree only until the first DMA: CPU::total_cycles does not advance while
// the CPU is halted, so after a 513-cycle transfer it is an odd number of
// cycles behind the real bus and every later DMA picks the wrong phase. That
// is invisible in a test that runs one DMA from reset and fatal in a ROM that
// runs several.
void PPU::request_OAM_DMA()
{
    remaining_dma_cycles = 513 + (bus->cpu_cycles % 2);
    dma_current_memory_source_addr = registers.OAMDMA << 8;
}

void PPU::perform_OAM_DMA_cycle()
{
    // first 1 or 2 cyles are dummy
    remaining_dma_cycles--;
    if (remaining_dma_cycles >= 512) {
        // std::cout << "DUMMY " << std::dec << remaining_dma_cycles << std::endl;
        return;
    }

    if (remaining_dma_cycles % 2) {
        //alternate read/write cycle;
        // std::cout << "READ " << std::dec << remaining_dma_cycles << std::endl;
        return;
    }

    // std::cout << "WRITE " << std::dec << remaining_dma_cycles << " (" << std::hex << dma_current_memory_source_addr << ")" << std::endl;

    bus->write(PPU::OAMDATA, bus->read(dma_current_memory_source_addr++));
}

void PPU::write(const uint16_t addr, const uint8_t data)
{
    // std::cout << "PPU WRITE " << std::hex << addr << " " << (unsigned)data << std::endl;

    // Every write drives the PPU's internal data bus latch, regardless of
    // which register it targets (including PPUSTATUS itself, which is
    // read-only: the write still reaches the bus, it just has no register to
    // land in). This latch is what PPUSTATUS's open-bus bits 0-4 read back
    // from below.
    drive_open_bus(data);

    // PPUSTATUS ($2002) is read-only. Real hardware simply ignores writes to
    // it (the byte still lands on the bus latch above, but no register is
    // updated). CPU code can legally do this (e.g. speculative writes), so
    // this must not abort.
    if (addr == PPUSTATUS) {
        return;
    }

    const RegisterMMap reg = static_cast<PPU::RegisterMMap>(addr);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
    switch (reg) {
    case PPUCTRL:
        // Ignored until the reset lockout expires. The write does not happen
        // at all, so the decoded flags must not be refreshed either.
        if (in_reset_write_lockout()) {
            break;
        }
        // Bit 7 is one of the two inputs to /NMI, so this write can move the
        // line in either direction. Enabling while the vblank flag is already
        // set drives it low immediately - that is 04-nmi_control's "Should
        // occur immediately if enabled while VBL flag is set" - and disabling
        // takes it back high, which is how 08-nmi_off_timing cancels an NMI
        // that has been asserted but not yet sampled. Writing $80 when NMI is
        // already enabled is not an edge and must not produce a second
        // interrupt; update_nmi_line, being edge-triggered, gets that for
        // free rather than needing a "was it already enabled" guard.
        registers.PPUCTRL = data;
        // $2000 write: t: ...GH.. ........ <- d: ......GH
        //
        // The nametable select is not a register of its own either - bits 0-1
        // of $2000 ARE bits 10-11 of t. base_nametable_addr, which update_flags
        // derives below, is the same two bits in another form; t is the one
        // rendering reads.
        temp_addr = static_cast<uint16_t>((temp_addr & ~0x0C00) | ((data & 0x03) << 10));
        update_flags();
        break;
    case PPUMASK:
        if (in_reset_write_lockout()) {
            break;
        }
        registers.PPUMASK = data;
        update_flags();
        break;
    case OAMADDR:
        registers.OAMADDR = data;
        break;
    case OAMDATA:
        // Byte 2 of each sprite is its attributes, and bits 2-4 of that byte
        // have no storage in the PPU at all - they are not merely masked on
        // read. Dropping them here rather than on the read path means sprite
        // rendering cannot see them either, which is what "do not exist"
        // means.
        //
        // This is what blargg's oam_stress measures: its failure map marks
        // every index where n % 4 == 2 and nothing else.
        OAM_memory[registers.OAMADDR] = (registers.OAMADDR % 4 == 2) ? (data & 0xE3) : data;
        ++registers.OAMADDR;
        break;
    case PPUSCROLL:
        if (in_reset_write_lockout()) {
            break;
        }
        // $2005 is a write port into t and x, not a register of its own.
        //
        //   first  (w=0):  t: ....... ...ABCDE <- d: ABCDE...
        //                  x: FGH              <- d: .....FGH ; w: <- 1
        //   second (w=1):  t: FGH..AB CDE..... <- d: ABCDEFGH ; w: <- 0
        //
        // The first write puts the X scroll's top five bits into coarse X and
        // its low three into fine X; the second splits the Y scroll the same
        // way between coarse Y (bits 5-9) and fine Y (bits 12-14).
        if (high_byte_input) {
            temp_addr = static_cast<uint16_t>((temp_addr & ~0x001F) | (data >> 3));
            fine_x = data & 0x07;
        } else {
            temp_addr = static_cast<uint16_t>((temp_addr & ~0x73E0) | ((data & 0x07) << 12) |
                                              ((data & 0xF8) << 2));
        }
        high_byte_input = !high_byte_input;
        break;
    case PPUADDR:
        if (in_reset_write_lockout()) {
            break;
        }
        //   first  (w=0):  t: .CDEFGH ........ <- d: ..CDEFGH
        //                  t: bit 14 cleared             ; w: <- 1
        //   second (w=1):  t: ....... ABCDEFGH <- d: ABCDEFGH
        //                  v: <- t                       ; w: <- 0
        //
        // Masking the incoming byte with $3F is what clears bit 14 and drops
        // the two bits above the 14-bit address space in one go. The pair is
        // staged in t and only committed to v by the second write, so a
        // PPUDATA access in between still uses the previous address.
        if (high_byte_input) {
            temp_addr = static_cast<uint16_t>((temp_addr & 0x00FF) | ((data & 0x3F) << 8));
        } else {
            temp_addr = static_cast<uint16_t>((temp_addr & 0xFF00) | data);
            registers.PPUADDR = temp_addr;
        }
        high_byte_input = !high_byte_input;
        break;
    case PPUDATA:
        // std::cout << "WRITE TO PPUDATA " << std::hex << "(" << (unsigned)registers.PPUDATA << ") <= " << std::hex << (unsigned)data << " PTR " << registers.PPUADDR << std::endl;
        // Must use vram_step, not ++, so the write path agrees with the read
        // path below.
        ppu_bus_write(registers.PPUADDR, data);
        advance_vram_address();
        break;
    case OAMDMA:
        registers.OAMDMA = data;
        // std::cout << "WRITE TO OAMDMA " << std::hex << "(" << (unsigned)registers.OAMDMA << ") <= " << std::hex << (unsigned)data << std::endl;
        request_OAM_DMA();
        break;
    }
#pragma GCC diagnostic pop

    update_nmi_line();
}

// /NMI is a level: the PPU pulls it low for as long as the vblank flag is set
// and PPUCTRL bit 7 asks for an interrupt. Only its edges are interesting to
// the CPU, so this reports them and nothing else.
void PPU::update_nmi_line()
{
    const bool asserted = nmi_line_asserted();
    if (asserted == nmi_line) {
        return;
    }

    nmi_line = asserted;
    if (asserted) {
        bus->cpu.raise_NMI();
    } else {
        bus->cpu.lower_NMI();
    }
}

uint8_t PPU::read(const uint16_t addr)
{
    // PPUCTRL, PPUMASK, OAMADDR, PPUSCROLL and PPUADDR are write-only.
    // Reading them is perfectly legal from the CPU's point of view (nestest
    // and plenty of real games do it, intentionally or not); real hardware
    // returns whatever is currently sitting on the PPU's internal data bus
    // latch rather than trapping. We approximate that with `open_bus`,
    // updated on every register access below and in PPU::write.
    const RegisterMMap reg = static_cast<PPU::RegisterMMap>(addr);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
    switch (reg) {
    case PPUSTATUS: {
        // A read landing on the exact dot the vblank flag would be set kills
        // the set outright. The CPU's access is placed just ahead of that
        // dot's tick (see Bus::clock), so the flag is not set yet and this
        // read reports it clear either way; the flag on `this` is what stops
        // the tick from setting it a moment later.
        if (scanline == vblank_start_scanline && cycle == 1) {
            suppress_vblank_flag_set = true;
        }

        // Bits 0-4 are unused/open bus: they reflect the last value driven on
        // the PPU data bus rather than real status bits.
        uint8_t data = (registers.PPUSTATUS & 0xE0) | (open_bus & 0x1F);
        registers.PPUSTATUS &= 0x7F;
        // A PPUSTATUS read resets the $2005/$2006 write pair. It clears only
        // the write toggle; hardware leaves the addresses themselves intact,
        // so PPUSCROLL/PPUADDR must not be zeroed here.
        high_byte_input = true;
        drive_open_bus(data, 0xE0);

        // Clearing the flag releases /NMI. If the CPU has not sampled the
        // assertion yet, this is what suppresses the interrupt entirely
        // (06-suppression rows 05 and 06: flag reads back set, no NMI).
        update_nmi_line();
        return data;
    };
    case OAMDATA:
        // Unlike $2007, a read of $2004 does NOT advance the address. Software
        // reading OAM back has to drive $2003 for each byte.
        drive_open_bus(OAM_memory[registers.OAMADDR]);
        return open_bus;

    case PPUDATA: {
        const uint16_t addr = registers.PPUADDR & vram_addr_mask;
        const uint8_t fetched = ppu_bus_read(addr);

        // Reads below $3F00 are buffered: what comes back is the value the
        // PREVIOUS read fetched, and this read refills the latch. Software
        // therefore has to read $2007 twice to get the first byte, which is a
        // thing real games rely on rather than a quirk to smooth over.
        //
        // Palette reads are not buffered - they return immediately - but the
        // latch is still refilled, from the NAMETABLE underneath the palette
        // ($2F00-$2FFF mirrored), not from the palette itself.
        uint8_t result;
        if (addr < 0x3F00) {
            result = vram_read_buffer;
            vram_read_buffer = fetched;
        } else {
            // Palette RAM drives only bits 0-5 of the bus. Bits 6-7 are left
            // to whatever the open bus already held, so a palette read does not
            // fully define the byte it returns.
            //
            // Greyscale (PPUMASK bit 0) also applies here, not just to
            // rendering: it forces the low four bits, collapsing every colour
            // onto the grey column of the palette.
            const uint8_t value = fetched & (greyscale ? 0x30 : 0x3F);
            result = static_cast<uint8_t>((open_bus & 0xC0) | value);

            // The latch is still refilled, from the nametable UNDERNEATH the
            // palette. Routed through ppu_bus_read so the decode stays in one
            // place: $3F00-$3FFF folds onto $2F00-$2FFF.
            vram_read_buffer = ppu_bus_read(static_cast<uint16_t>(addr & 0x2FFF));
        }

        advance_vram_address();

        // A palette read drives only bits 0-5; bits 6-7 were never driven, so
        // they keep ageing from whenever they last were. A buffered read drives
        // all eight.
        drive_open_bus(result, addr < 0x3F00 ? 0xFF : 0x3F);
        return result;
    }
    default:
        // Write-only register read as open bus.
        return open_bus;
    }
#pragma GCC diagnostic pop
    return 0;
}

void PPU::update_flags()
{
    // NOTE: this reads the internal register state directly (registers.*),
    // not through PPU::read(). PPU::read() models CPU-facing register access
    // semantics (e.g. PPUCTRL/PPUMASK are write-only from the CPU's side and
    // now correctly return open-bus there); calling it here would either hit
    // the write-only open-bus path (wrong value) or, previously, an assert.
    // This function needs the PPU's own idea of what was last written.
    base_nametable_addr = 0x2000 + 0x400 * (registers.PPUCTRL & 0x3);
    vram_step = registers.PPUCTRL & 0x4 ? Vertical : Horizontal;
    // Bit 3 selects the sprite pattern table and bit 4 the background one;
    // in both cases 0 means $0000 and 1 means $1000.
    sprite_pattern_8x8_table_addr = (registers.PPUCTRL & 0x8) ? 0x1000 : 0x0000;
    bg_pattern_table_address = (registers.PPUCTRL & 0x10) ? 0x1000 : 0x0000;
    big_sprites = (registers.PPUCTRL & 0x20);
    // bit 6 PPU master/slave mode. Not used in NES.
    MMI_on_V_Blank = registers.PPUCTRL & 0x80;

    greyscale = registers.PPUMASK & 0x1;
    show_bg_in_leftmost = registers.PPUMASK & 0x2;
    show_sprites_in_leftmost = registers.PPUMASK & 0x4;
    show_background = registers.PPUMASK & 0x8;
    show_sprites = registers.PPUMASK & 0x10;
    emphasize_red = registers.PPUMASK & 0x20;
    emphasize_green = registers.PPUMASK & 0x40;
    emphasize_blue = registers.PPUMASK & 0x80;
}
