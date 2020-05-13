#include <fstream>
#include <functional>

#include "cpu.h"
#include "gtest/gtest.h"
namespace tests
{
struct Klaus2m5Suite {
    uint16_t feedback_register;
    uint16_t target_trap;
    std::string path;
};

struct TEST_MEMORY {
    std::array<uint8_t, 64 * 1024> memory = {0};
    void write(const uint16_t addr, const uint8_t data) { memory[addr] = data; };
    uint8_t read(const uint16_t addr) { return memory[addr]; };
};

bool klaus2m5_test(Klaus2m5Suite suite)
{
    TEST_MEMORY ram;
    auto read = std::bind(&TEST_MEMORY::read, &ram, std::placeholders::_1);
    auto write = std::bind(&TEST_MEMORY::write, &ram, std::placeholders::_1, std::placeholders::_2);

    CPU cpu(read, write);

    std::ifstream file(suite.path, std::ios::binary | std::ios::ate);

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(ram.memory.data()), size);
    cpu.reset();

    if (suite.feedback_register) {
        cpu.write(suite.feedback_register, 0x0);
    }

    while (true) {
        auto previous_pc = cpu.registers.PC;
        bool executed = cpu.clock(false);

        if (suite.feedback_register) {
            uint8_t feedback_reg = cpu.read(suite.feedback_register);
            if ((feedback_reg & 0x2)) {
                cpu.write(suite.feedback_register, feedback_reg & ~0x2);
                cpu.raise_NMI();
                continue;
            } else if (feedback_reg & 0x1) {
                cpu.write(suite.feedback_register, feedback_reg & ~0x1);
                cpu.raise_IRQ();
                continue;
            }
        }

        if (executed && previous_pc == cpu.registers.PC) {
            std::cerr << "TRAP " << std::hex << previous_pc << std::endl;
            return previous_pc == suite.target_trap;
        }
    }
}

GTEST_TEST(testCPU, 6502_Klaus2m5_funtional_test)
{
    Klaus2m5Suite suite{0x0000, 0x336d, "test_files/6502_functional_test.bin"};
    EXPECT_EQ(true, klaus2m5_test(suite));
};

GTEST_TEST(testCPU, 6502_Klaus2m5_interrupt_test)
{
    Klaus2m5Suite suite{0xbffc, 0x06f5, "test_files/6502_interrupt_test.bin"};
    EXPECT_EQ(true, klaus2m5_test(suite));
};

}  // namespace tests
