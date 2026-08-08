// APU frame counter.
//
// Audio is not implemented; the frame counter is, because it is the NES's only
// source of maskable interrupts. Without it the CPU's whole /IRQ path was
// unreachable: the SingleStepTests vectors carry no interrupts and the PPU only
// drives /NMI, so nothing exercised it.
//
// These pin the behaviour directly. 1-cli_latency covers it end to end, but a
// ROM failing tells you far less about where than a test does.
#include <array>
#include <cstdint>

#include "../include/bus.h"
#include "gtest/gtest.h"

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

// The write must be executed by the CPU, not poked in with Bus::write. The
// divider reset is specified relative to the write CYCLE, and Bus::clock runs
// the CPU's store before APU::clock within a tick - so a store issued outside
// the clock loop is offset by one from every store a real program makes. An
// earlier version of this test poked the register directly and consequently
// measured a path no ROM takes.
GTEST_TEST(testAPU, four_step_mode_asserts_the_frame_irq_on_the_right_cycle)
{
    Bus console;

    //          cycles      cumulative CPU cycle after the instruction
    //  LDA #$00   2                       2
    //  STA $4017  4                       6   <- the write lands on cycle 6
    //  JMP self   3
    const uint8_t program[8] = {0xA9, 0x00, 0x8D, 0x17, 0x40, 0x4C, 0x05, 0x04};
    console.write_ram(0x0400, sizeof(program), program);
    console.cpu.registers.PC = 0x0400;
    console.cpu.cycles_left = 0;

    const uint64_t at = cycles_until_frame_irq(console);
    ASSERT_NE(at, 0u) << "the frame counter never asserted /IRQ in 4-step mode";

    // A literal, on purpose. Two earlier versions of this assertion were
    // useless: a +/-100 window that would have tolerated an off-by-99, and then
    // an "exact" one written as write_delay_even_cycle + mode0_irq_cycle, which
    // reads precisely but compares the implementation against itself - so
    // corrupting either constant moved the expectation with it and the test
    // still passed.
    //
    // 29838 = 6     the CPU cycle STA $4017 performs its write
    //       + 4     divider reset delay; the write landed on an even cycle
    //       + 29828 first cycle of mode 0's three-cycle IRQ window
    //
    // The 6 is arithmetic on the program above; the other two are from the
    // NESdev frame counter page. The sibling test below pins the part of this
    // that is pure hardware - that an odd-cycle write resets one cycle sooner.
    EXPECT_EQ(at, 29838u) << "frame IRQ asserted on the wrong cycle";
}

// The hardware-derived half of the write-delay behaviour: the reset lands 3
// cycles after an odd-cycle write and 4 after an even one, so shifting the
// write's parity by one CPU cycle must move the whole sequence by exactly one.
//
// This is what makes the absolute constant above meaningful. On its own that
// number could be satisfied by any delay at all; this pins the difference.
GTEST_TEST(testAPU, write_parity_shifts_the_divider_reset_by_one_cycle)
{
    uint64_t irq_cycle[2] = {0, 0};

    for (int odd = 0; odd < 2; ++odd) {
        Bus console;

        // An extra 3-cycle LDA $00 flips the parity of the cycle STA $4017
        // lands on. A 2-cycle NOP would not - it preserves parity, which is an
        // easy way to write this test and have it prove nothing.
        uint8_t program[16] = {};
        size_t i = 0;
        if (odd) {
            program[i++] = 0xA5;  // LDA $00   (3 cycles)
            program[i++] = 0x00;
        }
        program[i++] = 0xA9;  // LDA #$00
        program[i++] = 0x00;
        program[i++] = 0x8D;  // STA $4017
        program[i++] = 0x17;
        program[i++] = 0x40;
        program[i++] = 0x4C;  // JMP self
        program[i] = static_cast<uint8_t>(0x05 + (odd ? 3 : 0));
        program[i + 1] = 0x04;

        console.write_ram(0x0400, sizeof(program), program);
        console.cpu.registers.PC = 0x0400;
        console.cpu.cycles_left = 0;

        irq_cycle[odd] = cycles_until_frame_irq(console);
        ASSERT_NE(irq_cycle[odd], 0u) << "no frame IRQ with odd=" << odd;
    }

    // The odd variant's write happens 3 CPU cycles later (the extra LDA) but
    // its divider reset is 1 cycle sooner, so the IRQ moves by 3 - 1 = 2.
    EXPECT_EQ(irq_cycle[1] - irq_cycle[0], 2u)
        << "an odd-cycle $4017 write must reset the divider one cycle sooner than an even-cycle one";
}

// The flag is asserted across three consecutive cycles, not set once. A read of
// $4015 anywhere in that window sees it, so acknowledging on each cycle should
// see it re-assert exactly twice more and then stop.
GTEST_TEST(testAPU, the_irq_window_spans_exactly_three_cycles)
{
    Bus console;
    seed_spin_loop(console);
    console.write(APU::FRAMECOUNTER, 0x00);
    ASSERT_NE(cycles_until_frame_irq(console), 0u);

    int assertions = 0;
    for (int cpu_cycle = 0; cpu_cycle < 8; ++cpu_cycle) {
        if (console.apu.frame_irq_asserted()) {
            ++assertions;
            console.read(APU::APUSTATUS);  // acknowledge
        }
        for (int i = 0; i < 12; ++i) {
            console.clock();
        }
    }

    EXPECT_EQ(assertions, 3) << "mode 0 holds /IRQ low across cycles 29828, 29829 and 29830";
}

// The sequence period, measured as the gap between two assertions rather than
// read off the implementation.
GTEST_TEST(testAPU, the_four_step_sequence_period_is_29830_cycles)
{
    Bus console;
    seed_spin_loop(console);
    console.write(APU::FRAMECOUNTER, 0x00);

    const uint64_t first = cycles_until_frame_irq(console);
    ASSERT_NE(first, 0u);

    // Acknowledge and step past the three-cycle window so the next assertion
    // found is the start of the following sequence.
    for (int cpu_cycle = 0; cpu_cycle < 4; ++cpu_cycle) {
        console.read(APU::APUSTATUS);
        for (int i = 0; i < 12; ++i) {
            console.clock();
        }
    }

    const uint64_t second = cycles_until_frame_irq(console);
    ASSERT_NE(second, 0u) << "the sequence did not repeat";
    EXPECT_EQ(second - first, 29830u) << "the 4-step sequence is 29830 CPU cycles long";
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
    EXPECT_EQ(cycles_until_frame_irq(console, APU::mode1_length + 5000), 0u) << "5-step mode has no IRQ step at all";
}

// The frame interrupt flag belongs to the START of the cycle it is set on: a
// $4015 read placed on that very cycle reads it back SET.
//
// This is not a curiosity. blargg's sync_apu - which every cpu_interrupts_v2
// ROM calls before it measures anything - is built on it:
//
//     sta SNDMODE     ; $4017 = 0, restarting the sequence
//     delay 29825
//     lda #$40
//     bit SNDCHN      ; the read lands exactly on the first IRQ-window cycle
//     bne :+          ; burn one extra clock iff the flag is already set
//
// The branch is how the routine cancels out the divider's unknown phase. Read
// the flag back clear here and the branch falls through, the routine's
// compensation is skipped, and every later instruction in the ROM sits one CPU
// cycle away from where the frame counter thinks it is.
//
// The cycle numbers below are the same arithmetic as
// four_step_mode_asserts_the_frame_irq_on_the_right_cycle:
//
//   29838 = 6 (the cycle STA $4017 writes) + 4 (even-cycle divider reset delay)
//               + 29828 (first cycle of mode 0's IRQ window)
//
// and LDA abs reads on its fourth cycle, so an LDA starting on cycle 29835
// reads on 29838.
namespace
{
// Runs the $4017 program below, then forces an LDA $4015 to start on
// `start_cycle`, and returns the value it loaded.
uint8_t lda_4015_starting_on_cycle(const uint64_t start_cycle)
{
    Bus console;

    //          cycles      cumulative CPU cycle after the instruction
    //  LDA #$00   2                       2
    //  STA $4017  4                       6   <- the write lands on cycle 6
    //  JMP self   3
    const uint8_t program[8] = {0xA9, 0x00, 0x8D, 0x17, 0x40, 0x4C, 0x05, 0x04};
    console.write_ram(0x0400, sizeof(program), program);

    // LDA $4015, parked somewhere the spin loop will never reach on its own.
    const uint8_t reader[3] = {0xAD, 0x15, 0x40};
    console.write_ram(0x0500, sizeof(reader), reader);

    console.cpu.registers.PC = 0x0400;
    console.cpu.cycles_left = 0;
    console.cpu.registers.A = 0xFF;

    // Spin until the CPU cycle before the one the LDA is to start on, then
    // redirect it. Breaking into the JMP self loop part-way through is safe:
    // cycles_left is a boundary marker, and zeroing it forces a fresh fetch.
    while (console.cpu.total_cycles < start_cycle - 1) {
        console.clock();
    }
    console.cpu.registers.PC = 0x0500;
    console.cpu.cycles_left = 0;

    while (console.cpu.total_cycles < start_cycle + 3) {
        console.clock();
    }

    return console.cpu.registers.A;
}
}  // namespace

GTEST_TEST(testAPU, a_4015_read_on_the_cycle_the_flag_is_set_reads_it_back_set)
{
    EXPECT_EQ(lda_4015_starting_on_cycle(29835) & 0x40, 0x40)
        << "a $4015 read on cycle 29838 must see the frame interrupt flag";
}

// The other side of the same edge, so the assertion above pins a cycle rather
// than a half-open range: one cycle earlier the flag is not there yet.
GTEST_TEST(testAPU, a_4015_read_one_cycle_before_the_flag_is_set_reads_it_back_clear)
{
    EXPECT_EQ(lda_4015_starting_on_cycle(29834) & 0x40, 0x00)
        << "a $4015 read on cycle 29837 is one cycle too early to see the flag";
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

// The tests above only observe the LINE. This one observes the CPU acting on
// it, repeatedly - which is the entire point of the level model, and which was
// covered by nothing here at all: a review found that mutating the poll to
// ignore irq_line left every APU test passing.
//
// Driven at the CPU directly rather than through a Bus, because that is the
// unit under test and it makes the vector controllable without a cartridge.
// /IRQ is a wire-OR of several open-drain sources. Each pulls the shared line
// low independently, so one source releasing must NOT release another's
// assertion - which is exactly what a single bool would have done, and what
// this would have got wrong the moment a DMC or mapper IRQ was added.
GTEST_TEST(testAPU, irq_sources_are_ored_not_overwritten)
{
    struct FlatMemory {
        std::array<uint8_t, 64 * 1024> memory{};
        uint8_t read(uint16_t a) const { return memory[a]; }
        void write(uint16_t a, uint8_t d) { memory[a] = d; }
    } mem;

    CPU cpu([&mem](uint16_t a) { return mem.read(a); }, [&mem](uint16_t a, uint8_t d) { mem.write(a, d); });

    EXPECT_FALSE(cpu.irq_line_asserted());

    cpu.set_IRQ_line(CPU::IRQSource::apu_frame_counter, true);
    cpu.set_IRQ_line(CPU::IRQSource::cartridge, true);
    EXPECT_TRUE(cpu.irq_line_asserted());

    // The frame counter acknowledges. The cartridge is still holding the line.
    cpu.set_IRQ_line(CPU::IRQSource::apu_frame_counter, false);
    EXPECT_TRUE(cpu.irq_line_asserted()) << "one source releasing must not release another's assertion";

    // And only when the last source lets go does /IRQ go high.
    cpu.set_IRQ_line(CPU::IRQSource::cartridge, false);
    EXPECT_FALSE(cpu.irq_line_asserted());

    // Asserting twice and releasing once must still release: a source owns a
    // bit, not a count.
    cpu.set_IRQ_line(CPU::IRQSource::apu_dmc, true);
    cpu.set_IRQ_line(CPU::IRQSource::apu_dmc, true);
    cpu.set_IRQ_line(CPU::IRQSource::apu_dmc, false);
    EXPECT_FALSE(cpu.irq_line_asserted());
}

// RESET does not reach into a device and cancel its pending interrupt. /IRQ is
// a wire held low by the APU or a mapper; the CPU resetting says nothing about
// whether that device is still asserting.
//
// Clearing it on reset would drop a real pending interrupt while $4015 still
// reported it, leaving flag and line out of step until the next sequence
// re-drove it. Mutation testing found nothing pinned this.
GTEST_TEST(testAPU, reset_does_not_release_a_device_held_irq_line)
{
    Bus console;
    seed_spin_loop(console);
    console.write(APU::FRAMECOUNTER, 0x00);
    ASSERT_NE(cycles_until_frame_irq(console), 0u);

    ASSERT_TRUE(console.apu.frame_irq_asserted());
    ASSERT_TRUE(console.cpu.irq_line_asserted());

    console.cpu.reset();

    EXPECT_TRUE(console.apu.frame_irq_asserted()) << "the APU still has an unacknowledged frame interrupt";
    EXPECT_TRUE(console.cpu.irq_line_asserted())
        << "RESET cleared a line the APU is still holding; the flag and the line are now out of step";

    // And acknowledging still works normally afterwards.
    EXPECT_EQ(console.read(APU::APUSTATUS) & 0x40, 0x40);
    EXPECT_FALSE(console.cpu.irq_line_asserted());
}

GTEST_TEST(testAPU, a_held_irq_line_is_taken_again_after_every_handler)
{
    struct FlatMemory {
        std::array<uint8_t, 64 * 1024> memory{};
        uint8_t read(uint16_t a) const { return memory[a]; }
        void write(uint16_t a, uint8_t d) { memory[a] = d; }
    } mem;

    CPU cpu([&mem](uint16_t a) { return mem.read(a); }, [&mem](uint16_t a, uint8_t d) { mem.write(a, d); });

    mem.memory[0x1000] = 0xEA;  // NOP
    mem.memory[0x1001] = 0x4C;  // JMP $1000
    mem.memory[0x1002] = 0x00;
    mem.memory[0x1003] = 0x10;

    // Handler increments $10 and returns WITHOUT acknowledging.
    mem.memory[0xFFFE] = 0x00;
    mem.memory[0xFFFF] = 0x90;
    mem.memory[0x9000] = 0xE6;  // INC $10
    mem.memory[0x9001] = 0x10;
    mem.memory[0x9002] = 0x40;  // RTI

    cpu.registers.PC = 0x1000;
    cpu.registers.SP = 0xFD;
    cpu.registers.P = 0x24;
    cpu.set_flag(CPU::FLAGS::I, false);
    cpu.cycles_left = 0;

    // A device holds /IRQ low and never releases it.
    cpu.set_IRQ_line(CPU::IRQSource::cartridge, true);

    for (int i = 0; i < 4000; ++i) {
        cpu.clock(false);
        cpu.sample_interrupts();
    }

    // Each pass through the handler bumps $10. A one-shot latch would run it
    // once; a level line runs it over and over.
    EXPECT_GT(mem.memory[0x0010], 2) << "a held /IRQ line must be taken again after every handler; it ran the handler "
                                     << static_cast<int>(mem.memory[0x0010]) << " time(s)";
}

}  // namespace apu
}  // namespace tests
