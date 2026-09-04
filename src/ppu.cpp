#include "../include/ppu.h"

#include <algorithm>
#include <cstring>

#include "../include/bus.h"

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

    // The $2007 read buffer refill, landing some dots after the read returned -
    // see PPU::pending_read_buffer.
    //
    // TWO MUTATIONS SURVIVE HERE AND NEITHER IS A HOLE. Comparing against 1
    // rather than 0 commits a dot early, which is the same as a delay of 4 - and
    // 4, 5 and 6 are all admissible, so no oracle can separate them; that slack
    // IS the measured window, not a gap in the tests. Dropping the != 0 guard
    // lets the counter wrap and re-commit every 256 dots, which is idempotent:
    // by then vram_read_buffer already holds pending_read_buffer, and any new
    // read resets the counter before it matters.
    if (pending_read_buffer_dots != 0 && --pending_read_buffer_dots == 0) {
        vram_read_buffer = pending_read_buffer;
    }

    decay_open_bus();

    // OAM DMA is NOT driven from here. It steals CPU cycles, so it advances at
    // the CPU's rate; Bus::clock runs it. Ticking it once per dot made the
    // transfer three times too short.

    // An if-chain rather than a switch, because `case 0 ... 239:` is a GCC
    // range extension and -pedantic rejects it. The two warnings it produced
    // were invisible in practice: this file only recompiles when something it
    // includes changes, so an ordinary incremental build printed nothing and
    // looked clean.
    //
    // The bounds are written with both ends explicit. `scanline` is signed, so
    // a bare `scanline < post_render_scanline` would route a negative value
    // into the visible-scanline path instead of the error branch below, which
    // is the one behaviour the switch had for free.
    if (scanline >= 0 && scanline < post_render_scanline) {
        process_visible_scanline();
    } else if (scanline == post_render_scanline) {
        // idle
    } else if (scanline == vblank_start_scanline) {
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
    } else if (scanline > vblank_start_scanline && scanline < pre_render_scanline) {
        // Idle. NESdev's PPU rendering page, on the vblank lines after 241:
        // "The PPU makes no
        // memory accesses during these scanlines, so PPU memory can be freely
        // accessed by the program."
        //
        // 241 is the only vblank line that does anything - it sets the flag at
        // tick 1 - and it is the branch directly above. These nineteen burn
        // dots until the pre-render line, so an empty body is the correct model
        // rather than an unfinished one.
        // https://www.nesdev.org/wiki/PPU_rendering
    } else if (scanline == pre_render_scanline) {
        if (cycle == 0) {
            oam_refresh_bug();
        }
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
    } else {
        std::cerr << "Scanline out of range: " << scanline << std::endl;
    }

    advance_dot();

    // Anything above may have moved the vblank flag, and /NMI follows it.
    update_nmi_line();
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
// The framebuffer holds a 6-bit palette INDEX plus three emphasis bits, not a
// colour. Both are captured here and for different reasons: greyscale because
// it is part of the palette LOOKUP, emphasis because $2001 can be rewritten
// mid-frame, so it varies per scanline and nothing later can recover it. What
// emphasis does to the colour is still the display's job - see apply_emphasis
// in frame_dump.h - and only the capture happens here.
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

    // --- the sprite multiplexer ------------------------------------------
    //
    // Only ONE sprite reaches the pixel: the lowest-numbered secondary-OAM slot
    // with an opaque pixel here. Secondary OAM is filled in primary-OAM order,
    // so "lowest slot" is "lowest OAM index", and that IS the sprite-versus-
    // sprite priority rule - a sprite occludes every higher-numbered sprite
    // regardless of its own priority bit. Attribute bit 5 decides only sprite
    // versus BACKGROUND, below.
    const bool sprites_visible = show_sprites && (x >= 8 || show_sprites_in_leftmost);

    uint8_t sprite_pixel = 0;
    uint8_t sprite_palette = 0;
    bool sprite_behind_background = false;
    bool sprite_zero_here = false;

    for (int i = 0; i < sprite_count; ++i) {
        // The X counter has not counted down to this sprite's left edge yet.
        if (sprite_x_counter[i] != 0) {
            continue;
        }

        const uint8_t bits = static_cast<uint8_t>(((sprite_pattern_shift_low[i] >> 7) & 0x01) |
                                                  (((sprite_pattern_shift_high[i] >> 7) & 0x01) << 1));
        if (bits == 0) {
            continue;
        }

        sprite_pixel = bits;
        sprite_palette = static_cast<uint8_t>(sprite_attribute_latch[i] & 0x03);
        sprite_behind_background = (sprite_attribute_latch[i] & 0x20) != 0;
        sprite_zero_here = (i == 0) && sprite_zero_in_units;
        break;
    }

    // --- sprite 0 hit -----------------------------------------------------
    //
    // NESdev, PPUSTATUS bit 6: set "when any opaque pixel of sprite 0 overlaps
    // an opaque pixel of background, REGARDLESS OF SPRITE PRIORITY" - so
    // sprite_behind_background is deliberately NOT consulted here. Nor does it
    // matter whether sprite 0 won the multiplexer, though in practice it always
    // does: it sits in slot 0, so an opaque pixel of it beats every other slot.
    //
    // "Cannot detect collision at X=255, nor anywhere where either sprites or
    // backgrounds are disabled via PPUMASK. This includes X=0..7 when the
    // leftmost 8 pixels are hidden." Both left-clips are already folded in -
    // `pixel` came through background_visible and `sprite_pixel` through
    // sprites_visible - so a hit under either hidden column cannot reach here.
    //
    // X=255 is excluded by the hardware itself: the pixel pipeline has nowhere
    // to carry the comparison to. Deleting that one condition makes blargg's
    // 11.edge_timing report FAILED #3 and 06.right_edge FAILED #2.
    if (sprites_visible && sprite_zero_here && sprite_pixel != 0 && pixel != 0 && x != 255) {
        set_sprite0_hit();
    }

    // --- background versus sprite ----------------------------------------
    //
    // A transparent pixel always loses. When both are opaque, attribute bit 5
    // decides. Sprite palettes live in the upper half of palette RAM at $3F10.
    // FORCED BACKDROP, the "background palette hack". With both rendering bits
    // clear the PPU is in forced blank and would normally drive the backdrop -
    // but palette RAM has only ONE address input, so whatever v holds is what
    // the palette outputs. NESdev's PPU palettes page:
    //
    //   "During forced blank, the PPU normally draws the backdrop color.
    //    However, if the current VRAM address in v points into palette RAM
    //    ($3F00-$3FFF), then the color at that address will be drawn, instead,
    //    overriding the backdrop color."
    //
    // v is used unmodified, and that is the point rather than laziness: it is
    // the same single decoder, so the $3F10/$14/$18/$1C aliases resolve exactly
    // as they do during rendering. It is also why $3F04/$3F08/$3F0C become
    // visible here - during rendering nothing ever selects them, so this is
    // their only route to the screen.
    //
    // Micro Machines depends on this, and blargg's full_palette suite is built
    // entirely on it, which is what finally made it testable.
    //
    // Masked with $3F00 rather than compared against a range because the PPU
    // bus is 14 bits while v is 15 - bit 14 is not an address line here.
    const bool forced_blank = !rendering_enabled();

    uint16_t palette_addr;
    if (forced_blank && (registers.PPUADDR & 0x3F00) == 0x3F00) {
        palette_addr = registers.PPUADDR;
    } else if (sprites_visible && sprite_pixel != 0 && (pixel == 0 || !sprite_behind_background)) {
        palette_addr = static_cast<uint16_t>(0x3F10 + (sprite_palette << 2) + sprite_pixel);
    } else if (pixel == 0) {
        // Colour 0 of every palette is not stored: it reads through to the
        // backdrop at $3F00. A transparent pixel and a disabled background are
        // therefore the same pixel.
        palette_addr = 0x3F00;
    } else {
        palette_addr = static_cast<uint16_t>(0x3F00 + (palette << 2) + pixel);
    }

    // Greyscale (PPUMASK bit 0) forces the low four bits of the index, which
    // collapses every hue onto the grey column of the NES palette.
    const uint8_t colour = static_cast<uint8_t>(ppu_bus_read(palette_addr) & (greyscale ? 0x30 : 0x3F));

    // Emphasis is captured per pixel rather than read once by the display.
    // full_palette rewrites $2001 mid-frame - `tya / and #$E0 / sta $2001` - so
    // it changes between scanlines of one picture, and a display path sampling
    // the current PPUMASK would paint every row with the last value written.
    //
    // It rides alongside the index instead of being folded into it because the
    // two are separate hardware stages: greyscale masks the palette LOOKUP,
    // emphasis attenuates the signal that comes OUT. Merging them here would
    // discard which is which. See the framebuffer declaration for the encoding.
    // Taken straight off PPUMASK rather than reassembled from the three decoded
    // bools: they are bits 5, 6 and 7 in that order, so this IS the same field,
    // and rebuilding it would be redundant work on the hottest loop here -
    // 61,440 pixels a frame.
    const uint16_t emphasis = static_cast<uint16_t>((registers.PPUMASK >> 5) & 0x07);
    framebuffer[scanline * screen_width + x] = static_cast<uint16_t>(colour | (emphasis << 6));

    // The units advance at the END of the dot, so the pixel chosen above is the
    // one the counters described on entry. A sprite whose counter has reached
    // zero shifts one pixel out of the top of its pattern registers; after
    // eight shifts both bytes are zero and the sprite stops being opaque of its
    // own accord, which is why nothing here tracks a per-sprite pixel count.
    for (int i = 0; i < sprite_count; ++i) {
        if (sprite_x_counter[i] != 0) {
            --sprite_x_counter[i];
            continue;
        }
        sprite_pattern_shift_low[i] = static_cast<uint8_t>(sprite_pattern_shift_low[i] << 1);
        sprite_pattern_shift_high[i] = static_cast<uint8_t>(sprite_pattern_shift_high[i] << 1);
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

// OAM byte 0 is the Y one LESS than the first line the sprite appears on -
// sprites are delayed a scanline by evaluation running a line ahead - so a
// sprite with Y = 0 first appears on scanline 1, and one with Y = 255 never
// appears at all.
//
// Evaluation on line L is for line L+1, so the sprite covers L+1 when
// (L+1) - (Y+1) is within its height; the two +1s cancel and the comparison is
// against the CURRENT scanline. Writing it as target_scanline - (Y + 1) would
// be the same arithmetic spelled less obviously.
bool PPU::sprite_in_range(const uint8_t y) const
{
    const int row = scanline - static_cast<int>(y);
    // Read live, not latched at the start of the line: 5.Emulator's test 5
    // changes sprite height part-way through and expects the overflow flag's
    // timing to move with it.
    return row >= 0 && row < (big_sprites ? 16 : 8);
}

// Dots 1-64. Hardware writes $FF into one byte of secondary OAM on each even
// dot, which is why this is 32 writes across 64 dots rather than a memset.
//
// $FF matters as a value, not just as "cleared": it is a Y of 255, which
// sprite_in_range rejects on every scanline, so an unfilled slot is naturally
// an empty one.
void PPU::clear_secondary_oam_dot()
{
    if ((cycle % 2) == 0) {
        secondary_oam[(cycle / 2) - 1] = 0xFF;
    }
}

// The 2C02G/H OAM hardware refresh bug.
//
// NESdev, $2003: "if OAMADDR is not less than eight when rendering starts, the
// eight bytes starting at OAMADDR & 0xF8 are copied to the first eight bytes of
// OAM". The sprite-evaluation page states the same effect with the condition
// "not zero" rather than "not less than eight" - the two agree, because
// OAMADDR & $F8 is 0 for any OAMADDR below 8 and the copy is then OAM[0..7]
// onto itself.
//
// "When rendering starts" is once per FRAME, not once per scanline, which is
// why this sits on the pre-render line rather than inside the per-line
// evaluation. It is filed under Errata and is revision-specific: no ROM in the
// suite covers it, and it is modelled because leaving it out while modelling
// the adjacent non-zero-OAMADDR behaviour in detail was an inconsistency an
// adversarial review flagged, not because a game is known to need it.
void PPU::oam_refresh_bug()
{
    if (!rendering_enabled() || registers.OAMADDR < 8) {
        return;
    }

    const uint8_t source = static_cast<uint8_t>(registers.OAMADDR & 0xF8);
    for (int i = 0; i < 8; ++i) {
        OAM_memory[i] = OAM_memory[static_cast<uint8_t>(source + i)];
    }
}

// Dot 65. Evaluation starts at OAMADDR exactly, INCLUDING its low two bits.
//
// NESdev, $2003: "If OAMADDR is unaligned and does not point to the Y position
// (first byte) of an OAM entry, then whatever it points to (tile index,
// attribute, or X coordinate) will be reinterpreted as a Y position, and the
// following bytes will be similarly reinterpreted."
//
// So m is the byte index within the record, seeded from OAMADDR rather than
// forced to 0. Masking OAMADDR with $FC to align it to a record boundary is the
// obvious reading and is not what the hardware does - and a test here once
// asserted the masked form as though it were a hardware fact.
void PPU::begin_sprite_evaluation()
{
    sprite_eval_oamaddr = registers.OAMADDR;

    sprite_eval_n = static_cast<uint8_t>(sprite_eval_oamaddr >> 2);
    sprite_eval_m = static_cast<uint8_t>(sprite_eval_oamaddr & 0x03);
    sprite_eval_copy = 0;
    sprite_eval_found = 0;
    sprite_eval_latch = 0;
    sprite_zero_in_secondary = false;
    sprite_eval_state = SpriteEvalState::ScanY;
}

// Dots 65-256, one dot at a time.
//
// Hardware does one primary-OAM read on each ODD dot and one secondary-OAM
// write (or the decision that stands in for it) on each EVEN dot. Splitting it
// that way is not decoration: it is what puts the overflow flag's set on a
// specific dot, which is the whole of 3.Timing.
void PPU::tick_sprite_evaluation()
{
    if (cycle % 2) {
        sprite_evaluation_read();
    } else {
        sprite_evaluation_write();
    }
}

// The odd dot. There is only ever ONE address: 4n + m.
//
// A normal scan holds m at 0 and steps n, so it reads each sprite's Y. The
// buggy overflow scan steps BOTH, so it reads some other byte of a later
// sprite and treats that as a Y. Sharing the address expression is what makes
// the bug a property of how n and m advance rather than a special case here.
void PPU::sprite_evaluation_read()
{
    if (sprite_eval_state == SpriteEvalState::Done) {
        return;
    }

    const uint8_t address = static_cast<uint8_t>(sprite_eval_n * 4 + sprite_eval_m);
    sprite_eval_latch = OAM_memory[address];
}

// One byte forward through OAM, carrying from m into n. A copy walks the array
// linearly; only the out-of-range path in ScanY skips a whole record at a time.
void PPU::sprite_evaluation_advance_byte()
{
    ++sprite_eval_m;
    if (sprite_eval_m == 4) {
        sprite_eval_m = 0;
        sprite_evaluation_advance_n();
    }
}

// n only ever moves forward, and the scan ends when it runs off the end of OAM
// rather than wrapping. That is why a non-zero OAMADDR makes the sprites below
// it disappear instead of being picked up on a second pass.
void PPU::sprite_evaluation_advance_n()
{
    ++sprite_eval_n;
    if (sprite_eval_n > 63) {
        sprite_eval_state = SpriteEvalState::Done;
    }
}

// The even dot: what the byte just read means, and where the scan goes next.
//
// Transcribed from NESdev "PPU sprite evaluation". Every caller of
// sprite_evaluation_advance_n leaves it until LAST, because it can force the
// Done state and must not have that overwritten by a state assignment after it.
void PPU::sprite_evaluation_write()
{
    switch (sprite_eval_state) {
    case SpriteEvalState::ScanY: {
        const uint8_t y = sprite_eval_latch;

        // The Y is written into the next free slot BEFORE it is known to be in
        // range; an out-of-range one is simply overwritten by the next
        // candidate. Hardware does this because the write is unconditional and
        // the range check gates only the following three bytes.
        if (sprite_eval_found < 8) {
            secondary_oam[sprite_eval_found * 4] = y;
        }

        if (sprite_in_range(y)) {
            if (sprite_eval_found < 8) {
                // Slot 0 is the only one that can raise the sprite 0 hit, and
                // it counts as "sprite 0" only when it holds the sprite the
                // scan STARTED at - not merely whichever sprite happened to
                // land in slot 0.
                //
                // NESdev, $2003: a non-zero OAMADDR "can cause the sprite at
                // OAMADDR to be treated as it was sprite 0, both for sprite-0
                // hit and priority". So the comparison is against the starting
                // index, not against 0. Writing it as `n == 0` looks equivalent
                // - and is, whenever OAMADDR is 0, which is every blargg
                // sprite_hit ROM - but it silently drops the whole behaviour
                // the moment a game moves OAMADDR mid-screen.
                //
                // If the sprite at OAMADDR is off this line it never reaches
                // slot 0 at all, and the later sprite that does is NOT sprite
                // 0: the line simply has no hit candidate.
                if (sprite_eval_found == 0) {
                    sprite_zero_in_secondary = (sprite_eval_n == (sprite_eval_oamaddr >> 2));
                }
                // The destination offset in secondary OAM is counted
                // separately from the source byte index. They coincide only
                // when OAMADDR was aligned; a misaligned scan copies four
                // CONSECUTIVE bytes starting at the reinterpreted Y, which is
                // not the same as bytes 1-3 of the record it happens to be in.
                sprite_eval_copy = 1;
                sprite_evaluation_advance_byte();
                sprite_eval_state = SpriteEvalState::CopySprite;
            } else {
                // The 9th in-range sprite. This dot is the one 3.Timing
                // measures.
                set_sprite_overflow();
                sprite_eval_copy = 1;
                sprite_evaluation_advance_byte();
                sprite_eval_state = SpriteEvalState::OverflowCopy;
            }
            break;
        }

        // Out of range.
        if (sprite_eval_found >= 8) {
            // THE OVERFLOW SEARCH BUG, emulated as buggy on purpose.
            //
            // Once eight sprites have been found the byte index advances
            // ALONGSIDE the sprite index instead of staying at 0, so sprite n
            // has byte (n mod 4) misread as its Y. NESdev calls this
            // "increment n AND m (without carry)".
            //
            // 4.Obscure tests exactly this and nothing else: its test 2 wants
            // the second byte of sprite #10 treated as a Y, test 3 the third
            // byte of sprite #11, and so on. Advancing only n - the "sensible"
            // reading - makes every one of those checks fail.
            sprite_eval_m = static_cast<uint8_t>((sprite_eval_m + 1) & 0x03);
        }
        sprite_evaluation_advance_n();
        break;
    }

    case SpriteEvalState::CopySprite:
        secondary_oam[sprite_eval_found * 4 + sprite_eval_copy] = sprite_eval_latch;
        ++sprite_eval_copy;
        sprite_evaluation_advance_byte();
        if (sprite_eval_copy == 4) {
            sprite_eval_copy = 0;
            ++sprite_eval_found;
            sprite_eval_state = SpriteEvalState::ScanY;
        }
        break;

    case SpriteEvalState::OverflowCopy:
        // Three dummy reads whose values go nowhere - secondary OAM is full.
        // They still cost dots, which is the only reason they are modelled.
        ++sprite_eval_copy;
        sprite_evaluation_advance_byte();
        if (sprite_eval_copy == 4) {
            sprite_eval_copy = 0;
            sprite_eval_state = SpriteEvalState::ScanY;
        }
        break;

    case SpriteEvalState::Done:
        // Hardware keeps reading OAM[n][0] and failing to copy it until
        // hblank. Nothing observable comes of it.
        break;
    }
}

// Dot 257. Freezes the evaluation's result into the units the next line will
// draw from.
//
// Safe to overwrite the units here because this line's own pixels (dots 1-256)
// are finished. The units are single-buffered for exactly that reason.
void PPU::load_sprite_units()
{
    sprite_count = sprite_eval_found;
    sprite_zero_in_units = sprite_zero_in_secondary;

    // Units past the sprite count are cleared rather than left stale. A sprite
    // count that shrinks between lines would otherwise leave the previous
    // line's pattern bits in a unit that render_pixel no longer bounds-checks
    // its way past.
    for (int i = 0; i < 8; ++i) {
        sprite_pattern_shift_low[i] = 0;
        sprite_pattern_shift_high[i] = 0;
        sprite_attribute_latch[i] = 0;
        sprite_x_counter[i] = 0xFF;
    }
}

// Dots 257-320: eight groups of eight dots, one sprite each.
//
// The group's internal schedule mirrors a background tile fetch - two dots of
// garbage nametable, two of garbage attribute, then pattern low and pattern
// high - because it IS the same fetch machinery, pointed at OAM instead of the
// nametable.
//
// So the accesses sit on the SAME phase as fetch_background_byte's: the address
// goes on the bus at the FIRST dot of each two-dot pair. For the first sprite
// that puts pattern low at 257 + 4 = 261 and pattern high at 263, matching
// background tile 0's 5 and 7.
//
// That agreement is not cosmetic. These two functions are the only places the
// MMC3 counter can see A12 during rendering, and 4-scanline_timing measures the
// distance between them to the dot: it hard-codes the scanline-0 IRQ as 256
// dots later with $2000=$08 (sprites at $1000, background at $0000, so the
// sprite fetch is the only thing that raises A12) than with $2000=$10 (the
// reverse, so the first background pattern fetch raises it). 5 + 256 = 261, and
// the ROM fails on a discrepancy of one. This WAS 262 - the pair's second dot -
// and that single dot was the whole of the failure it reported for a long time.
//
// The two GARBAGE fetches of each group, at dots 257-258 and 259-260, are two
// garbage NAMETABLE bytes - NOT a nametable and an attribute, which is the
// natural reading and the wrong one. NESdev's PPU rendering page lists the
// group as "Garbage nametable byte, Garbage nametable byte, Pattern table tile
// low, Pattern table tile high". Both are $2xxx either way, so nothing
// downstream depends on it.
//
// Same page: "All garbage nametable bytes except the first are the same address
// as the first nametable fetch on the upcoming scanline", the first being "a
// mix due to the PPU's bus being multiplexed". That multiplexing is not
// modelled and both go out at tile_address(). Bounded to nothing observable
// here: what a mapper sees is A12, and $2xxx holds it low whichever nametable
// address it is.
//
// Driving them at all matters because A12 LOW is as observable as A12 high.
// Without them A12 stayed high across dots 257-260 of every group; with them it
// drops for four dots. Four is under mmc3_a12_filter_dots so no extra edge is
// counted on an ordinary line - which is exactly why omitting them looked
// harmless for so long - but the filter is a threshold, not an absence, and
// 8x16 sprites alternating pattern tables push the low period past it.
void PPU::fetch_sprite_pattern()
{
    const int index = (cycle - 257) / 8;
    const int step = (cycle - 257) % 8;

    // The garbage pair, on the same phase rule as everything else here: the
    // address goes out on the FIRST dot of each two-dot access. Ahead of the
    // dummy branch below, because hardware performs these on all eight groups
    // whether or not the slot holds a sprite.
    if (step == 0 || step == 2) {
        (void)ppu_bus_read(tile_address());
    }

    // Hardware performs EIGHT sprite pattern fetches on every rendering line,
    // however few sprites the line actually has. The empty slots read tile $FF,
    // because that is what the dot 1-64 clear left in secondary OAM, and the
    // read still happens - the PPU has no way to skip a fetch, the eight groups
    // of eight dots are wired into the timing.
    //
    // For eight lines of NROM output that is invisible, which is why this used
    // to return early. It stops being invisible the moment a mapper watches the
    // address bus: with the background at $0000 and sprites at $1000 - the
    // standard MMC3 layout - the ONLY thing that raises A12 on a line is the
    // sprite fetch. Skipping it on a line with no sprites means skipping that
    // line's IRQ clock, so a raster split drifts by one scanline for every
    // empty line above it. 4-scanline_timing is what catches that.
    const bool dummy = index >= sprite_count;

    if (dummy) {
        // The address of a dummy fetch: tile $FF out of the sprite pattern
        // table. Only bit 12 of it is ever observed, and that bit is the table
        // select - which is the whole reason this branch exists.
        //
        // In 8x16 mode the table comes from bit 0 of the tile number rather
        // than PPUCTRL, and tile $FF has that bit set, so the dummy fetches
        // land at $1FF0 regardless of what PPUCTRL says. That is not a detail
        // to smooth over: it is why an 8x16 game gets its A12 rise on lines
        // with no sprites even with PPUCTRL pointing at $0000.
        const uint16_t table = big_sprites ? 0x1000 : sprite_pattern_8x8_table_addr;
        const uint16_t dummy_address = static_cast<uint16_t>(table + 0x0FF0);

        // The fetched bytes are deliberately DISCARDED rather than loaded into
        // a unit. Hardware does load them, but into a unit whose X counter is
        // $FF, so it can never reach the screen; render_pixel stops at
        // sprite_count and would ignore them anyway. Reading and dropping keeps
        // the bus activity - the part that is observable - without putting
        // anything into the rendering state that could change a pixel.
        if (step == 4) {
            (void)ppu_bus_read(dummy_address);
        } else if (step == 6) {
            (void)ppu_bus_read(static_cast<uint16_t>(dummy_address + 8));
        }
        return;
    }

    const uint8_t y = secondary_oam[index * 4];
    const uint8_t tile = secondary_oam[index * 4 + 1];
    const uint8_t attributes = secondary_oam[index * 4 + 2];

    // The row within the sprite, by the same reasoning as sprite_in_range: the
    // sprite is drawn on the NEXT line, and OAM's Y is one less than its first
    // line, so the two offsets cancel.
    const int row = scanline - static_cast<int>(y);
    const int height = big_sprites ? 16 : 8;
    const bool flip_vertically = attributes & 0x80;

    // Vertical flip mirrors within the WHOLE sprite, so for an 8x16 it swaps
    // the two tiles as well as the rows inside them - which the arithmetic
    // below gets for free by flipping the row index BEFORE splitting it into
    // "which tile" and "which row of that tile". Flipping after the split
    // would mirror each tile in place and leave the halves in the wrong order.
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

    const bool flip_horizontally = attributes & 0x40;

    switch (step) {
    case 2:
        // Bits 2-4 of the attribute byte do not exist in OAM at all (see the
        // OAMDATA write path), so what lands here is already only the palette,
        // priority and flip bits.
        sprite_attribute_latch[index] = attributes;
        break;
    case 3:
        sprite_x_counter[index] = secondary_oam[index * 4 + 3];
        break;
    case 4: {
        const uint8_t low = ppu_bus_read(address);
        sprite_pattern_shift_low[index] = flip_horizontally ? reverse_bits(low) : low;
        break;
    }
    case 6: {
        // The two bitplanes of a tile are eight bytes apart, not interleaved.
        const uint8_t high = ppu_bus_read(static_cast<uint16_t>(address + 8));
        sprite_pattern_shift_high[index] = flip_horizontally ? reverse_bits(high) : high;
        break;
    }
    default:
        // The second dot of each pair, where hardware holds the same address on
        // the bus and latches the byte. Steps 2 and 3 above are the attribute
        // and X bytes, which come out of secondary OAM rather than off the
        // address bus, so their dot within the group is unobservable and is
        // only placed in the garbage-attribute window for tidiness.
        break;
    }
}

// The background pipeline, for the 240 visible scanlines and for the pre-render
// line, which runs the same fetches but draws nothing.
void PPU::process_visible_scanline()
{
    const bool pre_render = (scanline == pre_render_scanline);

    if (rendering_enabled()) {
        // --- the sprite pipeline, one line ahead of the pixels ------------
        //
        // Evaluation during dots 65-256 of THIS line fills secondary OAM for
        // the NEXT one, and the fetches at 257-320 load it into the output
        // units. Everything is single-buffered, which is safe only because the
        // three phases are disjoint and the units are not touched until dot
        // 257, by which time this line's own pixels (dots 1-256) are done.
        //
        // An earlier design resolved sprites at dot 0 of the line being drawn.
        // That picked up OAM rewritten between the two points a line early,
        // and - because it sat inside this rendering_enabled() guard - left a
        // stale result in place when rendering was enabled mid-frame, so a
        // line whose evaluation never happened could still report a hit.
        // Hardware has no secondary OAM for such a line and cannot.
        if (cycle >= 1 && cycle <= 64) {
            clear_secondary_oam_dot();
        }

        // Sprite evaluation reads OAM from OAMADDR onwards, so this is the
        // moment that decides which sprite lands in slot 0 and can therefore
        // raise the hit flag. Latched because the clear below destroys OAMADDR
        // before the sprites are fetched at 257-320.
        if (cycle == 65) {
            begin_sprite_evaluation();
        }

        // The pre-render line clears secondary OAM and latches OAMADDR like
        // any other, but does NOT evaluate: it would be evaluating for line 0,
        // and no sprite can appear there. A sprite is drawn on lines Y+1
        // onwards, so reaching line 0 would need Y=255, which is exactly the
        // value hardware treats as off-screen (2.Details test 7). Leaving
        // sprite_eval_found at 0 is what gives line 0 its empty unit set.
        if (!pre_render && cycle >= 65 && cycle <= 256) {
            tick_sprite_evaluation();
        }

        if (cycle == 257) {
            load_sprite_units();
        }

        if (cycle >= 257 && cycle <= 320) {
            fetch_sprite_pattern();
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

        // Dots 1-256 fetch the tiles for THIS line (two tiles ahead of the
        // pixel being drawn), 321-336 the first two of the NEXT one. Both
        // regions shift; the gap between them, where hardware is fetching
        // sprite patterns, does not.
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
    } else if (cycle == 257) {
        // No evaluation window means no sprites on the next line. Without this
        // the units keep whatever the last enabled line left in them, so
        // enabling rendering part-way down the screen would draw a line of
        // sprites that hardware never evaluated - the same stale-state bug the
        // old sprite-0 shortcut had, in its new form.
        sprite_eval_found = 0;
        sprite_zero_in_secondary = false;
        load_sprite_units();
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
//
// MMC1 adds two more: it can tie CIRAM A10 to a constant and point all four
// slots at ONE screen, giving up the second in exchange for being able to
// scroll the whole 32x30 without a seam. That is a runtime choice written to
// its control register, not a wiring decision, which is why ROM::mirroring is
// an enum the mapper assigns rather than the header bit it used to be.
uint16_t PPU::nametable_offset(const uint16_t addr) const
{
    const uint16_t index = (addr - 0x2000) & 0x0FFF;
    const uint16_t screen = index / 0x0400;
    const uint16_t within = index & 0x03FF;

    // WHICH page a slot shows is the cartridge's answer, not the PPU's: on
    // TxSROM it comes from the CHR bank registers and there is no mirroring
    // mode involved at all. The PPU's part is turning an address into a slot.
    const uint16_t bank = bus->rom.nametable_page(screen);

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

// Every access that reaches the PPU's address bus, and the one place a
// scanline-counting mapper can watch it from.
//
// The palette is the exception, and it has to be: palette RAM lives INSIDE the
// 2C02, so a palette access drives nothing externally. render_pixel reads
// $3F00-$3F1F on every visible dot, and $3F00 has bit 12 set - hooking it would
// hold A12 high for the whole of every scanline and the MMC3 counter would
// never see a single edge. Only $0000-$3EFF, the pattern tables and nametables
// that physically leave the chip, are offered to the mapper.
void PPU::observe_ppu_address_bus(const uint16_t addr)
{
    if (addr < 0x3F00) {
        bus->rom.mmc3_observe_a12(addr, total_cycles);
    }
}

uint8_t PPU::ppu_bus_read(const uint16_t addr)
{
    const uint16_t a = addr & vram_addr_mask;
    observe_ppu_address_bus(a);

    if (a < 0x2000) {
        // Pattern tables live on the cartridge: CHR-ROM if it brought any,
        // otherwise the console's CHR-RAM.
        // Routed through chr_read rather than indexed directly: on CNROM the
        // visible 8KB is whichever bank the cartridge last latched, so
        // indexing chr_rom would pin it to bank 0 forever.
        //
        // CHR-RAM goes through the mapper too. The array is ours; the address
        // decoding is the cartridge's, and MMC1 in 4KB mode pages an 8KB chip
        // as two halves. Indexing by `a` directly unbanks it silently.
        const uint8_t value =
            bus->rom.has_chr_rom() ? bus->rom.chr_read(a) : chr_ram[bus->rom.chr_ram_offset(a) % chr_ram_size];

        // AFTER the byte, and that ordering is the whole reason this is a
        // second hook rather than a line in observe_ppu_address_bus above.
        // MMC2 and MMC4 change CHR bank when the PPU reads particular tiles,
        // and the read that TRIGGERS the change is still served by the old
        // bank - the new one applies from the next fetch. Notifying first
        // would make every latching tile fetch itself out of the wrong bank,
        // which is a one-tile graphical glitch that no test would name.
        bus->rom.observe_pattern_fetch(a);
        return value;
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
    observe_ppu_address_bus(a);

    if (a < 0x2000) {
        // CHR-ROM is not writable; CHR-RAM is. A cartridge with CHR-ROM
        // silently discards the write, which is what the hardware does. That
        // holds for CNROM too: the bank register is written through the CPU
        // bus at $8000-$FFFF, never through this one.
        if (bus->rom.has_chr_rom()) {
            return;
        }
        chr_ram[bus->rom.chr_ram_offset(a) % chr_ram_size] = data;
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
// realign when it would otherwise start on the wrong phase. 512 in the one case
// below, where a DMC DMA has already paid for both.
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
// A DMC DMA THAT ENDED ON THE PREVIOUS CYCLE PAYS FOR BOTH. The halt exists to
// stop the CPU and the alignment to reach a get cycle, and a DMC DMA immediately
// before this has just done both - so the transfer starts straight away and costs
// 512. Neither reference emulator charges them here: Mesen and Nintendulator both
// run the two DMAs in ONE loop, where the cycles are spent once by whichever unit
// needs them first.
//
// MEASURED in lockstep against Mesen at the row that needs it, by comparing OAM
// transfer progress rather than PC - the CPU is frozen throughout, so PC can only
// differ at the resume and cannot say where the difference accrued. At row 05 of
// sprdma_and_dmc_dma the two first differ 11 cycles past the landmark, ours $00
// against Mesen's $01: our fetch is at +9 and this is entered at +10, so we spent
// +10 halting and +11 aligning while Mesen had already written a sprite byte.
// Row 04 is the control - a cycle separates its fetch from this call, the CPU does
// come back, the halt is genuinely needed, and OAM progress never differs there.
//
// AND 512 IS A CONSTANT HERE WHERE THE LENGTH ABOVE IS PARITY-DEPENDENT, which is
// not an oversight: the DMC's read only ever happens on a get, so the cycle after
// it is always a put and there is nothing left to decide. Measured rather than
// assumed - across all sixteen rows of sprdma_and_dmc_dma the fetch lands on an
// even Bus::cpu_cycles, 16 of 16, so no row reaches this line on the other phase.
void PPU::request_OAM_DMA()
{
    const bool dmc_just_finished =
        bus->dmc_fetch_cycle != Bus::kNoDmcFetch && bus->cpu_cycles == bus->dmc_fetch_cycle + 1;
    remaining_dma_cycles = dmc_just_finished ? 512 : static_cast<uint16_t>(513 + (bus->cpu_cycles % 2));
    dma_current_memory_source_addr = registers.OAMDMA << 8;
}

void PPU::perform_OAM_DMA_cycle()
{
    // The leading halt cycle, plus the alignment cycle when there was one.
    remaining_dma_cycles--;
    if (remaining_dma_cycles >= 512) {
        return;
    }

    // DMA alternates read and write cycles, and the read really does happen on
    // the read cycle - a whole CPU cycle before the write that consumes it.
    //
    // Doing both on the write cycle is invisible for the usual sources: RAM
    // and ROM hold still between the two. It is not invisible when the source
    // page is the PPU register file ($2000-$20FF), because the PPU advances
    // three dots per CPU cycle, so every $2002 sampled that way came back
    // three dots late - which is precisely what blargg's ppu_read_buffer
    // test 67 measures, sampling the sprite 0 hit flag through a DMA.
    if (remaining_dma_cycles % 2) {
        dma_latch = bus->read(dma_current_memory_source_addr++);
        return;
    }

    bus->write(PPU::OAMDATA, dma_latch);
}

void PPU::write(const uint16_t addr, const uint8_t data)
{
    // Every write to the PPU's own register file drives its internal data bus
    // latch, regardless of which register it targets - including PPUSTATUS,
    // which is read-only: the write still reaches the bus, it just has no
    // register to land in. This latch is what PPUSTATUS's open-bus bits 0-4
    // read back from below.
    //
    // $4014 is the exception, and it is not a special case so much as a
    // reminder that it was never a PPU register at all. OAMDMA lives in the
    // 2A03 next to the APU and controller ports; it is routed here only
    // because this class owns the transfer. A write to it never appears on the
    // PPU's data bus, so it must not disturb the latch.
    //
    // Getting this wrong is invisible almost everywhere, because $4014 is
    // written and then the DMA immediately overwrites the latch anyway. It
    // shows up when the DMA's SOURCE is the PPU register file: blargg's
    // ppu_read_buffer parks a known byte in the latch, starts a DMA from
    // $2000-$20FF, and expects the reads of the write-only registers to hand
    // that byte back. We handed back $20 - the page number written to $4014.
    if (addr != OAMDMA) {
        drive_open_bus(data);
    }

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
        // NESdev, OAMDATA: writes "during rendering (on the pre-render line and
        // the visible lines 0-239, provided either sprite or background
        // rendering is enabled) do not modify values in OAM, but do perform a
        // glitchy increment of OAMADDR, bumping only the high 6 bits (i.e., it
        // bumps the [n] value in PPU sprite evaluation".
        //
        // Bumping only the high six bits is OAMADDR += 4: n is OAMADDR >> 2, so
        // advancing n by one skips a whole sprite record. A game writing OAM
        // mid-frame therefore corrupts its own sprite indices rather than
        // storing anything, which is why the advice is to use $4014 instead.
        if (rendering_enabled() && (scanline < post_render_scanline || scanline == pre_render_scanline)) {
            registers.OAMADDR = static_cast<uint8_t>(registers.OAMADDR + 4);
            break;
        }

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
            temp_addr = static_cast<uint16_t>((temp_addr & ~0x73E0) | ((data & 0x07) << 12) | ((data & 0xF8) << 2));
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

            // v is the address bus. Committing a new value to it drives the
            // lines whether or not anything is read through them, so a mapper
            // watching A12 sees this exactly as it sees a rendering fetch.
            //
            // This is the hook blargg's IRQ ROMs are built on - they clock the
            // counter "by writing to $2006 to change the current VRAM address",
            // with rendering off and no fetches happening at all. Only the
            // SECOND write does it: the first stages a byte in t, and t is not
            // wired to anything outside the PPU.
            observe_ppu_address_bus(registers.PPUADDR & vram_addr_mask);
        }
        high_byte_input = !high_byte_input;
        break;
    case PPUDATA:
        // Must use vram_step, not ++, so the write path agrees with the read
        // path below.
        ppu_bus_write(registers.PPUADDR, data);
        advance_vram_address();
        // As on the read path: the post-increment v is left on the address bus
        // and the mapper sees it.
        observe_ppu_address_bus(registers.PPUADDR & vram_addr_mask);
        break;
    case OAMDMA:
        registers.OAMDMA = data;
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

    // $4014 is NOT one of them. OAM DMA lives in the 2A03 alongside the CPU,
    // and only Bus::decode routes it here at all - so a read of it sees the CPU
    // data bus, not the PPU's internal latch. They are different wires.
    //
    // This mirrors the write side, where a $4014 write is already stopped from
    // driving the PPU latch. blargg's cpu_exec_space_apu catches the read half:
    // it executes THROUGH $4000-$40FF, and the fetch at $4014 has to come back
    // as CPU open bus for execution to reach $4015 at all.
    if (addr == OAMDMA) {
        return Device::open_bus();
    }

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
        //
        // During dots 1-64 of a rendering scanline the PPU is busy filling
        // secondary OAM with $FF, and $2004 is wired to that, not to primary
        // OAM. NESdev, PPU sprite evaluation, cycles 1-64: "Secondary OAM
        // (32-byte buffer for current sprites on scanline) is initialized to
        // $FF - attempting to read $2004 will return $FF."
        //
        // This is the other half of the sentence clear_secondary_oam_dot
        // implements. Modelling only the write half meant the $FF was visible
        // in rendering but not to software, which is the sort of split that
        // makes a game's OAM readback look like a memory bug.
        if (rendering_enabled() && scanline <= post_render_scanline - 1 && cycle >= 1 && cycle <= 64) {
            drive_open_bus(0xFF);
            return open_bus;
        }
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
            pending_read_buffer = fetched;
            pending_read_buffer_dots = kReadBufferRefillDots;
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
            pending_read_buffer = ppu_bus_read(static_cast<uint16_t>(addr & 0x2FFF));
            pending_read_buffer_dots = kReadBufferRefillDots;
        }

        advance_vram_address();

        // The increment leaves the NEW v on the address bus, and that is a
        // second thing the mapper sees. It is not the same event as the read
        // above: a read at $0FFF has A12 low, and the increment to $1000 is
        // what raises it. 3-A12_clocking's subtest 5, "should be clocked when
        // A12 changes to 1 via PPUDATA read", is precisely this case - the
        // access itself never had A12 high at all.
        observe_ppu_address_bus(registers.PPUADDR & vram_addr_mask);

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
    // Reads registers.* directly, NOT through PPU::read(). That models the
    // CPU's view, where PPUCTRL and PPUMASK are write-only and come back as
    // open bus - so decoding the flags through it would read the bus latch
    // instead of the value last written. This needs the PPU's own copy.
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
