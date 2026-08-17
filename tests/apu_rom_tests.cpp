// blargg's apu_test suite - the oracle for building the APU.
//
// Every other ROM suite here guards work that is finished. This one measures
// work in progress: the frame counter, its /IRQ and the length counters exist;
// the envelope, the sweep, the triangle's linear counter, the channels' output,
// the mixer and the DMC do not, and clock_quarter_frame() is still empty.
//
// So the file is arranged as a WORK QUEUE, not a pass/fail gate. Six ROMs pass
// today and are asserted to keep passing; two fail and are pinned to their
// exact status and message. Pinning rather than skipping is the point - see the
// $AB/ATX precedent in instr_test_roms.cpp. A skipped failure says nothing when
// it changes; a pinned one fails loudly the moment the behaviour moves, in
// either direction.
//
// THE PINS HAVE ALREADY EARNED THEIR KEEP. This file began with three ROMs
// passing and five pinned. Implementing the length counters broke three of
// those pins, which is how their completion was announced - the assertion fails
// and its message says to promote the ROM into the list above. Keep that
// property when editing: a pin that is merely deleted teaches nothing.
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

constexpr const char* kFetch = "Run tests/test_files/fetch_apu_test.sh to fetch it.";

blargg::RomResult run(const std::string& name) { return blargg::run_rom(rom_path(name), kMaxFrames); }

}  // namespace

// --- what already works ------------------------------------------------------

class ApuRomsThatPass : public ::testing::TestWithParam<const char*>
{
};

TEST_P(ApuRomsThatPass, reports_pass)
{
    const std::string name = GetParam();
    SKIP_IF_ROM_ABSENT(rom_path(name), kFetch);

    const blargg::RomResult result = run(name);

    ASSERT_TRUE(result.completed) << name << ": no verdict within " << kMaxFrames << " frames";
    EXPECT_EQ(0, result.status) << name << " reported:\n" << result.message;
}

// Six of the eight. The frame counter and its IRQ were passing before any of
// this; the three length ROMs joined them when the length counters landed, and
// each did so by FAILING its pin first, which is what those pins are for.
INSTANTIATE_TEST_SUITE_P(
    ApuTest,
    ApuRomsThatPass,
    ::testing::Values("1-len_ctr", "2-len_table", "3-irq_flag", "4-jitter", "5-len_timing", "6-irq_flag_timing"),
    [](const ::testing::TestParamInfo<const char*>& info) {
        std::string name = info.param;
        for (char& c : name) {
            if (c == '-' || c == '.') {
                c = '_';
            }
        }
        return name;
    });

// --- the queue ---------------------------------------------------------------
//
// Each of these asserts the CURRENT failure exactly. When the feature lands the
// assertion fails, and the message says so - that is the intended way to find
// out you have finished something, and the reason not to write these as skips.

namespace
{

// Pins one not-yet-implemented ROM. `expected_status` and `fragment` come from
// the measured baseline, not from what the ROM ought to say one day.
void expect_pinned_failure(const std::string& name, const uint8_t expected_status, const std::string& fragment)
{
    const blargg::RomResult result = run(name);

    ASSERT_TRUE(result.completed) << name << ": no verdict within " << kMaxFrames << " frames";

    EXPECT_EQ(expected_status, result.status)
        << name << " changed status. If it now PASSES (0), the feature is done: delete this\n"
        << "  pin and move the ROM into the ApuRomsThatPass list above.\n"
        << "  Full message:\n"
        << result.message;

    EXPECT_NE(std::string::npos, result.message.find(fragment))
        << name << " still fails, but on a DIFFERENT subtest than the one recorded. That is\n"
        << "  either progress or a regression, and which one matters - check blargg's\n"
        << "  readme.txt for the new number.\n"
        << "  Expected to contain: " << fragment << "\n"
        << "  Full message:\n"
        << result.message;
}

}  // namespace

// The DMC, which is Phase 3 and deliberately last: it steals CPU cycles, and
// the cycle-exact bus is the most heavily verified thing in this project.
GTEST_TEST(apuRomQueue, the_dmc_is_not_implemented)
{
    SKIP_IF_ROM_ABSENT(rom_path("7-dmc_basics"), kFetch);
    expect_pinned_failure("7-dmc_basics", 2, "DMC isn't working well enough to test further");
}

GTEST_TEST(apuRomQueue, the_dmc_rate_table_is_not_implemented)
{
    SKIP_IF_ROM_ABSENT(rom_path("8-dmc_rates"), kFetch);
    expect_pinned_failure("8-dmc_rates", 2, "Rate 0's period is too short");
}

}  // namespace apu_rom
}  // namespace tests
