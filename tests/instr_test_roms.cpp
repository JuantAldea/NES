// blargg's instr_test-v5: all 256 opcodes, official and unofficial, across
// every addressing mode.
//
// Locally this overlaps the 512 SingleStepTests, which verify each opcode
// per-cycle against hardware-captured bus traces and are strictly more
// rigorous. The reason this file exists is CI: those vectors are 1.1 GB and are
// not fetched there, so before this the whole of CI's instruction-level
// coverage was nestest and Klaus. These sixteen ROMs are a few hundred KB.
//
// The rom_singles are used rather than all_instrs.nes because the combined
// builds are mapper 1 (MMC1), which this emulator does not support. Verified
// from their headers.
#include <cstdint>
#include <string>

#include "blargg_rom_harness.h"
#include "gtest/gtest.h"
#include "rom_fixture.h"

namespace tests
{
namespace instr_test
{
namespace
{

// Measured: every one of these reports within a handful of frames; the slowest
// observed was well under 100. The cap exists so a hang fails with a diagnosis
// rather than stalling the suite.
constexpr uint64_t kMaxFrames = 600;

constexpr const char* kFetch = "run tests/test_files/fetch_instr_test.sh";

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/instr_test/" + name + ".nes";
}

// Start::PowerOn, not the default reset: Bus's constructor already reset this
// CPU before a cartridge was mapped, and a reset subtracts 3 from S rather than
// reloading it.
//
// This used to be a private copy of the $6000 loop. It handled every status
// except $81, so a ROM asking for a reset would have been reported as "failed
// with code 129" - a request misread as a verdict. None of these sixteen asks
// today, which is the only reason that never showed.
blargg::RomResult run_rom(const std::string& name)
{
    return blargg::run_rom(rom_path(name), kMaxFrames, blargg::Start::PowerOn);
}

using Result = blargg::RomResult;

}  // namespace

// The fifteen that pass outright.
class InstrTestRoms : public ::testing::TestWithParam<std::string>
{
};

TEST_P(InstrTestRoms, reports_pass)
{
    const std::string name = GetParam();
    REQUIRE_ROM(rom_path(name), kFetch);

    const Result result = run_rom(name);

    ASSERT_TRUE(result.completed) << name << ": no verdict within " << kMaxFrames << " frames";
    EXPECT_EQ(0, result.status) << name << " failed with code " << static_cast<int>(result.status) << ":\n  "
                                << result.message;
}

INSTANTIATE_TEST_SUITE_P(InstrTest,
                         InstrTestRoms,
                         ::testing::Values("01-basics",
                                           "02-implied",
                                           "04-zero_page",
                                           "05-zp_xy",
                                           "06-absolute",
                                           "07-abs_xy",
                                           "08-ind_x",
                                           "09-ind_y",
                                           "10-branches",
                                           "11-stack",
                                           "12-jmp_jsr",
                                           "13-rts",
                                           "14-rti",
                                           "15-brk",
                                           "16-special"),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                             std::string s = info.param;
                             for (char& c : s) {
                                 if (!std::isalnum(static_cast<unsigned char>(c))) {
                                     c = '_';
                                 }
                             }
                             return s;
                         });

// 03-immediate is asserted separately BECAUSE IT FAILS, and the failure is
// pinned rather than hidden.
//
// It reports $01 "AB ATX #n". $AB (LXA/ATX) computes
// A = X = (A | magic) & immediate, where `magic` is an analogue property of the
// physical chip - it varies with the die, its temperature, and what is floating
// on the bus. There is no correct value, and two external oracles here disagree
// about which convention to encode. Measured directly, by changing the constant
// and re-running both:
//
//   magic = $FF   03-immediate PASSES, SingleStepTests op_ab fails 3 cases
//   magic = $EE   03-immediate fails on ATX, SingleStepTests op_ab PASSES
//   magic = $00   both fail
//
// $EE is kept because SingleStepTests checks every opcode per-cycle against
// captured hardware traces and is the more rigorous instrument; ATX is 3 cases
// out of thousands. That is a defensible choice between two conventions for
// undefined behaviour, not a claim that blargg is wrong.
//
// Excluding this ROM from the suite would have been the easy move and would
// have hidden the disagreement. Asserting the exact failure keeps it visible:
// this test fails if ATX is ever changed (say to $FF), and it fails if
// 03-immediate ever breaks for a DIFFERENT reason - which is the case worth
// catching, since everything else in that ROM does pass today.
GTEST_TEST(instrTest, immediate_fails_only_on_the_unstable_ATX_opcode)
{
    REQUIRE_ROM(rom_path("03-immediate"), kFetch);

    const Result result = run_rom("03-immediate");

    ASSERT_TRUE(result.completed) << "03-immediate: no verdict within " << kMaxFrames << " frames";

    EXPECT_EQ(1, result.status)
        << "03-immediate's status changed. If it now passes, ATX was probably changed to $FF -\n"
           "  check SingleStepTests op_ab, and update this test and fetch_instr_test.sh.";

    EXPECT_NE(std::string::npos, result.message.find("AB ATX"))
        << "03-immediate still fails, but no longer on ATX. THIS IS A REAL REGRESSION in some\n"
           "  other immediate-mode opcode - the ATX divergence is known and deliberate, this is not.\n"
           "  Reported: "
        << result.message;
}

}  // namespace instr_test
}  // namespace tests
