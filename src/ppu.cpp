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

void PPU::clock()
{
    ++total_cycles;

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
    open_bus = data;

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
        // std::cout << "WRITE TO OAMDATA " << std::hex << "(" << (unsigned)registers.OAMADDR << ") <= " << std::hex << (unsigned)data << " PTR " << (unsigned)registers.OAMADDR << std::endl;
        OAM_memory[registers.OAMADDR++] = data;

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
        VRAM[registers.PPUADDR & vram_addr_mask] = data;
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
        open_bus = data;

        // Clearing the flag releases /NMI. If the CPU has not sampled the
        // assertion yet, this is what suppresses the interrupt entirely
        // (06-suppression rows 05 and 06: flag reads back set, no NMI).
        update_nmi_line();
        return data;
    };
    case OAMDATA:
        //reads during vertical or forced blanking return the value from OAM at that address but do not increment. ?¿?¿?¿?
        //return OAM_memory[registers.OAMADDR--];
        open_bus = OAM_memory[registers.OAMADDR];
        return open_bus;

    case PPUDATA: {
        uint8_t data = VRAM[registers.PPUADDR & vram_addr_mask];
        registers.PPUADDR = (registers.PPUADDR + vram_step) & vram_addr_mask;
        open_bus = data;
        return data;
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
