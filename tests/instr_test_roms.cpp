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

#include "../include/bus.h"
#include "gtest/gtest.h"
#include "nametable_screen.h"

namespace tests
{
namespace instr_test
{
namespace
{

using nametable_screen::run_one_frame;

constexpr uint16_t kStatusAddr = 0x6000;
constexpr uint16_t kSignatureAddr = 0x6001;
constexpr uint16_t kMessageAddr = 0x6004;
constexpr uint8_t kStatusRunning = 0x80;

// Measured: every one of these reports within a handful of frames; the slowest
// observed was well under 100. The cap exists so a hang fails with a diagnosis
// rather than stalling the suite.
constexpr int kMaxFrames = 600;

struct Result {
    bool completed = false;
    uint8_t status = 0xFF;
    std::string message;
};

Result run_rom(const std::string& name)
{
    Result result;

    Bus console;
    const std::string path = std::string(NES_TEST_FILES_DIR) + "/instr_test/" + name + ".nes";
    if (!console.load_cartridge(path)) {
        ADD_FAILURE() << "could not load " << path << " - run tests/test_files/fetch_instr_test.sh";
        return result;
    }

    // power_on(), not reset(): Bus's constructor already reset this CPU before a
    // cartridge was mapped, and a reset subtracts 3 from S rather than
    // reloading it.
    console.cpu.power_on();

    for (int frame = 0; frame < kMaxFrames; ++frame) {
        run_one_frame(console);

        if (console.read(kSignatureAddr) != 0xDE || console.read(static_cast<uint16_t>(kSignatureAddr + 1)) != 0xB0 ||
            console.read(static_cast<uint16_t>(kSignatureAddr + 2)) != 0x61) {
            continue;
        }

        const uint8_t status = console.read(kStatusAddr);
        if (status == kStatusRunning) {
            continue;
        }

        result.completed = true;
        result.status = status;
        for (uint16_t i = 0; i < 256; ++i) {
            const uint8_t c = console.read(static_cast<uint16_t>(kMessageAddr + i));
            if (c == 0x00) {
                break;
            }
            result.message.push_back(static_cast<char>(c));
        }
        return result;
    }
    return result;
}

}  // namespace

// The fifteen that pass outright.
class InstrTestRoms : public ::testing::TestWithParam<std::string>
{
};

TEST_P(InstrTestRoms, reports_pass)
{
    const std::string name = GetParam();
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
