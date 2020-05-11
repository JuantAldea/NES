#include "bus.h"
#include <vector>
#include <fstream>
#include <iostream>
int main(int argc, char **argv)
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

   //console.cpu.write(0xbffc, 0x0);

    while(true) {
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
