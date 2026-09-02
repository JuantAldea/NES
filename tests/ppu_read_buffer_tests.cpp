// Blargg's ppu_read_buffer test, which his readme calls a "mammoth test pack
// ... mostly centering around the PPU $2007 read buffer".
//
// It was unreachable until CNROM (mapper 3) landed - it is mapper 3 with four
// CHR banks. Wiring it up matters because it is the only ROM in the suite that
// exercises the $2007-during-rendering path at all; that behaviour was
// otherwise pinned by hand-written unit tests alone, having been added on the
// false assumption that blargg's vram_access covered it.
//
// It reports through the standard $6000 protocol, signature and all, despite
// belonging to the era that mostly does not - which is why it goes through
// blargg_rom_harness.h and not nametable_screen.h.
//
// It passes in full.
#include <cstdint>
#include <string>

#include "blargg_rom_harness.h"
#include "gtest/gtest.h"

namespace tests
{
namespace ppu_read_buffer
{

// MEASURED: the ROM reports on frame 1266, and the cap is 2x that. It is by
// some way the longest-running ROM in the suite.
//
// 1266 frames is 21.1s at 60.0988 Hz, which corroborates blargg's own readme -
// "the test will take about 20 seconds" on hardware. That agreement is the
// check worth having: a frame count that did not match his stated runtime
// would mean the ROM was taking a different path through itself.
constexpr uint64_t kMaxFrames = 2532;

std::string rom_path() { return std::string(NES_TEST_FILES_DIR) + "/ppu_read_buffer/test_ppu_read_buffer.nes"; }

// The pack covers far more than its name suggests: CIRAM through $2007 with
// both increment modes, PPU I/O mirroring, CHR-ROM reads through $2007, CNROM
// bank switching, sprite 0 hit, and OAM loaded from RAM, from ROM and from the
// PPU register file. It is the broadest single check in the suite.
GTEST_TEST(ppuReadBuffer, passes)
{
    const blargg::RomResult result = blargg::run_rom(rom_path(), kMaxFrames);

    ASSERT_TRUE(result.saw_signature) << "ppu_read_buffer never wrote the $DE $B0 $61 signature within " << kMaxFrames
                                      << " frames. It is mapper 3 (CNROM), so suspect the cartridge before the PPU - "
                                         "run tests/test_files/fetch_ppu_read_buffer.sh if the ROM is simply absent.\n"
                                      << "  CPU cycles run: " << result.cpu_cycles << ", final PC: " << std::hex
                                      << result.final_pc;

    ASSERT_TRUE(result.completed) << "the ROM started but never reported a result within " << kMaxFrames
                                  << " frames; last status was 0x" << std::hex << static_cast<int>(result.last_status);

    EXPECT_EQ(0, result.status) << "ppu_read_buffer reported failure " << static_cast<int>(result.status) << ":\n"
                                << result.message;
}

}  // namespace ppu_read_buffer
}  // namespace tests
