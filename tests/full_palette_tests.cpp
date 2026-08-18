// blargg's full_palette suite: the oracle for the last two PPU gaps.
//
// include/ppu.h has carried these as deliberate omissions since the renderer
// was written - PPUMASK colour emphasis (bits 5-7) is not applied, and the
// forced-backdrop case is not modelled. Neither had an oracle, which is why
// they stayed open: there was nothing to turn red.
//
// These ROMs close that. They are not palette-lookup checks - they are built ON
// the forced-backdrop trick, using it as their rendering technique. NESdev's
// PPU palettes page states the rule they exploit:
//
//   "During forced blank, the PPU normally draws the backdrop color. However,
//    if the current VRAM address in v points into palette RAM ($3F00-$3FFF),
//    then the color at that address will be drawn, instead, overriding the
//    backdrop color."
//
// and blargg's full_palette.s does exactly that, disabling rendering mid-frame
// and stepping v through palette RAM while writing the emphasis bits with
// `tya / and #$E0 / sta $2001`.
//
// THEY REPORT NO VERDICT. There is no $6000 status byte - the ROM's handlers
// are `irq: nmi: rti` and it only draws. So blargg_rom_harness.h does not apply
// and cannot be made to: a harness waiting on that protocol would wait forever.
// What is assertable is what the picture CONTAINS.
//
// The metric is the number of distinct palette indices in the finished frame,
// which is blunt but honest: the ROM's whole purpose is to put every colour on
// screen at once, so a renderer that cannot reach them shows few. See
// fetch_full_palette.sh for the measured baseline and the reasoning.
#include <cstdint>
#include <cstring>
#include <string>

#include "../include/bus.h"
#include "gtest/gtest.h"
#include "rom_fixture.h"

namespace tests
{
namespace full_palette
{
namespace
{

using tests::fixture::distinct_indices;
using tests::fixture::distinct_pixels;

std::string rom_path(const std::string& name) { return std::string(NES_TEST_FILES_DIR) + "/full_palette/" + name; }

constexpr const char* kFetch = "run tests/test_files/fetch_full_palette.sh";

// Frames measured as sufficient in fetch_full_palette.sh: the count is stable
// at 60, 300 and 900, so 300 is well past settling without being slow. The ROM
// spends its first frames synchronising the CPU to VBL.
constexpr int kFrames = 300;

// Named for what it does. This used to hide behind a macro called
// SKIP_IF_ABSENT, so six tests emulated ~2,100 frames between them and the call
// sites read as a file check. The load is asserted rather than returning a
// bool: presence is already established by REQUIRE_ROM, so a failure here means
// the file exists and will not parse, which is a broken fixture rather than a
// missing one and should stop the test.
void load_and_run(Bus& console, const std::string& name, int frames = kFrames)
{
    ASSERT_TRUE(console.load_cartridge(rom_path(name))) << "the ROM is present but did not load: " << rom_path(name);
    console.cpu.power_on();
    for (int f = 0; f < frames; ++f) {
        console.run_frame();
    }
}

}  // namespace

// Structural, in the spirit of visual_rom_tests.cpp: the ROM boots, runs to a
// picture and does not hang. Worth its own case because the suite is cycle-
// timed to the dot - it synchronises to VBL by exploiting that its loop takes
// 27 cycles against a 29780.67-cycle frame - so "it ran at all" is a real
// result and the thing that would break first.
GTEST_TEST(fullPalette, the_rom_runs_to_a_picture)
{
    Bus console;
    REQUIRE_ROM(rom_path("full_palette.nes"), kFetch);
    load_and_run(console, "full_palette.nes");

    EXPECT_GT(console.ppu.frame, 0u) << "the PPU never completed a frame";
    EXPECT_GT(distinct_indices(console.ppu), 1) << "the screen is a single flat colour - the ROM drew nothing";
}

// Determinism, which matters more here than for most ROMs: the grid only comes
// out if the CPU and PPU agree to the dot, so an unstable result would mean a
// timing wobble rather than a palette problem, and would make every other
// assertion in this file meaningless.
GTEST_TEST(fullPalette, two_runs_produce_the_same_picture)
{
    Bus a;
    Bus b;
    REQUIRE_ROM(rom_path("full_palette.nes"), kFetch);
    load_and_run(a, "full_palette.nes");
    load_and_run(b, "full_palette.nes");

    EXPECT_EQ(0, std::memcmp(a.ppu.framebuffer, b.ppu.framebuffer, sizeof(a.ppu.framebuffer)))
        << "the same ROM produced two different frames - the timing is not deterministic";
}

// --- the gap, pinned ---------------------------------------------------------
//
// The ROM draws 56 palette entries, and 57 distinct indices is the COMPLETE
// result rather than a near miss. The seven never drawn are $0E, $1E, $1F,
// $2E, $2F, $3E and $3F, and that is the ROM's doing, not ours: each row group
// points v at $3F3F and then writes fourteen consecutive entries starting at
// (Y & $18) << 1, so it covers $x0-$xD and stops. Four groups of fourteen is
// 56, plus the $0F the palette was blackened with = 57.
//
// Those same 56 are exactly the entries NESdev says emphasis darkens -
// "$00-$0D, $10-$1D, $20-$2D, and $30-$3D" - which is presumably why blargg
// chose them: $xE and $xF are the entries emphasis leaves alone, so they would
// carry no information in a grid whose purpose is showing emphasis.
//
// Before forced backdrop existed this was 5 (01 0F 11 21 31), almost all of it
// the blackened backdrop, because a disabled renderer emitted $3F00 instead of
// the colour v points at. See fetch_full_palette.sh for that baseline.
//
// EXACT, not a lower bound: 57 is what a correct renderer produces, and a
// change in either direction is a finding. Higher would mean colours appearing
// that the ROM never writes.
GTEST_TEST(fullPalette, the_whole_grid_is_drawn_through_the_forced_backdrop)
{
    Bus console;
    REQUIRE_ROM(rom_path("full_palette.nes"), kFetch);
    load_and_run(console, "full_palette.nes");

    EXPECT_EQ(57, distinct_indices(console.ppu))
        << "The ROM writes 56 palette entries ($x0-$xD of four groups) plus the $0F it\n"
           "  blackens the palette with. A drop means the forced-backdrop path stopped\n"
           "  selecting v; a rise means something is on screen the ROM never wrote.";
}

GTEST_TEST(fullPalette, the_smooth_variant_draws_the_same_grid)
{
    Bus console;
    REQUIRE_ROM(rom_path("full_palette_smooth.nes"), kFetch);
    load_and_run(console, "full_palette_smooth.nes");
    EXPECT_EQ(57, distinct_indices(console.ppu)) << "see the_whole_grid_is_drawn_through_the_forced_backdrop";
}

// The animated one, so a single frame is a moving subset rather than the whole
// grid - measured at 49 on frame 300. Held to a floor instead of an exact
// figure for that reason: what it guards is that the mechanism works at all
// under a changing palette, not the phase the animation happens to be in.
GTEST_TEST(fullPalette, the_flowing_variant_animates_through_the_palette)
{
    Bus console;
    REQUIRE_ROM(rom_path("flowing_palette.nes"), kFetch);
    load_and_run(console, "flowing_palette.nes");
    EXPECT_GT(distinct_indices(console.ppu), 40) << "see the_whole_grid_is_drawn_through_the_forced_backdrop";
}

// --- emphasis ----------------------------------------------------------------

// The whole point of the suite: it cycles every emphasis state over the grid
// with `tya / and #$E0 / sta $2001`, so a correct capture yields far more
// distinct 9-bit pixels than the 57 palette indices alone.
//
// MEASURED at 456, which is 57 x 8 EXACTLY - every one of the 57 indices
// appears under every one of the 8 emphasis states. I predicted 228 on the
// assumption that the ROM pairs each emphasis state with one group of rows;
// that was wrong, and the measurement is why this comment says so rather than
// the guess. It redraws the full grid under all eight, which is what "400+
// colors" in blargg's header means.
//
// The product being exact is itself the strongest assertion available here: it
// says no index-emphasis combination is missing and none is spurious. A drop
// means emphasis stopped being captured; a rise means pixels carry emphasis the
// ROM never set.
//
// This ALSO settles a question no source answered. The NESdev wiki does not say
// whether emphasis applies to the forced-backdrop output, and the agent that
// researched it flagged the point as inference rather than fact. The ROM
// answers it: every pixel here comes out of the forced-backdrop path, so if
// emphasis did not apply to it this count would collapse to 57.
GTEST_TEST(fullPalette, every_emphasis_state_reaches_the_framebuffer)
{
    Bus console;
    REQUIRE_ROM(rom_path("full_palette.nes"), kFetch);
    load_and_run(console, "full_palette.nes");

    EXPECT_EQ(57 * 8, distinct_pixels(console.ppu))
        << "distinct index+emphasis pairs. 57 would mean emphasis is not being captured at\n"
           "  all; the framebuffer's high three bits would be stuck at zero.";
}

// Emphasis must be sampled per pixel, not once per frame. The ROM rewrites
// $2001 mid-frame, so a single scanline's worth of emphasis painted over the
// whole picture would still produce SOME variety - this checks that more than
// one emphasis value survives within one frame, which a display-time read of
// PPUMASK could not manage.
GTEST_TEST(fullPalette, emphasis_varies_within_a_single_frame)
{
    Bus console;
    REQUIRE_ROM(rom_path("full_palette.nes"), kFetch);
    load_and_run(console, "full_palette.nes");

    bool seen[8] = {false};
    int n = 0;
    for (int i = 0; i < PPU::screen_width * PPU::screen_height; ++i) {
        const uint8_t e = PPU::pixel_emphasis(console.ppu.framebuffer[i]);
        if (!seen[e]) {
            seen[e] = true;
            ++n;
        }
    }

    EXPECT_EQ(8, n) << "all eight emphasis states should appear in one frame; saw " << n
                    << ". One means emphasis is being read at display time rather than captured.";
}

}  // namespace full_palette
}  // namespace tests
