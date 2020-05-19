#include <fstream>
#include <iostream>
#include <vector>

#include "bus.h"
#if 0
int main(int argc, char** argv)
{
    Bus console;
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(console.ram.memory.data()), size);
    console.cpu.reset();
    /*
    while(true) {
        //std::cout << std::hex << console.cpu.registers.PC << std::endl;
        auto previous_pc = console.cpu.registers.PC;
        bool executed = console.cpu.clock(false);
        if (executed && previous_pc == console.cpu.registers.PC) {
            std::cerr << "TRAP: " << std::hex << previous_pc << std::endl;
            exit(1);
        }
        //std::cout << std::hex << console.cpu.registers.PC << std::endl;
    }
    */

    // console.cpu.write(0xbffc, 0x0);

    while (true) {
        auto previous_pc = console.cpu.registers.PC;
        bool executed = console.cpu.clock(false);
        /*
                uint8_t feedback_reg = console.cpu.read(0xbffc);
                if ((feedback_reg & 0x2)) {
                    console.cpu.write(0xbffc, feedback_reg & ~0x2);
                    console.cpu.raise_NMI();
                    continue;
                } else if (feedback_reg & 0x1) {
                    console.cpu.write(0xbffc, feedback_reg & ~0x1);
                    console.cpu.raise_IRQ();
                    continue;
                }
        */
        if (executed && previous_pc == console.cpu.registers.PC) {
            std::cerr << "TRAP" << std::hex << previous_pc << std::endl;
            return previous_pc;
        }
    }
}
#endif

int main(void)
{
    Bus console;
    uint8_t bytes[256];

    for (auto i = 0; i < 256; i++) {
        bytes[i] = i;
    }

    uint8_t target_oam_addr = 0;
    uint16_t source_addr = 0x0200;

    console.write_ram(source_addr, 256, bytes);

    console.write(PPU::OAMADDR, target_oam_addr);

    console.write(PPU::OAMDMA, source_addr >> 8);

    do {
        console.clock();
    } while (console.ppu.dma_in_progress());

    for (auto i = 0; i < 256; i++) {
        std::cout << (unsigned)bytes[i] << " == " << (unsigned)console.ppu.OAM_memory[i] << std::endl;
        if (bytes[i] != console.ppu.OAM_memory[(i + source_addr) % 256]) {
            return 1;
        }
    }

    return 0;
}
