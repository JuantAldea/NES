// Smoke tests for the two freely-licensed homebrew programs.
//
// These exist because of a real hole. UNROM was covered only by synthetic
// images written in unrom_tests.cpp - my own idea of what a mapper-2 cartridge
// looks like, checked against my own idea of how it behaves. 240pee.nes is the
// only REAL mapper-2 image here, and until this file it was run by hand and
// looked at, which is not a regression guard.
//
// The same applied to sprite rendering. The blargg ROMs measure the overflow
// FLAG and sprite 0 HIT; spritecans is the only thing that puts a screen full
// of sprites under sustained load and keeps the evaluation saturated.
//
// WHAT THESE ARE NOT: correctness oracles. Neither program reports a verdict -
// they draw, and judging the drawing is still a human job (see
// fetch_visual_roms.sh). What is asserted here is structural: the ROM loads,
// runs without hanging, exercises the machinery it is supposed to exercise, and
// produces the same output twice. A renderer could satisfy all of that and
// still look wrong. It could not, however, satisfy it with UNROM banking broken
// or sprite evaluation dead, which is the point.
#include <cstdint>
#include <cstring>
#include <string>

#include "gtest/gtest.h"

#include "../include/bus.h"

namespace tests
{
namespace visual_rom
{
namespace
{

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/visual/" + name;
}

struct SpriteLoad {
    int max_on_a_line = 0;
    int lines_with_sprites = 0;
};

// Walks one whole frame a dot at a time, sampling how many sprites evaluation
// placed on each visible scanline. Dot 260 is after the evaluation window has
// closed and before the next line reopens it, so the count is settled.
SpriteLoad measure_sprite_load(Bus& console)
{
    SpriteLoad load;
    const uint64_t start = console.ppu.frame;
    int last_line = -1;

    while (console.ppu.frame == start) {
        console.clock();
        if (console.ppu.scanline < 240 && console.ppu.cycle == 260) {
            if (console.ppu.sprite_count > load.max_on_a_line) {
                load.max_on_a_line = console.ppu.sprite_count;
            }
            if (console.ppu.sprite_count > 0 && console.ppu.scanline != last_line) {
                ++load.lines_with_sprites;
                last_line = console.ppu.scanline;
            }
        }
    }
    return load;
}

int distinct_colours(const PPU& ppu)
{
    bool seen[64] = {false};
    int n = 0;
    for (int i = 0; i < PPU::screen_width * PPU::screen_height; ++i) {
        const uint8_t c = ppu.framebuffer[i] & 0x3F;
        if (!seen[c]) {
            seen[c] = true;
            ++n;
        }
    }
    return n;
}

}  // namespace

// --- 240pee: the only real UNROM image here --------------------------------

GTEST_TEST(visualRoms, the_240p_suite_runs_and_switches_prg_banks_under_its_own_control)
{
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom_path("240pee.nes")))
        << "visual ROMs absent - run tests/test_files/fetch_visual_roms.sh";

    ASSERT_EQ(2, console.rom.mapper_id) << "240pee is UNROM";
    ASSERT_EQ(0, console.rom.prg_bank) << "precondition: power-on bank";

    console.cpu.reset();
    for (int f = 0; f < 240; ++f) {
        console.run_frame();
    }

    // Measured: it reaches bank 2 by frame 240. Asserted as "not 0" rather than
    // "== 2" because the exact bank is the ROM's business and could change with
    // its version; what matters is that real software drove the mapper. The
    // synthetic tests in unrom_tests.cpp prove the banking is CORRECT - this
    // proves a real cartridge actually uses it.
    EXPECT_NE(0, console.rom.prg_bank) << "the ROM never switched banks; UNROM latching is not being exercised";

    // Measured: 10 distinct colours over 56818 non-backdrop pixels. A blank or
    // single-colour screen is the failure worth catching, so the bound is loose.
    EXPECT_GT(distinct_colours(console.ppu), 3) << "the screen is blank or nearly so";
}

GTEST_TEST(visualRoms, the_240p_suite_renders_deterministically)
{
    Bus a;
    Bus b;
    ASSERT_TRUE(a.load_cartridge(rom_path("240pee.nes")))
        << "visual ROMs absent - run tests/test_files/fetch_visual_roms.sh";
    ASSERT_TRUE(b.load_cartridge(rom_path("240pee.nes")));

    a.cpu.reset();
    b.cpu.reset();
    for (int f = 0; f < 120; ++f) {
        a.run_frame();
        b.run_frame();
    }

    // Two runs of the same ROM must agree exactly. Anything reading
    // uninitialised memory, or depending on wall-clock time, shows up here -
    // and would make every other assertion in the suite intermittent.
    EXPECT_EQ(0, std::memcmp(a.ppu.framebuffer, b.ppu.framebuffer, sizeof(a.ppu.framebuffer)))
        << "two identical runs produced different frames";
}

// --- spritecans: sustained sprite load -------------------------------------

GTEST_TEST(visualRoms, spritecans_saturates_sprite_evaluation_without_exceeding_eight)
{
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom_path("spritecans.nes")))
        << "visual ROMs absent - run tests/test_files/fetch_visual_roms.sh";

    console.cpu.reset();
    // The program adds cans over time, so an early frame has too few sprites to
    // stress anything. Measured: by frame 900 it saturates.
    for (int f = 0; f < 900; ++f) {
        console.run_frame();
    }

    const SpriteLoad load = measure_sprite_load(console);

    // Measured: exactly 8 on the busiest line, 227 lines carrying sprites.
    //
    // The equality is the assertion that matters, and it cuts both ways: 8 means
    // real software is pushing evaluation to its limit, and NOT MORE than 8
    // means the per-line cap holds under that load. A cap that leaked would
    // overflow secondary OAM, which is 32 bytes.
    EXPECT_EQ(8, load.max_on_a_line) << "expected the eight-sprite cap to be reached and respected";
    EXPECT_GT(load.lines_with_sprites, 100) << "sprites should cover most of the screen by now";
}

GTEST_TEST(visualRoms, spritecans_renders_deterministically)
{
    Bus a;
    Bus b;
    ASSERT_TRUE(a.load_cartridge(rom_path("spritecans.nes")))
        << "visual ROMs absent - run tests/test_files/fetch_visual_roms.sh";
    ASSERT_TRUE(b.load_cartridge(rom_path("spritecans.nes")));

    a.cpu.reset();
    b.cpu.reset();
    for (int f = 0; f < 300; ++f) {
        a.run_frame();
        b.run_frame();
    }

    EXPECT_EQ(0, std::memcmp(a.ppu.framebuffer, b.ppu.framebuffer, sizeof(a.ppu.framebuffer)))
        << "two identical runs produced different frames";
}

}  // namespace visual_rom
}  // namespace tests
