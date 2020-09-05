#include "ppu.h"

#include <cassert>
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
void PPU::clock()
{
    ++total_cycles;
    if (dma_in_progress()) {
        perform_OAM_DMA_cycle();
    }

    int VINT = 1;
    int scanline = 1;

    //341 clocks/scanline
    // external PPU memory accessed every two clocks = 170 reads
    //+ 1 spare cycle

    if (!VINT) {
        return;
    }

    if (scanline < 20) {
        return;
    }

    if (scanline == 20){
        //dummy scanline
        return;
    }
    if (scanline == 261) {
        //set VINT
        return;
    }



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
    assert(addr != PPUSTATUS);
    registers.PPUSTATUS &= 0xF0;
    registers.PPUSTATUS |= (data & 0x0F);

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
    assert(addr == PPUSTATUS || addr == OAMDATA || addr == PPUDATA);
    const RegisterMMap reg = static_cast<PPU::RegisterMMap>(addr);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
    switch (reg) {
    case PPUSTATUS: {
        uint8_t data = registers.PPUSTATUS;
        registers.PPUSTATUS &= 0x7F;
        registers.PPUSCROLL = 0x0;
        registers.PPUADDR = 0x0;
        update_flags();
        return data;
    };
    case OAMDATA:
        //reads during vertical or forced blanking return the value from OAM at that address but do not increment. ?¿?¿?¿?
        //return OAM_memory[registers.OAMADDR--];
        return OAM_memory[registers.OAMADDR];

    case PPUDATA:
        uint8_t data = VRAM[registers.PPUADDR];
        registers.PPUADDR += vram_step;
        return data;
    }
#pragma GCC diagnostic pop
    return 0;
}

void PPU::update_flags()
{
    base_nametable_addr = 0x2000 + 0x400 * (read(PPUCTRL) & 0x3);
    vram_step = read(PPUCTRL) & 0x4 ? Vertical : Horizontal;
    sprite_pattern_8x8_table_addr = (read(PPUCTRL) & 0x8) ? 0x0000 : 0x1000;
    bg_pattern_table_address = (read(PPUCTRL) & 0x10) ? 0x0000 : 0x1000;
    big_sprites = (read(PPUCTRL) & 0x20);
    // bit 6 PPU master/slave mode. Not used in NES.
    MMI_on_V_Blank = read(PPUCTRL) & 0x80;

    greyscale = read(PPUMASK) & 0x1;
    show_bg_in_leftmost = read(PPUMASK) & 0x2;
    show_sprites_in_leftmost = read(PPUMASK) & 0x4;
    show_background = read(PPUMASK) & 0x8;
    show_sprites = read(PPUMASK) & 0x10;
    emphasize_red = read(PPUMASK) & 0x20;
    emphasize_green = read(PPUMASK) & 0x40;
    emphasize_blue = read(PPUMASK) & 0x80;
}
