// The sprite 0 hit flag, rule by rule.
//
// The eleven blargg sprite_hit ROMs are the real oracle for this; these tests
// exist so that a broken rule says WHICH rule, instead of "FAILED #3". Each one
// arranges an overlap that is opaque on exactly one side of the rule under test.
//
// From NESdev, PPUSTATUS bit 6:
//   "The sprite 0 hit flag is immediately set when any opaque pixel of sprite 0
//    overlaps any opaque pixel of background, regardless of sprite priority."
//   "Sprite 0 hit cannot detect collision at X=255, nor anywhere where either
//    sprites or backgrounds are disabled via PPUMASK. This includes X=0..7 when
//    the leftmost 8 pixels are hidden."
//   "The flag stays set until dot 1 of the prerender scanline."
#include <cstdint>

#include "../include/bus.h"
#include "gtest/gtest.h"

namespace tests
{
namespace sprite0
{
namespace
{

// PPUMASK bits, so the tests read as what they turn on rather than as hex.
constexpr uint8_t kShowBackgroundLeft = 0x02;
constexpr uint8_t kShowSpritesLeft = 0x04;
constexpr uint8_t kShowBackground = 0x08;
constexpr uint8_t kShowSprites = 0x10;
constexpr uint8_t kEverything = kShowBackgroundLeft | kShowSpritesLeft | kShowBackground | kShowSprites;

void run_past_reset_lockout(PPU& ppu)
{
    while (ppu.in_reset_write_lockout()) {
        ppu.clock();
    }
}

void clock_until(PPU& ppu, int scanline, int cycle)
{
    for (uint64_t guard = 0; guard < 2ull * 341 * 262; ++guard) {
        if (ppu.scanline == scanline && ppu.cycle == cycle) {
            return;
        }
        ppu.clock();
    }
    ADD_FAILURE() << "never reached scanline " << scanline << " dot " << cycle;
}

// An 8x8 tile whose low bitplane is `rows` and whose high bitplane is zero: a
// set bit is colour 1 (opaque), a clear bit colour 0 (transparent). Opacity is
// all the hit flag looks at, so one bitplane is enough to express it.
void write_tile(PPU& ppu, uint16_t table, uint8_t index, const uint8_t rows[8])
{
    for (uint16_t row = 0; row < 8; ++row) {
        ppu.ppu_bus_write(static_cast<uint16_t>(table + index * 16 + row), rows[row]);
        ppu.ppu_bus_write(static_cast<uint16_t>(table + index * 16 + 8 + row), 0x00);
    }
}

constexpr uint8_t kSolid[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr uint8_t kEmpty[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
// Only the leftmost column of the tile is opaque - bit 7 is the leftmost pixel.
constexpr uint8_t kLeftColumnOnly[8] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
// Only the top row.
constexpr uint8_t kTopRowOnly[8] = {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Tile 0 is transparent, tile 1 solid, in both pattern tables. Sprites and
// background then differ only in which tile index they name.
void write_the_standard_tiles(PPU& ppu)
{
    write_tile(ppu, 0x0000, 0, kEmpty);
    write_tile(ppu, 0x0000, 1, kSolid);
}

// Fills nametable 0 with `tile`, attributes left at zero.
void fill_background(PPU& ppu, uint8_t tile)
{
    for (uint16_t i = 0; i < 32 * 30; ++i) {
        ppu.ppu_bus_write(static_cast<uint16_t>(0x2000 + i), tile);
    }
}

void place_sprite0(PPU& ppu, uint8_t y, uint8_t tile, uint8_t attributes, uint8_t x)
{
    ppu.OAM_memory[0] = y;
    ppu.OAM_memory[1] = tile;
    ppu.OAM_memory[2] = attributes;
    ppu.OAM_memory[3] = x;
}

// Renders one frame from the pre-render line - where the flag is cleared - to
// the post-render line, and reports whether the hit happened during it.
//
// The flag is read out of the register directly rather than through
// PPU::read($2002), which would also clear vblank and disturb the next frame.
bool hit_during_one_frame(PPU& ppu)
{
    clock_until(ppu, PPU::pre_render_scanline, 1);
    ppu.clock();  // dot 1 of the pre-render line clears the flag
    EXPECT_EQ(0x00, ppu.registers.PPUSTATUS & 0x40) << "the pre-render line must clear the flag";

    clock_until(ppu, PPU::post_render_scanline, 0);
    return (ppu.registers.PPUSTATUS & 0x40) != 0;
}

// The common case: solid background, solid sprite 0, everything enabled.
void set_up_an_overlapping_frame(PPU& ppu, uint8_t sprite_x, uint8_t sprite_y, uint8_t mask = kEverything)
{
    run_past_reset_lockout(ppu);
    write_the_standard_tiles(ppu);
    fill_background(ppu, 1);
    place_sprite0(ppu, sprite_y, 1, 0x00, sprite_x);

    ppu.write(PPU::PPUCTRL, 0x00);  // 8x8 sprites, both pattern tables at $0000
    ppu.write(PPU::PPUMASK, mask);
    ppu.write(PPU::PPUSCROLL, 0x00);
    ppu.write(PPU::PPUSCROLL, 0x00);
}

}  // namespace

GTEST_TEST(testSprite0Hit, opaque_over_opaque_sets_the_flag)
{
    Bus console;
    set_up_an_overlapping_frame(console.ppu, 64, 64);

    EXPECT_TRUE(hit_during_one_frame(console.ppu));
}

GTEST_TEST(testSprite0Hit, a_transparent_background_does_not_hit)
{
    Bus console;
    set_up_an_overlapping_frame(console.ppu, 64, 64);
    fill_background(console.ppu, 0);  // tile 0 is all colour 0

    EXPECT_FALSE(hit_during_one_frame(console.ppu));
}

GTEST_TEST(testSprite0Hit, a_transparent_sprite_does_not_hit)
{
    Bus console;
    set_up_an_overlapping_frame(console.ppu, 64, 64);
    console.ppu.OAM_memory[1] = 0;  // tile 0

    EXPECT_FALSE(hit_during_one_frame(console.ppu));
}

GTEST_TEST(testSprite0Hit, the_flag_is_cleared_at_dot_1_of_the_pre_render_line)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up_an_overlapping_frame(ppu, 64, 64);

    ASSERT_TRUE(hit_during_one_frame(ppu));

    // It survives the whole of vblank...
    clock_until(ppu, PPU::pre_render_scanline, 1);
    EXPECT_EQ(0x40, ppu.registers.PPUSTATUS & 0x40) << "still set on the way into the pre-render line";

    // ...and no further.
    ppu.clock();
    EXPECT_EQ(0x00, ppu.registers.PPUSTATUS & 0x40);
}

// --- the PPUMASK exclusions ------------------------------------------------

GTEST_TEST(testSprite0Hit, no_hit_while_the_background_is_disabled)
{
    Bus console;
    set_up_an_overlapping_frame(console.ppu, 64, 64, kShowSprites | kShowSpritesLeft | kShowBackgroundLeft);

    EXPECT_FALSE(hit_during_one_frame(console.ppu));
}

GTEST_TEST(testSprite0Hit, no_hit_while_sprites_are_disabled)
{
    Bus console;
    set_up_an_overlapping_frame(console.ppu, 64, 64, kShowBackground | kShowBackgroundLeft | kShowSpritesLeft);

    EXPECT_FALSE(hit_during_one_frame(console.ppu));
}

GTEST_TEST(testSprite0Hit, no_hit_in_the_leftmost_eight_pixels_while_either_is_clipped)
{
    // A sprite entirely inside the left eight columns: x 0-7.
    {
        Bus console;
        set_up_an_overlapping_frame(console.ppu, 0, 64, kEverything);
        ASSERT_TRUE(hit_during_one_frame(console.ppu)) << "control: with nothing clipped it must hit";
    }
    {
        Bus console;
        set_up_an_overlapping_frame(console.ppu, 0, 64, kEverything & ~kShowBackgroundLeft);
        EXPECT_FALSE(hit_during_one_frame(console.ppu)) << "background clipped in the left eight";
    }
    {
        Bus console;
        set_up_an_overlapping_frame(console.ppu, 0, 64, kEverything & ~kShowSpritesLeft);
        EXPECT_FALSE(hit_during_one_frame(console.ppu)) << "sprites clipped in the left eight";
    }
}

GTEST_TEST(testSprite0Hit, a_sprite_straddling_the_clip_still_hits_to_the_right_of_it)
{
    // x = 4: columns 0-3 fall at screen x 4-7, under the clip; columns 4-7 at
    // x 8-11, outside it. The clip removes only the pixels it covers.
    Bus console;
    set_up_an_overlapping_frame(console.ppu, 4, 64, kEverything & ~kShowBackgroundLeft);

    EXPECT_TRUE(hit_during_one_frame(console.ppu));
}

// --- X = 255 ---------------------------------------------------------------

GTEST_TEST(testSprite0Hit, no_hit_at_x_255)
{
    // At x = 255 only the sprite's leftmost column is on screen, and hardware
    // cannot detect a collision there at all.
    Bus console;
    set_up_an_overlapping_frame(console.ppu, 255, 64);

    EXPECT_FALSE(hit_during_one_frame(console.ppu));
}

GTEST_TEST(testSprite0Hit, x_254_still_hits)
{
    // One pixel left of the exclusion, so the exclusion is at 255 exactly and
    // not "somewhere near the right edge".
    Bus console;
    set_up_an_overlapping_frame(console.ppu, 254, 64);

    EXPECT_TRUE(hit_during_one_frame(console.ppu));
}

// --- flipping --------------------------------------------------------------

namespace
{
// Background opaque only from x = 104 (tile column 13) rightwards, so which of
// sprite 0's eight columns is opaque decides whether there is a hit.
void set_up_a_horizontal_edge(PPU& ppu, uint8_t attributes)
{
    run_past_reset_lockout(ppu);
    write_tile(ppu, 0x0000, 0, kEmpty);
    write_tile(ppu, 0x0000, 1, kSolid);
    write_tile(ppu, 0x0000, 2, kLeftColumnOnly);

    for (uint16_t row = 0; row < 30; ++row) {
        for (uint16_t column = 0; column < 32; ++column) {
            ppu.ppu_bus_write(static_cast<uint16_t>(0x2000 + row * 32 + column), column >= 13 ? 1 : 0);
        }
    }

    // x = 100: columns 0-3 at screen x 100-103 (transparent background),
    // columns 4-7 at 104-107 (opaque).
    place_sprite0(ppu, 64, 2, attributes, 100);

    ppu.write(PPU::PPUCTRL, 0x00);
    ppu.write(PPU::PPUMASK, kEverything);
    ppu.write(PPU::PPUSCROLL, 0x00);
    ppu.write(PPU::PPUSCROLL, 0x00);
}

// Background opaque only from y = 72 (tile row 9) downwards.
void set_up_a_vertical_edge(PPU& ppu, uint8_t attributes)
{
    run_past_reset_lockout(ppu);
    write_tile(ppu, 0x0000, 0, kEmpty);
    write_tile(ppu, 0x0000, 1, kSolid);
    write_tile(ppu, 0x0000, 2, kTopRowOnly);

    for (uint16_t row = 0; row < 30; ++row) {
        for (uint16_t column = 0; column < 32; ++column) {
            ppu.ppu_bus_write(static_cast<uint16_t>(0x2000 + row * 32 + column), row >= 9 ? 1 : 0);
        }
    }

    // Y = 64 means the sprite occupies scanlines 65-72. Its row 0 lands on 65,
    // where the background is transparent; its row 7 on 72, where it is not.
    place_sprite0(ppu, 64, 2, attributes, 64);

    ppu.write(PPU::PPUCTRL, 0x00);
    ppu.write(PPU::PPUMASK, kEverything);
    ppu.write(PPU::PPUSCROLL, 0x00);
    ppu.write(PPU::PPUSCROLL, 0x00);
}
}  // namespace

GTEST_TEST(testSprite0Hit, horizontal_flip_moves_the_opaque_column)
{
    {
        Bus console;
        set_up_a_horizontal_edge(console.ppu, 0x00);
        EXPECT_FALSE(hit_during_one_frame(console.ppu))
            << "unflipped, the opaque column is at x=100, over transparent background";
    }
    {
        Bus console;
        set_up_a_horizontal_edge(console.ppu, 0x40);  // horizontal flip
        EXPECT_TRUE(hit_during_one_frame(console.ppu))
            << "flipped, it is at x=107, over opaque background";
    }
}

GTEST_TEST(testSprite0Hit, vertical_flip_moves_the_opaque_row)
{
    {
        Bus console;
        set_up_a_vertical_edge(console.ppu, 0x00);
        EXPECT_FALSE(hit_during_one_frame(console.ppu))
            << "unflipped, the opaque row is scanline 65, over transparent background";
    }
    {
        Bus console;
        set_up_a_vertical_edge(console.ppu, 0x80);  // vertical flip
        EXPECT_TRUE(hit_during_one_frame(console.ppu)) << "flipped, it is scanline 72";
    }
}

// --- 8x16 sprites ----------------------------------------------------------

namespace
{
// Sprite tile $02: an 8x16 sprite's tile number names the TOP tile and its bit
// 0 selects the pattern table, so this is tiles 2 (top) and 3 (bottom) of
// $0000. Tile 2 is transparent and tile 3 solid, so a hit can only come from
// the bottom half - which only exists in 8x16 mode.
void set_up_a_tall_sprite(PPU& ppu, bool tall)
{
    run_past_reset_lockout(ppu);
    write_tile(ppu, 0x0000, 1, kSolid);
    write_tile(ppu, 0x0000, 2, kEmpty);
    write_tile(ppu, 0x0000, 3, kSolid);
    fill_background(ppu, 1);
    place_sprite0(ppu, 64, 0x02, 0x00, 64);

    ppu.write(PPU::PPUCTRL, tall ? 0x20 : 0x00);  // bit 5: 8x16 sprites
    ppu.write(PPU::PPUMASK, kEverything);
    ppu.write(PPU::PPUSCROLL, 0x00);
    ppu.write(PPU::PPUSCROLL, 0x00);
}
}  // namespace

GTEST_TEST(testSprite0Hit, the_bottom_half_of_an_8x16_sprite_can_hit)
{
    {
        Bus console;
        set_up_a_tall_sprite(console.ppu, false);
        EXPECT_FALSE(hit_during_one_frame(console.ppu)) << "8x8: only the transparent tile 2 is drawn";
    }
    {
        Bus console;
        set_up_a_tall_sprite(console.ppu, true);
        EXPECT_TRUE(hit_during_one_frame(console.ppu)) << "8x16: tile 3 follows it, and is solid";
    }
}

GTEST_TEST(testSprite0Hit, an_8x16_sprite_takes_its_pattern_table_from_tile_bit_0)
{
    // Tile $01: bit 0 set, so the pattern comes from $1000 and the top tile is
    // index 0 there - NOT from the table PPUCTRL bit 3 selects.
    Bus console;
    PPU& ppu = console.ppu;
    run_past_reset_lockout(ppu);

    write_tile(ppu, 0x0000, 0, kEmpty);  // what an 8x8 sprite would have used
    write_tile(ppu, 0x0000, 1, kSolid);
    write_tile(ppu, 0x1000, 0, kSolid);  // the top half of tile $01 in 8x16 mode
    fill_background(ppu, 1);
    place_sprite0(ppu, 64, 0x01, 0x00, 64);

    // Bit 5 on (8x16), bit 3 clear (sprite pattern table $0000 for 8x8).
    ppu.write(PPU::PPUCTRL, 0x20);
    ppu.write(PPU::PPUMASK, kEverything);
    ppu.write(PPU::PPUSCROLL, 0x00);
    ppu.write(PPU::PPUSCROLL, 0x00);

    EXPECT_TRUE(hit_during_one_frame(ppu));
}

// --- the scanline a sprite occupies ----------------------------------------

GTEST_TEST(testSprite0Hit, oam_y_is_one_less_than_the_first_scanline)
{
    // Sprite evaluation runs a line ahead, so OAM byte 0 is the scanline BEFORE
    // the one the sprite first appears on.
    Bus console;
    PPU& ppu = console.ppu;
    run_past_reset_lockout(ppu);
    write_the_standard_tiles(ppu);
    place_sprite0(ppu, 64, 1, 0x00, 0);
    ppu.write(PPU::PPUCTRL, 0x00);

    ppu.scanline = 64;
    ppu.evaluate_sprite0_for_scanline();
    EXPECT_FALSE(ppu.sprite0_on_this_scanline) << "scanline 64 is the Y byte itself";

    ppu.scanline = 65;
    ppu.evaluate_sprite0_for_scanline();
    EXPECT_TRUE(ppu.sprite0_on_this_scanline) << "the sprite starts on the line after";

    ppu.scanline = 72;
    ppu.evaluate_sprite0_for_scanline();
    EXPECT_TRUE(ppu.sprite0_on_this_scanline) << "eight lines, 65-72";

    ppu.scanline = 73;
    ppu.evaluate_sprite0_for_scanline();
    EXPECT_FALSE(ppu.sprite0_on_this_scanline) << "and no more";

    // In 8x16 mode the same sprite covers sixteen lines, 65-80. Tile $02 means
    // tiles 2 and 3; only the bottom one needs to be opaque for the last line
    // of the range to register.
    ppu.write(PPU::PPUCTRL, 0x20);
    ppu.OAM_memory[1] = 0x02;
    write_tile(ppu, 0x0000, 3, kSolid);
    ppu.scanline = 80;
    ppu.evaluate_sprite0_for_scanline();
    EXPECT_TRUE(ppu.sprite0_on_this_scanline);

    ppu.scanline = 81;
    ppu.evaluate_sprite0_for_scanline();
    EXPECT_FALSE(ppu.sprite0_on_this_scanline);
}

}  // namespace sprite0
}  // namespace tests
