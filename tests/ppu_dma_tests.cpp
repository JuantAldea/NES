#include <functional>

#include "bus.h"
#include "gtest/gtest.h"
namespace tests
{
bool dma_test(void)
{
    Bus console;
    uint8_t bytes[256];
    for (auto i = 0; i < 256; i++) {
        bytes[i] = i;
    }

    uint8_t target_oam_addr = 0;
    uint16_t source_addr = 0x0200;

    console.write_ram(source_addr, 256, bytes);

    console.write(PPU::OAMADDR, target_oam_addr);

    console.write(PPU::OAMDMA, source_addr >> 8);

    do {
        console.clock();
    } while (console.ppu.dma_in_progress());

    for (auto i = 0; i < 256; i++) {
        if (bytes[i] != console.ppu.OAM_memory[(i + source_addr) % 256]) {
            return false;
        }
    }

    return true;
}

GTEST_TEST(testDMA, dma_test) { EXPECT_EQ(true, dma_test()); };

};  // namespace tests
