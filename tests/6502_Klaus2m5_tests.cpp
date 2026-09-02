#include <fstream>
#include <functional>

#include "../include/cpu.h"
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

// Where the suite stopped, and how far it got before stopping. The cycle count
// matters for the interrupt suite: its traps are shared by several cases, so an
// address alone cannot say WHICH one fired.
struct Klaus2m5Result {
    uint16_t trap_pc = 0;
    uint64_t cycles = 0;
};

Klaus2m5Result klaus2m5_test(Klaus2m5Suite suite)
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
        return {};
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(ram.memory.data()), size)) {
        ADD_FAILURE() << "short read on test fixture: " << path;
        return {};
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
                std::cout << "TRAP " << std::hex << previous_pc << std::dec << " after " << cpu.total_cycles
                          << " cycles" << std::endl;
                return {cpu.registers.PC, cpu.total_cycles};
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
    return {};
}

GTEST_TEST(testCPU, 6502_Klaus2m5_funtional_test)
{
    Klaus2m5Suite suite{0x0000, 0x336d, "6502_functional_test.bin"};
    EXPECT_EQ(suite.target_trap, klaus2m5_test(suite).trap_pc);
}

// This suite stops at $075C rather than its success trap at $06F5, and that is
// CORRECT for a hardware-accurate NMOS 6502. It is the one place in this project
// where an external oracle is knowingly not satisfied, so the reasoning is
// recorded here in full rather than in a commit message.
//
// $075C is the `bne *` after `tsx / lda $102,x / and #$10` in Klaus's nmi_trap:
// it fires when an interrupt pushed P with the B flag SET. The run reaches it in
// the suite's final case, "test overlapping NMI, IRQ & BRK" ($06D7-$06F5), which
// asserts both NMI and IRQ and then executes BRK. The NMI lands inside the BRK
// sequence and hijacks it - the sequence vectors through $FFFA while still
// pushing B set, because BRK is what started it.
//
// Klaus documents this himself, at that exact trap in 6502_interrupt_test.a65:
//
//     trap_ne     ;unexpected B-flag! - this may fail on a real 6502
//                 ;due to a hardware bug on concurrent BRK & NMI
//
// and earlier, ";may fail due to a bug on a real NMOS 6502 - NMI could mask
// BRK". Both "may"s are unconditional on real silicon. Blargg's 2-nmi_and_brk
// requires the opposite - it prints $36 (B set) for five consecutive rows - and
// the NESdev CPU-interrupts page and Visual6502 agree with Blargg. Upstream has
// this open as Klaus2m5 issue #23.
//
// The two oracles are mutually exclusive, and exactly so: gating
// CPU::poll_interrupt_hijack to `Schedule::interrupt` (i.e. never hijacking BRK)
// flips this test to $06F5 and flips 2-nmi_and_brk to failing, with nothing
// else in the suite moving. Verified by doing it.
//
// The CYCLE COUNT is asserted as well as the address, and that is the load
// bearing half.
//
// ASSERTING THE ADDRESS ALONE IS NOT ENOUGH, though it reads as though it
// should be. $075C sits inside nmi_trap, which is shared by all six NMIs the
// suite fires, so the address cannot distinguish "ran every case and hit the
// documented caveat in the last one" from "failed at the FIRST NMI and landed
// in the same handler".
//
// DEMONSTRATED: a change making genuine NMIs push B set trapped at $075C after
// 1 NMI and 1646 cycles instead of 6 NMIs and 2721 - and an address-only
// assertion passed regardless.
//
// 2721 is the measured cycle count of a correct run. It is not derived from
// anything in src/, so it moves if and only if execution up to the trap
// changes - which is the property the address was wrongly credited with.
GTEST_TEST(testCPU, 6502_Klaus2m5_interrupt_test_stops_at_the_known_brk_nmi_conflict)
{
    constexpr uint16_t known_brk_nmi_trap = 0x075c;
    constexpr uint64_t cycles_of_a_correct_run = 2721;

    Klaus2m5Suite suite{0xbffc, known_brk_nmi_trap, "6502_interrupt_test.bin"};
    const Klaus2m5Result result = klaus2m5_test(suite);

    EXPECT_EQ(suite.target_trap, result.trap_pc)
        << "Klaus's interrupt suite stopped somewhere other than the known BRK/NMI "
           "hijack disagreement at $075C. Everything up to that point must still pass.";

    EXPECT_EQ(cycles_of_a_correct_run, result.cycles)
        << "the suite reached $075C but took a different number of cycles to get there, "
           "so it trapped on a different NMI than the documented BRK/NMI case";
}

}  // namespace tests
