#include "ppu.h"

#include <cstring>

#include "bus.h"
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

    if (dma_in_progress()) {
        perform_OAM_DMA_cycle();
        //TODO return?
    }
    int scanline = 1;
    const int cycle = total_cycles % 341;
    //341 clocks/scanline
    // external PPU memory accessed every two clocks = 170 reads
    //+ 1 spare cycle

    //Pre-render scanline
    switch (scanline) {
        case -1:
        case 261:
            // prefetch tile info for first two tiles
            if (cycle == 1) {
               clear_vblank();
               clear_sprite0_hit();
               clear_sprite_overflow();
            }
            break;
        case 0 ... 239:
            process_visible_scanline();
            break;
        case 240:
            // Post-render scanline
            // idle
            break;
        case 241:
            //NMI is raised on the second cycle of scanline 241
            if (cycle == 1) {
               bus->cpu.raise_NMI();
               set_vblank();
            }
            break;
        case 242 ... 260:
            // TODO is it idle as well?
            break;
        default:
            std::cerr << "Scanline out of range: " << scanline << std::endl;
            break;
    }

    scanline += (cycle == 0);
    /*

    - Sprite DMA is 6144 clock cycles long (or in CPU clock cycles, 6144/12).
    256 individual transfers are made from CPU memory to a temp register inside
    the CPU, then from the CPU's temp reg, to $2004.

    - One scanline is EXACTLY 1364 cycles long. In comparison to the CPU's
    speed, one scanline is 1364/12 CPU cycles long.

    - One frame is EXACTLY 357368 cycles long, or EXACTLY 262 scanlines long.

    */
}

void PPU::process_visible_scanline()
{
    // 341 PPU cycles per scanline
    const int cycle = total_cycles % 341;
    // render background and sprite. Visible scanlines
    if (cycle == 0) {
        //idle
        return;
    } else if (1 <= cycle && cycle <= 256) {
        // - Output pixel based on VRAM
        // - Prefetch next tiles
        // - Sprite evaluation for next scanline
    } else if (257 <= cycle && cycle <= 340) {
        //prefetch tile data for next line’s first two tiles
    }
    return;
}
void PPU::request_OAM_DMA()
{
    remaining_dma_cycles = 513 + (total_cycles + 1) % 2;
    dma_current_memory_source_addr = registers.OAMDMA << 8;

    // std::cout << "REQUESTING DMA from " << std::hex << dma_current_memory_source_addr << std::endl;
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
        // After power/reset, writes to this register are ignored for about 30,000 cycles.
        if (total_cycles < 300000) {
            break;
        }
        registers.PPUCTRL = data;
        update_flags();
        break;
    case PPUMASK:
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
        if (high_byte_input) {
            registers.PPUSCROLL = 0;
            registers.PPUSCROLL |= (data << 4);
        } else {
            registers.PPUSCROLL &= 0xFF00;
            registers.PPUSCROLL |= data;
        }
        high_byte_input = !high_byte_input;
        break;
    case PPUADDR:
        if (high_byte_input) {
            registers.PPUADDR = 0;
            registers.PPUADDR |= (data << 4);
        } else {
            registers.PPUADDR &= 0xFF00;
            registers.PPUADDR |= data;
        }
        high_byte_input = !high_byte_input;
        break;
    case PPUDATA:
        // std::cout << "WRITE TO PPUDATA " << std::hex << "(" << (unsigned)registers.PPUDATA << ") <= " << std::hex << (unsigned)data << " PTR " << registers.PPUADDR << std::endl;
        VRAM[registers.PPUADDR++] = data;
        break;
    case OAMDMA:
        registers.OAMDMA = data;
        // std::cout << "WRITE TO OAMDMA " << std::hex << "(" << (unsigned)registers.OAMDMA << ") <= " << std::hex << (unsigned)data << std::endl;
        request_OAM_DMA();
        break;
    }
#pragma GCC diagnostic pop
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
        // Bits 0-4 are unused/open bus: they reflect the last value driven on
        // the PPU data bus rather than real status bits.
        uint8_t data = (registers.PPUSTATUS & 0xE0) | (open_bus & 0x1F);
        registers.PPUSTATUS &= 0x7F;
        registers.PPUSCROLL = 0x0;
        registers.PPUADDR = 0x0;
        update_flags();
        open_bus = data;
        return data;
    };
    case OAMDATA:
        //reads during vertical or forced blanking return the value from OAM at that address but do not increment. ?¿?¿?¿?
        //return OAM_memory[registers.OAMADDR--];
        open_bus = OAM_memory[registers.OAMADDR];
        return open_bus;

    case PPUDATA: {
        uint8_t data = VRAM[registers.PPUADDR];
        registers.PPUADDR += vram_step;
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
    sprite_pattern_8x8_table_addr = (registers.PPUCTRL & 0x8) ? 0x0000 : 0x1000;
    bg_pattern_table_address = (registers.PPUCTRL & 0x10) ? 0x0000 : 0x1000;
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
