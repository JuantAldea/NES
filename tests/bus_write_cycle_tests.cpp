// Which CPU cycles are WRITE cycles, watched at the bus.
//
// DMC DMA cannot halt the CPU on a write cycle - "if the CPU is writing, it
// ignores the halt...repeating until successful" - so its state machine needs
// to know. The CPU has no notion of it, and threading a flag through 79
// operations and every addressing mode would be a large change to the most
// heavily verified component here. Bus::write is the one place every access
// already passes through, so the answer is watched there instead.
//
// These tests exist because that indirection is only correct if it really does
// identify the same cycles the CPU is writing on. The instruction timings below
// are from the 6502's published schedules, not from this emulator.
#include <cstdint>
#include <string>
#include <vector>

#include "../include/bus.h"
#include "gtest/gtest.h"

namespace tests
{
namespace bus_write_cycle
{
namespace
{

// Runs `cycles` CPU cycles of a program seeded at $0400 and reports, for each,
// whether it was a write cycle.
std::vector<bool> write_cycles(const std::vector<uint8_t>& program, int cycles)
{
    Bus console;
    console.write_ram(0x0400, program.size(), program.data());
    console.cpu.registers.PC = 0x0400;
    console.cpu.cycles_left = 0;

    std::vector<bool> out;
    int seen = 0;
    while (seen < cycles) {
        const uint64_t before = console.cpu.total_cycles;
        console.clock();
        if (console.cpu.total_cycles != before) {
            out.push_back(console.cpu_wrote_this_cycle);
            ++seen;
        }
    }
    return out;
}

std::string as_string(const std::vector<bool>& cycles)
{
    std::string s;
    for (const bool wrote : cycles) {
        s.push_back(wrote ? 'W' : 'r');
    }
    return s;
}

}  // namespace

// LDA #$00 is 2 cycles and never writes; STA $0300 is 4, and only its last
// cycle is the write. Anything that reported the store's whole 4 cycles, or the
// wrong one of them, would break DMC DMA's halt in a way no APU test could see.
GTEST_TEST(busWriteCycle, only_the_final_cycle_of_an_absolute_store_is_a_write)
{
    const std::vector<uint8_t> program = {
        0xA9, 0x00,        // LDA #$00      2 cycles, no write
        0x8D, 0x00, 0x03,  // STA $0300     4 cycles, write on the 4th
        0xEA,              // NOP           2 cycles, no write
    };

    EXPECT_EQ(
        "rr"
        "rrrW"
        "rr",
        as_string(write_cycles(program, 8)));
}

// A read-modify-write is the case that matters most: it writes on TWO
// consecutive cycles, which is why NESdev says "delays of up to 3 cycles are
// possible, with read-modify-write instructions having 2 consecutive writes".
// INC $0300 is 6 cycles - the dummy write of the old value, then the real one.
GTEST_TEST(busWriteCycle, a_read_modify_write_writes_on_two_consecutive_cycles)
{
    const std::vector<uint8_t> program = {
        0xEE,
        0x00,
        0x03,  // INC $0300     6 cycles, writes on the 5th and 6th
        0xEA,  // NOP
    };

    EXPECT_EQ(
        "rrrrWW"
        "rr",
        as_string(write_cycles(program, 8)))
        << "the dummy write of the old value is a real bus write and must count as one";
}

// PHA pushes on its 3rd and last cycle. Included because the stack is a
// different address path, and a flag watched at the wrong layer could easily
// see RAM writes but not stack ones.
GTEST_TEST(busWriteCycle, a_stack_push_counts_as_a_write)
{
    const std::vector<uint8_t> program = {
        0x48,  // PHA   3 cycles, write on the 3rd
        0xEA,  // NOP
    };

    EXPECT_EQ(
        "rrW"
        "rr",
        as_string(write_cycles(program, 5)));
}

// The negative case. Nothing outside a CPU cycle may set the flag, or the DMA
// would refuse to halt on cycles where the CPU is doing nothing of the kind.
GTEST_TEST(busWriteCycle, writes_from_outside_a_cpu_cycle_do_not_count)
{
    Bus console;

    console.write(0x0300, 0x42);
    EXPECT_FALSE(console.cpu_wrote_this_cycle) << "a test or debugger write is not a CPU write cycle";

    const uint8_t program[2] = {0xEA, 0xEA};  // NOP, NOP
    console.write_ram(0x0400, sizeof(program), program);
    EXPECT_FALSE(console.cpu_wrote_this_cycle) << "write_ram is not a CPU write cycle either";
}

}  // namespace bus_write_cycle
}  // namespace tests
