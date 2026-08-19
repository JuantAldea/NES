// What DMC DMA does to the CPU - the one part of the delta modulation channel
// that is deliberately not implemented.
//
// The DMC's memory reader fetches in zero cycles. On hardware it halts the CPU:
// per NESdev's DMA page, "DMC DMA normally takes 3 or 4 cycles, depending on
// whether alignment is needed", made of a halt cycle, an always-present dummy
// cycle, an optional alignment cycle, and the get cycle that performs the read.
// The halt only lands on a READ cycle - "if the CPU is writing, it ignores the
// halt...repeating until successful" - so a read-modify-write can delay it by
// two cycles and an interrupt by three.
//
// THE POINT OF THIS FILE IS THAT THE GAP IS MEASURED RATHER THAN ASSERTED.
// sprdma_and_dmc_dma prints a 16-row table of DMA lengths, CRCs its own output
// and prints a verdict. It fails today, and the numbers say why: every row
// reads 512 or 513, the pure OAM DMA lengths, with no DMC interference at all.
// That is the missing stall showing up as data rather than as an opinion.
//
// read_write_2007 already passes and is asserted to keep passing, which is the
// other half: a stall implementation that breaks what already works should not
// be able to hide behind the ROM it was written to fix.
//
// NOT WIRED, AND WHY - three ROMs in dmc_dma_during_read4 draw nothing at all,
// and one prints no verdict:
//
//   dma_2007_read, dma_2007_write, dma_4016_read
//       Blank, and NOT hung. Measured over 300 frames: 8.9M CPU cycles with the
//       PC oscillating over two or three addresses around $E066 - a tight wait
//       loop. They are waiting on something that never happens, and the obvious
//       candidate is the behaviour this suite exists to test. They are fetched
//       so that becomes checkable the moment the stall lands, but asserting on
//       a blank screen now would only pin our own ignorance.
//   double_2007_read
//       Prints a table and the CRC D84F6815 and stops. Compare-by-eye, not
//       self-checking, so it cannot be an automatic oracle without a known-good
//       CRC from hardware.
//
// The four dmc_tests ROMs behave the same way and are not fetched at all yet.
#include <string>

#include "gtest/gtest.h"
#include "nametable_screen.h"
#include "rom_fixture.h"

namespace tests
{
namespace dmc_dma
{
namespace
{

std::string rom_path(const std::string& name) { return std::string(NES_TEST_FILES_DIR) + "/dmc_dma/" + name + ".nes"; }

constexpr const char* kFetch = "run tests/test_files/fetch_dmc_dma.sh";

// Measured: read_write_2007 settles at frame 14, the sprdma pair at 157. 900 is
// far above that on purpose - these are a FLOOR and rise as ROMs get further.
constexpr uint64_t kMaxFrames = 900;
constexpr uint64_t kSettleFrames = 90;

#define REQUIRE_DMC_DMA_ROM(name) REQUIRE_ROM(rom_path(name), kFetch)

// Runs until the whole screen stops changing. The whole screen, not the first
// row: these ROMs print a table and then a verdict underneath it, so a reader
// watching one row would stop while the table was still filling.
std::string run_until_settled(const std::string& name)
{
    Bus console;
    EXPECT_TRUE(console.load_cartridge(rom_path(name))) << "the ROM is present but did not load: " << rom_path(name);
    console.cpu.reset();

    std::string last;
    uint64_t stable = 0;
    for (uint64_t frame = 0; frame < kMaxFrames; ++frame) {
        nametable_screen::run_one_frame(console);

        std::string text = nametable_screen::read_text(console);
        if (text == last) {
            if (++stable >= kSettleFrames) {
                break;
            }
            continue;
        }
        stable = 0;
        last = std::move(text);
    }
    return last;
}

bool screen_says(const std::string& screen, const std::string& word) { return screen.find(word) != std::string::npos; }

}  // namespace

// --- what already works ------------------------------------------------------

GTEST_TEST(dmcDma, a_2007_read_and_write_around_a_dma_still_pass)
{
    REQUIRE_DMC_DMA_ROM("read_write_2007");

    const std::string screen = run_until_settled("read_write_2007");

    EXPECT_TRUE(screen_says(screen, "Passed")) << "read_write_2007 passed before the DMC stall existed and must keep\n"
                                                  "  passing after it. Full screen:\n"
                                               << screen;
}

// --- the queue ---------------------------------------------------------------

// Pinned to its current failure, like every other unfinished feature here. When
// the stall lands this fails, and the message says to promote it.
class SprdmaAndDmcDma : public ::testing::TestWithParam<const char*>
{
};

TEST_P(SprdmaAndDmcDma, is_blocked_on_the_cpu_stall)
{
    const std::string name = GetParam();
    REQUIRE_DMC_DMA_ROM(name);

    const std::string screen = run_until_settled(name);

    ASSERT_TRUE(screen_says(screen, "Failed") || screen_says(screen, "Passed"))
        << name
        << " reached no verdict at all, which is neither the recorded failure nor a\n"
           "  pass. Full screen:\n"
        << screen;

    EXPECT_TRUE(screen_says(screen, "Failed"))
        << name
        << " now PASSES. The DMC's CPU stall must be implemented: delete this pin\n"
           "  and assert the pass instead. Full screen:\n"
        << screen;

    // The failure is pinned to its CAUSE, not just its verdict. Every row of the
    // table reads 512 or 513 - the OAM DMA lengths with no DMC interference -
    // so if the stall is implemented but wrongly, this stops matching and says
    // something different from "still not implemented".
    EXPECT_TRUE(screen_says(screen, "513") && screen_says(screen, "512"))
        << name
        << " still fails, but the DMA lengths it prints are no longer the plain 512 /\n"
           "  513 of an uninterrupted OAM DMA. That means a stall IS happening and is\n"
           "  wrong, which is a different problem from it being absent.\n"
           "  Full screen:\n"
        << screen;
}

INSTANTIATE_TEST_SUITE_P(DmcDma,
                         SprdmaAndDmcDma,
                         ::testing::Values("sprdma_and_dmc_dma", "sprdma_and_dmc_dma_512"),
                         [](const ::testing::TestParamInfo<const char*>& info) { return std::string(info.param); });

}  // namespace dmc_dma
}  // namespace tests
