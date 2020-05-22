#include <fstream>
#include <iostream>
#include <vector>

#include "bus.h"
#if 1
int main(int argc, char** argv)
{
    Bus console;
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(console.ram.memory.data()), size);
    console.cpu.reset();

    uint16_t feedback_register = 0;  //0xbffc;

    if (feedback_register) {
        console.write(feedback_register, 0x0);
    }

    while (true) {
        auto previous_pc = console.cpu.registers.PC;
        bool executed = console.cpu.clock(false);
        if (feedback_register) {
            uint8_t feedback_reg = console.cpu.read(feedback_register);
            if (feedback_reg & 0x2) {
                console.write(feedback_register, feedback_reg & ~0x2);
                console.cpu.raise_NMI();
                continue;
            } else if (feedback_reg & 0x1) {
                console.write(feedback_register, feedback_reg & ~0x1);
                console.cpu.raise_IRQ();
                continue;
            }
        }
        if (executed && previous_pc == console.cpu.registers.PC) {
            std::cerr << "TRAP " << std::hex << previous_pc << std::endl;
            return previous_pc;
        }
    }
}
#else

int main(void)
{
    Bus console;
    uint8_t bytes[256];

    for (auto i = 0; i < 256; i++) {
        bytes[i] = i;
    }

    uint16_t source_addr = 0x0200;

    console.write_ram(source_addr, 256, bytes);

    uint8_t dma_program[256];
    // NOP
    memset(dma_program, 0xEA, 256);

    // LDA #$02
    dma_program[0x32] = 0xA9;
    dma_program[0x33] = 0x02;
    // STA $4014
    dma_program[0x34] = 0x8D;
    dma_program[0x35] = 0x14;
    dma_program[0x36] = 0x40;

    console.write_ram(0x400, 256, dma_program);
    console.write(PPU::OAMADDR, 0);

    console.cpu.registers.PC = 0x400;

    while (!console.ppu.dma_in_progress()) {
        console.clock();
    }

    // finish DMA
    while (console.ppu.dma_in_progress()) {
        console.clock();
    }
    std::cout << "DMA OVER";
    return 0;
}
#endif
