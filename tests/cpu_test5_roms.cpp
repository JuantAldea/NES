// blargg's nes_cpu_test5: every instruction, checked the opposite way round from
// every other CPU oracle here.
//
// instr_test-v5 codes each instruction's behaviour as its own test. This suite,
// per its readme, sets "many combinations of input values for registers, flags,
// and memory, running the instruction under test, then updating a running
// checksum with the resulting values", and compares that checksum against "what
// a NES gives". The two therefore fail differently: a case nobody thought to
// write down is invisible to the first and inside the checksum of the second.
//
// THEY REPORT ON SCREEN, listing failing instructions by opcode and name, so
// they go through nametable_screen.h: blargg::run_rom sees no $6000 signature
// from them at all.
//
// THE ONE DISAGREEMENT IS OPCODE $AB, WHICH THIS REPO ALREADY CHOSE. cpu.nes
// reports "AB ATX #n" and "Errors: 1"; official.nes, which omits the unofficial
// instructions, passes outright. $AB (LXA/ATX) computes
// A = X = (A | magic) & immediate, where `magic` is an analogue property of the
// physical chip - it varies with the die, its temperature and what is floating
// on the bus - so there is no correct value. tests/instr_test_roms.cpp records
// the measurement behind picking $EE: SingleStepTests op_ab passes with it and
// fails 3 cases with $FF, and it is the more rigorous instrument.
//
// That this suite disagrees about exactly that opcode and agrees about every
// other one is the reason it earns its place. Two oracles built on different
// principles, one divergence between them, and it is the one already known.
#include <string>

#include "../include/bus.h"
#include "gtest/gtest.h"
#include "nametable_screen.h"
#include "rom_fixture.h"

namespace tests
{
namespace cpu_test5
{
namespace
{

constexpr const char* kFetch = "tests/test_files/fetch_cpu_test5.sh";

// official.nes settles by frame 639 and cpu.nes by 981, both measured. The cap
// is several times that: a ROM that gets further runs longer, and a ROM that
// hangs must report rather than sit here.
constexpr int kMaxFrames = 4000;

// Long enough that a pause between instruction groups is not mistaken for the
// end. cpu.nes holds the screen still for hundreds of frames while a group
// grinds - a 120-frame settle stopped it at 01-implied and read that as the
// final screen.
constexpr uint64_t kSettleFrames = 900;

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/cpu_test5/" + name + ".nes";
}

std::string run_until_settled(const std::string& name)
{
    Bus console;
    EXPECT_TRUE(console.load_cartridge(rom_path(name))) << "the ROM is present but did not load: " << rom_path(name);
    console.cpu.reset();

    std::string last;
    uint64_t stable = 0;
    for (int frame = 0; frame < kMaxFrames; ++frame) {
        nametable_screen::run_one_frame(console);

        std::string text = nametable_screen::read_text(console);
        if (text != last) {
            stable = 0;
            last = std::move(text);
            continue;
        }
        if (++stable >= kSettleFrames) {
            break;
        }
    }
    return last;
}

bool screen_says(const std::string& screen, const std::string& word) { return screen.find(word) != std::string::npos; }

}  // namespace

// Every official instruction, by a method no other oracle here uses.
GTEST_TEST(cpuTest5, official_instructions_all_pass)
{
    REQUIRE_ROM(rom_path("official"), kFetch);

    const std::string screen = run_until_settled("official");

    ASSERT_TRUE(screen_says(screen, "All tests complete"))
        << "official.nes reached no verdict within " << kMaxFrames
        << " frames. It lists failing\n"
           "  instructions by opcode, so the screen below names what broke if anything did:\n"
        << screen;

    EXPECT_FALSE(screen_says(screen, "Failed")) << "official.nes now fails. Every instruction it covers is official,\n"
                                                   "  so there is no undefined behaviour to excuse this. Full screen:\n"
                                                << screen;
    EXPECT_FALSE(screen_says(screen, "Error")) << "official.nes reports an error. Full screen:\n" << screen;
}

// The unofficial instructions too, and the failure is pinned rather than hidden -
// the same treatment 03-immediate gets in instr_test_roms.cpp, for the same
// opcode and the same reason.
GTEST_TEST(cpuTest5, all_instructions_fail_only_on_the_unstable_ATX_opcode)
{
    REQUIRE_ROM(rom_path("cpu"), kFetch);

    const std::string screen = run_until_settled("cpu");

    ASSERT_TRUE(screen_says(screen, "Failed") || screen_says(screen, "All tests complete"))
        << "cpu.nes reached no verdict within " << kMaxFrames << " frames. Full screen:\n"
        << screen;

    EXPECT_TRUE(screen_says(screen, "AB ATX"))
        << "cpu.nes no longer fails on ATX.\n"
           "  If it now passes entirely, $AB was probably changed to $FF - check SingleStepTests\n"
           "  op_ab, which fails 3 cases with that constant, and update this test and\n"
           "  instr_test_roms.cpp together.\n"
           "  If it fails on something else instead, THAT is a real regression: every instruction\n"
           "  in this ROM except ATX passes today. Full screen:\n"
        << screen;

    // The count, not just the name: a second opcode failing beside ATX would
    // otherwise pass the check above unnoticed.
    EXPECT_TRUE(screen_says(screen, "Errors: 1"))
        << "cpu.nes fails on more than ATX alone. ATX is the one deliberate divergence here;\n"
           "  anything beside it is a regression. Full screen:\n"
        << screen;
}

}  // namespace cpu_test5
}  // namespace tests
