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
// ALL SIX PASS.
//
// What each ROM checks, from blargg's readme - the reset column is the half
// that only became testable when the harness learned to press the button:
//
//   4015_cleared       at power and reset, $4015 is cleared
//   irq_flag_cleared   at power and reset, the frame IRQ flag is clear
//   len_ctrs_enabled   at power and reset, length counters are enabled
//   4017_written       at reset $4017 is rewritten with the last value written
//   4017_timing        the 9-12 clock delay, at power and at reset; it PRINTS
//                      the figure it measures
//   works_immediately  NOT a reset test, despite sitting in this suite. It
//                      configures all five channels including $4010/$4013 and
//                      $4015 bit 4, then reads $4015 four times and compares
//                      the log. Bit 4 is the DMC's bytes-remaining, so this
//                      one gates on the DMC and not on reset behaviour at all.
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
// run includes the reset delay and the second half of the ROM. Those are
// COMPLETION figures, not floors, because all six pass - so 600 only has to
// absorb the emulator getting slower per frame, not a ROM getting further.
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

INSTANTIATE_TEST_SUITE_P(ApuReset,
                         ApuResetRoms,
                         ::testing::Values("4015_cleared",
                                           "irq_flag_cleared",
                                           "len_ctrs_enabled",
                                           "4017_written",
                                           "4017_timing",
                                           "works_immediately"),
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
    for (const char* name :
         {"4015_cleared", "irq_flag_cleared", "len_ctrs_enabled", "4017_written", "4017_timing", "works_immediately"}) {
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

}  // namespace apu_reset_rom
}  // namespace tests
