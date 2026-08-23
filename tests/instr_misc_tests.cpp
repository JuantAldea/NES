// blargg's instr_misc rom_singles - four CPU behaviours no other oracle here
// reaches.
//
// A COMPLETENESS SWEEP, and it found nothing. All four passed on first contact,
// which was the expected result rather than a disappointment: nestest, Klaus,
// the 512 SingleStepTests cases and instr_test-v5 all pass, so the instruction
// core was already the best-covered part of this emulator. The rows stay
// because "correct" and "untested" look identical until something moves, and
// because of what 04 covers - see below.
//
//   01-abs_x_wrap       $FFFF wraps to 0 for STA abs,X and LDA abs,X
//   02-branch_wrap      branching past either end of RAM wraps around
//   03-dummy_reads      the dummy read before the real access, for LDA/STA
//                       with (ZP,X), (ZP),Y and ABS,X, plus ROL ABS,X
//   04-dummy_reads_apu  the same for every instruction that does one,
//                       UNOFFICIAL OPCODES INCLUDED, printing the opcode of
//                       each failure
//
// 04 is the one that earns its place. cpu_dummy_reads, over in
// cpu_behaviour_tests.cpp, already covers the documented cases; this one sweeps
// the whole opcode table including the unofficial half, and reports offenders
// by opcode rather than by subtest number. It is the only thing in the suite
// that would notice a dummy read going missing from an unofficial instruction -
// the same class of behaviour as opcode $AB, which this project already knows
// is where the disagreements live. It also needs $4015 IRQ flag reads to work
// at all, so it quietly depends on the APU's frame counter being right.
//
// WHY THESE ARE ORACLES AND apu_mixer IS NOT, since both report status $00 on
// this emulator and both use the same protocol. The difference is the message,
// not the status byte: these print the literal string "Passed", while apu_mixer
// prints "1. Should play short tone. / 2. Should be nearly silent." - a script
// for a listener, where $00 means only that the ROM reached the end. Anyone
// adding an APU oracle should read tests/apu_rom_tests.cpp first.
#include <cstdint>
#include <string>

#include "blargg_rom_harness.h"
#include "gtest/gtest.h"
#include "rom_fixture.h"

namespace tests
{
namespace instr_misc
{
namespace
{

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/instr_misc/" + name + ".nes";
}

// MEASURED: the four report at frames 11, 12, 56 and 140. These are COMPLETION
// figures rather than floors, because all four already pass - so the cap only
// has to absorb the emulator getting slower per frame, not a ROM getting
// further, and 400 is comfortably over 2x the slowest. A hang therefore fails
// with a diagnosis in well under a second instead of stalling the suite.
constexpr uint64_t kMaxFrames = 400;

constexpr const char* kFetch = "run tests/test_files/fetch_instr_misc.sh";

}  // namespace

class InstrMiscRoms : public ::testing::TestWithParam<const char*>
{
};

TEST_P(InstrMiscRoms, reports_pass)
{
    const std::string name = GetParam();
    REQUIRE_ROM(rom_path(name), kFetch);

    const blargg::RomResult result = blargg::run_rom(rom_path(name), kMaxFrames);

    // A timeout here is a different diagnosis from a failure, so it gets a
    // different message. These ROMs are NROM and report through $6000, so
    // nothing about mappers or the screen can be at fault: a ROM that never
    // reports has either jammed on an opcode or is waiting on $4015, which is
    // what 04 needs and the others do not.
    ASSERT_TRUE(result.completed) << name << ": no verdict within " << kMaxFrames
                                  << " frames. Suspect a jammed opcode, or - for 04-dummy_reads_apu only - the "
                                     "$4015 IRQ flag read it waits on.\n"
                                  << result.message;

    EXPECT_EQ(0, result.status) << name << " reported:\n"
                                << result.message
                                << "\n04 prints the OPCODE of each instruction whose dummy read is wrong, which is "
                                   "the number to look up - not a subtest index.";
}

INSTANTIATE_TEST_SUITE_P(InstrMisc,
                         InstrMiscRoms,
                         ::testing::Values("01-abs_x_wrap", "02-branch_wrap", "03-dummy_reads", "04-dummy_reads_apu"),
                         [](const ::testing::TestParamInfo<const char*>& info) {
                             std::string name = info.param;
                             // gtest wants an identifier: 01-abs_x_wrap is not one.
                             for (char& c : name) {
                                 if (c == '-') {
                                     c = '_';
                                 }
                             }
                             return name;
                         });

}  // namespace instr_misc
}  // namespace tests
