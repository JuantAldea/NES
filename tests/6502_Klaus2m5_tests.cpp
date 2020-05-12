#include <fstream>

#include "gtest/gtest.h"

#include "bus.h"

struct Klaus2m5Suite {
    uint16_t feedback_register;
    uint16_t target_trap;
    std::string path;
};

bool klaus2m5_test(Klaus2m5Suite suite)
{
    Bus console;
    std::ifstream file(suite.path, std::ios::binary | std::ios::ate);

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(console.ram.memory.data()), size);
    console.cpu.reset();

    if (suite.feedback_register) {
        console.cpu.write(suite.feedback_register, 0x0);
    }

    while(true) {
        auto previous_pc = console.cpu.registers.PC;
        bool executed = console.cpu.clock(false);

        if (suite.feedback_register){
            uint8_t feedback_reg = console.cpu.read(suite.feedback_register);
            if ((feedback_reg & 0x2)) {
                console.cpu.write(suite.feedback_register, feedback_reg & ~0x2);
                console.cpu.raise_NMI();
                continue;
            } else if (feedback_reg & 0x1) {
                console.cpu.write(suite.feedback_register, feedback_reg & ~0x1);
                console.cpu.raise_IRQ();
                continue;
            }
        }

        if (executed && previous_pc == console.cpu.registers.PC) {
            std::cerr << "TRAP " << std::hex << previous_pc << std::endl;
            return previous_pc == suite.target_trap;
        }
    }
}

GTEST_TEST(testCPU, 6502_Klaus2m5_funtional_test)
{
    Klaus2m5Suite suite {0x0000, 0x336d, "test_files/6502_functional_test.bin"};
    EXPECT_EQ(true, klaus2m5_test(suite));
};

GTEST_TEST(testCPU, 6502_Klaus2m5_interrupt_test)
{
    Klaus2m5Suite suite {0xbffc, 0x06f5, "test_files/6502_interrupt_test.bin"};
    EXPECT_EQ(true, klaus2m5_test(suite));
};
