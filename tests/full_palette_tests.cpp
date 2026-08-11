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

namespace tests
{
namespace full_palette
{
namespace
{

std::string rom_path(const std::string& name) { return std::string(NES_TEST_FILES_DIR) + "/full_palette/" + name; }

// Distinct 6-bit palette indices in the framebuffer. The mask is deliberate:
// the framebuffer holds an index, not a colour, so this counts what the PPU
// selected rather than what a display would make of it.
int distinct_indices(const PPU& ppu)
{
    bool seen[64] = {false};
    int n = 0;
    for (int i = 0; i < PPU::screen_width * PPU::screen_height; ++i) {
        const uint8_t c = static_cast<uint8_t>(ppu.framebuffer[i] & 0x3F);
        if (!seen[c]) {
            seen[c] = true;
            ++n;
        }
    }
    return n;
}

// Frames measured as sufficient in fetch_full_palette.sh: the count is stable
// at 60, 300 and 900, so 300 is well past settling without being slow. The ROM
// spends its first frames synchronising the CPU to VBL.
constexpr int kFrames = 300;

bool run_or_skip(Bus& console, const std::string& name, int frames = kFrames)
{
    if (!console.load_cartridge(rom_path(name))) {
        return false;
    }
    console.cpu.power_on();
    for (int f = 0; f < frames; ++f) {
        console.run_frame();
    }
    return true;
}

#define SKIP_IF_ABSENT(console, name)                                                 \
    if (!run_or_skip(console, name)) {                                                \
        GTEST_SKIP() << "no ROM at " << rom_path(name)                                \
                     << "\n  Run tests/test_files/fetch_full_palette.sh to fetch it." \
                        "\n  NOTE: ctest counts this skip as a pass. It is not one."; \
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
    SKIP_IF_ABSENT(console, "full_palette.nes");

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
    SKIP_IF_ABSENT(a, "full_palette.nes");
    ASSERT_TRUE(run_or_skip(b, "full_palette.nes"));

    EXPECT_EQ(0, std::memcmp(a.ppu.framebuffer, b.ppu.framebuffer, sizeof(a.ppu.framebuffer)))
        << "the same ROM produced two different frames - the timing is not deterministic";
}

// --- the gap, pinned ---------------------------------------------------------
//
// THIS TEST IS EXPECTED TO BE DELETED. It asserts the CURRENT, WRONG output so
// that the gap is visible in the suite rather than only in a comment, exactly
// as mmc3Irq.scanline_timing_is_not_yet_correct did before 4-scanline_timing
// was fixed. It is not a statement that 5 is right.
//
// Measured with emphasis and forced backdrop both absent: 5 distinct indices,
// 01 0F 11 21 31. $0F dominates because it is the black the ROM fills the
// palette with, and a disabled renderer currently emits the backdrop ($3F00)
// instead of the colour v points at - so almost the whole screen is one colour
// the hardware would not show. The other four are bands from where the
// background was still enabled while the ROM rewrote palette RAM underneath it.
//
// When forced backdrop lands this count jumps toward 64, and when emphasis
// lands the display path gains the other three bits. Both will fail this
// assertion, which is the point: replace it with the real bound, do not relax
// it.
GTEST_TEST(fullPalette, forced_backdrop_and_emphasis_are_not_yet_implemented)
{
    Bus console;
    SKIP_IF_ABSENT(console, "full_palette.nes");

    EXPECT_EQ(5, distinct_indices(console.ppu))
        << "The palette output changed. If it went UP, forced backdrop and/or emphasis now\n"
           "  work - delete this test and assert the real figure (the ROM draws all 64\n"
           "  colours, so the bound should be near that). If it went DOWN, something that\n"
           "  used to render has stopped.";
}

// The other two ROMs in the suite drive the same mechanism on different
// timing, so they are held to the same floor rather than left unrun - an
// implementation that fixed one and not the others would otherwise hide it.
GTEST_TEST(fullPalette, the_smooth_variant_shows_the_same_gap)
{
    Bus console;
    SKIP_IF_ABSENT(console, "full_palette_smooth.nes");
    EXPECT_EQ(5, distinct_indices(console.ppu)) << "see forced_backdrop_and_emphasis_are_not_yet_implemented";
}

GTEST_TEST(fullPalette, the_flowing_variant_shows_the_same_gap)
{
    Bus console;
    SKIP_IF_ABSENT(console, "flowing_palette.nes");
    EXPECT_EQ(5, distinct_indices(console.ppu)) << "see forced_backdrop_and_emphasis_are_not_yet_implemented";
}

}  // namespace full_palette
}  // namespace tests
