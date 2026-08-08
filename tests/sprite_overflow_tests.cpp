// Blargg's sprite overflow tests (sprite_overflow_tests).
//
// These are the external oracle for sprite EVALUATION - the half of the sprite
// pipeline that never reaches a pixel. The overflow flag is a by-product of the
// dot-by-dot scan of primary OAM, so a ROM that only ever reads $2002 bit 5
// still pins down when the scan starts, how fast it walks OAM, when it stops,
// and what it does after secondary OAM is full.
//
// The five must be run and fixed IN ORDER: blargg's readme notes that later
// ROMs assume the earlier ones already work, so a failure in 4.Obscure while
// 1.Basics is red says nothing at all.
//
// What each one is for, and what it caught here:
//
//   1.Basics    the flag exists and follows rendering state
//   2.Details   left clip, Y=239/240/255 edges, 8x16, "7 or fewer never sets"
//   3.Timing    WHEN the flag is set, to within a CPU clock or two. This is the
//               one that requires the per-dot state machine: it measures the
//               set against the CPU's own clock, so an implementation that
//               computes the flag once per line lands on the wrong dot however
//               correct its answer is.
//   4.Obscure   the overflow search bug, and nothing else - see the comment on
//               the m-advance in PPU::sprite_evaluation_write
//   5.Emulator  written specifically to defeat emulators that cache the flag:
//               it changes OAM, rendering state and sprite height mid-frame and
//               expects the flag's timing to move each time
#include <cstdint>
#include <string>

#include "../include/bus.h"
#include "gtest/gtest.h"
#include "nametable_screen.h"

namespace tests
{
namespace sprite_overflow
{

using nametable_screen::read_text;
using nametable_screen::run_one_frame;

// MEASURED, with sprite evaluation in place, as the frame each ROM first prints
// its verdict on:
//
//   1.Basics 14   2.Details 22   3.Timing 144   4.Obscure 21   5.Emulator 12
//
// The worst case is 3.Timing at 144 - it sweeps a write across a range of dots
// and needs a frame per position - so the cap is 2 x 144 = 288. That is enough
// headroom for a ROM that legitimately takes longer after a change, and low
// enough that a genuine hang is reported in a fraction of a second per ROM
// rather than being waited out. The loop stops as soon as a verdict appears, so
// a passing ROM never pays for the headroom.
//
// These numbers replace the pre-implementation measurements (7, 8, 31, 8, 8),
// which were time-to-FIRST-FAILURE and so a floor rather than a budget: every
// one of them is far below the frame its ROM now needs to finish, and a cap set
// from them would have reported four spurious hangs.
constexpr uint64_t kMaxFrames = 288;

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/sprite_overflow/" + name + ".nes";
}

// These ROMs print "FAILED: #2" - WITH a colon.
//
// The sprite_hit suite's predicate looks for "FAILED #" without one, and
// reusing it here matches nothing: every ROM then runs to the frame cap and
// reports as a timeout, hiding five perfectly good failure codes behind a
// fabricated hang. The reader (nametable_screen.h) is shared between the two
// suites; the verdict predicate deliberately is not.
bool has_verdict(const std::string& screen)
{
    if (screen.find("PASSED") != std::string::npos) {
        return true;
    }
    const size_t failed = screen.find("FAILED: #");
    if (failed == std::string::npos) {
        return false;
    }
    // Only accept the failure once its number has landed, so a screen caught
    // mid-write reports the code that was actually drawn rather than a
    // truncated one.
    const size_t digit = failed + 9;
    return digit < screen.size() && screen[digit] >= '0' && screen[digit] <= '9';
}

std::string run_to_verdict(const std::string& name)
{
    Bus console;
    if (!console.load_cartridge(rom_path(name))) {
        ADD_FAILURE() << "could not load " << rom_path(name) << " - run tests/test_files/fetch_sprite_overflow.sh";
        return {};
    }

    console.cpu.reset();

    for (uint64_t frame = 0; frame < kMaxFrames; ++frame) {
        run_one_frame(console);

        const std::string screen = read_text(console);
        if (has_verdict(screen)) {
            return screen;
        }
    }

    return read_text(console);
}

// Trims the screen to the rows with something on them, for failure output.
std::string visible(const std::string& screen)
{
    std::string out;
    size_t start = 0;
    while (start < screen.size()) {
        const size_t end = screen.find('\n', start);
        const std::string row = screen.substr(start, end - start);
        if (!row.empty()) {
            out += "    " + row + "\n";
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return out;
}

class SpriteOverflowRoms : public ::testing::TestWithParam<std::string>
{
};

// The blank-screen check is kept ahead of the verdict check because the two
// failures mean completely different things: a blank screen is the CPU or the
// cartridge mapping, not the PPU, and reporting it as "no PASSED found" would
// send the reader to the wrong place.
TEST_P(SpriteOverflowRoms, reports_passed)
{
    const std::string name = GetParam();
    const std::string screen = run_to_verdict(name);

    ASSERT_FALSE(screen.find_first_not_of(" \n") == std::string::npos)
        << name << ": the ROM drew nothing within " << kMaxFrames
        << " frames. It reports by writing tile indices into the nametable, so a blank "
           "screen means it never got that far - suspect the CPU or the cartridge "
           "mapping rather than the PPU.";

    EXPECT_NE(std::string::npos, screen.find("PASSED"))
        << name << " did not report PASSED within " << kMaxFrames << " frames:\n"
        << visible(screen);
}

INSTANTIATE_TEST_SUITE_P(SpriteOverflow,
                         SpriteOverflowRoms,
                         ::testing::Values("1.Basics", "2.Details", "3.Timing", "4.Obscure", "5.Emulator"),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                             std::string name = info.param;
                             // "1.Basics" is not a legal test name.
                             for (char& c : name) {
                                 if (c == '.') {
                                     c = '_';
                                 }
                             }
                             return name;
                         });

}  // namespace sprite_overflow
}  // namespace tests
