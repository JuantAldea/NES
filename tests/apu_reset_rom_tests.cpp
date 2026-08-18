// blargg's apu_reset suite - the machine's APU state at power, and after a
// soft RESET.
//
// This suite drove two features, in order. APU::power_on() first: the APU used
// to come up with everything zeroed, which is not the hardware's power-on state
// ("$00 written to $4017, then a 9-12 clock delay, then execution from the
// reset vector"), and four of these six failed on that alone. Then Bus::reset()
// and the harness pressing RESET, without which the other half of every ROM
// here was unreachable.
//
// FIVE OF SIX PASS. The sixth, works_immediately, is filed under power-on by
// its name and by this suite's own baseline, and that is wrong. Its source
// configures all five channels - including $4010/$4013 and $4015 bit 4 - then
// reads $4015 four times and compares the log. Bit 4 is the DMC's
// bytes-remaining, which this APU always reports as 0. It never even reaches
// the reset half; no amount of reset work will move it, and the DMC will.
//
// What each ROM checks, from blargg's readme - the reset column is the half
// that only became testable when the harness learned to press the button:
//
//   4015_cleared      at power and reset, $4015 is cleared
//   irq_flag_cleared  at power and reset, the frame IRQ flag is clear
//   len_ctrs_enabled  at power and reset, length counters are enabled
//   4017_written      at reset $4017 is rewritten with the last value written
//   4017_timing       the 9-12 clock delay, at power and at reset; it PRINTS
//                     the figure it measures
//
// The ROMs are fetched, not committed, and a missing one FAILS rather than
// skips - CI fetches them, so absence means the fetch step broke, and a skip
// exits 0 and would turn that broken run green.
#include <string>

#include "blargg_rom_harness.h"
#include "gtest/gtest.h"
#include "rom_fixture.h"

namespace tests
{
namespace apu_reset_rom
{
namespace
{

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/apu_reset/" + name + ".nes";
}

// Measured: the six report between frames 20 and 51, the later ones because a
// run now includes the reset delay and the second half of the ROM. 600 is far
// above that on purpose - these are a FLOOR and rise as ROMs get further, so a
// tight budget would turn progress into a timeout.
constexpr uint64_t kMaxFrames = 600;

constexpr const char* kFetch = "run tests/test_files/fetch_apu_reset.sh";

blargg::RomResult run(const std::string& name) { return blargg::run_rom(rom_path(name), kMaxFrames); }

}  // namespace

// --- what passes -------------------------------------------------------------

class ApuResetRoms : public ::testing::TestWithParam<const char*>
{
};

TEST_P(ApuResetRoms, reports_pass)
{
    const std::string name = GetParam();
    REQUIRE_ROM(rom_path(name), kFetch);

    const blargg::RomResult result = run(name);

    ASSERT_FALSE(result.needs_reset) << name << " asked for more than " << blargg::kMaxResets
                                     << " resets, which is a defect rather than a verdict";
    ASSERT_TRUE(result.completed) << name << ": no verdict within " << kMaxFrames << " frames";

    EXPECT_EQ(0, result.status) << name << " failed with code " << static_cast<int>(result.status) << " after "
                                << result.resets_driven << " reset(s):\n  " << result.message;
}

INSTANTIATE_TEST_SUITE_P(
    ApuReset,
    ApuResetRoms,
    ::testing::Values("4015_cleared", "irq_flag_cleared", "len_ctrs_enabled", "4017_written", "4017_timing"),
    [](const ::testing::TestParamInfo<const char*>& info) {
        std::string name = info.param;
        for (char& c : name) {
            if (!std::isalnum(static_cast<unsigned char>(c))) {
                c = '_';
            }
        }
        return name;
    });

// Every one of these needs the RESET button. Asserting that the harness really
// pressed it stops the suite from quietly reverting to testing only the power-on
// half: if run_rom stopped driving resets, the ROMs above would still report
// $81, `completed` would be false, and the failure would read as a timeout
// rather than as the missing feature it is.
GTEST_TEST(apuResetHarness, every_passing_rom_needed_a_reset_driven)
{
    for (const char* name : {"4015_cleared", "irq_flag_cleared", "len_ctrs_enabled", "4017_written", "4017_timing"}) {
        REQUIRE_ROM(rom_path(name), kFetch);
        const blargg::RomResult result = run(name);
        EXPECT_GT(result.resets_driven, 0u) << name << " reached its verdict without a reset being driven";
    }
}

// 4017_timing does not merely pass or fail - it PRINTS the delay it measured,
// and that number chose power_on_delay. blargg accepts the whole 9-12 window,
// so a passing verdict alone would not notice the constant changing within it.
// Asserted against the constant so the two cannot drift apart unnoticed.
GTEST_TEST(apuResetRoms, reports_the_4017_delay_we_implement)
{
    REQUIRE_ROM(rom_path("4017_timing"), kFetch);

    const blargg::RomResult result = run("4017_timing");
    const std::string expected = "Delay after effective $4017 write: " + std::to_string(APU::power_on_delay);

    EXPECT_NE(std::string::npos, result.message.find(expected))
        << "4017_timing measured a different delay than APU::power_on_delay claims.\n"
           "  Expected to contain: "
        << expected
        << "\n"
           "  blargg's readme accepts 9-12; 4-irq_and_dma additionally requires an\n"
           "  EVEN value. Full message:\n"
        << result.message;
}

// --- the queue ---------------------------------------------------------------

// Pinned, not skipped, and pinned to the DMC rather than to power-on: see the
// header. When the DMC lands this fails, and the message says to promote it.
GTEST_TEST(apuResetRomQueue, works_immediately_is_blocked_on_the_dmc)
{
    REQUIRE_ROM(rom_path("works_immediately"), kFetch);

    const blargg::RomResult result = run("works_immediately");

    ASSERT_TRUE(result.completed) << "works_immediately: no verdict within " << kMaxFrames << " frames";

    EXPECT_EQ(2, result.status) << "works_immediately changed status. If it now PASSES (0), the DMC is far\n"
                                   "  enough along: delete this pin and move the ROM into the list above.\n"
                                   "  Full message:\n"
                                << result.message;

    EXPECT_NE(std::string::npos, result.message.find("At power, writes should work immediately"))
        << "works_immediately still fails, but on a DIFFERENT subtest than the one\n"
           "  recorded - check blargg's source for the new number.\n"
           "  Full message:\n"
        << result.message;

    // It fails before ever asking for a reset, which is the evidence that this
    // is a power-stage failure and not a reset-stage one.
    EXPECT_EQ(0u, result.resets_driven) << "works_immediately now reaches the reset half - re-read which subtest\n"
                                           "  it is failing, because the DMC diagnosis above may no longer hold.";
}

}  // namespace apu_reset_rom
}  // namespace tests
