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
#include <cmath>
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

// The one part of the reload rule that NO ROM distinguishes, pinned here so it
// is at least stated.
//
// 11.len_reload_timing establishes that a load coinciding with a length clock
// is dropped when the counter is not zero and honoured when it is. It does not
// pin WHICH value that test uses - the counter before the clock, or after it.
// The two differ in exactly one case, a counter of 1 being decremented to 0,
// and the ROM never exercises it: replacing value_before_clock with the
// post-clock value passes all eleven of blargg_apu_2005, all of apu_test and
// all of apu_reset.
//
// So this assertion is REASONED, not measured, and the reasoning is that on
// hardware the write and the clock reach the length counter together - the
// value it holds at that moment is the one before the decrement, and it is the
// decrement itself that suppresses the reload. If an oracle ever contradicts
// this, believe the oracle: nothing here outranks a ROM.
GTEST_TEST(testAPU, a_reload_coinciding_with_the_clock_uses_the_pre_clock_counter_value)
{
    // Replays one identical sequence: bring pulse1's length counter to 1, then
    // switch mode again to fire another half-frame clock. `load_at` places a
    // $4003 store immediately after the given clock of that final phase, or
    // nowhere when negative. Returns the clock on which the counter changed and
    // the value it ended on.
    //
    // Written as a replay rather than with a hard-coded delay because a $4017
    // write takes effect 3 or 4 cycles later depending on the parity of the
    // cycle it lands on, so the figure differs between the two writes here.
    // Assuming one delay for both is what made the first version of this test
    // fail against a correct implementation.
    struct Run {
        int changed_on = -1;
        int final_value = -1;
    };
    const auto replay = [](int load_at) {
        Bus console;
        APU& apu = console.apu;
        apu.write(0x4015, 0x01);  // enable pulse1
        apu.write(0x4000, 0x00);  // halt clear
        apu.write(0x4003, 0x18);  // table[3] = 2
        apu.clock();              // let the deferred load land

        // Down to 1: switch mode, which clocks a half-frame when it takes
        // effect. Clocked until the counter actually moves, so no delay is
        // assumed.
        apu.write(0x4017, 0x80);
        for (int c = 0; c < 16 && apu.length_counter(APU::pulse1) != 1; ++c) {
            apu.clock();
        }

        Run run;
        const uint8_t before = apu.length_counter(APU::pulse1);
        apu.write(0x4017, 0x80);
        for (int c = 1; c <= 16; ++c) {
            apu.clock();
            // After the clock, never before: Bus::clock ticks the APU and only
            // then runs the CPU's store, so a store "on cycle N" always reaches
            // an APU that has already clocked for N.
            if (c == load_at) {
                apu.write(0x4003, 0x18);
            }
            if (run.changed_on < 0 && apu.length_counter(APU::pulse1) != before) {
                run.changed_on = c;
            }
        }
        run.final_value = apu.length_counter(APU::pulse1);
        return run;
    };

    const Run undisturbed = replay(-1);
    ASSERT_EQ(1, undisturbed.changed_on > 0 ? 1 : 0) << "the mode switch never clocked a half-frame";
    ASSERT_EQ(0, undisturbed.final_value) << "a counter of 1 should have been clocked to 0";

    // The store must be pending when that clock arrives, so it goes one cycle
    // earlier.
    const Run coinciding = replay(undisturbed.changed_on - 1);

    EXPECT_EQ(0, coinciding.final_value)
        << "the counter held 1 when the clock and the reload arrived together, so the\n"
           "  reload should have been dropped and the decrement should have taken it to 0.\n"
           "  Reading 2 means the post-clock value was used to decide instead - see\n"
           "  LengthCounter::value_before_clock.";
}

// --- the frame-counter units: envelope, sweep, linear counter ---------------
//
// NONE OF THESE HAS A ROM ORACLE, and that is not an oversight in this file.
// The CPU cannot read an envelope's decay level, a sweep's divider or the
// triangle's linear counter through any register, so no ROM reporting through
// $6000 can test them. The 25 APU ROMs that pass here cover the length counters
// and the frame IRQ, which ARE visible. apu_mixer and volume_tests look like
// the missing oracles and are not - see the header of apu_rom_tests.cpp.
//
// So these are written from two documents that agree with each other, and are
// checked by MUTATION instead: each was confirmed to fail when the behaviour it
// names is broken. Without that step a test written from the same model as the
// code proves only that the model was applied twice.

// APU::clock() alone is the unit under test; stepping the whole console to
// reach a quarter-frame boundary would drag the CPU and PPU in with it.
void advance_apu(APU& apu, const int cpu_cycles)
{
    for (int i = 0; i < cpu_cycles; ++i) {
        apu.clock();
    }
}

// MEASURED, by clocking a V=0 envelope one cycle at a time and recording when
// its decay moved: the first quarter-frame clock after power_on() lands at CPU
// cycle 7451, and the rest follow every 7458.
//
// The figures usually quoted for these - 3729, 7457, 11186, 14915 - are APU
// CYCLES, and the APU divides the CPU clock by two. Writing them here as CPU
// cycles made every one of these tests fail on its first run, which is the same
// factor-of-two trap the triangle's timer carries. The 7451 rather than 7457 is
// power_on_delay and the divider's phase.
//
// THE GAPS ARE NOT CONSTANT, and an earlier version of this comment said they
// were. Measured: 7456, 7458, 7458, 7458, then 7456 again - the pattern repeats
// every four clocks because the real boundaries fall on APU HALF-cycles and the
// four of them sum to 29830, the mode-0 period.
//
// So a step of 7458 gains 2 cycles per frame sequence rather than holding
// station. That is still safe here - the margin starts at 9 and GROWS, reaching
// about 17 over the longest test - but it is safe by drifting the right way,
// not by not drifting, and the difference matters to whoever tunes this next.
constexpr int to_first_quarter_frame = 7460;
constexpr int one_quarter_frame = 7458;

GTEST_TEST(testAPUUnits, envelope_start_flag_loads_decay_to_15_on_the_next_clock)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4000, 0x03);
    // $4003 sets the start flag. nesdev lists "The envelope is also restarted"
    // among its side effects - but not now: it is consumed on the NEXT
    // quarter-frame clock.
    apu.write(0x4003, 0x08);
    EXPECT_EQ(0, apu.envelope_decay(APU::pulse1)) << "the start flag must not act at the write";

    advance_apu(apu, to_first_quarter_frame);
    EXPECT_EQ(15, apu.envelope_decay(APU::pulse1)) << "the first quarter-frame clock reloads decay to 15";
}

GTEST_TEST(testAPUUnits, envelope_decay_steps_once_per_clock_when_the_period_is_zero)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4000, 0x00);  // V = 0: the divider reloads to 0, so every clock steps
    apu.write(0x4003, 0x08);

    advance_apu(apu, to_first_quarter_frame);
    ASSERT_EQ(15, apu.envelope_decay(APU::pulse1));

    advance_apu(apu, one_quarter_frame);
    EXPECT_EQ(14, apu.envelope_decay(APU::pulse1));
    advance_apu(apu, one_quarter_frame);
    EXPECT_EQ(13, apu.envelope_decay(APU::pulse1));
}

GTEST_TEST(testAPUUnits, constant_volume_selects_the_source_but_the_decay_still_runs)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    // nesdev: "The constant volume flag has no effect besides selecting the
    // volume source; the decay level will still be updated when constant volume
    // is selected." Gating the envelope clock on this bit is the documented
    // trap, so this asserts both halves.
    apu.write(0x4000, 0x17);  // constant volume, V = 7
    apu.write(0x4003, 0x08);
    advance_apu(apu, to_first_quarter_frame);

    EXPECT_EQ(7, apu.envelope_volume(APU::pulse1)) << "constant volume reports the period field";
    EXPECT_EQ(15, apu.envelope_decay(APU::pulse1)) << "and the decay was still reloaded underneath";
}

GTEST_TEST(testAPUUnits, the_envelope_loop_flag_is_the_length_counter_halt_bit)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    // Bit 5 of $4000, and this is the one assertion here with an oracle behind
    // it: the same bit halts the length counter, so decoding it at the wrong
    // position would break blargg's 1-len_ctr as well as this.
    apu.write(0x4015, 0x01);
    apu.write(0x4000, 0x20);  // loop/halt set, V = 0
    apu.write(0x4003, 0x08);

    advance_apu(apu, to_first_quarter_frame);
    ASSERT_EQ(15, apu.envelope_decay(APU::pulse1));

    for (int i = 0; i < 15; ++i) {
        advance_apu(apu, one_quarter_frame);
    }
    EXPECT_EQ(0, apu.envelope_decay(APU::pulse1));
    advance_apu(apu, one_quarter_frame);
    EXPECT_EQ(15, apu.envelope_decay(APU::pulse1)) << "loop set: the decay reloads rather than sticking at 0";
}

// THE ASYMMETRY, pinned with the documentation's own worked example rather than
// with numbers taken from this implementation: nesdev says "Making 20 negative
// produces a change amount of -21" on pulse 1 and "-20" on pulse 2, and blargg
// reaches the same result from the other side ("on the second square channel,
// the inverted value is incremented by 1").
GTEST_TEST(testAPUUnits, pulse1_negates_with_ones_complement_and_pulse2_with_twos)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4002, 0x80);
    apu.write(0x4003, 0x02);  // period 0x280 = 640, so 640 >> 5 = 20
    apu.write(0x4001, 0x0D);  // negate, shift 5
    ASSERT_EQ(640, apu.pulse_timer_period(APU::pulse1));
    EXPECT_EQ(640 - 21, apu.sweep_target_period(APU::pulse1)) << "pulse 1 subtracts c + 1";

    apu.write(0x4006, 0x80);
    apu.write(0x4007, 0x02);
    apu.write(0x4005, 0x0D);
    ASSERT_EQ(640, apu.pulse_timer_period(APU::pulse2));
    EXPECT_EQ(640 - 20, apu.sweep_target_period(APU::pulse2)) << "pulse 2 subtracts c";
}

// nesdev: "Muting happens regardless of whether the sweep unit is disabled
// (because either the Enabled flag or the Shift count are zero) and regardless
// of whether the sweep divider is outputting a clock signal."
GTEST_TEST(testAPUUnits, a_disabled_sweep_still_mutes_on_target_overflow)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    // $4001 never written: enabled false, shift 0. With shift 0 the change
    // amount is the period itself, so anything from $400 up doubles past $7FF -
    // the wiki's explanation for why games avoid the pulse channels' bottom
    // octave.
    apu.write(0x4002, 0x00);
    apu.write(0x4003, 0x04);  // period 0x400
    ASSERT_EQ(0x400, apu.pulse_timer_period(APU::pulse1));
    EXPECT_TRUE(apu.sweep_is_muting(APU::pulse1)) << "target 0x800 exceeds 0x7FF; the sweep being off is irrelevant";
}

GTEST_TEST(testAPUUnits, sweep_muting_boundaries_are_strict)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    // Negate on so the target cannot overflow and only the period floor is
    // under test. "If the current period is less than 8" - 8 itself is audible.
    apu.write(0x4001, 0x08);

    apu.write(0x4002, 0x07);
    apu.write(0x4003, 0x00);
    EXPECT_TRUE(apu.sweep_is_muting(APU::pulse1)) << "period 7 is below the floor";

    apu.write(0x4002, 0x08);
    apu.write(0x4003, 0x00);
    EXPECT_FALSE(apu.sweep_is_muting(APU::pulse1)) << "period 8 is exactly the floor and is audible";
}

GTEST_TEST(testAPUUnits, linear_counter_reloads_then_decrements_when_control_is_clear)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4008, 0x0A);  // control clear, reload value 10
    apu.write(0x400B, 0x00);  // sets the reload flag

    advance_apu(apu, to_first_quarter_frame);
    EXPECT_EQ(10, apu.linear_counter_value()) << "the reload flag loads the counter on the first clock";

    advance_apu(apu, one_quarter_frame);
    EXPECT_EQ(9, apu.linear_counter_value()) << "control clear: step 2 cleared the flag, so it counts down";
    advance_apu(apu, one_quarter_frame);
    EXPECT_EQ(8, apu.linear_counter_value());
}

GTEST_TEST(testAPUUnits, a_set_control_flag_makes_the_linear_counter_reload_forever)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    // "the reload flag is not cleared unless the control flag is also clear",
    // so with bit 7 held set the counter reasserts its reload value on every
    // clock instead of counting down. Both sources state this explicitly.
    apu.write(0x4008, 0x8A);
    apu.write(0x400B, 0x00);

    advance_apu(apu, to_first_quarter_frame);
    ASSERT_EQ(10, apu.linear_counter_value());
    advance_apu(apu, one_quarter_frame);
    EXPECT_EQ(10, apu.linear_counter_value()) << "control set: the reload flag never clears, so it never counts down";
    advance_apu(apu, one_quarter_frame);
    EXPECT_EQ(10, apu.linear_counter_value());
}

// --- gaps found by adversarial review, not by writing more of the same -------
//
// The tests above were checked by mutation and six of seven mutants died, which
// read as good coverage and was not. A review that fetched the sources itself
// and built its OWN mutants killed only four of ten: clock_sweep turned out to
// be executed by no test in any state where it could do anything, the $4001
// Enabled and period fields were never written non-zero, and two tests asserted
// less than their names claimed. These close those.
//
// MEASURED, the same way the quarter-frame constants were: half-frame clocks
// land at CPU cycle 14907, then 29823, then 44737 - gaps of 14916 and 14914,
// alternating for the same half-cycle reason the quarter-frames do. Only a few
// steps are taken here so the drift never matters.
constexpr int to_first_half_frame = 14910;
constexpr int one_half_frame = 14915;

// clock_sweep, step ordering. THE REVIEW'S FIRST FINDING: swapping the two
// steps used to pass all 998 cases.
//
// With the divider period P = 0 and the reload flag set by the $4001 write, the
// divider is zero coming into the first half-frame clock, so step 1 must update
// the period THERE. An implementation that reloads the divider first sees a
// non-zero counter and skips this update entirely.
GTEST_TEST(testAPUUnits, sweep_updates_the_period_on_the_first_half_frame_after_a_write)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4002, 0x00);
    apu.write(0x4003, 0x02);  // period 0x200 = 512
    apu.write(0x4001, 0x81);  // enabled, P = 0, negate clear, shift 1
    ASSERT_EQ(512, apu.pulse_timer_period(APU::pulse1));

    advance_apu(apu, to_first_half_frame);
    EXPECT_EQ(768, apu.pulse_timer_period(APU::pulse1)) << "512 + (512 >> 1); a reload-first ordering would skip this";

    advance_apu(apu, one_half_frame);
    EXPECT_EQ(1152, apu.pulse_timer_period(APU::pulse1)) << "and again on the next half-frame at P = 0";
}

// The divider period field really is bits 6-4, and P means P+1 clocks. With
// P = 1 the period must move on every SECOND half-frame, not every one.
GTEST_TEST(testAPUUnits, sweep_divider_period_field_is_bits_6_to_4)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4002, 0x00);
    apu.write(0x4003, 0x02);  // period 512
    apu.write(0x4001, 0x91);  // enabled, P = 1, shift 1

    advance_apu(apu, to_first_half_frame);
    ASSERT_EQ(768, apu.pulse_timer_period(APU::pulse1)) << "the reload flag forces the first update";

    advance_apu(apu, one_half_frame);
    EXPECT_EQ(768, apu.pulse_timer_period(APU::pulse1)) << "P = 1: this clock only decrements the divider";

    advance_apu(apu, one_half_frame);
    EXPECT_EQ(1152, apu.pulse_timer_period(APU::pulse1)) << "and the one after it updates";
}

// The Enabled bit is bit 7 and is read the right way round. Inverting it used
// to pass every test in the repository.
GTEST_TEST(testAPUUnits, a_sweep_with_the_enable_bit_clear_never_moves_the_period)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4002, 0x00);
    apu.write(0x4003, 0x02);
    // Identical to the test above except bit 7, and negate set so the target
    // cannot overflow and mute the channel for an unrelated reason.
    apu.write(0x4001, 0x09);  // NOT enabled, shift 1, negate

    advance_apu(apu, to_first_half_frame);
    EXPECT_EQ(512, apu.pulse_timer_period(APU::pulse1)) << "disabled: the period must not move";
    advance_apu(apu, one_half_frame);
    EXPECT_EQ(512, apu.pulse_timer_period(APU::pulse1));
}

// THE MUTING CEILING, at exactly the boundary. "If at any time the target
// period is greater than $7FF" - so a target of exactly $7FF is audible. The
// only ceiling case tested before was $800, one step past, which left an
// off-by-one invisible.
GTEST_TEST(testAPUUnits, a_target_of_exactly_7ff_does_not_mute)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4002, 0x55);
    apu.write(0x4003, 0x05);  // period 0x555 = 1365
    apu.write(0x4001, 0x01);  // negate clear, shift 1 -> target 1365 + 682 = 2047
    ASSERT_EQ(0x555, apu.pulse_timer_period(APU::pulse1));
    ASSERT_EQ(0x7FF, apu.sweep_target_period(APU::pulse1));
    EXPECT_FALSE(apu.sweep_is_muting(APU::pulse1)) << "0x7FF is exactly the ceiling and is audible";
}

// The decay is CLOCKED under constant volume, not merely reloaded by the start
// flag. The earlier test asserted decay == 15 one quarter-frame after a write,
// which the start branch supplies unconditionally - so an implementation that
// skipped the whole envelope clock whenever constant volume was set passed it.
GTEST_TEST(testAPUUnits, the_decay_keeps_counting_while_constant_volume_is_selected)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4000, 0x1F);  // constant volume, V = 15 -> divider period 15
    apu.write(0x4003, 0x08);

    advance_apu(apu, to_first_quarter_frame);
    ASSERT_EQ(15, apu.envelope_decay(APU::pulse1)) << "the start flag reloaded it";
    EXPECT_EQ(15, apu.envelope_volume(APU::pulse1)) << "and the mixer sees the constant, not the decay";

    // V = 15, so the decay steps once every 16 quarter-frames.
    for (int i = 0; i < 16; ++i) {
        advance_apu(apu, one_quarter_frame);
    }
    EXPECT_EQ(14, apu.envelope_decay(APU::pulse1)) << "the decay must still be running underneath";
    EXPECT_EQ(15, apu.envelope_volume(APU::pulse1)) << "while the reported volume stays the constant";
}

// The loop flag's NEGATIVE case. Every earlier envelope test ran with loop set
// or never reached zero, so an implementation that always reloaded to 15 -
// making every envelope loop forever, the most audible envelope error there is -
// passed the whole suite.
GTEST_TEST(testAPUUnits, a_non_looping_envelope_sticks_at_zero)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4015, 0x01);
    apu.write(0x4000, 0x00);  // loop/halt CLEAR, V = 0
    apu.write(0x4003, 0x08);

    advance_apu(apu, to_first_quarter_frame);
    ASSERT_EQ(15, apu.envelope_decay(APU::pulse1));

    for (int i = 0; i < 15; ++i) {
        advance_apu(apu, one_quarter_frame);
    }
    ASSERT_EQ(0, apu.envelope_decay(APU::pulse1));

    advance_apu(apu, one_quarter_frame);
    EXPECT_EQ(0, apu.envelope_decay(APU::pulse1)) << "loop clear: the decay stays at 0 rather than reloading";
    advance_apu(apu, one_quarter_frame);
    EXPECT_EQ(0, apu.envelope_decay(APU::pulse1));
}

// The linear counter stops at zero rather than wrapping to 255.
GTEST_TEST(testAPUUnits, the_linear_counter_stops_at_zero)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4008, 0x01);  // control clear, reload value 1
    apu.write(0x400B, 0x00);

    advance_apu(apu, to_first_quarter_frame);
    ASSERT_EQ(1, apu.linear_counter_value());
    advance_apu(apu, one_quarter_frame);
    EXPECT_EQ(0, apu.linear_counter_value());
    advance_apu(apu, one_quarter_frame);
    EXPECT_EQ(0, apu.linear_counter_value()) << "it must stop at zero, not wrap to 255";
}

// --- phase 2: the waveform generators ---------------------------------------
//
// Same situation as phase 1 - no ROM can see any of this - with one difference
// worth stating: THE MUTATION SET FOR THESE WAS WRITTEN BEFORE THE TESTS.
//
// Phase 1's tests were written first and mutated afterwards, and an adversarial
// review then killed six mutants the suite had missed, because a mutation set
// derived from the same assumptions as the tests inherits their blind spots.
// The list these were built to kill is in the commit message; each test below
// exists because a specific single-token change to src/apu.cpp would otherwise
// go unnoticed.

// One APU cycle is two CPU cycles for the pulses and the noise; the triangle
// runs at the CPU rate. advance_apu() counts CPU cycles throughout.
constexpr int cpu_cycles_per_apu_cycle = 2;

// THE DUTY TABLE IS READ DOWNWARD, and the two asymmetric duties are the only
// ones that can show it. Duty 0 must emit 0 1 0 0 0 0 0 0 and duty 3 must emit
// 1 0 0 1 1 1 1 1 - read the table upward instead and duty 0 becomes
// 0 0 0 0 0 0 0 1, which is the same waveform shifted, and duty 3 inverts its
// leading run. Duties 1 and 2 are near-symmetric and would not catch it, which
// is why they are not the fixture.
GTEST_TEST(testAPUWaves, pulse_duty_0_emits_the_documented_waveform_in_time_order)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4000, 0x00);  // duty 0
    apu.write(0x4002, 0x00);
    apu.write(0x4003, 0x00);  // period 0 -> the sequencer steps every APU cycle

    const uint8_t expected[8] = {0, 1, 0, 0, 0, 0, 0, 0};
    for (int step = 0; step < 8; ++step) {
        EXPECT_EQ(expected[step], apu.pulse_output(APU::pulse1)) << "duty 0, step " << step;
        advance_apu(apu, cpu_cycles_per_apu_cycle);
    }
}

GTEST_TEST(testAPUWaves, pulse_duty_3_emits_the_documented_waveform_in_time_order)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4000, 0xC0);  // duty 3
    apu.write(0x4002, 0x00);
    apu.write(0x4003, 0x00);

    const uint8_t expected[8] = {1, 0, 0, 1, 1, 1, 1, 1};
    for (int step = 0; step < 8; ++step) {
        EXPECT_EQ(expected[step], apu.pulse_output(APU::pulse1)) << "duty 3, step " << step;
        advance_apu(apu, cpu_cycles_per_apu_cycle);
    }
}

// The timer divides: at period t the sequencer advances every t+1 APU cycles.
//
// MEASURED AS AN INTERVAL, not as an absolute position, because the divider's
// starting value is not something a $4003 write sets - "The period divider is
// not reset" - so after power-on it is whatever the preceding cycles left. The
// interval is the documented property; the phase at any given cycle is not.
GTEST_TEST(testAPUWaves, the_pulse_timer_advances_the_sequencer_every_t_plus_one_apu_cycles)
{
    Bus console;
    APU& apu = console.apu;

    apu.write(0x4000, 0x00);
    apu.write(0x4002, 0x03);
    apu.write(0x4003, 0x00);  // period 3 -> one step per 4 APU cycles

    // Walk to the next step so the divider is at a known point, then time the
    // two after it.
    const uint8_t start = apu.pulse_sequence_position(APU::pulse1);
    int guard = 0;
    while (apu.pulse_sequence_position(APU::pulse1) == start && guard++ < 100) {
        advance_apu(apu, cpu_cycles_per_apu_cycle);
    }
    ASSERT_LT(guard, 100) << "the sequencer never advanced at all";

    for (int repeat = 0; repeat < 2; ++repeat) {
        const uint8_t before = apu.pulse_sequence_position(APU::pulse1);
        advance_apu(apu, 3 * cpu_cycles_per_apu_cycle);
        EXPECT_EQ(before, apu.pulse_sequence_position(APU::pulse1)) << "three APU cycles is short of the period";
        advance_apu(apu, cpu_cycles_per_apu_cycle);
        EXPECT_EQ((before + 7) & 0x07, apu.pulse_sequence_position(APU::pulse1)) << "the fourth steps it, and DOWNWARD";
    }
}

// The pulses run at the APU rate, so a single CPU cycle must not move them.
GTEST_TEST(testAPUWaves, the_pulse_timer_runs_at_the_apu_rate_not_the_cpu_rate)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4000, 0x00);
    apu.write(0x4002, 0x00);
    apu.write(0x4003, 0x00);  // period 0

    // NOT a whole sequence: at period 0 eight APU cycles is eight steps and
    // sixteen CPU cycles would be sixteen, and both land back on 0. An earlier
    // version asserted exactly that and could not tell the two rates apart.
    // Three steps can.
    advance_apu(apu, 3 * cpu_cycles_per_apu_cycle);
    EXPECT_EQ(5, apu.pulse_sequence_position(APU::pulse1)) << "three APU cycles is three steps down from 0";

    advance_apu(apu, 4 * cpu_cycles_per_apu_cycle);
    EXPECT_EQ(1, apu.pulse_sequence_position(APU::pulse1)) << "four more, not eight";
}

// "The sequencer is immediately restarted at the first value of the current
// sequence ... The period divider is not reset." Both halves are asserted,
// because an implementation that also reset the divider would pass a
// phase-only test.
GTEST_TEST(testAPUWaves, writing_4003_restarts_the_phase_but_not_the_timer)
{
    Bus console;
    APU& apu = console.apu;

    apu.write(0x4000, 0x00);
    apu.write(0x4002, 0x07);
    apu.write(0x4003, 0x00);  // period 7 -> one step per 8 APU cycles

    // Land exactly on a step so the divider is known to be full.
    const uint8_t start = apu.pulse_sequence_position(APU::pulse1);
    int guard = 0;
    while (apu.pulse_sequence_position(APU::pulse1) == start && guard++ < 100) {
        advance_apu(apu, cpu_cycles_per_apu_cycle);
    }
    ASSERT_LT(guard, 100);

    // Four APU cycles in: halfway through this timer period.
    advance_apu(apu, 4 * cpu_cycles_per_apu_cycle);

    apu.write(0x4003, 0x00);
    EXPECT_EQ(0, apu.pulse_sequence_position(APU::pulse1)) << "the phase restarts at once";

    // The divider kept its remaining 4 cycles rather than reloading to 8.
    advance_apu(apu, 3 * cpu_cycles_per_apu_cycle);
    EXPECT_EQ(0, apu.pulse_sequence_position(APU::pulse1));
    advance_apu(apu, cpu_cycles_per_apu_cycle);
    EXPECT_EQ(7, apu.pulse_sequence_position(APU::pulse1)) << "a reset divider would have needed 8 more, not 4";
}

// Changing the duty must not move the phase.
GTEST_TEST(testAPUWaves, changing_the_duty_leaves_the_sequencer_position_alone)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4000, 0x00);
    apu.write(0x4002, 0x00);
    apu.write(0x4003, 0x00);
    advance_apu(apu, 3 * cpu_cycles_per_apu_cycle);
    const uint8_t position = apu.pulse_sequence_position(APU::pulse1);
    ASSERT_EQ(5, position);

    apu.write(0x4000, 0x80);  // duty 2
    EXPECT_EQ(position, apu.pulse_sequence_position(APU::pulse1));
}

// THE TRIANGLE RUNS AT THE CPU RATE. At period 0 it steps every CPU cycle, so
// N cycles advance it N steps - half that if it were clocked with the pulses.
GTEST_TEST(testAPUWaves, the_triangle_timer_runs_at_the_cpu_rate_not_the_apu_rate)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4015, 0x04);  // enable the triangle's length counter
    apu.write(0x4008, 0x7F);  // control set, large reload -> linear counter runs
    apu.write(0x400A, 0x00);
    apu.write(0x400B, 0x08);  // length load, period 0
    // The linear counter needs a quarter-frame to load from the reload flag.
    advance_apu(apu, to_first_quarter_frame);
    ASSERT_EQ(0x7F, apu.linear_counter_value());

    // Asserted on the INDEX, not on the output difference. The output delta is
    // only 4 while the walk stays on one side of the ramp, and nothing pinned
    // that - a change to power_on_delay or to the clock parity moves the
    // starting index and the delta stops meaning what the name says.
    const uint8_t before = apu.triangle_sequence_position();
    advance_apu(apu, 4);
    EXPECT_EQ((before + 4) & 0x1F, apu.triangle_sequence_position())
        << "four CPU cycles must be four steps, not the two an APU-rate clock would give";
}

// The 32-step sequence itself, in order, from the top.
GTEST_TEST(testAPUWaves, the_triangle_walks_its_32_step_sequence)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4015, 0x04);
    apu.write(0x4008, 0x7F);
    apu.write(0x400A, 0x00);
    apu.write(0x400B, 0x08);
    advance_apu(apu, to_first_quarter_frame);
    ASSERT_EQ(0x7F, apu.linear_counter_value());

    // Walk to sequence INDEX 0, not to output value 15: the sequence visits 15
    // at both ends of the ramp, so stopping on the value could start the walk
    // at index 31 and compare a rotation of the table against the table.
    int guard = 0;
    while (apu.triangle_sequence_position() != 0 && guard++ < 200) {
        advance_apu(apu, 1);
    }
    ASSERT_LT(guard, 200) << "the triangle never reached index 0";
    const uint8_t expected[32] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5,  4,  3,  2,  1,  0,
                                  0,  1,  2,  3,  4,  5,  6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int step = 0; step < 32; ++step) {
        EXPECT_EQ(expected[step], apu.triangle_output()) << "triangle step " << step;
        advance_apu(apu, 1);
    }
}

// THE TRIANGLE FREEZES, it does not go silent. "The sequencer is clocked by the
// timer as long as both the linear counter and the length counter are nonzero."
// An implementation that gated the mixer instead of the clock would report 0
// here; the hardware holds the last step. (The Mega Man pop is the OTHER
// silencing method - an ultrasonic period - which never enters this branch.)
GTEST_TEST(testAPUWaves, the_triangle_sequencer_freezes_when_the_linear_counter_is_zero)
{
    Bus console;
    APU& apu = console.apu;
    apu.power_on();

    apu.write(0x4015, 0x04);
    apu.write(0x4008, 0x02);  // control CLEAR, reload 2 -> the counter will run out
    apu.write(0x400A, 0x00);
    apu.write(0x400B, 0x08);

    advance_apu(apu, to_first_quarter_frame);
    ASSERT_EQ(2, apu.linear_counter_value());
    advance_apu(apu, one_quarter_frame);
    advance_apu(apu, one_quarter_frame);
    ASSERT_EQ(0, apu.linear_counter_value()) << "the linear counter has run out";

    // 50 cycles, NOT a multiple of 32. An earlier version of this advanced 64,
    // which is exactly two passes of the 32-step sequence - so a sequencer that
    // was still running landed back on the same output and the test could not
    // tell it from a frozen one. Removing the linear-counter half of the gate
    // passed because of that alias, and nothing else in the suite noticed.
    const uint8_t frozen = apu.triangle_output();
    const uint8_t frozen_index = apu.triangle_sequence_position();
    advance_apu(apu, 50);
    EXPECT_EQ(frozen_index, apu.triangle_sequence_position()) << "the sequencer must HOLD its step";
    EXPECT_EQ(frozen, apu.triangle_output()) << "and therefore its output; it must not advance and must not zero";
}

// The LFSR's power-on value, tested through the only state that is reachable.
//
// "On power-up, the shift register is loaded with the value 1." That value is
// not directly observable: Bus's constructor calls APU::power_on(), whose
// documented 10-cycle delay is ten real APU::clock() calls, and the LFSR runs
// during them - correctly, since on hardware it is free-running from power.
// Five of those ten are APU cycles, and with no period yet selected it clocks
// on each.
//
// So the assertion is on where a register seeded with 1 MUST have arrived by
// the time anything can look. With $400E at its power-on $00 the period is 4
// CPU cycles, so those ten cycles produce three LFSR clocks:
// 1 -> 16384 -> 8192 -> 4096.
//
// That figure was not read off this implementation. It was reproduced by
// replaying the documented algorithm from a seed of 1 in a standalone program,
// which arrives at 4096 in 3 clocks independently. The assertion therefore pins
// the seed, the feedback rule and the CPU-to-APU divider at once.
//
// A register seeded with 0 is the real hazard - it XORs to 0 forever and the
// channel is silent for the whole run - and it would still read 0 here.
GTEST_TEST(testAPUWaves, the_noise_shift_register_is_seeded_with_one_at_power_on)
{
    Bus console;
    EXPECT_EQ(4096, console.apu.noise_shift_register())
        << "1 advanced three times by power_on()'s ten-cycle delay; a seed of 0 would still be 0";
}

// THE FEEDBACK IS TAKEN FROM THE PRE-SHIFT BITS, and the tap is bit 1 in normal
// mode. Both are checked by walking the documented algorithm independently here
// and comparing step for step - computing the XOR after the shift, or tapping
// bit 6 when the mode flag is clear, diverges within a few steps.
GTEST_TEST(testAPUWaves, the_noise_lfsr_matches_the_documented_algorithm_in_normal_mode)
{
    Bus console;
    APU& apu = console.apu;

    apu.write(0x400E, 0x00);  // mode clear, period index 0 -> 4 CPU cycles

    // Seeded from the live register rather than from 1: the algorithm is what
    // is under test here, and the power-on value is pinned by its own test.
    uint16_t reference = apu.noise_shift_register();
    for (int step = 0; step < 40; ++step) {
        // The wiki's three steps, written out separately from the implementation
        // so this is a second statement of the rule rather than a copy of it.
        const uint16_t feedback = (reference & 1u) ^ ((reference >> 1) & 1u);
        reference = static_cast<uint16_t>(reference >> 1);
        reference = static_cast<uint16_t>((reference & 0x3FFFu) | (feedback << 14));

        advance_apu(apu, 4);
        ASSERT_EQ(reference, apu.noise_shift_register()) << "diverged at step " << step;
    }
}

GTEST_TEST(testAPUWaves, the_noise_lfsr_taps_bit_6_in_mode_1)
{
    Bus console;
    APU& apu = console.apu;

    apu.write(0x400E, 0x80);  // mode SET, period index 0

    uint16_t reference = apu.noise_shift_register();
    for (int step = 0; step < 40; ++step) {
        const uint16_t feedback = (reference & 1u) ^ ((reference >> 6) & 1u);
        reference = static_cast<uint16_t>(reference >> 1);
        reference = static_cast<uint16_t>((reference & 0x3FFFu) | (feedback << 14));

        advance_apu(apu, 4);
        ASSERT_EQ(reference, apu.noise_shift_register()) << "diverged at step " << step;
    }
}

// The period table is data rather than a formula, so a wrong entry is only
// visible by timing a specific one. Index 4 is 64 CPU cycles.
//
// The interval is measured AFTER the first reload, because a $400E write does
// not reset the divider - the timer runs out its current value first, so the
// first gap after the write is not the new period.
GTEST_TEST(testAPUWaves, the_noise_period_table_is_indexed_by_the_low_four_bits)
{
    Bus console;
    APU& apu = console.apu;

    apu.write(0x400E, 0x04);  // index 4 -> 64 CPU cycles

    uint16_t value = apu.noise_shift_register();
    int guard = 0;
    while (apu.noise_shift_register() == value && guard++ < 5000) {
        advance_apu(apu, 1);
    }
    ASSERT_LT(guard, 5000) << "the LFSR never clocked";

    value = apu.noise_shift_register();
    advance_apu(apu, 62);
    EXPECT_EQ(value, apu.noise_shift_register()) << "62 cycles is short of the 64-cycle period";
    advance_apu(apu, 2);
    EXPECT_NE(value, apu.noise_shift_register()) << "and the 64th clocks it";
}

// The noise channel's silence polarity is INVERTED relative to the pulses:
// "The mixer receives the current envelope volume except when: Bit 0 of the
// shift register is set". For the pulses it is the sequencer output being ZERO
// that silences. Reading this backwards mutes exactly when it should not.
GTEST_TEST(testAPUWaves, noise_is_silent_exactly_when_shift_register_bit_0_is_set)
{
    Bus console;
    APU& apu = console.apu;
    apu.write(0x400E, 0x00);  // period index 0 -> 4 CPU cycles

    // Walk the register and check the reported silence against bit 0 at every
    // step, so both polarities are exercised rather than whichever one the
    // power-on state happens to give.
    int saw_silent = 0;
    int saw_audible = 0;
    for (int step = 0; step < 60; ++step) {
        const bool bit0 = (apu.noise_shift_register() & 1u) != 0;
        ASSERT_EQ(bit0, apu.noise_output_is_silent()) << "step " << step;
        bit0 ? ++saw_silent : ++saw_audible;
        advance_apu(apu, 4);
    }
    EXPECT_GT(saw_silent, 0) << "the walk never reached a silent state, so it proved nothing";
    EXPECT_GT(saw_audible, 0) << "the walk never reached an audible state, so it proved nothing";
}

// --- gaps a second adversarial review found ---------------------------------
//
// The phase-2 mutation set was written before the tests, which was the right
// correction to phase 1 and still left 14 of an independent reviewer's 25
// mutants alive. What the list omitted was not subtle behaviour - it was whole
// register paths and table rows that no test ever touched: pulse 2, duty rows 1
// and 2, fourteen of the sixteen noise periods, and every triangle period other
// than zero. Writing the attack first does not help if the attack only aims
// where the tests already point.

// THE CPU/APU PHASE. Nothing detected a one-cycle inversion here, because every
// other test measures INTERVALS between steps and an interval is exactly what a
// phase shift preserves.
//
// Bus's constructor calls power_on(), whose delay is power_on_delay = 10 real
// clocks, so apu_cycles is 10 when it returns. The next clock makes it 11, and
// 11 is odd - which is an APU cycle, per the $4017 write-delay mapping and the
// odd frame-sequencer boundaries. With the pulse period at its power-on 0 the
// timer is always due, so that single cycle must step the sequencer.
//
// Clock the pulses on the even half instead and the step lands one cycle later.
GTEST_TEST(testAPUWaves, the_pulse_timer_clocks_on_the_same_half_cycle_as_the_frame_counter)
{
    Bus console;
    APU& apu = console.apu;

    const uint8_t before = apu.pulse_sequence_position(APU::pulse1);
    advance_apu(apu, 1);
    EXPECT_NE(before, apu.pulse_sequence_position(APU::pulse1))
        << "apu_cycles 11 is odd, and odd is the APU cycle; clocking on even puts this a cycle late";
}

// Every duty row, not just the two that can distinguish the read direction.
// Direction and VALUES are different questions, and the earlier tests only
// answered the first.
GTEST_TEST(testAPUWaves, every_duty_row_emits_its_documented_waveform)
{
    const uint8_t expected[4][8] = {
        {0, 1, 0, 0, 0, 0, 0, 0},  // 12.5%
        {0, 1, 1, 0, 0, 0, 0, 0},  // 25%
        {0, 1, 1, 1, 1, 0, 0, 0},  // 50%
        {1, 0, 0, 1, 1, 1, 1, 1},  // 25% negated
    };

    for (int duty = 0; duty < 4; ++duty) {
        Bus console;
        APU& apu = console.apu;
        apu.write(0x4000, static_cast<uint8_t>(duty << 6));
        apu.write(0x4002, 0x00);
        apu.write(0x4003, 0x00);  // period 0, and this resets the phase to 0

        for (int step = 0; step < 8; ++step) {
            EXPECT_EQ(expected[duty][step], apu.pulse_output(APU::pulse1)) << "duty " << duty << ", step " << step;
            advance_apu(apu, cpu_cycles_per_apu_cycle);
        }
    }
}

// PULSE 2 EXISTS. Its register cases are copies of pulse 1's and no test used
// them, so a copy-paste slip writing pulse1's fields from $4004/$4007 would
// have shipped.
GTEST_TEST(testAPUWaves, pulse2_has_its_own_duty_and_phase)
{
    Bus console;
    APU& apu = console.apu;

    apu.write(0x4000, 0x00);  // pulse 1: duty 0
    apu.write(0x4004, 0xC0);  // pulse 2: duty 3
    apu.write(0x4002, 0x00);
    apu.write(0x4003, 0x00);
    apu.write(0x4006, 0x00);
    apu.write(0x4007, 0x00);

    // Both are at phase 0; the duties differ, so the outputs must differ.
    EXPECT_EQ(0, apu.pulse_output(APU::pulse1)) << "duty 0 step 0";
    EXPECT_EQ(1, apu.pulse_output(APU::pulse2)) << "duty 3 step 0 - a write to $4004 must not land on pulse 1";

    // And $4007 must restart pulse 2's phase, not pulse 1's.
    advance_apu(apu, 3 * cpu_cycles_per_apu_cycle);
    const uint8_t p1 = apu.pulse_sequence_position(APU::pulse1);
    ASSERT_NE(0, apu.pulse_sequence_position(APU::pulse2));
    apu.write(0x4007, 0x00);
    EXPECT_EQ(0, apu.pulse_sequence_position(APU::pulse2));
    EXPECT_EQ(p1, apu.pulse_sequence_position(APU::pulse1)) << "$4007 must leave pulse 1's phase alone";
}

// THE TRIANGLE'S PERIOD. Every earlier triangle test left it at zero, where a
// reload of period, period*2 or period/2 are all the same value - so a literal
// factor of two in the reload passed the suite that this file spends a
// paragraph warning about.
GTEST_TEST(testAPUWaves, the_triangle_timer_period_divides_at_the_documented_rate)
{
    Bus console;
    APU& apu = console.apu;

    apu.write(0x4015, 0x04);
    apu.write(0x4008, 0x7F);
    apu.write(0x400A, 0x03);  // period 3 -> one step per 4 CPU cycles
    apu.write(0x400B, 0x08);  // high bits 0, length load
    advance_apu(apu, to_first_quarter_frame);
    ASSERT_EQ(0x7F, apu.linear_counter_value());

    // Land on a step so the divider is full, then time the next two.
    const uint8_t start = apu.triangle_sequence_position();
    int guard = 0;
    while (apu.triangle_sequence_position() == start && guard++ < 100) {
        advance_apu(apu, 1);
    }
    ASSERT_LT(guard, 100);

    for (int repeat = 0; repeat < 2; ++repeat) {
        const uint8_t before = apu.triangle_sequence_position();
        advance_apu(apu, 3);
        EXPECT_EQ(before, apu.triangle_sequence_position()) << "three CPU cycles is short of the period";
        advance_apu(apu, 1);
        EXPECT_EQ((before + 1) & 0x1F, apu.triangle_sequence_position()) << "the fourth steps it";
    }
}

// $400B's low three bits are the period's high three. With them set the period
// is at least $100, so the sequencer must be far slower than at period 0.
GTEST_TEST(testAPUWaves, the_triangle_period_takes_its_high_bits_from_400b)
{
    Bus console;
    APU& apu = console.apu;

    apu.write(0x4015, 0x04);
    apu.write(0x4008, 0x7F);
    apu.write(0x400A, 0x00);
    apu.write(0x400B, 0x09);  // high bits = 1 -> period 0x100 = 256
    advance_apu(apu, to_first_quarter_frame);
    ASSERT_EQ(0x7F, apu.linear_counter_value());

    const uint8_t start = apu.triangle_sequence_position();
    int guard = 0;
    while (apu.triangle_sequence_position() == start && guard++ < 400) {
        advance_apu(apu, 1);
    }
    ASSERT_LT(guard, 400) << "the triangle never stepped";

    // Period 256 is 257 cycles per step - counting down from 256 through 0 and
    // reloading. Both sides are asserted so the boundary is pinned rather than
    // just the magnitude.
    // Advanced in two unequal chunks, and the first is deliberately NOT a
    // multiple of 32. Stepping straight to 256 hid a mutant that dropped the
    // high bits entirely: at the period 0 that leaves, 256 steps is exactly
    // eight passes of the 32-step sequence and lands on the same index. That is
    // the third alias of this shape in this file.
    const uint8_t before = apu.triangle_sequence_position();
    advance_apu(apu, 100);
    EXPECT_EQ(before, apu.triangle_sequence_position()) << "100 cycles is well short of the period";
    advance_apu(apu, 156);
    EXPECT_EQ(before, apu.triangle_sequence_position()) << "256 cycles is one short of it";
    advance_apu(apu, 1);
    EXPECT_EQ((before + 1) & 0x1F, apu.triangle_sequence_position())
        << "the 257th steps it; shifting the high bits changes this";
}

// EVERY ENTRY OF THE NOISE PERIOD TABLE. The table is data rather than a
// formula, so a single wrong value is invisible to anything but a test that
// times that specific entry - and two spot checks left fourteen of the sixteen
// unguarded, which is exactly the hazard this file writes down for the table
// and then did not act on.
//
// This also subsumes the index mask: indices 8 and above decode differently
// under a 3-bit mask, so a narrowed mask fails here rather than needing its own
// case.
GTEST_TEST(testAPUWaves, every_noise_period_table_entry_clocks_at_its_documented_rate)
{
    // NTSC, from https://www.nesdev.org/wiki/APU_Noise and blargg's apu_ref.txt,
    // which agree exactly. Periods are in CPU cycles.
    const uint16_t expected[16] = {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};

    for (int index = 0; index < 16; ++index) {
        Bus console;
        APU& apu = console.apu;
        apu.write(0x400E, static_cast<uint8_t>(index));

        // Land on a clock so the divider is full, then time the next one.
        uint16_t value = apu.noise_shift_register();
        int guard = 0;
        while (apu.noise_shift_register() == value && guard++ < 9000) {
            advance_apu(apu, 1);
        }
        ASSERT_LT(guard, 9000) << "index " << index << " never clocked";

        value = apu.noise_shift_register();
        advance_apu(apu, expected[index] - 1);
        EXPECT_EQ(value, apu.noise_shift_register()) << "index " << index << ": clocked before " << expected[index];
        advance_apu(apu, 1);
        EXPECT_NE(value, apu.noise_shift_register()) << "index " << index << ": did not clock at " << expected[index];
    }
}

// $400E's Mode flag is bit 7 alone. Writing $40 must leave it CLEAR - a mask of
// $C0 would set it and switch the LFSR to the bit-6 tap.
GTEST_TEST(testAPUWaves, the_noise_mode_flag_is_bit_7_alone)
{
    Bus console;
    APU& apu = console.apu;

    apu.write(0x400E, 0x40);  // bit 6 set, bit 7 clear -> mode CLEAR, index 0

    uint16_t reference = apu.noise_shift_register();
    for (int step = 0; step < 20; ++step) {
        // The bit-1 tap, which is mode-clear behaviour.
        const uint16_t feedback = (reference & 1u) ^ ((reference >> 1) & 1u);
        reference = static_cast<uint16_t>(reference >> 1);
        reference = static_cast<uint16_t>((reference & 0x3FFFu) | (feedback << 14));

        advance_apu(apu, 4);
        ASSERT_EQ(reference, apu.noise_shift_register())
            << "diverged at step " << step << "; bit 6 is not the mode bit";
    }
}

// --- phase 3: the mixer -----------------------------------------------------
//
// Still no oracle, and now the stakes change: this is where every earlier phase
// becomes audible, so an error here is the one a listener would notice and no
// test in this repository would.
//
// The lesson from two reviews is that a mutation set aimed by the author lands
// where the tests already point. So these were written against a list of ways
// to be wrong that a reader of the wiki would recognise but a reader of this
// code might not: the table constants substituted for the exact ones (they are
// deliberately different and look like a typo), the three tnd divisors swapped
// or equalised, the +100 terms, the divide-by-zero guards, each of the four
// pulse gates, the noise length gate, the triangle gated to zero instead of
// held, and the DMC truncated to four bits.

// The formula, restated from the wiki rather than called, so a test failure
// means the two disagree rather than that one function was renamed.
double reference_mix(int p1, int p2, int tri, int noise, int dmc)
{
    const double pulse_sum = p1 + p2;
    const double pulse_out = pulse_sum == 0.0 ? 0.0 : 95.88 / ((8128.0 / pulse_sum) + 100.0);
    const double tnd_sum = (tri / 8227.0) + (noise / 12241.0) + (dmc / 22638.0);
    const double tnd_out = tnd_sum == 0.0 ? 0.0 : 159.79 / ((1.0 / tnd_sum) + 100.0);
    return pulse_out + tnd_out;
}

GTEST_TEST(testAPUMixer, silence_is_exactly_zero_and_never_a_nan)
{
    const float out = APU::mix_levels(0, 0, 0, 0, 0);
    EXPECT_EQ(0.0f, out);
    EXPECT_FALSE(std::isnan(out));

    // AND THE ZERO GUARDS ARE NOT WHAT MAKES THAT TRUE, which took a mutation
    // to find out. Removing both of them changes nothing: under IEEE 754,
    // 8128/0 is +inf, inf + 100 is inf, and 95.88/inf is exactly 0 - the same
    // answer the guard produces. Measured, not reasoned: an unguarded
    // expression compiled and run separately returns 0, not NaN.
    //
    // An earlier version of this comment asserted the opposite, that an
    // unguarded implementation "returns NaN here". The wiki's warning - "the
    // result for that group should be treated as zero rather than undefined due
    // to the division by 0 that otherwise results" - is real, but it bites an
    // integer or fixed-point implementation, not this one. The guards stay
    // because they are the correct thing to write and because a future
    // fixed-point rewrite would need them; they are documentation here, not
    // behaviour, and no test can honestly claim to cover them.
}

// Each group's guard separately: one group silent while the other is not must
// still produce a finite, correct number.
GTEST_TEST(testAPUMixer, one_silent_group_does_not_poison_the_other)
{
    const float pulses_only = APU::mix_levels(15, 15, 0, 0, 0);
    EXPECT_FALSE(std::isnan(pulses_only)) << "the tnd group is empty here";
    EXPECT_NEAR(reference_mix(15, 15, 0, 0, 0), pulses_only, 1e-6);

    const float tnd_only = APU::mix_levels(0, 0, 15, 0, 0);
    EXPECT_FALSE(std::isnan(tnd_only)) << "the pulse group is empty here";
    EXPECT_NEAR(reference_mix(0, 0, 15, 0, 0), tnd_only, 1e-6);
}

// The whole input space of the pulse group, against the formula.
GTEST_TEST(testAPUMixer, the_pulse_group_matches_the_documented_formula)
{
    for (int p1 = 0; p1 <= 15; ++p1) {
        for (int p2 = 0; p2 <= 15; ++p2) {
            EXPECT_NEAR(reference_mix(p1, p2, 0, 0, 0),
                        APU::mix_levels(static_cast<uint8_t>(p1), static_cast<uint8_t>(p2), 0, 0, 0), 1e-6)
                << "pulse1 " << p1 << ", pulse2 " << p2;
        }
    }
}

// Every DMC level, 0-127. THE RANGE IS THE POINT: the other three channels are
// 0-15, and an implementation that masked this to four bits would agree with
// the formula for the first sixteen values and diverge for the remaining 112.
GTEST_TEST(testAPUMixer, the_dmc_contributes_over_its_full_seven_bit_range)
{
    for (int dmc = 0; dmc <= 127; ++dmc) {
        EXPECT_NEAR(reference_mix(0, 0, 0, 0, dmc), APU::mix_levels(0, 0, 0, 0, static_cast<uint8_t>(dmc)), 1e-6)
            << "dmc level " << dmc;
    }
    EXPECT_GT(APU::mix_levels(0, 0, 0, 0, 127), APU::mix_levels(0, 0, 0, 0, 15))
        << "a 4-bit mask would make these equal";
}

// THROUGH THE LIVE CHANNEL, not just the formula. The test above calls
// mix_levels directly, so it never touches dmc_level() - and masking that
// accessor to four bits survived the whole suite because of it. $4011 is
// writable at any time and is exactly how games play PCM, so this is the path
// that matters.
GTEST_TEST(testAPUMixer, the_dmc_level_reaches_the_mixer_at_its_full_range)
{
    Bus console;
    APU& apu = console.apu;

    apu.write(0x4011, 0x7F);
    ASSERT_EQ(127, apu.dmc_level()) << "seven bits, not four";
    const float loud = apu.mixer_output();

    apu.write(0x4011, 0x0F);
    ASSERT_EQ(15, apu.dmc_level());
    const float quiet = apu.mixer_output();

    EXPECT_GT(loud, quiet) << "a level masked to four bits would make these equal";

    // THE TRIANGLE IS IN THIS SUM even though nothing enabled it, and the
    // reference has to say so. Its sequencer is gated off, so it holds index 0
    // and presents 15 to the mixer forever - a constant DC term from a channel
    // that is doing nothing. That is not a bug to fix here: it is why the real
    // hardware follows the DAC with high-pass filters at 90Hz and 440Hz, and it
    // is the direct consequence of the triangle freezing rather than muting.
    ASSERT_EQ(15, apu.triangle_level()) << "held at its power-on step";
    EXPECT_NEAR(reference_mix(0, 0, apu.triangle_level(), 0, 127), loud, 1e-6);
}

// THE THREE TND DIVISORS ARE DIFFERENT AND ORDERED. 8227 < 12241 < 22638, so
// per unit the triangle is loudest and the DMC quietest. Equalising them, or
// swapping any two, is invisible to a test that only feeds one channel at a
// time and compares against a reference built from the same constants - so this
// asserts the ORDERING, which is a property of the hardware rather than of the
// numbers as written.
GTEST_TEST(testAPUMixer, the_three_tnd_divisors_are_distinct_and_correctly_ordered)
{
    const float tri = APU::mix_levels(0, 0, 10, 0, 0);
    const float noise = APU::mix_levels(0, 0, 0, 10, 0);
    const float dmc = APU::mix_levels(0, 0, 0, 0, 10);

    EXPECT_GT(tri, noise) << "the triangle's divisor 8227 is the smallest, so it is loudest per unit";
    EXPECT_GT(noise, dmc) << "the noise divisor 12241 is smaller than the DMC's 22638";
}

// The exact numerators, not the lookup tables'. 95.52 and 163.67 belong to the
// table approximation and are deliberately different to renormalise it; reading
// one for the other looks like a typo fix and changes every sample.
GTEST_TEST(testAPUMixer, the_numerators_are_the_exact_forms_not_the_table_forms)
{
    // At full pulses the two numerators differ by 95.88/95.52, about 0.38%,
    // which is far outside the tolerance used above but well inside anything a
    // "looks about right" check would accept.
    const double exact = 95.88 / ((8128.0 / 30.0) + 100.0);
    const double table_form = 95.52 / ((8128.0 / 30.0) + 100.0);
    ASSERT_NE(exact, table_form);
    EXPECT_NEAR(exact, APU::mix_levels(15, 15, 0, 0, 0), 1e-6);

    const double exact_tnd = 159.79 / ((1.0 / (15.0 / 8227.0)) + 100.0);
    const double table_tnd = 163.67 / ((1.0 / (15.0 / 8227.0)) + 100.0);
    ASSERT_NE(exact_tnd, table_tnd);
    EXPECT_NEAR(exact_tnd, APU::mix_levels(0, 0, 15, 0, 0), 1e-6);
}

// --- the gates --------------------------------------------------------------

// Drives a pulse channel to a state where it is genuinely sounding: enabled,
// length loaded, period in range, duty position on a high step, constant
// volume so the level is predictable.
void make_pulse1_audible(APU& apu)
{
    apu.write(0x4015, 0x01);
    apu.write(0x4000, 0x9F);  // duty 2 (50%), constant volume 15, halt set
    apu.write(0x4001, 0x08);  // sweep disabled, negate on so no overflow mute
    apu.write(0x4002, 0x40);
    apu.write(0x4003, 0x08);  // period 0x40, length load, phase reset to 0

    // One clock first: a length load is DEFERRED to the next cycle, so a helper
    // that only clocked inside a conditional loop could return with the counter
    // still at zero and every level it set up reading as silence.
    advance_apu(apu, 2);
    ASSERT_NE(0, apu.length_counter(APU::pulse1)) << "the deferred length load has not landed";

    // Duty 2 is 0 0 0 0 1 1 1 1 read downward from 0: step 0 is 0, so advance
    // to a step whose output is 1. At period 0x40 a step is 65 APU cycles, so
    // the guard has to allow a whole sequence - 8 * 65 - not a round 200.
    int guard = 0;
    while (apu.pulse_output(APU::pulse1) == 0 && guard++ < 1000) {
        advance_apu(apu, 2);
    }
    ASSERT_LT(guard, 1000);
}

GTEST_TEST(testAPUMixer, an_audible_pulse_presents_its_envelope_volume)
{
    Bus console;
    APU& apu = console.apu;
    make_pulse1_audible(apu);
    EXPECT_EQ(15, apu.pulse_level(APU::pulse1)) << "constant volume 15, and nothing is gating it";
}

GTEST_TEST(testAPUMixer, a_pulse_is_silent_while_its_sequencer_output_is_zero)
{
    Bus console;
    APU& apu = console.apu;
    make_pulse1_audible(apu);
    ASSERT_EQ(15, apu.pulse_level(APU::pulse1));

    // Walk to a low step of the duty cycle. The channel is otherwise unchanged.
    int guard = 0;
    while (apu.pulse_output(APU::pulse1) != 0 && guard++ < 1000) {
        advance_apu(apu, 2);
    }
    ASSERT_LT(guard, 1000);
    EXPECT_EQ(0, apu.pulse_level(APU::pulse1)) << "the duty bit is 0, so the mixer gets 0";
}

GTEST_TEST(testAPUMixer, a_pulse_with_a_zero_length_counter_is_silent)
{
    Bus console;
    APU& apu = console.apu;
    make_pulse1_audible(apu);
    ASSERT_EQ(15, apu.pulse_level(APU::pulse1));

    // $4015 bit 0 clear zeroes the length counter and blocks reload.
    apu.write(0x4015, 0x00);
    ASSERT_EQ(0, apu.length_counter(APU::pulse1));
    EXPECT_EQ(0, apu.pulse_level(APU::pulse1)) << "length zero silences regardless of the duty bit";
}

GTEST_TEST(testAPUMixer, a_swept_out_pulse_is_silent_even_though_its_sequencer_runs)
{
    Bus console;
    APU& apu = console.apu;
    make_pulse1_audible(apu);
    ASSERT_EQ(15, apu.pulse_level(APU::pulse1));

    // A period below 8 mutes via the sweep unit, with the sweep disabled -
    // "Muting happens regardless of whether the sweep unit is disabled".
    apu.write(0x4002, 0x04);
    apu.write(0x4003, 0x08);  // period 4
    ASSERT_TRUE(apu.sweep_is_muting(APU::pulse1));

    int guard = 0;
    while (apu.pulse_output(APU::pulse1) == 0 && guard++ < 1000) {
        advance_apu(apu, 2);
    }
    ASSERT_LT(guard, 1000) << "the sequencer must still be running while muted";
    EXPECT_EQ(0, apu.pulse_level(APU::pulse1)) << "muted: the mixer gets 0 even on a high duty step";
}

GTEST_TEST(testAPUMixer, noise_is_silent_when_its_length_counter_is_zero)
{
    Bus console;
    APU& apu = console.apu;

    apu.write(0x4015, 0x08);  // enable noise
    apu.write(0x400C, 0x3F);  // constant volume 15, halt set
    apu.write(0x400E, 0x00);
    apu.write(0x400F, 0x08);  // length load

    // Same trap as the pulse helper, and this one actually fired: the LFSR's
    // bit 0 happened to be clear already, so the loop below never executed, so
    // the DEFERRED length load never landed, and the channel read as silent for
    // a reason that had nothing to do with what the test was checking.
    advance_apu(apu, 4);
    ASSERT_NE(0, apu.length_counter(APU::noise)) << "the deferred length load has not landed";

    int guard = 0;
    while (apu.noise_output_is_silent() && guard++ < 200) {
        advance_apu(apu, 4);
    }
    ASSERT_LT(guard, 200);
    ASSERT_EQ(15, apu.noise_level()) << "constant volume 15, shift bit 0 clear, length loaded";

    apu.write(0x4015, 0x00);
    ASSERT_EQ(0, apu.length_counter(APU::noise));
    EXPECT_EQ(0, apu.noise_level()) << "the length counter is the OTHER half of the noise gate";
}

// The triangle is NOT gated to zero. Its silencing stops the sequencer, which
// then holds its step, so the level the mixer sees is that held value - which
// is why the hardware pops rather than muting.
GTEST_TEST(testAPUMixer, a_silenced_triangle_presents_its_held_step_not_zero)
{
    Bus console;
    APU& apu = console.apu;

    apu.write(0x4015, 0x04);
    apu.write(0x4008, 0x02);  // control clear, reload 2 - it will run out
    apu.write(0x400A, 0x00);
    apu.write(0x400B, 0x08);

    advance_apu(apu, to_first_quarter_frame);
    advance_apu(apu, one_quarter_frame);
    advance_apu(apu, one_quarter_frame);
    ASSERT_EQ(0, apu.linear_counter_value());

    const uint8_t held = apu.triangle_level();
    advance_apu(apu, 50);
    EXPECT_EQ(held, apu.triangle_level()) << "the held step, not zero";
    // And the run is only meaningful if it actually stopped somewhere audible.
    EXPECT_NE(0, held) << "this run froze on a zero step, so it cannot tell holding from gating";
}

}  // namespace apu
}  // namespace tests
