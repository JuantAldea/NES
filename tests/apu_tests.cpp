// APU frame counter.
//
// Audio is not implemented; the frame counter is, because it is the NES's only
// source of maskable interrupts. Without it the CPU's whole /IRQ path was
// unreachable: the SingleStepTests vectors carry no interrupts and the PPU only
// drives /NMI, so nothing exercised it.
//
// These pin the behaviour directly. 1-cli_latency covers it end to end, but a
// ROM failing tells you far less about where than a test does.
#include <cstdint>

#include "gtest/gtest.h"

#include "../include/bus.h"

namespace tests
{
namespace apu
{

// Runs the console until the frame IRQ line asserts, or gives up. Returns the
// CPU cycle it asserted on, or 0.
uint64_t cycles_until_frame_irq(Bus& console, uint64_t max_cpu_cycles = 80000)
{
    for (uint64_t i = 0; i < max_cpu_cycles * 12; ++i) {
        console.clock();
        if (console.apu.frame_irq_asserted()) {
            return console.cpu.total_cycles;
        }
    }
    return 0;
}

// A program that just spins, so the CPU is doing something harmless while the
// frame counter runs. $4017 is written by the test, not by the program.
void seed_spin_loop(Bus& console)
{
    const uint8_t program[3] = {0x4C, 0x00, 0x04};  // JMP $0400
    console.write_ram(0x0400, sizeof(program), program);
    console.cpu.registers.PC = 0x0400;
    console.cpu.cycles_left = 0;
}

GTEST_TEST(testAPU, four_step_mode_asserts_the_frame_irq)
{
    Bus console;
    seed_spin_loop(console);
    console.write(APU::FRAMECOUNTER, 0x00);  // 4-step, IRQ enabled

    const uint64_t at = cycles_until_frame_irq(console);
    ASSERT_NE(at, 0u) << "the frame counter never asserted /IRQ in 4-step mode";

    // The sequence is 29830 CPU cycles and the flag is set near its end. Allow
    // for the write's own delayed divider reset rather than pinning an exact
    // cycle, which would be asserting the implementation against itself.
    EXPECT_GT(at, APU::mode0_length - 100) << "asserted far too early to be the end of the sequence";
    EXPECT_LT(at, APU::mode0_length + 100) << "asserted far too late";
}

GTEST_TEST(testAPU, irq_inhibit_suppresses_the_frame_irq)
{
    Bus console;
    seed_spin_loop(console);
    console.write(APU::FRAMECOUNTER, 0x40);  // 4-step, IRQ inhibited

    EXPECT_EQ(cycles_until_frame_irq(console, 40000), 0u) << "bit 6 of $4017 must inhibit the frame interrupt";
}

GTEST_TEST(testAPU, five_step_mode_never_asserts_the_frame_irq)
{
    Bus console;
    seed_spin_loop(console);
    console.write(APU::FRAMECOUNTER, 0x80);  // 5-step

    // Run past a full 5-step sequence, which is longer than the 4-step one.
    EXPECT_EQ(cycles_until_frame_irq(console, APU::mode1_length + 5000), 0u)
        << "5-step mode has no IRQ step at all";
}

GTEST_TEST(testAPU, reading_4015_acknowledges_the_frame_irq)
{
    Bus console;
    seed_spin_loop(console);
    console.write(APU::FRAMECOUNTER, 0x00);
    ASSERT_NE(cycles_until_frame_irq(console), 0u);

    const uint8_t status = console.read(APU::APUSTATUS);
    EXPECT_EQ(status & 0x40, 0x40) << "$4015 bit 6 reports the frame interrupt";
    EXPECT_FALSE(console.apu.frame_irq_asserted()) << "reading $4015 acknowledges and releases the line";
    EXPECT_EQ(console.read(APU::APUSTATUS) & 0x40, 0x00) << "and it stays clear on a second read";
}

GTEST_TEST(testAPU, setting_the_inhibit_bit_acknowledges_a_pending_frame_irq)
{
    Bus console;
    seed_spin_loop(console);
    console.write(APU::FRAMECOUNTER, 0x00);
    ASSERT_NE(cycles_until_frame_irq(console), 0u);

    console.write(APU::FRAMECOUNTER, 0x40);
    EXPECT_FALSE(console.apu.frame_irq_asserted()) << "writing $4017 with bit 6 set clears a pending frame interrupt";
}

// The distinguishing property of /IRQ against /NMI: it is a level, not an edge.
// While the APU holds it low and I is clear, the CPU takes the interrupt again
// after every handler. A one-shot latch would fire once and go quiet - which is
// exactly what this emulator did before the frame counter existed, and why the
// asymmetry was invisible.
GTEST_TEST(testAPU, the_frame_irq_is_a_level_not_a_pulse)
{
    Bus console;
    seed_spin_loop(console);
    console.write(APU::FRAMECOUNTER, 0x00);
    ASSERT_NE(cycles_until_frame_irq(console), 0u);

    ASSERT_TRUE(console.apu.frame_irq_asserted());
    EXPECT_TRUE(console.cpu.irq_line_asserted()) << "the APU's flag and the CPU's /IRQ input are the same signal";

    // Not acknowledged, so it must still be asserted many cycles later.
    for (int i = 0; i < 2000; ++i) {
        console.clock();
    }
    EXPECT_TRUE(console.cpu.irq_line_asserted()) << "an unacknowledged frame interrupt must stay asserted";

    console.read(APU::APUSTATUS);
    EXPECT_FALSE(console.cpu.irq_line_asserted()) << "and drop only when acknowledged";
}

}  // namespace apu
}  // namespace tests
