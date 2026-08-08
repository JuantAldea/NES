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
        EXPECT_TRUE(hit_during_one_frame(console.ppu)) << "flipped, it is at x=107, over opaque background";
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

// How many sprites the output units hold while `line` is being drawn.
//
// This reads the pipeline's real result rather than calling an evaluator
// directly: evaluation for line L happens during line L-1's dots 65-256 and is
// loaded into the units at its dot 257, so sampling part-way through line L's
// own visible dots is the only honest way to ask "is the sprite on this line".
uint8_t sprites_on_line(PPU& ppu, int line)
{
    clock_until(ppu, line, 100);
    return ppu.sprite_count;
}

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
    // Evaluation only runs while rendering is enabled, so unlike the old
    // direct-call version this has to actually turn it on.
    ppu.write(PPU::PPUMASK, kEverything);

    clock_until(ppu, PPU::pre_render_scanline, 1);

    EXPECT_EQ(0u, sprites_on_line(ppu, 64)) << "scanline 64 is the Y byte itself";
    EXPECT_EQ(1u, sprites_on_line(ppu, 65)) << "the sprite starts on the line after";
    EXPECT_EQ(1u, sprites_on_line(ppu, 72)) << "eight lines, 65-72";
    EXPECT_EQ(0u, sprites_on_line(ppu, 73)) << "and no more";
}

GTEST_TEST(testSprite0Hit, an_8x16_sprite_covers_sixteen_scanlines)
{
    Bus console;
    PPU& ppu = console.ppu;
    run_past_reset_lockout(ppu);
    write_the_standard_tiles(ppu);
    place_sprite0(ppu, 64, 0x02, 0x00, 0);
    // Tile $02 in 8x16 mode means tiles 2 and 3 of the table bit 0 selects.
    write_tile(ppu, 0x0000, 3, kSolid);

    ppu.write(PPU::PPUCTRL, 0x20);  // 8x16
    ppu.write(PPU::PPUMASK, kEverything);

    clock_until(ppu, PPU::pre_render_scanline, 1);

    EXPECT_EQ(1u, sprites_on_line(ppu, 80)) << "sixteen lines, 65-80";
    EXPECT_EQ(0u, sprites_on_line(ppu, 81)) << "and no more";
}

// --- OAMADDR decides which sprite is "sprite 0" ---------------------------
//
// Sprite evaluation begins at OAMADDR, and the sprite that can raise the hit
// flag is the first one evaluated - so it is the sprite at OAMADDR, not OAM[0].
// This is why software is told to write 0 to $2003 before starting an OAM DMA.
//
// Every blargg sprite_hit ROM leaves OAMADDR at 0, so none of them can catch
// this; it needs a hand-written case.

// Puts a sprite that WILL overlap into the second OAM record, and parks the
// first record off-screen so it can never hit. Only a non-zero OAMADDR can
// produce a hit from this arrangement.
void park_the_hitting_sprite_in_record_one(PPU& ppu)
{
    place_sprite0(ppu, 0xFF, 1, 0x00, 0xFF);  // OAM[0..3]: off-screen
    ppu.OAM_memory[4] = 99;                   // OAM[4..7]: covers lines 100-107
    ppu.OAM_memory[5] = 1;                    // solid tile
    ppu.OAM_memory[6] = 0x00;
    ppu.OAM_memory[7] = 64;
}

// Runs a frame from the pre-render line, writing OAMADDR at a given dot of
// line 99 - the line whose evaluation decides line 100 - and reports whether a
// hit happened by the end of line 100.
bool hit_on_line_100_after_writing_oamaddr(PPU& ppu, int at_dot, uint8_t oamaddr)
{
    clock_until(ppu, PPU::pre_render_scanline, 1);
    ppu.clock();  // clears the flag
    EXPECT_EQ(0x00, ppu.registers.PPUSTATUS & 0x40);

    clock_until(ppu, 99, at_dot);
    ppu.write(PPU::OAMADDR, oamaddr);

    clock_until(ppu, 101, 0);
    return (ppu.registers.PPUSTATUS & 0x40) != 0;
}

GTEST_TEST(testSprite0Hit, a_non_zero_oamaddr_moves_which_sprite_can_hit)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up_an_overlapping_frame(ppu, 64, 64);
    park_the_hitting_sprite_in_record_one(ppu);

    // Dot 10 is before evaluation begins at dot 65, so this value is the one
    // evaluation latches. The sprite in the second record is then the one that
    // can hit.
    EXPECT_TRUE(hit_on_line_100_after_writing_oamaddr(ppu, 10, 0x04))
        << "evaluation starts at OAMADDR, so OAM[4..7] is the sprite that can hit";
}

GTEST_TEST(testSprite0Hit, oamaddr_left_at_zero_evaluates_the_first_record)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up_an_overlapping_frame(ppu, 64, 64);
    park_the_hitting_sprite_in_record_one(ppu);

    // The same frame with OAMADDR still 0 evaluates the off-screen record, so
    // nothing hits. Without this the test above could pass for the wrong
    // reason.
    EXPECT_FALSE(hit_on_line_100_after_writing_oamaddr(ppu, 10, 0x00))
        << "with OAMADDR at 0 the first record is OAM[0], which is parked off-screen";
}

// NESdev, $2003: "If OAMADDR is unaligned and does not point to the Y position
// (first byte) of an OAM entry, then whatever it points to (tile index,
// attribute, or X coordinate) will be reinterpreted as a Y position, and the
// following bytes will be similarly reinterpreted."
//
// This replaces a test that asserted the OPPOSITE - that OAMADDR is aligned
// down to the record containing it - and wrote that up as though it were the
// hardware rule. It was this emulator's simplification presented as spec, and
// because it passed, correcting the simplification would have meant deleting a
// green test: exactly the shape of thing that keeps a bug alive.
GTEST_TEST(testSprite0Hit, a_misaligned_oamaddr_reinterprets_the_following_bytes)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up_an_overlapping_frame(ppu, 64, 64);

    // Records 0 and 1 are parked off-screen, so nothing is found by reading OAM
    // as aligned four-byte records at all.
    place_sprite0(ppu, 0xFF, 1, 0x00, 0xFF);
    ppu.OAM_memory[4] = 0xFF;
    ppu.OAM_memory[5] = 0xFF;

    // A sprite laid out from OAM[6] onwards, straddling the boundary between
    // records 1 and 2. As records this is nonsense; read from $06 it is
    // Y=99 (covering line 100), a solid tile, attribute 0, X=64.
    ppu.OAM_memory[6] = 99;
    ppu.OAM_memory[7] = 1;
    ppu.OAM_memory[8] = 0x00;
    ppu.OAM_memory[9] = 64;

    EXPECT_TRUE(hit_on_line_100_after_writing_oamaddr(ppu, 10, 0x06))
        << "the byte at OAMADDR must be reinterpreted as a Y position";

    // The control, and the thing the old behaviour did: starting at $04 reads
    // OAM[4] = $FF as the Y, which is off-screen, and nothing else is in range.
    // A build that aligns $06 down to $04 fails the assertion above and passes
    // this one, so the pair distinguishes the two implementations rather than
    // merely observing that something happened.
    EXPECT_FALSE(hit_on_line_100_after_writing_oamaddr(ppu, 10, 0x04))
        << "an aligned start at $04 reads $FF as the Y, so nothing is in range";
}

GTEST_TEST(testSprite0Hit, oamaddr_written_after_evaluation_starts_is_too_late)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up_an_overlapping_frame(ppu, 64, 64);
    park_the_hitting_sprite_in_record_one(ppu);

    // Evaluation latches OAMADDR at dot 65. A write after that cannot move
    // which sprite is evaluated for the line that follows.
    EXPECT_FALSE(hit_on_line_100_after_writing_oamaddr(ppu, 100, 0x04))
        << "OAMADDR is latched when evaluation begins at dot 65, not read at the end of it";
}

GTEST_TEST(testSprite0Hit, oamaddr_written_during_vblank_is_cleared_before_line_0)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up_an_overlapping_frame(ppu, 64, 64);
    park_the_hitting_sprite_in_record_one(ppu);

    // Hardware holds OAMADDR at 0 across dots 257-320 of every rendering
    // scanline, and the pre-render line is one of them. So a value written
    // during vblank - the natural place to write it - is gone before line 0 is
    // ever evaluated. This is why sprite 0 is OAM[0] in practice, and why the
    // "write 0 to $2003 before a DMA" advice is about DMA rather than about
    // sprite 0 selection.
    clock_until(ppu, PPU::vblank_start_scanline, 10);
    ppu.write(PPU::OAMADDR, 0x04);

    clock_until(ppu, PPU::pre_render_scanline, 1);
    ppu.clock();
    ASSERT_EQ(0x00, ppu.registers.PPUSTATUS & 0x40);

    clock_until(ppu, 110, 0);
    EXPECT_EQ(0x00, ppu.registers.PPUSTATUS & 0x40)
        << "the pre-render line clears OAMADDR, so a vblank write cannot select a sprite";
    EXPECT_EQ(0x00, ppu.registers.OAMADDR) << "OAMADDR must have been cleared during rendering";
}

GTEST_TEST(testSprite0Hit, oam_written_after_the_window_does_not_reach_that_line)
{
    Bus console;
    PPU& ppu = console.ppu;

    // Sprite 0 parked well below the line under test, so nothing can hit yet.
    set_up_an_overlapping_frame(ppu, 64, 200);

    clock_until(ppu, PPU::pre_render_scanline, 1);
    ppu.clock();  // clears the flag
    ASSERT_EQ(0x00, ppu.registers.PPUSTATUS & 0x40);

    // Past dot 257 of line 99: line 100 has already been evaluated.
    clock_until(ppu, 99, 258);

    // Now move sprite 0 onto lines 100-107. Line 100 must not see it, because
    // its evaluation window has closed.
    ppu.OAM_memory[0] = 99;
    ppu.OAM_memory[3] = 64;

    clock_until(ppu, 101, 0);
    EXPECT_EQ(0x00, ppu.registers.PPUSTATUS & 0x40)
        << "OAM changed after line 100's evaluation window, so line 100 cannot hit";

    // Line 101 was evaluated at line 100's dot 257, with the new OAM in place,
    // so it does hit - which is what makes the check above non-vacuous.
    clock_until(ppu, 108, 0);
    EXPECT_NE(0x00, ppu.registers.PPUSTATUS & 0x40)
        << "the following lines were evaluated with the new OAM and must hit";
}

GTEST_TEST(testSprite0Hit, a_line_whose_evaluation_window_was_skipped_cannot_hit)
{
    Bus console;
    PPU& ppu = console.ppu;

    // Sprite 0 covers lines 100-107, with rendering on to begin with.
    set_up_an_overlapping_frame(ppu, 64, 99);

    clock_until(ppu, PPU::pre_render_scanline, 1);
    ppu.clock();
    ASSERT_EQ(0x00, ppu.registers.PPUSTATUS & 0x40);

    // Part-way down the sprite, so the output units are live and hold sprite 0.
    clock_until(ppu, 101, 100);
    ASSERT_EQ(1u, ppu.sprite_count) << "setup: sprite 0 should be in the units on line 101";
    ASSERT_TRUE(ppu.sprite_zero_in_units) << "setup: unit 0 should be OAM sprite 0";

    // Turn rendering off before line 102's evaluation window closes, so that
    // window passes with rendering disabled and hardware builds no secondary
    // OAM for line 102.
    ppu.write(PPU::PPUMASK, 0x00);

    // Clear the hit already recorded for lines 100-101 by hand - mid-frame
    // there is no other way, and reading $2002 would also clear vblank and
    // disturb the rest of the frame.
    ppu.registers.PPUSTATUS = static_cast<uint8_t>(ppu.registers.PPUSTATUS & ~0x40);

    // Rendering comes back part-way through line 102. Its evaluation never
    // happened, so it must not hit however much of it is drawn - the stale
    // "present" from line 101 must not carry over.
    clock_until(ppu, 102, 40);
    ppu.write(PPU::PPUMASK, kEverything);

    clock_until(ppu, 103, 0);
    EXPECT_EQ(0x00, ppu.registers.PPUSTATUS & 0x40)
        << "line 102's evaluation window passed with rendering off, so it has no sprites";

    // Line 103 onwards was evaluated with rendering on again, so normal service
    // resumes - which is what stops the check above passing for the wrong
    // reason.
    clock_until(ppu, 106, 0);
    EXPECT_NE(0x00, ppu.registers.PPUSTATUS & 0x40) << "lines evaluated after rendering came back must hit normally";
}

}  // namespace sprite0
}  // namespace tests
