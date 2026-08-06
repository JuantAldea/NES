// Blargg's ppu_read_buffer test, which his readme calls a "mammoth test pack
// ... mostly centering around the PPU $2007 read buffer".
//
// It was unreachable until CNROM (mapper 3) landed - it is mapper 3 with four
// CHR banks. Wiring it up matters because it is the only ROM in the suite that
// exercises the $2007-during-rendering path at all; that behaviour was
// otherwise pinned by hand-written unit tests alone, having been added on the
// false assumption that blargg's vram_access covered it.
//
// A note in fetch_ppu_address_space.sh says this ROM "does not use the protocol
// either". That is wrong, and is corrected there: it reports through the
// standard $6000 protocol, signature and all.
//
// STATUS: it does not pass yet, and the one remaining failure is pinned below
// rather than papered over. See the comment on the test.
#include <cstdint>
#include <string>

#include "gtest/gtest.h"

#include "blargg_rom_harness.h"

namespace tests
{
namespace ppu_read_buffer
{

// MEASURED: the ROM reports on frame 1284, and takes about 1.7s of wall time to
// get there. The cap is 2x that. It is by some way the longest-running ROM in
// the suite, but it still finishes inside the time oam_stress takes (~2.4s), so
// with ctest running tests in parallel it does not move the total.
//
// blargg's own readme says "the test will take about 20 seconds" on hardware,
// which is the 1284 frames.
constexpr uint64_t kMaxFrames = 2568;

// The test number the ROM reports, not an address or a bitmask.
//
// 67 is "sprite 0 hit test using DMA ($4014) using PPU I/O bus as source", run
// with $4014 <- #$20 - so OAM DMA whose source page is $2000-$20FF, meaning 256
// reads of the PPU register file rather than of RAM or ROM. The ROM's own
// summary table shows every other row passing:
//
//     Direct poke   OK  OK
//     DMA with ROM  OK  OK
//     DMA + PPU bus OK  ??45
//     DMA with RAM
//
// Everything else in this pack passes, including the $2007 read buffer itself,
// which is what makes the ROM worth having wired up now rather than after the
// remaining case is fixed.
constexpr uint8_t kKnownRemainingFailure = 67;

std::string rom_path()
{
    return std::string(NES_TEST_FILES_DIR) + "/ppu_read_buffer/test_ppu_read_buffer.nes";
}

// Pins the CURRENT state deliberately, and is meant to fail in both directions.
//
// If the DMA-from-PPU-registers case gets fixed, this test goes red and should
// be tightened to require a pass. If anything else in the PPU regresses, the
// ROM reports a different (or additional) failure and this goes red too. A test
// that only said "it fails somehow" would catch neither.
GTEST_TEST(ppuReadBuffer, reports_only_the_known_dma_from_ppu_registers_failure)
{
    const blargg::RomResult result = blargg::run_rom(rom_path(), kMaxFrames);

    ASSERT_TRUE(result.saw_signature)
        << "ppu_read_buffer never wrote the $DE $B0 $61 signature within " << kMaxFrames
        << " frames. It is mapper 3 (CNROM), so suspect the cartridge before the PPU - "
           "run tests/test_files/fetch_ppu_read_buffer.sh if the ROM is simply absent.\n"
        << "  CPU cycles run: " << result.cpu_cycles << ", final PC: " << std::hex << result.final_pc;

    ASSERT_TRUE(result.completed) << "the ROM started but never reported a result within " << kMaxFrames
                                  << " frames; last status was 0x" << std::hex << static_cast<int>(result.last_status);

    EXPECT_EQ(kKnownRemainingFailure, result.status)
        << "ppu_read_buffer's result changed.\n"
        << "  If it is now 0, everything passes: delete this test and assert a pass instead.\n"
        << "  If it is anything else, something regressed - the ROM covers most of the PPU.\n"
        << "  Reported message:\n"
        << result.message;
}

}  // namespace ppu_read_buffer
}  // namespace tests
