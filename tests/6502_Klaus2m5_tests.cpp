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

uint16_t klaus2m5_test(Klaus2m5Suite suite)
{
    TEST_MEMORY ram;
    auto read = std::bind(&TEST_MEMORY::read, &ram, std::placeholders::_1);
    auto write = std::bind(&TEST_MEMORY::write, &ram, std::placeholders::_1, std::placeholders::_2);

    CPU cpu(read, write);

    const std::string path = std::string(NES_TEST_FILES_DIR) + "/" + suite.path;
    std::ifstream file(path, std::ios::binary | std::ios::ate);

    // Without this check a missing fixture leaves memory zeroed, the CPU executes
    // BRK at $0000, and the suite reports a misleading "TRAP 0" instead of a
    // missing-file error.
    if (!file) {
        ADD_FAILURE() << "could not open test fixture: " << path;
        return 0;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(ram.memory.data()), size)) {
        ADD_FAILURE() << "short read on test fixture: " << path;
        return 0;
    }
    cpu.reset();
    // These test images don't follow the $FFFC/$FFFD reset-vector convention;
    // per the suite's documentation, execution is expected to start at $0400.
    cpu.registers.PC = 0x400;

    if (suite.feedback_register) {
        cpu.write(suite.feedback_register, 0x0);
    }

    uint16_t previous_pc = 0;
    while (true) {
        if (cpu.clock(false)) {
            if (previous_pc == cpu.registers.PC) {
                std::cout << "TRAP " << std::hex << previous_pc << std::endl;
                return cpu.registers.PC;
            }
            previous_pc = cpu.registers.PC;
        }

        if (suite.feedback_register) {
            uint8_t feedback_reg = cpu.read(suite.feedback_register);
            if ((feedback_reg & 0x2)) {
                cpu.write(suite.feedback_register, feedback_reg & ~0x2);
                cpu.raise_NMI();
            } else if (feedback_reg & 0x1) {
                cpu.write(suite.feedback_register, feedback_reg & ~0x1);
                cpu.raise_IRQ();
            }
        }
    }
    return 0;
}

GTEST_TEST(testCPU, 6502_Klaus2m5_funtional_test)
{
    Klaus2m5Suite suite{0x0000, 0x336d, "6502_functional_test.bin"};
    EXPECT_EQ(suite.target_trap, klaus2m5_test(suite));
}

GTEST_TEST(testCPU, 6502_Klaus2m5_interrupt_test)
{
    Klaus2m5Suite suite{0xbffc, 0x06f5, "6502_interrupt_test.bin"};
    EXPECT_EQ(suite.target_trap, klaus2m5_test(suite));
}

}  // namespace tests
