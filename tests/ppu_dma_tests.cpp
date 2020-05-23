#include <functional>

#include "bus.h"
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
    while (!console.ppu.dma_in_progress()) {
        console.clock();
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

    // NOP is one byte long and takes 2 cycles so after 99 cycles 49'5 NOP should have been executed
    // one NOP cycle left
    //EXPECT_EQ(1, console.cpu.cycles_left);

    // PC points to instruction after STA (PC = 0x437)
    EXPECT_EQ(0x400 + 49 + 2 + 3 + 1, console.cpu.registers.PC);

    std::cout << "Cycles " << std::hex << console.cpu.registers.PC << std::endl;

    // finish DMA
    while (console.ppu.dma_in_progress()) {
        console.clock();
    }

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
        EXPECT_TRUE(dma_test(1));
    }
}

};  // namespace tests
