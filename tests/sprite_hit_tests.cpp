// Blargg's sprite 0 hit tests (sprite_hit_tests_2005.10.05).
//
// These are the external oracle for the background rendering pipeline. Sprite 0
// hit fires when an opaque pixel of sprite 0 overlaps an opaque BACKGROUND
// pixel, so passing them requires the background pipeline to put the right
// pixel in the right place on the right dot. 02.alignment checks pixel-exact
// tile placement, 05.left_clip the PPUMASK left-8 window, and 09-11 the
// dot-exact timing of the flag. They test more of the background than of
// sprites.
//
// All eleven report PASSED, and every one of them is load-bearing: with the
// background pipeline and the hit flag in place there is no longer any ROM here
// whose pass comes for free. 11.edge_timing used to be exactly that - all three
// of its checks are negative ("hit time shouldn't be based on pixels under left
// clip / at X=255 / off right edge") and a hit that never happens is never too
// early, so it passed while the PPU could not draw a pixel. It was pinned as a
// trap until it could be shown to FAIL, which it now can: deleting the X=255
// exclusion from PPU::check_sprite0_hit makes it report FAILED #3 (and
// 06.right_edge FAILED #2). Its pass counts for something now, and the
// vacuous-pass test that guarded it has been removed.
#include <cstdint>
#include <string>

#include "../include/bus.h"
#include "gtest/gtest.h"
#include "nametable_screen.h"

namespace tests
{
namespace sprite_hit
{

using nametable_screen::read_text;
using nametable_screen::run_one_frame;

// MEASURED, with the pipeline and sprite-0 hit in place, as the frame each ROM
// first prints its verdict on:
//
//   01.basics 33   02.alignment 30   03.corners 19   04.flip 19
//   05.left_clip 28   06.right_edge 21   07.screen_bottom 24
//   08.double_height 19   09.timing_basics 67   10.timing_order 63
//   11.edge_timing 62
//
// The worst case is 09.timing_basics at 67, so the cap is 2 x 67 = 134: enough
// headroom for a ROM that legitimately takes longer after a change, and low
// enough that a genuine hang is reported in well under a second per ROM rather
// than being waited out. The loop stops as soon as a verdict appears, so a
// passing ROM never pays for the headroom.
//
// It was 2000 before, a number chosen when the ROMs could not pass and their
// real budget was unknown.
constexpr uint64_t kMaxFrames = 134;

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/sprite_hit/" + name + ".nes";
}

// A verdict is terminal: the ROM prints it and stops. Waiting for the whole
// screen to stop changing would work too but costs hundreds of extra frames per
// ROM, and 11 ROMs x 2000 frames of headroom is not free.
//
// "FAILED" is only accepted once its number has landed, so a screen caught
// mid-write reports the code that was actually drawn rather than a truncated
// one.
bool has_verdict(const std::string& screen)
{
    if (screen.find("PASSED") != std::string::npos) {
        return true;
    }
    const size_t failed = screen.find("FAILED #");
    if (failed == std::string::npos) {
        return false;
    }
    const size_t digit = failed + 8;
    return digit < screen.size() && screen[digit] >= '0' && screen[digit] <= '9';
}

// Runs until the ROM reports, and returns the whole screen.
std::string run_to_verdict(const std::string& name)
{
    Bus console;
    if (!console.load_cartridge(rom_path(name))) {
        ADD_FAILURE() << "could not load " << rom_path(name) << " - run tests/test_files/fetch_sprite_hit.sh";
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

class SpriteHitRoms : public ::testing::TestWithParam<std::string>
{
};

// The blank-screen check is kept ahead of the verdict check because the two
// failures mean completely different things: a blank screen is the CPU or the
// cartridge mapping, not the PPU, and reporting it as "no PASSED found" would
// send the reader to the wrong place.
TEST_P(SpriteHitRoms, reports_passed)
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

INSTANTIATE_TEST_SUITE_P(SpriteHit,
                         SpriteHitRoms,
                         ::testing::Values("01.basics",
                                           "02.alignment",
                                           "03.corners",
                                           "04.flip",
                                           "05.left_clip",
                                           "06.right_edge",
                                           "07.screen_bottom",
                                           "08.double_height",
                                           "09.timing_basics",
                                           "10.timing_order",
                                           "11.edge_timing"),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                             std::string name = info.param;
                             // "01.basics" is not a legal test name.
                             for (char& c : name) {
                                 if (c == '.') {
                                     c = '_';
                                 }
                             }
                             return name;
                         });

}  // namespace sprite_hit
}  // namespace tests
