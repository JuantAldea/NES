// The CPU's own instruction timing, from the outside.
//
// SingleStepTests already checks every opcode per-cycle against captured
// hardware bus traces, which is stricter than anything here - but it does not
// run in CI, needing 1.1 GB of vectors CI does not fetch. These three do run
// there, in a few hundred KB, and they check the same property from a different
// direction: a program measuring its own execution on the machine rather than a
// vector file comparing states.
//
// THEY WERE FETCHED AS A DIAGNOSTIC. sprdma_and_dmc_dma reports a uniform
// eleven-cycle excess that no part of the DMA model accounts for, and what it
// measures is how long a code sequence took - so instruction timing was the
// obvious suspect outside that model. It is not the cause: all three pass
// unmodified. They are kept so that stays true and the question does not get
// asked again.
//
// Two protocols, because the suites are from different eras. instr_timing uses
// blargg's $6000 PRG-RAM protocol; cpu_timing_test6 predates it and prints to
// the screen, in capitals, which is why the two are read differently below.
#include <string>

#include "blargg_rom_harness.h"
#include "gtest/gtest.h"
#include "nametable_screen.h"
#include "rom_fixture.h"

namespace tests
{
namespace cpu_timing
{
namespace
{

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/cpu_timing/" + name + ".nes";
}

constexpr const char* kFetch = "run tests/test_files/fetch_cpu_timing.sh";

// 1-instr_timing announces "takes about 25 seconds" in its own output, which is
// ~1500 frames of real work. This is not a hang budget with room to spare, it is
// most of the runtime, so it is set well above what was measured.
constexpr uint64_t kMaxFrames = 3000;

#define REQUIRE_CPU_TIMING_ROM(name) REQUIRE_ROM(rom_path(name), kFetch)

}  // namespace

class InstrTimingRoms : public ::testing::TestWithParam<const char*>
{
};

TEST_P(InstrTimingRoms, reports_pass)
{
    const std::string name = GetParam();
    REQUIRE_CPU_TIMING_ROM(name);

    const blargg::RomResult result = blargg::run_rom(rom_path(name), kMaxFrames);

    ASSERT_TRUE(result.completed) << name << ": no verdict within " << kMaxFrames << " frames";
    EXPECT_EQ(0, result.status) << name << " failed with code " << static_cast<int>(result.status) << ":\n  "
                                << result.message;
}

// 1-instr_timing times the official instructions, the NOPs, alternate SBC and
// the unofficial ones; its own text notes it does not cover the 8 branches or
// the 12 illegal instructions, which is what 2-branch_timing is for. Both are
// needed to cover what the pair claims.
INSTANTIATE_TEST_SUITE_P(CpuTiming,
                         InstrTimingRoms,
                         ::testing::Values("1-instr_timing", "2-branch_timing"),
                         [](const ::testing::TestParamInfo<const char*>& info) {
                             std::string name = info.param;
                             for (char& c : name) {
                                 if (!std::isalnum(static_cast<unsigned char>(c))) {
                                     c = '_';
                                 }
                             }
                             return name;
                         });

// Read at a FIXED frame count rather than by waiting for the screen to settle.
// This ROM computes for sixteen seconds with the screen unchanged, so a reader
// that stops when the text stops moving stops during the test and reports the
// banner as the result.
GTEST_TEST(cpuTiming, the_16_second_timing_test_passes)
{
    REQUIRE_CPU_TIMING_ROM("cpu_timing_test");

    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom_path("cpu_timing_test")));
    console.cpu.reset();

    // Measured: PASSED appears at about frame 1200.
    for (int frame = 0; frame < 1500; ++frame) {
        nametable_screen::run_one_frame(console);
    }

    const std::string screen = nametable_screen::read_text(console);

    // Capitals, unlike the $6000-protocol ROMs' "Passed" - matching on the wrong
    // case would make this pass for a screen that never said so.
    EXPECT_NE(std::string::npos, screen.find("PASSED")) << "cpu_timing_test6 did not report PASSED. Full screen:\n"
                                                        << screen;
    EXPECT_EQ(std::string::npos, screen.find("FAILED")) << "cpu_timing_test6 reported a failure. Full screen:\n"
                                                        << screen;
}

}  // namespace cpu_timing
}  // namespace tests
