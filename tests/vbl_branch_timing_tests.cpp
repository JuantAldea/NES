// Blargg's vbl_nmi_timing and branch_timing_tests - ten ROMs this project had
// never run.
//
// Found by asking which timing oracles in the mirror were going unused. The DMC
// DMA investigation had reached a point where sprdma_and_dmc_dma reported an
// eleven-cycle excess on a timed code sequence and nothing in the DMA model
// explained it, so anything timing the CPU or the CPU/PPU relationship was
// worth checking. Neither suite is the cause - all ten pass unmodified - and
// they are kept because a passing oracle that guards nothing is coverage left
// on the floor.
//
// OVERLAPPING BUT NOT REDUNDANT. ppu_vbl_nmi, fetched here as blargg_ppu,
// covers the same vblank and NMI ground through the $6000 protocol; this is the
// older screen-reporting suite reaching it by different code. And instr_timing
// states in its own output that it does not time the 8 branches, which is
// exactly what branch_timing_tests does - from the outside, as a program timing
// itself.
//
// They print PASSED in capitals, not the $6000 protocol's "Passed", so the
// match below is case-sensitive on purpose: matching loosely would accept a
// screen that never said it.
#include <string>

#include "gtest/gtest.h"
#include "nametable_screen.h"
#include "rom_fixture.h"

namespace tests
{
namespace vbl_branch_timing
{
namespace
{

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/vbl_branch_timing/" + name + ".nes";
}

constexpr const char* kFetch = "run tests/test_files/fetch_vbl_branch_timing.sh";

// MEASURED as the frame each ROM first shows PASSED: the vbl_nmi_timing seven
// land between 97 (3.even_odd_frames) and 174 (1.frame_basics), the branch
// three between 11 and 13. So this is ~5x the slowest.
//
// Read at a fixed count rather than by waiting for the screen to stop changing:
// these ROMs sit on an unchanged screen while they work, so a settle-detector
// stops mid-test and reports the title as the verdict.
constexpr int kFrames = 900;

std::string run_and_read(const std::string& name)
{
    Bus console;
    EXPECT_TRUE(console.load_cartridge(rom_path(name))) << "present but did not load: " << rom_path(name);
    console.cpu.reset();
    for (int frame = 0; frame < kFrames; ++frame) {
        nametable_screen::run_one_frame(console);
    }
    return nametable_screen::read_text(console);
}

}  // namespace

class VblNmiTimingRoms : public ::testing::TestWithParam<const char*>
{
};

TEST_P(VblNmiTimingRoms, reports_passed)
{
    const std::string name = GetParam();
    REQUIRE_ROM(rom_path(name), kFetch);

    const std::string screen = run_and_read(name);

    EXPECT_NE(std::string::npos, screen.find("PASSED")) << name << " did not report PASSED. Full screen:\n" << screen;
    EXPECT_EQ(std::string::npos, screen.find("FAILED")) << name << " reported a failure. Full screen:\n" << screen;
}

INSTANTIATE_TEST_SUITE_P(VblNmiTiming,
                         VblNmiTimingRoms,
                         ::testing::Values("1.frame_basics",
                                           "2.vbl_timing",
                                           "3.even_odd_frames",
                                           "4.vbl_clear_timing",
                                           "5.nmi_suppression",
                                           "6.nmi_disable",
                                           "7.nmi_timing"),
                         [](const ::testing::TestParamInfo<const char*>& info) {
                             std::string name = info.param;
                             for (char& c : name) {
                                 if (!std::isalnum(static_cast<unsigned char>(c))) {
                                     c = '_';
                                 }
                             }
                             return name;
                         });

class BranchTimingRoms : public ::testing::TestWithParam<const char*>
{
};

TEST_P(BranchTimingRoms, reports_passed)
{
    const std::string name = GetParam();
    REQUIRE_ROM(rom_path(name), kFetch);

    const std::string screen = run_and_read(name);

    EXPECT_NE(std::string::npos, screen.find("PASSED")) << name << " did not report PASSED. Full screen:\n" << screen;
    EXPECT_EQ(std::string::npos, screen.find("FAILED")) << name << " reported a failure. Full screen:\n" << screen;
}

INSTANTIATE_TEST_SUITE_P(BranchTiming,
                         BranchTimingRoms,
                         ::testing::Values("1.Branch_Basics", "2.Backward_Branch", "3.Forward_Branch"),
                         [](const ::testing::TestParamInfo<const char*>& info) {
                             std::string name = info.param;
                             for (char& c : name) {
                                 if (!std::isalnum(static_cast<unsigned char>(c))) {
                                     c = '_';
                                 }
                             }
                             return name;
                         });

}  // namespace vbl_branch_timing
}  // namespace tests
