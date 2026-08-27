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

}  // namespace apu
}  // namespace tests
