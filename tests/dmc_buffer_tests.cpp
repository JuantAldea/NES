// The DMC's one-byte sample buffer, and when it is occupied.
//
// This is not a curiosity. Whether the buffer is full at the moment $4015
// enables the channel decides which of TWO different DMAs happens: a load, if
// the buffer is empty and one must be fetched now, or a reload later when the
// buffer drains. They halt on opposite phases of the APU clock and cost
// different numbers of CPU cycles - 3 against 4 - so a buffer that is occupied
// when hardware's would be empty is worth a cycle on every sample start.
//
// That difference is why these exist. sprdma_and_dmc_dma disagrees with this
// emulator by a constant offset, and classifying loads against reloads showed
// this ROM producing 349 reloads and zero loads - every $4015 write finding the
// buffer already full. Emulators that pass it need both kinds, so the occupancy
// is the thing to pin, and it is pinnable without the ROM.
//
// The figures below are not read off this implementation. They come from the
// rate table and the structure of the channel: at rate index 0 the timer period
// is 428 CPU cycles, the output unit consumes one bit per period and a byte is
// eight bits, so a byte lasts 428 x 8 = 3424 cycles - which is exactly the
// dmc_timer_modulo that blargg's own dmc_timer.s is built around.
//
// THREE OF THESE WERE WIDENED WHEN THE DMC DMA STALL LANDED. The prediction
// that they would need it was written here first, before the change, and named
// the three by name; it was right, and the record of it is worth keeping.
//
// Mesen2 does not perform a $4015 enable's fetch on the cycle of the write. It
// schedules it 2 or 3 cycles later depending on cycle parity - _transferStartDelay,
// which it says matches dmc_dma_start_test - and only the output unit emptying
// the buffer starts one immediately. That is now what this emulator does, and
// exactly the three tests predicted began failing: they asserted occupancy at
// the instant of the write, which was the old inline fetch's behaviour rather
// than a hardware requirement. blargg's 7-dmc_basics #19 requires only that the
// byte arrive before the CPU's next INSTRUCTION reads $4015, and that ROM
// passes - as does 8-dmc_rates.
//
// The fourth test did not need widening and did not fail, which is the useful
// control: it measures the steady-state byte period, so the start delay is
// visible only at the enable and does not touch the rate.
//
// HOW THEY WERE WIDENED, because "relax until green" is the failure mode here.
// Each now asserts BOTH edges of the window - that the buffer is NOT filled on
// the write cycle, and that it IS filled on an exact later cycle. The first
// edge is what keeps the test able to fail: a revert to inline fetching would
// satisfy any assertion that merely waited long enough. The exact cycle is
// derived from the model rather than measured off this code, and start_sample
// pins the APU cycle parity so the figure can be exact instead of a range wide
// enough to hide a one-cycle regression.
//
// Deriving it went wrong once, in the direction worth warning about: the first
// derivation summed the two terms that are documented in prose - the scheduled
// delay and the length of a load - got 5, and disagreed with the measured 6.
// The missing term was a phase wait that only exists in the code. The right
// response to that gap was to go and find the third term, not to change the 5
// to a 6; had the assertion simply been fitted to the measurement, the fact
// that the parity table below is currently redundant would never have surfaced.
// See the comment on the first test for the full three-term derivation.
#include <cstdint>

#include "../include/bus.h"
#include "gtest/gtest.h"

namespace tests
{
namespace dmc_buffer
{
namespace
{

constexpr int kByteCycles = 3424;  // 428 x 8, rate index 0

// A CPU spinning on a JMP, so the channel runs against something harmless.
void seed_spin_loop(Bus& console)
{
    const uint8_t program[3] = {0x4C, 0x00, 0x04};  // JMP $0400
    console.write_ram(0x0400, sizeof(program), program);
    console.cpu.registers.PC = 0x0400;
    console.cpu.cycles_left = 0;
}

void run_cpu_cycles(Bus& console, const int cycles)
{
    for (int i = 0; i < cycles * 12; ++i) {
        console.clock();
    }
}

// The $4015 enable schedules its fetch 2 cycles out on an even APU cycle and 3
// on an odd one (apu.cpp:429), so the arrival cycle is only a fixed number once
// the parity is fixed. Pinning it to EVEN is what lets the tests below assert an
// exact cycle instead of a range wide enough to hide a one-cycle regression.
//
// APU::apu_cycles is private, but Bus::cpu_cycles is an exact proxy: the only
// call site of APU::clock() is `++cpu_cycles; apu.clock();` in Bus::clock_CPU
// (bus.cpp:228-229), and both counters start at zero, so they are equal for the
// whole life of the machine and therefore share a parity.
void pin_even_apu_cycle(Bus& console)
{
    if (console.cpu_cycles % 2 != 0) {
        run_cpu_cycles(console, 1);
    }
    ASSERT_EQ(0u, console.cpu_cycles % 2) << "parity pin failed, so every cycle count below is off by one";
}

// Starts the DMC at the slowest rate with a sample of `length_register` in
// $4013's units - so (n * 16) + 1 bytes - and no looping or IRQ.
//
// The enable is placed on an even APU cycle deliberately - see pin_even_apu_cycle.
void start_sample(Bus& console, const uint8_t length_register)
{
    console.write(0x4010, 0x00);  // rate index 0, no IRQ, no loop
    console.write(0x4012, 0x00);  // sample at $C000
    console.write(0x4013, length_register);
    pin_even_apu_cycle(console);
    console.write(0x4015, 0x10);  // enable: SCHEDULES the sample, does not fetch
}

// CPU cycles from the $4015 enable until the fetched byte is in the buffer, or
// -1 if it never arrives. Bounded well above any documented figure so a hang is
// reported as a number rather than as a timeout.
int cycles_until_buffer_fills(Bus& console)
{
    for (int cycle = 1; cycle <= 32; ++cycle) {
        run_cpu_cycles(console, 1);
        if (console.apu.dmc_sample_buffer_filled()) {
            return cycle;
        }
    }
    return -1;
}

// The mirror of pin_even_apu_cycle, for the disable tests below - they are the
// one place in this file where the two parities produce different numbers.
void pin_odd_apu_cycle(Bus& console)
{
    if (console.cpu_cycles % 2 == 0) {
        run_cpu_cycles(console, 1);
    }
    ASSERT_EQ(1u, console.cpu_cycles % 2) << "parity pin failed, so every cycle count below is off by one";
}

// A channel with bytes still to fetch, settled far from its next fetch: the
// scheduled load has completed and the following one is a byte period away, so
// nothing but the disable can move bytes-remaining inside the window below.
void start_sample_and_settle(Bus& console)
{
    start_sample(console, 0x01);  // 17 bytes
    run_cpu_cycles(console, 6);   // the scheduled load, per the test above
    ASSERT_EQ(16, console.apu.dmc_bytes_remaining()) << "the settle did not leave the channel mid-sample";
    ASSERT_EQ(0u, console.cpu_cycles % 2) << "start_sample pinned even and 6 preserves it";
}

// CPU cycles from a $4015 disable until bytes-remaining is cleared, or -1 if it
// never is. Bounded well above either documented figure so a countdown that
// never expires is reported as a number rather than as a timeout.
int cycles_until_disable_takes_effect(Bus& console)
{
    for (int cycle = 1; cycle <= 8; ++cycle) {
        run_cpu_cycles(console, 1);
        if (console.apu.dmc_bytes_remaining() == 0) {
            return cycle;
        }
    }
    return -1;
}

}  // namespace

// The buffer is filled BY the enable but not ON it: a $4015 write schedules the
// fetch rather than performing it. blargg's 7-dmc_basics #19 - "there should be
// a one-byte buffer that's filled immediately if empty" - is a claim about what
// the CPU's next instruction can observe, not about the write cycle itself, and
// that ROM passes with this delay in place.
//
// Six cycles, and the derivation has three terms, not two - the first attempt
// at this comment said five by leaving the middle one out:
//
//   2   the enable SCHEDULES the transfer rather than performing it, 2 cycles
//       out on an even APU cycle and 3 on an odd one (apu.cpp:429).
//   1   the halt cannot begin until the next GET cycle (bus.cpp:318), and a
//       request that lands on the wrong phase waits one cycle for it.
//   3   halt, dummy, read - a LOAD halts on a get, so its dummy lands on a put
//       and the following get needs no alignment cycle (apu.h:124).
//
// The middle term is why the parity of the enable does NOT reach this number.
// Both delays land the halt on the same cycle: a 2 arrives on the wrong phase
// and waits, a 3 arrives on the right one and does not. Measured by inverting
// apu.cpp:429 to `? 3 : 2` and rebuilding - every figure in this file was
// unchanged, and 7-dmc_basics and 8-dmc_rates still passed. So that parity
// table is currently doing no work that the phase gate is not already doing.
// Worth resolving, but it is not these tests' business: they pin the arrival
// cycle, and the arrival cycle is 6 either way.
//
// start_sample pins the parity anyway, so that if the two mechanisms are ever
// separated this test measures one thing rather than an average of two.
GTEST_TEST(dmcBuffer, enabling_the_channel_schedules_the_fill_rather_than_performing_it)
{
    Bus console;
    seed_spin_loop(console);

    ASSERT_FALSE(console.apu.dmc_sample_buffer_filled()) << "nothing should be buffered before the channel is enabled";

    start_sample(console, 0x01);  // 17 bytes

    // The lower edge of the window, and the reason this is a widening rather
    // than a surrender: without it the test would pass just as well if the
    // fetch reverted to happening inline, which is what it was widened away
    // from.
    EXPECT_FALSE(console.apu.dmc_sample_buffer_filled()) << "a $4015 enable must not fetch on the cycle of the write";
    EXPECT_EQ(17, console.apu.dmc_bytes_remaining()) << "and must not have consumed a byte yet";

    EXPECT_EQ(6, cycles_until_buffer_fills(console))
        << "2 cycles of start delay, 1 waiting for a get cycle, then a 3-cycle load";
    EXPECT_EQ(16, console.apu.dmc_bytes_remaining()) << "and that fetch must have consumed one of the 17 bytes";
}

// The buffer holds ONE byte ahead of playback, so bytes-remaining reaches zero
// a whole byte before the channel goes quiet. This is what makes $4015 bit 4
// clear early, and it is the reason a one-byte sample reads back as finished
// immediately.
GTEST_TEST(dmcBuffer, bytes_remaining_empties_one_byte_before_playback_ends)
{
    Bus console;
    seed_spin_loop(console);
    start_sample(console, 0x00);  // exactly 1 byte

    ASSERT_EQ(6, cycles_until_buffer_fills(console)) << "the same scheduled load as the test above";

    EXPECT_EQ(0, console.apu.dmc_bytes_remaining()) << "a one-byte sample is fully fetched by that load";
    EXPECT_TRUE(console.apu.dmc_sample_buffer_filled()) << "but the byte is still sitting in the buffer, unplayed";
}

// The steady-state byte period, measured rather than assumed: the interval
// between two consecutive fetches is 428 x 8 = 3424 cycles at rate index 0,
// which is the dmc_timer_modulo blargg's own timer is built around.
//
// NOT measured from the enable, and that is the point of this test. The channel
// starts with its bits counter at zero, so the very first timer tick hands the
// buffer straight to the shift register instead of waiting out eight bits - the
// first byte is consumed after roughly one period, not eight. An earlier
// version of this test measured from the enable and failed against correct
// behaviour, which is worth leaving recorded: the startup transient is real and
// is not the steady state.
GTEST_TEST(dmcBuffer, consecutive_fetches_are_one_byte_period_apart)
{
    Bus console;
    seed_spin_loop(console);
    start_sample(console, 0x01);  // 17 bytes, so refills keep coming

    // Step until bytes-remaining moves twice, timing the second interval. The
    // first is the startup transient and is deliberately discarded.
    const auto cycles_until_next_fetch = [&console]() {
        const uint16_t before = console.apu.dmc_bytes_remaining();
        for (int cycle = 1; cycle <= 2 * kByteCycles; ++cycle) {
            run_cpu_cycles(console, 1);
            if (console.apu.dmc_bytes_remaining() != before) {
                return cycle;
            }
        }
        return -1;
    };

    const int transient = cycles_until_next_fetch();
    ASSERT_GT(transient, 0) << "no fetch at all within two byte periods";
    EXPECT_LT(transient, kByteCycles) << "the first fetch should arrive early, because the bits counter starts at 0";

    EXPECT_EQ(kByteCycles, cycles_until_next_fetch())
        << "at rate index 0 a byte is 428 x 8 cycles, and blargg's dmc_timer.s assumes exactly that";
    EXPECT_EQ(kByteCycles, cycles_until_next_fetch()) << "and it stays that way";

    EXPECT_TRUE(console.apu.dmc_sample_buffer_filled()) << "the buffer is refilled each time, not left empty";
}

// The reader is driven by the buffer, not by playback. NESdev, on the memory
// reader: "It only cares about the read buffer and will fill it if it's empty."
// So silencing the output by exhausting the sample must not leave a byte
// stranded in the buffer, and enabling again must not double-fetch.
GTEST_TEST(dmcBuffer, a_finished_sample_leaves_the_buffer_full_until_it_is_played)
{
    Bus console;
    seed_spin_loop(console);
    start_sample(console, 0x00);  // 1 byte: one scheduled load empties the count

    ASSERT_EQ(6, cycles_until_buffer_fills(console));
    ASSERT_EQ(0, console.apu.dmc_bytes_remaining());
    ASSERT_TRUE(console.apu.dmc_sample_buffer_filled());

    // Re-enabling restarts the sample. The buffer is still full from last time,
    // so no fetch is due until it drains - which is precisely the case that
    // makes this a reload rather than a load.
    console.write(0x4015, 0x10);
    EXPECT_EQ(1, console.apu.dmc_bytes_remaining()) << "the restart reloads the length";
    EXPECT_TRUE(console.apu.dmc_sample_buffer_filled()) << "and finds the buffer still occupied, so no load is due";

    // Once the byte is played, the outstanding byte is fetched.
    run_cpu_cycles(console, kByteCycles + 32);
    EXPECT_EQ(0, console.apu.dmc_bytes_remaining()) << "the queued byte is fetched when the buffer frees up";
}

// A $4015 DISABLE is deferred the same way an enable is, and these two tests
// exist because mechanical mutation said nothing was watching the countdown.
//
// Mutating apu.cpp's `if (dmc.disable_delay != 0 && --dmc.disable_delay == 0)`
// four ways, against every APU and DMC suite including the ROMs, left two alive:
// `!= 0` to `== 0` on the decrement, and `== 0` to `== 1`. Both make the abort
// fire ONE CYCLE EARLY, and nothing in 1075 tests could tell. What did die was
// the mutant that removes the abort altogether - blargg's 7-dmc_basics catches
// that, and it is the only test in the project that reaches this code with
// bytes still remaining, which it does five times. So the abort's EXISTENCE had
// an oracle and its TIMING had none.
//
// The delay is Mesen2's, read from DeltaModulationChannel.cpp, and this
// emulator's line is character-for-character the same test:
//
//   if((_console->GetCpu()->GetCycleCount() & 0x01) == 0) { _disableDelay = 2; }
//   else { _disableDelay = 3; }
//   ...
//   if(_disableDelay && --_disableDelay == 0) {
//
// so 2 on an even CPU cycle and 3 on an odd one, counted down once per CPU
// cycle and acted on when it reaches zero.
//
// THESE TWO RESOLVE, FOR THE DISABLE PATH ONLY, the open question recorded at
// the top of this file: that the enable's parity table "is currently doing no
// work that the phase gate is not already doing", because both of its values
// land the DMA on the same cycle. Nothing like that applies here. The abort is
// not a DMA, waits for no get cycle and clears bytes-remaining the instant the
// counter expires, so 2 and 3 are directly distinguishable - which is why these
// are written as a pair rather than as one test at a pinned parity. The enable
// side of that question is still open.
GTEST_TEST(dmcBuffer, a_disable_on_an_even_cycle_takes_effect_two_cycles_later)
{
    Bus console;
    seed_spin_loop(console);
    start_sample_and_settle(console);

    console.write(0x4015, 0x00);  // disable: SCHEDULES the abort

    // The lower edge, and the reason this can still fail: without it the test
    // would pass just as well against an abort performed inline on the write,
    // which is the behaviour the delay exists to rule out.
    EXPECT_EQ(16, console.apu.dmc_bytes_remaining()) << "a $4015 disable must not abort on the cycle of the write";

    EXPECT_EQ(2, cycles_until_disable_takes_effect(console))
        << "an even-cycle disable expires on the second cycle after it";
}

GTEST_TEST(dmcBuffer, a_disable_on_an_odd_cycle_takes_effect_three_cycles_later)
{
    Bus console;
    seed_spin_loop(console);
    start_sample_and_settle(console);
    pin_odd_apu_cycle(console);

    console.write(0x4015, 0x00);

    EXPECT_EQ(16, console.apu.dmc_bytes_remaining()) << "a $4015 disable must not abort on the cycle of the write";

    // The one cycle of difference from the test above is the whole content of
    // the parity table on this path.
    EXPECT_EQ(3, cycles_until_disable_takes_effect(console))
        << "an odd-cycle disable expires on the third cycle after it";
}

// A RE-ENABLE DOES NOT CALL OFF A DISABLE ALREADY IN FLIGHT, so a channel can
// end up enabled with nothing left to play.
//
// This was written as a prediction before it was run, which is the only reason
// it is worth anything: the disable arms a 2-cycle countdown; the re-enable one
// cycle later takes $4015's other branch, which is guarded by `bytes_remaining
// == 0` and so does nothing at all while 16 bytes are outstanding; and neither
// branch touches disable_delay. The countdown therefore expires on schedule and
// clears a sample the CPU has just asked to keep.
//
// PROVENANCE, because this is not a hardware measurement and must not be read
// as one. Mesen2's SetEnabled leaves _disableDelay alone on the enable path -
//
//   if(!enabled) {
//     if(_disableDelay == 0) { ...set 2 or 3... }
//   } else if(_bytesRemaining == 0) {
//     InitSample();
//     ...set _transferStartDelay...
//   }
//
// - and this emulator's write handler is the same shape, so the two agree. No
// ROM here reaches it: the instrumented run behind the two tests above saw 3786
// expiries and not one of them had a transfer pending. What this test pins is
// agreement with Mesen and the absence of a plausible "fix" - clearing
// disable_delay on re-enable - that nothing else would catch.
GTEST_TEST(dmcBuffer, re_enabling_does_not_call_off_a_disable_already_counting_down)
{
    Bus console;
    seed_spin_loop(console);
    start_sample_and_settle(console);

    console.write(0x4015, 0x00);  // arms the abort, 2 cycles out on this parity
    run_cpu_cycles(console, 1);
    ASSERT_EQ(16, console.apu.dmc_bytes_remaining()) << "the abort must not have landed yet, or this tests nothing";

    console.write(0x4015, 0x10);
    EXPECT_EQ(16, console.apu.dmc_bytes_remaining()) << "the re-enable finds bytes outstanding, so it restarts nothing";

    run_cpu_cycles(console, 1);
    EXPECT_EQ(0, console.apu.dmc_bytes_remaining()) << "and the in-flight abort lands anyway, on its original schedule";
}

}  // namespace dmc_buffer
}  // namespace tests
