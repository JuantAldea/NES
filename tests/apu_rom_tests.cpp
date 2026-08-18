// blargg's apu_test suite - the oracle for building the APU.
//
// Every other ROM suite here guards work that is finished. This one measures
// work in progress: the frame counter, its /IRQ and the length counters exist;
// the envelope, the sweep, the triangle's linear counter, the channels' output,
// the mixer and the DMC do not, and clock_quarter_frame() is still empty.
//
// ALL EIGHT NOW PASS, and the file records how it got there rather than only
// where it ended. It began with three passing and five pinned to their exact
// status and message. Pinning rather than skipping is the point - see the
// $AB/ATX precedent in instr_test_roms.cpp. A skipped failure says nothing when
// it changes; a pinned one fails loudly the moment the behaviour moves, in
// either direction, and every one of the five went out that way.
//
// THE PINS EARNED THEIR KEEP TWICE. Implementing the length counters broke
// three of them, and the DMC broke the last two - each time the assertion
// failed and its message said to promote the ROM into the list above, which is
// how a feature announces its own completion. Keep that property for whatever
// is pinned next: a pin that is merely deleted teaches nothing.
//
// WHICH ROMS PASS IS ITSELF A RESULT. 3-irq_flag, 4-jitter and 6-irq_flag_timing
// were passing before any of this work, confirming a frame counter - including
// the write-parity detail in APU::write - that had been reasoned out rather
// than measured, against an oracle it was never run against.
//
// See fetch_apu_test.sh for the measured baseline, and blargg's readme.txt for
// what each numbered failure means; 1-len_ctr alone enumerates seven distinct
// ways length counters go wrong, which is the order to implement them in.
#include <string>

#include "blargg_rom_harness.h"
#include "gtest/gtest.h"
#include "rom_fixture.h"

namespace tests
{
namespace apu_rom
{
namespace
{

std::string rom_path(const std::string& name) { return std::string(NES_TEST_FILES_DIR) + "/apu_test/" + name + ".nes"; }

// Measured: every ROM reports between frames 15 and 22. 600 is far above that
// on purpose - these numbers are a FLOOR and will rise as ROMs get further, so
// a tight budget would turn progress into a timeout.
constexpr uint64_t kMaxFrames = 600;

constexpr const char* kFetch = "run tests/test_files/fetch_apu_test.sh";

blargg::RomResult run(const std::string& name) { return blargg::run_rom(rom_path(name), kMaxFrames); }

}  // namespace

// --- what already works ------------------------------------------------------

class ApuRomsThatPass : public ::testing::TestWithParam<const char*>
{
};

TEST_P(ApuRomsThatPass, reports_pass)
{
    const std::string name = GetParam();
    REQUIRE_ROM(rom_path(name), kFetch);

    const blargg::RomResult result = run(name);

    ASSERT_TRUE(result.completed) << name << ": no verdict within " << kMaxFrames << " frames";
    EXPECT_EQ(0, result.status) << name << " reported:\n" << result.message;
}

// Six of the eight. The frame counter and its IRQ were passing before any of
// this; the three length ROMs joined them when the length counters landed, and
// each did so by FAILING its pin first, which is what those pins are for.
INSTANTIATE_TEST_SUITE_P(ApuTest,
                         ApuRomsThatPass,
                         ::testing::Values("1-len_ctr",
                                           "2-len_table",
                                           "3-irq_flag",
                                           "4-jitter",
                                           "5-len_timing",
                                           "6-irq_flag_timing",
                                           "7-dmc_basics",
                                           "8-dmc_rates"),
                         [](const ::testing::TestParamInfo<const char*>& info) {
                             std::string name = info.param;
                             for (char& c : name) {
                                 if (c == '-' || c == '.') {
                                     c = '_';
                                 }
                             }
                             return name;
                         });

}  // namespace apu_rom
}  // namespace tests
