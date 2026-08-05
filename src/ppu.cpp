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
            // prefetch tile info for first two tiles
            if (cycle == 1) {
               clear_vblank();
               clear_sprite0_hit();
               clear_sprite_overflow();
            }
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

void PPU::process_visible_scanline()
{
    // render background and sprite. Visible scanlines
    if (cycle == 0) {
        //idle
        return;
    }

    if (1 <= cycle && cycle <= 256) {
        // - Output pixel based on VRAM
        // - Prefetch next tiles
        // - Sprite evaluation for next scanline
    } else if (257 <= cycle && cycle <= 340) {
        //prefetch tile data for next line’s first two tiles
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
        // First write is the X scroll, second is the Y scroll.
        if (high_byte_input) {
            registers.PPUSCROLL = (registers.PPUSCROLL & 0x00FF) | (data << 8);
        } else {
            registers.PPUSCROLL = (registers.PPUSCROLL & 0xFF00) | data;
        }
        high_byte_input = !high_byte_input;
        break;
    case PPUADDR:
        if (in_reset_write_lockout()) {
            break;
        }
        // First write is the most significant byte. The address is 14 bits, so
        // the top two bits of that byte are discarded. The pair is staged in
        // temp_addr and only committed once the second write lands.
        if (high_byte_input) {
            temp_addr = (temp_addr & 0x00FF) | ((data & 0x3F) << 8);
        } else {
            temp_addr = (temp_addr & 0xFF00) | data;
            registers.PPUADDR = temp_addr;
        }
        high_byte_input = !high_byte_input;
        break;
    case PPUDATA:
        // std::cout << "WRITE TO PPUDATA " << std::hex << "(" << (unsigned)registers.PPUDATA << ") <= " << std::hex << (unsigned)data << " PTR " << registers.PPUADDR << std::endl;
        // Must use vram_step, not ++, so the write path agrees with the read
        // path below.
        ppu_bus_write(registers.PPUADDR, data);
        registers.PPUADDR = (registers.PPUADDR + vram_step) & vram_addr_mask;
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

        registers.PPUADDR = (registers.PPUADDR + vram_step) & vram_addr_mask;

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
