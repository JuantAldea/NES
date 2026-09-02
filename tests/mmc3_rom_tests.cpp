// blargg's MMC3 IRQ ROMs (mmc3_test_2), which are what the scanline counter in
// rom.cpp exists to satisfy.
//
// These test the counter almost entirely WITHOUT rendering. From blargg's
// readme: "The ROMs mainly test behavior by manually clocking the MMC3's IRQ
// counter by writing to $2006 to change the current VRAM address." That is why
// PPU::observe_ppu_address_bus is called from the $2006 write path and not only
// from the fetch pipeline - a counter wired to "end of scanline" would fail
// every one of these while still looking correct on a real game.
//
// Reporting is blargg's $6000 protocol, the same one instr_test_roms.cpp uses:
// a $DE $B0 $61 signature at $6001, a status byte at $6000 ($80 while running,
// $00 for pass), and a NUL-terminated message at $6004.
#include <cstdint>
#include <cstdio>
#include <string>

#include "blargg_rom_harness.h"
#include "gtest/gtest.h"
#include "rom_fixture.h"

namespace tests
{
namespace mmc3_rom
{
namespace
{

// MEASURED, not guessed. Every ROM was run with the frame of its verdict
// printed:
//
//   1-clocking 27   2-details 28   3-A12_clocking 26
//   4-scanline_timing 309   5-MMC3 26   6-MMC3_alt 31
//
// 4-scanline_timing is the outlier because it waits on real rendering rather
// than poking $2006: each of its thirteen subtests spends a whole frame
// synchronising to vblank before it can time anything.
//
// 309 is a COMPLETION figure and the cap is 2x it. The 75 recorded here
// previously was not comparable - back then the ROM stopped at its first
// failing subtest, so it was a floor, and the cap needed 4x to cover the
// subtests that had never run. Now that all thirteen do, headroom only has to
// absorb a change that makes the emulator slower per frame, not one that
// unlocks more work. A hang therefore costs ~1s (measured: 1.6ms/frame) and
// fails with a diagnosis instead of stalling the suite.
constexpr uint64_t kMaxFrames = 600;

constexpr const char* kFetch = "run tests/test_files/fetch_mmc3.sh";

std::string rom_path(const std::string& name) { return std::string(NES_TEST_FILES_DIR) + "/mmc3/" + name + ".nes"; }

// Start::PowerOn, not the default reset: Bus's constructor already reset this
// CPU before a cartridge was mapped, and a reset subtracts 3 from S rather than
// reloading it.
//
// Through the shared harness, which answers $81. None of these six ROMs asks
// for a reset today, so a loop that mishandled $81 looks correct here.
blargg::RomResult run_rom(const std::string& name)
{
    return blargg::run_rom(rom_path(name), kMaxFrames, blargg::Start::PowerOn);
}

using Result = blargg::RomResult;

}  // namespace

// The five that pass outright.
//
// 1-clocking         the counter decrements on an A12 toggle driven from $2006
// 2-details          reload edge cases, including a latch of 255
// 3-A12_clocking     the filter: which A12 rises count and which are too close
// 4-scanline_timing  the DOT the counter is clocked on, under real rendering
// 5-MMC3             the Sharp revision's reload-to-zero behaviour
//
// The two garbage nametable reads of each sprite fetch group are NOT what makes
// 4-scanline_timing pass: it passed while they were still unmodelled. They are
// on the bus today for an unrelated reason - A12 LOW is as observable as A12
// high.
//
// The cause is one dot. The ROM's own constants give it away - it
// asserts the scanline-0 IRQ arrives 256 dots later with $2000=$08 than with
// $2000=$10, i.e. that the sprite pattern fetch raises A12 exactly 256 dots
// after the background one does. fetch_background_byte read on the first dot of
// each two-dot access and fetch_sprite_pattern on the second, making the gap
// 257. Aligning the two phases is the whole fix; see fetch_sprite_pattern.
class Mmc3IrqRoms : public ::testing::TestWithParam<std::string>
{
};

TEST_P(Mmc3IrqRoms, reports_pass)
{
    const std::string name = GetParam();
    REQUIRE_ROM(rom_path(name), kFetch);

    const Result result = run_rom(name);

    ASSERT_TRUE(result.completed) << name << ": no verdict within " << kMaxFrames << " frames";
    EXPECT_EQ(0, result.status) << name << " failed with code " << static_cast<int>(result.status) << ":\n  "
                                << result.message;
}

INSTANTIATE_TEST_SUITE_P(Mmc3Irq,
                         Mmc3IrqRoms,
                         ::testing::Values("1-clocking", "2-details", "3-A12_clocking", "4-scanline_timing", "5-MMC3"),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                             std::string s = info.param;
                             for (char& c : s) {
                                 if (!std::isalnum(static_cast<unsigned char>(c))) {
                                     c = '_';
                                 }
                             }
                             return s;
                         });

// 6-MMC3_alt is asserted separately BECAUSE IT FAILS, and the failure is pinned
// rather than hidden - the same treatment instr_test_roms.cpp gives the
// unstable $AB opcode, and for the same reason.
//
// 5-MMC3 and 6-MMC3_alt test DIFFERENT SILICON. The MMC3 was made in two
// functionally distinct revisions, and they disagree about exactly one case:
// what happens when the counter is reloaded to zero.
//
//   5-MMC3      (Sharp, revision B/C) "Should reload and set IRQ every clock
//               when reload is 0"
//   6-MMC3_alt  (NEC, revision A)     "IRQ shouldn't be set when reloading to 0"
//
// Those are direct opposites, so no single implementation can pass both. This
// is not a gap to be closed; it is a fork in the hardware.
//
// Sharp is implemented because Super Mario Bros. 3 and Mega Man 3 are Sharp
// boards - it is the behaviour the games people actually run were written
// against, and picking the revision that breaks the flagship MMC3 titles to win
// a test would be exactly backwards.
//
// Excluding this ROM would have been the easy move and would have hidden the
// divergence. Asserting the exact failure keeps it visible: this test fails if
// the revision is ever switched, and - the case actually worth catching - it
// fails if 6-MMC3_alt ever breaks for some OTHER reason, since every subtest in
// it up to this point does pass today.
GTEST_TEST(mmc3Irq, alt_revision_fails_only_on_the_reload_to_zero_divergence)
{
    REQUIRE_ROM(rom_path("6-MMC3_alt"), kFetch);

    const Result result = run_rom("6-MMC3_alt");

    ASSERT_TRUE(result.completed) << "6-MMC3_alt: no verdict within " << kMaxFrames << " frames";

    EXPECT_EQ(2, result.status) << "6-MMC3_alt's status changed. If it now PASSES, the counter was probably\n"
                                   "  switched to the NEC revision A behaviour - check 5-MMC3, which asserts the\n"
                                   "  opposite and must then be failing.";

    EXPECT_NE(std::string::npos, result.message.find("reloading to 0"))
        << "6-MMC3_alt still fails, but no longer on the reload-to-zero case. THIS IS A REAL\n"
           "  REGRESSION in some other part of the IRQ counter - the revision divergence is known\n"
           "  and deliberate, this is not.\n"
           "  Reported: "
        << result.message;
}

}  // namespace mmc3_rom
}  // namespace tests
