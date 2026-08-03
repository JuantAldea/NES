#include <functional>

#include "../include/bus.h"
#include "gtest/gtest.h"
namespace tests
{
bool dma_test(uint8_t target_oam_addr)
{
    Bus console;
    uint8_t bytes[256];

    for (auto i = 0; i < 256; i++) {
        bytes[i] = i;
    }

    uint16_t source_addr = 0x0200;

    console.write_ram(source_addr, 256, bytes);

    uint8_t dma_program[256];
    // NOP
    memset(dma_program, 0xEA, 256);
    // NOP x50 -> 100 cycles, 50 bytes
    // LDA #$02 -> 2 cycles, 2 bytes
    dma_program[0x32] = 0xA9;
    dma_program[0x33] = 0x02;
    // STA $4014 -> 4 cycles, 3 bytes
    dma_program[0x34] = 0x8D;
    dma_program[0x35] = 0x14;
    dma_program[0x36] = 0x40;
    // up to here 106 cycles & 55 bytes

    console.write_ram(0x400, 256, dma_program);
    console.write(PPU::OAMADDR, target_oam_addr);

    console.cpu.registers.PC = 0x400;

    uint64_t pre_dma_cpu_cycles = 0;
    uint16_t pre_dma_cpu_pc = 0;

    // Bounded so a regression that stops $4014 reaching the PPU fails the test
    // instead of spinning forever. The DMA is requested 106 CPU cycles in, i.e.
    // 106*12 bus cycles; this cap is comfortably above that.
    constexpr uint64_t kMaxBusCyclesToDmaStart = 106 * 12 * 4;
    uint64_t guard = 0;
    while (!console.ppu.dma_in_progress()) {
        console.clock();
        if (++guard > kMaxBusCyclesToDmaStart) {
            ADD_FAILURE() << "DMA never started within " << kMaxBusCyclesToDmaStart
                          << " bus cycles: the write to $4014 is not reaching the PPU";
            return false;
        }
    }

    pre_dma_cpu_pc = console.cpu.registers.PC;
    pre_dma_cpu_cycles = console.cpu.total_cycles;

    EXPECT_EQ(0, console.cpu.cycles_left);
    EXPECT_EQ(106, console.cpu.total_cycles);

    // PPU clocks 3 times faster than CPU
    EXPECT_EQ(console.cpu.total_cycles * 3, console.ppu.total_cycles);
    // BUS clock is divided by 12 for the CPU
    EXPECT_EQ(console.total_cycles, console.cpu.total_cycles * 12);
    // BUS clock is divided by 12 for the PPU
    EXPECT_EQ(console.total_cycles, console.ppu.total_cycles * 4);

    std::cout << "Cycles " << std::dec << console.cpu.total_cycles << std::endl;

    std::cout << "Cycles " << std::dec << console.ppu.total_cycles << std::endl;
    auto ppuc = console.ppu.total_cycles;
    // PC points to instruction after STA (PC = 0x437)
    EXPECT_EQ(0x400 + 49 + 2 + 3 + 1, console.cpu.registers.PC);

    std::cout << "PC     " << std::hex << console.cpu.registers.PC << std::endl;

    // finish DMA (bounded for the same reason: a DMA that never completes must
    // fail the test rather than hang it)
    constexpr uint64_t kMaxBusCyclesToDmaEnd = 514 * 12 * 4;
    guard = 0;
    while (console.ppu.dma_in_progress()) {
        console.clock();
        if (++guard > kMaxBusCyclesToDmaEnd) {
            ADD_FAILURE() << "DMA never completed within " << kMaxBusCyclesToDmaEnd << " bus cycles";
            return false;
        }
    }

    std::cout << "Cycles " << std::dec << console.cpu.total_cycles << std::endl;
    std::cout << "Cycles " << std::dec << console.ppu.total_cycles - ppuc << std::endl;
    std::cout << "Cycles " << std::dec << console.total_cycles << std::endl;
    // CPU state should be the same
    EXPECT_EQ(pre_dma_cpu_cycles, console.cpu.total_cycles);

    EXPECT_EQ(pre_dma_cpu_pc, console.cpu.registers.PC);

    EXPECT_EQ(0, console.cpu.cycles_left);

    // test contents
    for (uint16_t i = 0; i < 256; i++) {
        EXPECT_EQ(i, bytes[i]);
        EXPECT_EQ(bytes[i], console.ppu.OAM_memory[(i + target_oam_addr) % 256]);
    }

    return true;
}

GTEST_TEST(testDMA, dma_test)
{
    for (auto i = 0; i < 256; i++) {
        EXPECT_TRUE(dma_test(i));
    }
}

// OAM DMA steals CPU cycles, so it advances at the CPU's rate: 256 read/write
// pairs plus a halt cycle is 513, and one more to realign when the write to
// $4014 landed on an odd CPU cycle.
//
// This is worth pinning explicitly because the transfer used to be driven from
// PPU::clock, once per dot, which made it take ~171 CPU cycles instead - and
// nothing here noticed, because the surrounding assertions only check that the
// CPU is frozen and that OAM ends up correct, both of which stayed true.
GTEST_TEST(testDMA, dma_takes_513_or_514_cpu_cycles)
{
    // `odd_alignment` inserts a 3-cycle LDA $00 to flip the parity of the CPU
    // cycle the $4014 write lands on. A NOP would not: at 2 cycles it preserves
    // parity, which is easy to get wrong when writing this test.
    for (const bool odd_alignment : {false, true}) {
        Bus console;

        uint8_t program[16];
        memset(program, 0xEA, sizeof(program));
        size_t i = 0;
        if (odd_alignment) {
            program[i++] = 0xA5;  // LDA $00  (3 cycles)
            program[i++] = 0x00;
        }
        program[i++] = 0xA9;  // LDA #$02
        program[i++] = 0x02;
        program[i++] = 0x8D;  // STA $4014
        program[i++] = 0x14;
        program[i++] = 0x40;

        console.write_ram(0x0400, sizeof(program), program);
        console.cpu.registers.PC = 0x0400;
        console.cpu.cycles_left = 0;

        while (!console.ppu.dma_in_progress()) {
            console.clock();
        }

        const uint64_t cpu_cycle_at_request = console.cpu.total_cycles;
        const uint64_t bus_cycles_before = console.total_cycles;
        ASSERT_EQ(cpu_cycle_at_request % 2, odd_alignment ? 1u : 0u) << "test setup did not achieve the parity it wanted";

        while (console.ppu.dma_in_progress()) {
            console.clock();
        }

        // The CPU is clocked once every 12 bus cycles, so the transfer's length
        // in CPU cycles is the elapsed bus cycles over 12.
        const uint64_t elapsed_cpu_cycles = (console.total_cycles - bus_cycles_before) / 12;
        EXPECT_EQ(elapsed_cpu_cycles, odd_alignment ? 514u : 513u)
            << "DMA starting on an " << (odd_alignment ? "odd" : "even") << " CPU cycle";

        EXPECT_EQ(console.cpu.total_cycles, cpu_cycle_at_request) << "the CPU must be halted for the whole transfer";
    }
}


// The phase OAM DMA aligns to is a property of the bus, and the bus keeps
// counting while the CPU is halted. CPU::total_cycles does not - so after one
// 513-cycle transfer it trails the real cycle count by an ODD number, and every
// later DMA reads the opposite parity from the one it is actually on.
//
// A single DMA from reset cannot see this; the test above runs exactly one.
// This one runs two, and the second is the one that matters.
//
// Cycle arithmetic, all of it from the program below and the 6502's documented
// instruction lengths:
//
//   1-2      LDA #$02
//   3-6      STA $4014, whose write is its fourth cycle -> lands on cycle 6,
//            an even cycle, so the first transfer is 513 cycles
//   7-519    the first transfer (513 stolen cycles)
//   520-521  NOP
//   522-525  STA $4014 again -> lands on cycle 525, an odd cycle, so the second
//            transfer is 514 cycles
//
// Note 6 and 525 differ in parity only because 513 is odd. Taking the parity
// from CPU::total_cycles instead gives 6 and 12 - both even - and a second
// transfer of 513.
GTEST_TEST(testDMA, a_second_dma_takes_its_phase_from_the_bus_not_from_the_halted_cpu)
{
    Bus console;

    const uint8_t program[11] = {
        0xA9, 0x02,        // LDA #$02
        0x8D, 0x14, 0x40,  // STA $4014
        0xEA,              // NOP
        0x8D, 0x14, 0x40,  // STA $4014
        0x4C, 0x09,        // JMP $xx09 (self; low byte fixed up below)
    };
    console.write_ram(0x0400, sizeof(program), program);
    console.write(0x040B, 0x04);

    console.cpu.registers.PC = 0x0400;
    console.cpu.cycles_left = 0;

    uint64_t length[2] = {0, 0};
    uint64_t started_on[2] = {0, 0};

    for (int transfer = 0; transfer < 2; ++transfer) {
        while (!console.ppu.dma_in_progress()) {
            console.clock();
        }
        started_on[transfer] = console.cpu_cycles;
        const uint64_t bus_cycles_before = console.total_cycles;

        while (console.ppu.dma_in_progress()) {
            console.clock();
        }
        length[transfer] = (console.total_cycles - bus_cycles_before) / 12;
    }

    EXPECT_EQ(started_on[0], 6u) << "the first $4014 write does not land where the arithmetic above says";
    EXPECT_EQ(started_on[1], 525u) << "the second $4014 write does not land where the arithmetic above says";

    EXPECT_EQ(length[0], 513u) << "a $4014 write on an even CPU cycle transfers in 513 cycles";
    EXPECT_EQ(length[1], 514u)
        << "the second transfer began on an odd CPU cycle and must take 514; getting 513 means the parity was "
           "taken from a counter that stopped during the first transfer";
}

};  // namespace tests
