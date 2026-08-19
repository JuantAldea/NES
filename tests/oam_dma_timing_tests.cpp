// How long an OAM DMA takes, and what decides 513 against 514.
//
// This was the last component the DMC DMA investigation had never checked
// independently. sprdma_and_dmc_dma times a code sequence containing an OAM
// DMA, so a systematic error in its length would appear in every row of that
// ROM's table - and Bus computes it from a formula, 513 + (cpu_cycles % 2),
// that nothing here had ever verified.
//
// The rule is from NESdev's forum, stated by Crudelios and agreed in the thread
// asking whether a $4014 test ROM exists: "SPR DMA should take 513 cycles if it
// starts on an even cycle, 514 if it starts on an odd cycle." That is the
// figure asserted below, not the formula's own output.
//
// It turns out to be correct, so it is not the source of that ROM's excess -
// but an unverified formula in the middle of a timing investigation is worth
// closing off permanently rather than checking once.
#include <cstdint>

#include "../include/bus.h"
#include "gtest/gtest.h"

namespace tests
{
namespace oam_dma_timing
{
namespace
{

// Runs a program that writes $4014 and reports how many CPU cycles the DMA held
// the bus, along with the parity of the cycle the write landed on.
struct DmaRun {
    uint64_t write_cycle_parity = 0;
    uint64_t cycles = 0;
};

DmaRun measure(const bool flip_parity)
{
    Bus console;

    // LDA $00 is three cycles, so it flips the parity of the cycle STA $4014
    // lands on. A two-cycle NOP would not - it preserves parity, which is an
    // easy way to write this test and have it prove nothing.
    uint8_t program[16] = {};
    size_t i = 0;
    if (flip_parity) {
        program[i++] = 0xA5;  // LDA $00
        program[i++] = 0x00;
    }
    program[i++] = 0xA9;  // LDA #$02
    program[i++] = 0x02;
    program[i++] = 0x8D;  // STA $4014
    program[i++] = 0x14;
    program[i++] = 0x40;
    program[i++] = 0xEA;  // NOP
    program[i] = 0xEA;

    console.write_ram(0x0400, sizeof(program), program);
    console.cpu.registers.PC = 0x0400;
    console.cpu.cycles_left = 0;

    // Read straight out of PPU::remaining_dma_cycles the moment the DMA is
    // armed, rather than inferred from watching dma_in_progress go true and
    // then false. Those edges are ambiguous by one depending on which side of
    // the transition is sampled, and two earlier versions of this test
    // disagreed with each other by exactly that - once reporting 514/515 and
    // once 512/513 for the same emulator. The counter is the number itself.
    DmaRun run;
    for (int master = 0; master < 12 * 900; ++master) {
        const bool before = console.ppu.dma_in_progress();

        console.clock();

        if (!before && console.ppu.dma_in_progress()) {
            // Sampled AFTER the tick, because Bus::clock increments cpu_cycles
            // before running the CPU's store - so this is the value the $4014
            // write itself saw. Sampling before the tick reports the opposite
            // parity and makes a correct implementation look inverted, which an
            // earlier version of this test duly concluded.
            run.write_cycle_parity = console.cpu_cycles % 2;
            run.cycles = console.ppu.remaining_dma_cycles;
            break;
        }
    }
    return run;
}

}  // namespace

// 513 on an even cycle, 514 on an odd one. Written as two literals rather than
// as "n" and "n + 1" so that a formula which produced 600 and 601 could not
// satisfy it.
GTEST_TEST(oamDmaTiming, takes_513_cycles_from_an_even_cycle_and_514_from_an_odd_one)
{
    // Which of the two programs lands on which parity is not assumed - an
    // earlier version asserted it and failed on that rather than on the
    // behaviour under test. They are sorted by the parity actually measured.
    const DmaRun a = measure(false);
    const DmaRun b = measure(true);
    ASSERT_NE(a.write_cycle_parity, b.write_cycle_parity)
        << "the padding no longer flips the parity of the $4014 write, so this test proves nothing";

    const DmaRun& from_even = (a.write_cycle_parity == 0) ? a : b;
    const DmaRun& from_odd = (a.write_cycle_parity == 0) ? b : a;

    // Literals rather than "n" and "n + 1": a formula producing 600 and 601
    // would satisfy a relative check and is not what the hardware does.
    EXPECT_EQ(513u, from_even.cycles) << "an OAM DMA started on an even cycle takes 513";
    EXPECT_EQ(514u, from_odd.cycles) << "and 514 from an odd one - the extra cycle is the alignment";
}

// The difference carries meaning independently: both figures could be wrong by
// the same amount and still differ by one.
GTEST_TEST(oamDmaTiming, the_odd_start_costs_exactly_one_cycle_more)
{
    const DmaRun a = measure(false);
    const DmaRun b = measure(true);
    const uint64_t longer = (a.cycles > b.cycles) ? a.cycles : b.cycles;
    const uint64_t shorter = (a.cycles > b.cycles) ? b.cycles : a.cycles;
    EXPECT_EQ(1u, longer - shorter);
}

}  // namespace oam_dma_timing
}  // namespace tests
