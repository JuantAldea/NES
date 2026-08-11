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
    SKIP_IF_ABSENT(console, "full_palette.nes");

    EXPECT_EQ(57, distinct_indices(console.ppu))
        << "The ROM writes 56 palette entries ($x0-$xD of four groups) plus the $0F it\n"
           "  blackens the palette with. A drop means the forced-backdrop path stopped\n"
           "  selecting v; a rise means something is on screen the ROM never wrote.";
}

GTEST_TEST(fullPalette, the_smooth_variant_draws_the_same_grid)
{
    Bus console;
    SKIP_IF_ABSENT(console, "full_palette_smooth.nes");
    EXPECT_EQ(57, distinct_indices(console.ppu)) << "see the_whole_grid_is_drawn_through_the_forced_backdrop";
}

// The animated one, so a single frame is a moving subset rather than the whole
// grid - measured at 49 on frame 300. Held to a floor instead of an exact
// figure for that reason: what it guards is that the mechanism works at all
// under a changing palette, not the phase the animation happens to be in.
GTEST_TEST(fullPalette, the_flowing_variant_animates_through_the_palette)
{
    Bus console;
    SKIP_IF_ABSENT(console, "flowing_palette.nes");
    EXPECT_GT(distinct_indices(console.ppu), 40) << "see the_whole_grid_is_drawn_through_the_forced_backdrop";
}

}  // namespace full_palette
}  // namespace tests
