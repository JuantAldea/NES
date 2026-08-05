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
// STATUS: rendering is not implemented yet, so the ROMs cannot pass. What is
// asserted here today is that their verdict is READABLE - that the instrument
// works before anything is built against it. The "must report PASSED"
// assertion arrives with the sprite-0 hit implementation.
//
// Measured with rendering entirely absent, which is why the codes below are
// known rather than assumed: 01-04 report #2, 06-10 report #3, 05 reports #4.
// The one exception is recorded in the vacuous-pass test at the bottom.
#include <cstdint>
#include <string>

#include "gtest/gtest.h"

#include "../include/bus.h"
#include "nametable_screen.h"

namespace tests
{
namespace sprite_hit
{

using nametable_screen::read_text;
using nametable_screen::run_one_frame;

// Generous. With rendering absent every ROM settles by frame 62; once sprite 0
// hit works the later subtests actually run, so the real budget is larger and
// unknown until then. This cap is a timeout, not a measurement - the loop below
// stops as soon as a verdict appears, so a correct ROM never pays for it.
constexpr uint64_t kMaxFrames = 2000;

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
        ADD_FAILURE() << "could not load " << rom_path(name)
                      << " - run tests/test_files/fetch_sprite_hit.sh";
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

// Guards the INSTRUMENT, not the emulator. Until rendering exists every ROM
// fails, and that is expected; what must hold is that the failure is legible.
// If this test starts reporting an empty screen, the reader has broken and
// every rendering verdict built on it is worthless.
TEST_P(SpriteHitRoms, reports_a_readable_verdict)
{
    const std::string name = GetParam();
    const std::string screen = run_to_verdict(name);

    ASSERT_FALSE(screen.find_first_not_of(" \n") == std::string::npos)
        << name << ": the ROM drew nothing within " << kMaxFrames
        << " frames. It reports by writing tile indices into the nametable, so a blank "
           "screen means it never got that far - suspect the CPU or the cartridge "
           "mapping rather than the PPU.";

    EXPECT_TRUE(has_verdict(screen))
        << name << " drew a screen but no PASSED/FAILED verdict:\n"
        << visible(screen);
}

INSTANTIATE_TEST_SUITE_P(SpriteHit, SpriteHitRoms,
                         ::testing::Values("01.basics", "02.alignment", "03.corners", "04.flip",
                                           "05.left_clip", "06.right_edge", "07.screen_bottom",
                                           "08.double_height", "09.timing_basics", "10.timing_order",
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

// 11.edge_timing reports PASSED with the PPU unable to draw a single pixel.
// All three of its checks are negative - "hit time shouldn't be based on pixels
// under left clip / at X=255 / off right edge" - and a hit that never happens is
// never too early.
//
// This is pinned because it is a trap: once the pipeline is written it would be
// tempting to read "11/11 PASSED" as eleven ROMs' worth of evidence, when one of
// them has been passing since before there was anything to test. When sprite 0
// hit works, this ROM must be shown capable of FAILING under mutation before its
// pass counts for anything.
GTEST_TEST(spriteHitOracle, edge_timing_passes_vacuously_while_rendering_is_absent)
{
    const std::string screen = run_to_verdict("11.edge_timing");

    EXPECT_NE(std::string::npos, screen.find("PASSED"))
        << "11.edge_timing no longer passes vacuously. If sprite 0 hit is now implemented "
           "this test has served its purpose and should be replaced by the real assertion; "
           "if it is not, something else has changed and the oracle needs re-checking.\n"
        << visible(screen);
}

}  // namespace sprite_hit
}  // namespace tests
