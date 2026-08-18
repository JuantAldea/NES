// blargg's apu_reset suite - the machine's APU state at power, and after a
// soft RESET.
//
// This suite is what drove APU::power_on(). Before it existed the APU came up
// with everything zeroed, which is NOT the hardware's power-on state: blargg's
// readme specifies it as "$00 written to $4017, then a 9-12 clock delay, then
// execution from the reset vector". Four of these six ROMs failed on that
// alone, and now do not. See the comment on APU::power_on_delay for why the
// delay is 10 and not the 9 the readme calls typical - cpu_interrupts_v2's
// 4-irq_and_dma pins the CPU/APU phase, and an odd delay inverts it.
//
// NONE OF THE SIX CAN FULLY PASS TODAY, for two unrelated reasons, and keeping
// them apart is the whole point of this file:
//
//   FIVE ARE BLOCKED ON THE HARNESS. They report $81 - blargg's "needs reset" -
//   meaning they passed every power-on check and are waiting for a soft RESET
//   to run the second half. blargg_rom_harness.h records that in
//   RomResult::needs_reset and does not drive one. So $81 here is a PASS of
//   everything reachable, not a failure, and asserting it is how the suite
//   notices when driving a reset becomes possible.
//
//   ONE IS BLOCKED ON THE DMC. works_immediately is filed under power-on by its
//   name and by this suite's own baseline, and that turned out to be wrong. Its
//   source sets up all five channels - including $4010/$4013 and $4015 bit 4 -
//   then reads $4015 four times and compares the log. Bit 4 is the DMC's
//   bytes-remaining, which this APU always reports as 0. No amount of power-on
//   work will move it; the DMC will.
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

// Measured: the six report between frames 10 and 20. 600 is far above that on
// purpose - these are a FLOOR and rise as ROMs get further, so a tight budget
// would turn progress into a timeout.
constexpr uint64_t kMaxFrames = 600;

constexpr const char* kFetch = "run tests/test_files/fetch_apu_reset.sh";

// Fails rather than skips, and names the script. See the header.
#define REQUIRE_ROM(path)                                               \
    do {                                                                \
        const std::string required = (path);                            \
        if (!tests::fixture::rom_present(required)) {                   \
            FAIL() << "could not load " << required << " - " << kFetch; \
        }                                                               \
    } while (0)

blargg::RomResult run(const std::string& name) { return blargg::run_rom(rom_path(name), kMaxFrames); }

}  // namespace

// --- passing everything reachable without a soft RESET ------------------------

class ApuResetRomsAwaitingReset : public ::testing::TestWithParam<const char*>
{
};

TEST_P(ApuResetRomsAwaitingReset, clears_the_power_on_half_and_asks_for_reset)
{
    const std::string name = GetParam();
    REQUIRE_ROM(rom_path(name));

    const blargg::RomResult result = run(name);

    // A real failure code is the interesting outcome here, so report the ROM's
    // own message rather than just the number.
    EXPECT_TRUE(result.needs_reset) << name << " no longer stops at $81 (needs reset). It reported status "
                                    << static_cast<int>(result.status)
                                    << ".\n"
                                       "  If the harness has gained the ability to drive a soft RESET, this\n"
                                       "  expectation is what is now stale - assert the real verdict instead.\n"
                                       "  Otherwise a power-on check regressed. Full message:\n"
                                    << result.message;
}

INSTANTIATE_TEST_SUITE_P(
    ApuReset,
    ApuResetRomsAwaitingReset,
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

// 4017_timing does not merely pass or fail - it PRINTS the delay it measured,
// which is the single most useful number in the suite and the one that chose
// power_on_delay. It accepts the whole 9-12 window, so this asserts the figure
// we actually feed it: if power_on_delay changes without this being reconsidered,
// this fails and says so.
GTEST_TEST(apuResetRoms, reports_the_power_on_4017_delay_we_implement)
{
    REQUIRE_ROM(rom_path("4017_timing"));

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
    REQUIRE_ROM(rom_path("works_immediately"));

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
}

}  // namespace apu_reset_rom
}  // namespace tests
