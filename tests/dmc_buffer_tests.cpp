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
// THESE THREE WILL NEED RELAXING WHEN THE DMC DMA STALL LANDS, and the reason
// is recorded here so it is not mistaken for a regression.
//
// Mesen2 does not perform a $4015 enable's fetch on the cycle of the write. It
// schedules it 2 or 3 cycles later depending on cycle parity - _transferStartDelay,
// which it says matches dmc_dma_start_test - and only the output unit emptying
// the buffer starts one immediately. Implementing that here was tried: every
// APU ROM in this repository still passed, and the only failures were
// enabling_the_channel_fills_the_buffer_at_once,
// bytes_remaining_empties_one_byte_before_playback_ends and
// a_finished_sample_leaves_the_buffer_full_until_it_is_played, from this file.
//
// So these assert something stricter than the hardware does. blargg's
// 7-dmc_basics #19 only requires the byte to have arrived before the CPU's next
// INSTRUCTION reads $4015, which a 2-3 cycle delay satisfies; "on the same
// cycle" is this implementation's behaviour, not a documented requirement.
//
// They are left strict for now because they are correct FOR THE CODE AS IT
// STANDS, which fetches inline, and relaxing them to a fixed window was tried
// and simply fails the other way round. Widen them in the same commit that adds
// the delay, not before - and widen them to what the ROM requires rather than
// to whatever the new code happens to do.
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

// Starts the DMC at the slowest rate with a sample of `length_register` in
// $4013's units - so (n * 16) + 1 bytes - and no looping or IRQ.
void start_sample(Bus& console, const uint8_t length_register)
{
    console.write(0x4010, 0x00);  // rate index 0, no IRQ, no loop
    console.write(0x4012, 0x00);  // sample at $C000
    console.write(0x4013, length_register);
    console.write(0x4015, 0x10);  // enable: starts the sample
}

void run_cpu_cycles(Bus& console, const int cycles)
{
    for (int i = 0; i < cycles * 12; ++i) {
        console.clock();
    }
}

}  // namespace

// The buffer is filled the moment the channel is enabled, not on the next timer
// tick. blargg's 7-dmc_basics #19 states it from the other side - "there should
// be a one-byte buffer that's filled immediately if empty" - and asserts that a
// one-byte sample has therefore already finished by the time the next
// instruction reads $4015.
GTEST_TEST(dmcBuffer, enabling_the_channel_fills_the_buffer_at_once)
{
    Bus console;
    seed_spin_loop(console);

    ASSERT_FALSE(console.apu.dmc_sample_buffer_filled()) << "nothing should be buffered before the channel is enabled";

    start_sample(console, 0x01);  // 17 bytes

    EXPECT_TRUE(console.apu.dmc_sample_buffer_filled()) << "the buffer must be filled by the enable itself";
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

    EXPECT_EQ(0, console.apu.dmc_bytes_remaining()) << "a one-byte sample is fully fetched by the enable";
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
    start_sample(console, 0x00);  // 1 byte: fetched at once, none remaining

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

}  // namespace dmc_buffer
}  // namespace tests
