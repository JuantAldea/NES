// Sprite rendering, rule by rule: secondary OAM, the eight-per-line limit, the
// overflow flag, flipping, and the sprite/background priority multiplexer.
//
// Blargg's five sprite_overflow ROMs and eleven sprite_hit ROMs are the real
// oracle. These tests exist so that a broken rule says WHICH rule instead of
// "FAILED: #7", and so that the parts no ROM covers - sprite pixels actually
// reaching the framebuffer, sprite-versus-sprite priority - are covered at all.
//
// The framebuffer holds a palette INDEX, so each test picks colours that make
// "which layer won" readable directly: background colour 1 is $01, sprite
// palette 0 colour 1 is $21, sprite palette 1 colour 1 is $25, and the backdrop
// is $0F. A pixel's value therefore names its source.
#include <cstdint>

#include "../include/bus.h"
#include "gtest/gtest.h"

namespace tests
{
namespace sprites
{
namespace
{

constexpr uint8_t kShowBackgroundLeft = 0x02;
constexpr uint8_t kShowSpritesLeft = 0x04;
constexpr uint8_t kShowBackground = 0x08;
constexpr uint8_t kShowSprites = 0x10;
constexpr uint8_t kEverything = kShowBackgroundLeft | kShowSpritesLeft | kShowBackground | kShowSprites;

// Palette indices chosen so that a framebuffer byte names the layer that
// produced it.
constexpr uint8_t kBackdrop = 0x0F;
constexpr uint8_t kBackgroundColour = 0x01;
constexpr uint8_t kSpriteColour = 0x21;
constexpr uint8_t kSpriteColourPalette1 = 0x25;

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

// An 8x8 tile whose low bitplane is `rows` and whose high bitplane is zero, so
// a set bit is colour 1 and a clear bit colour 0 (transparent).
void write_tile(PPU& ppu, uint16_t table, uint8_t index, const uint8_t rows[8])
{
    for (uint16_t row = 0; row < 8; ++row) {
        ppu.ppu_bus_write(static_cast<uint16_t>(table + index * 16 + row), rows[row]);
        ppu.ppu_bus_write(static_cast<uint16_t>(table + index * 16 + 8 + row), 0x00);
    }
}

constexpr uint8_t kSolid[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr uint8_t kEmpty[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
// Bit 7 is the leftmost pixel of a row, so this is the left column only.
constexpr uint8_t kLeftColumnOnly[8] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
constexpr uint8_t kTopRowOnly[8] = {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

void write_palettes(PPU& ppu)
{
    ppu.ppu_bus_write(0x3F00, kBackdrop);
    ppu.ppu_bus_write(0x3F01, kBackgroundColour);
    ppu.ppu_bus_write(0x3F11, kSpriteColour);
    ppu.ppu_bus_write(0x3F15, kSpriteColourPalette1);
}

void fill_background(PPU& ppu, uint8_t tile)
{
    for (uint16_t i = 0; i < 32 * 30; ++i) {
        ppu.ppu_bus_write(static_cast<uint16_t>(0x2000 + i), tile);
    }
}

void place_sprite(PPU& ppu, int index, uint8_t y, uint8_t tile, uint8_t attributes, uint8_t x)
{
    ppu.OAM_memory[index * 4 + 0] = y;
    ppu.OAM_memory[index * 4 + 1] = tile;
    ppu.OAM_memory[index * 4 + 2] = attributes;
    ppu.OAM_memory[index * 4 + 3] = x;
}

// Parks every sprite off-screen. $FF is the Y hardware itself treats as
// off-screen, so this is the state secondary OAM is cleared to.
void park_all_sprites(PPU& ppu)
{
    for (int i = 0; i < 256; ++i) {
        ppu.OAM_memory[i] = 0xFF;
    }
}

// Tile 0 transparent, tile 1 solid, in pattern table $0000.
void write_the_standard_tiles(PPU& ppu)
{
    write_tile(ppu, 0x0000, 0, kEmpty);
    write_tile(ppu, 0x0000, 1, kSolid);
}

void set_up(PPU& ppu, uint8_t background_tile = 0, uint8_t ctrl = 0x00)
{
    run_past_reset_lockout(ppu);
    write_palettes(ppu);
    write_the_standard_tiles(ppu);
    fill_background(ppu, background_tile);
    park_all_sprites(ppu);

    ppu.write(PPU::PPUCTRL, ctrl);
    ppu.write(PPU::PPUMASK, kEverything);
    ppu.write(PPU::PPUSCROLL, 0x00);
    ppu.write(PPU::PPUSCROLL, 0x00);
}

// Renders one whole frame, from the pre-render line to the post-render line.
void render_a_frame(PPU& ppu)
{
    clock_until(ppu, PPU::pre_render_scanline, 1);
    clock_until(ppu, PPU::post_render_scanline, 0);
}

uint8_t pixel_at(PPU& ppu, int x, int y)
{
    return ppu.framebuffer[y * PPU::screen_width + x];
}

}  // namespace

// --- sprite pixels reach the framebuffer at all ---------------------------

GTEST_TEST(testSprites, an_opaque_sprite_pixel_is_drawn_over_the_backdrop)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu);
    place_sprite(ppu, 0, 100, 1, 0x00, 64);

    render_a_frame(ppu);

    // The sprite covers lines 101-108 and columns 64-71.
    EXPECT_EQ(kSpriteColour, pixel_at(ppu, 64, 101)) << "top-left pixel of the sprite";
    EXPECT_EQ(kSpriteColour, pixel_at(ppu, 71, 108)) << "bottom-right pixel of the sprite";
    EXPECT_EQ(kBackdrop, pixel_at(ppu, 72, 101)) << "one column past the sprite";
    EXPECT_EQ(kBackdrop, pixel_at(ppu, 64, 100)) << "the line above: OAM Y is one less";
    EXPECT_EQ(kBackdrop, pixel_at(ppu, 64, 109)) << "one line past the sprite";
}

GTEST_TEST(testSprites, sprites_are_hidden_by_the_ppumask_left_clip)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu);

    // Straddling the clip boundary at x=4, NOT parked entirely inside it.
    // With the sprite wholly within x=0..7 both halves of this test reduce to
    // "nothing was drawn", which a sprite that failed to render for any
    // unrelated reason satisfies just as well. Straddling gives the test its
    // own positive control: the same sprite must vanish on one side of x=8 and
    // appear on the other, so only a correctly placed clip passes.
    place_sprite(ppu, 0, 100, 1, 0x00, 4);

    ppu.write(PPU::PPUMASK, kShowBackground | kShowSprites | kShowBackgroundLeft);
    render_a_frame(ppu);

    EXPECT_EQ(kBackdrop, pixel_at(ppu, 4, 101)) << "PPUMASK bit 2 is clear, so x=0..7 is blanked";
    EXPECT_EQ(kBackdrop, pixel_at(ppu, 7, 101)) << "x=7 is the last blanked column";
    EXPECT_EQ(kSpriteColour, pixel_at(ppu, 8, 101)) << "x=8 is the first visible column - the sprite IS there";
    EXPECT_EQ(kSpriteColour, pixel_at(ppu, 11, 101)) << "and continues to the end of the sprite";
}

// $2004 is wired to secondary OAM while it is being cleared, not to primary
// OAM. NESdev, PPU sprite evaluation, cycles 1-64: "Secondary OAM ... is
// initialized to $FF - attempting to read $2004 will return $FF."
GTEST_TEST(testSprites, reading_oamdata_during_the_secondary_oam_clear_returns_ff)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu);

    for (int i = 0; i < 256; ++i) {
        ppu.OAM_memory[i] = static_cast<uint8_t>(i);
    }
    ppu.write(PPU::PPUMASK, kEverything);

    clock_until(ppu, 100, 30);  // inside dots 1-64 of a visible line
    ppu.write(PPU::OAMADDR, 0x20);
    EXPECT_EQ(0xFF, ppu.read(PPU::OAMDATA)) << "during the clear, $2004 reads secondary OAM, not OAM[$20]";

    // The control: outside that window the same read returns primary OAM, so
    // this is not simply "$2004 always reads $FF".
    clock_until(ppu, 100, 100);
    ppu.write(PPU::OAMADDR, 0x20);
    EXPECT_EQ(0x20, ppu.read(PPU::OAMDATA)) << "past dot 64 the read reaches primary OAM again";
}

GTEST_TEST(testSprites, reading_oamdata_with_rendering_off_always_reaches_primary_oam)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu);

    for (int i = 0; i < 256; ++i) {
        ppu.OAM_memory[i] = static_cast<uint8_t>(i);
    }
    ppu.write(PPU::PPUMASK, 0x00);  // rendering disabled: no evaluation happens

    clock_until(ppu, 100, 30);
    ppu.write(PPU::OAMADDR, 0x20);
    EXPECT_EQ(0x20, ppu.read(PPU::OAMDATA)) << "with rendering off there is no clear to read through";
}

// --- the eight-sprites-per-line limit -------------------------------------

GTEST_TEST(testSprites, only_the_first_eight_sprites_on_a_line_are_drawn)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu);

    // Nine sprites on the same line, sixteen pixels apart so each has its own
    // column to be checked in.
    for (int i = 0; i < 9; ++i) {
        place_sprite(ppu, i, 100, 1, 0x00, static_cast<uint8_t>(i * 16));
    }

    clock_until(ppu, PPU::pre_render_scanline, 1);
    clock_until(ppu, 101, 100);

    // Secondary OAM holds eight sprites and no more. Asserted directly as well
    // as through the pixels, because a cap that is broken by more than one
    // sprite would run off the end of the eight output units.
    EXPECT_EQ(8u, ppu.sprite_count) << "secondary OAM holds at most eight sprites";

    clock_until(ppu, PPU::post_render_scanline, 0);

    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(kSpriteColour, pixel_at(ppu, i * 16, 101))
            << "sprite " << i << " is within the first eight and must be drawn";
    }
    EXPECT_EQ(kBackdrop, pixel_at(ppu, 8 * 16, 101))
        << "the ninth sprite on the line is dropped, not drawn";
}

GTEST_TEST(testSprites, the_ninth_sprite_on_a_line_sets_the_overflow_flag)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu);

    for (int i = 0; i < 9; ++i) {
        place_sprite(ppu, i, 100, 1, 0x00, static_cast<uint8_t>(i * 16));
    }

    clock_until(ppu, PPU::pre_render_scanline, 1);
    clock_until(ppu, 101, 200);

    EXPECT_NE(0x00, ppu.registers.PPUSTATUS & 0x20) << "nine sprites on a line set PPUSTATUS bit 5";
}

GTEST_TEST(testSprites, eight_sprites_on_a_line_do_not_set_the_overflow_flag)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu);

    // Exactly eight. This is the control that stops the test above passing for
    // an implementation that simply sets the flag whenever sprites exist.
    for (int i = 0; i < 8; ++i) {
        place_sprite(ppu, i, 100, 1, 0x00, static_cast<uint8_t>(i * 16));
    }

    clock_until(ppu, PPU::pre_render_scanline, 1);
    clock_until(ppu, 101, 200);

    EXPECT_EQ(0x00, ppu.registers.PPUSTATUS & 0x20) << "eight is the limit, not an overflow";
}

// --- the overflow search bug ----------------------------------------------

// Once eight sprites have been found, the byte index advances ALONGSIDE the
// sprite index instead of staying at 0, so sprite n has byte (n mod 4) misread
// as its Y coordinate. This is hardware's bug and has to be reproduced as one:
// blargg's 4.Obscure tests it and nothing else.
GTEST_TEST(testSprites, the_overflow_search_misreads_a_later_sprites_byte_as_its_y)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu);

    // Eight genuinely in-range sprites fill secondary OAM.
    for (int i = 0; i < 8; ++i) {
        place_sprite(ppu, i, 100, 1, 0x00, static_cast<uint8_t>(i * 16));
    }

    // Sprite 8 is off-screen, so the scan moves on with m stepped to 1.
    place_sprite(ppu, 8, 0xFF, 0xFF, 0xFF, 0xFF);

    // Sprite 9 is ALSO off-screen by its real Y, but its second byte - the tile
    // index - holds 100, which is in range for line 101. Only the buggy scan
    // reads that byte as a Y, so only the buggy scan sets the flag.
    place_sprite(ppu, 9, 0xFF, 100, 0xFF, 0xFF);

    clock_until(ppu, PPU::pre_render_scanline, 1);
    clock_until(ppu, 101, 200);

    EXPECT_NE(0x00, ppu.registers.PPUSTATUS & 0x20)
        << "sprite 9's SECOND byte must be misread as its Y once eight sprites are found; "
           "advancing only the sprite index leaves this clear";
}

GTEST_TEST(testSprites, the_overflow_search_does_not_fire_when_no_misread_byte_is_in_range)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu);

    for (int i = 0; i < 8; ++i) {
        place_sprite(ppu, i, 100, 1, 0x00, static_cast<uint8_t>(i * 16));
    }

    // Every remaining sprite is $FF in every byte, so no byte the buggy scan
    // could reach is ever in range. Without this control the test above would
    // pass for an implementation that sets the flag on any 9th sprite record
    // regardless of what it read.
    for (int i = 8; i < 64; ++i) {
        place_sprite(ppu, i, 0xFF, 0xFF, 0xFF, 0xFF);
    }

    clock_until(ppu, PPU::pre_render_scanline, 1);
    clock_until(ppu, 101, 200);

    EXPECT_EQ(0x00, ppu.registers.PPUSTATUS & 0x20)
        << "no byte reachable by the search is in range, so nothing should set the flag";
}

// --- flipping --------------------------------------------------------------

GTEST_TEST(testSprites, horizontal_flip_mirrors_the_row)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu);
    write_tile(ppu, 0x0000, 2, kLeftColumnOnly);

    place_sprite(ppu, 0, 100, 2, 0x00, 64);          // unflipped
    place_sprite(ppu, 1, 120, 2, 0x40, 64);          // horizontally flipped

    render_a_frame(ppu);

    EXPECT_EQ(kSpriteColour, pixel_at(ppu, 64, 101)) << "unflipped: leftmost column is opaque";
    EXPECT_EQ(kBackdrop, pixel_at(ppu, 71, 101)) << "unflipped: rightmost column is clear";

    EXPECT_EQ(kBackdrop, pixel_at(ppu, 64, 121)) << "flipped: leftmost column is now clear";
    EXPECT_EQ(kSpriteColour, pixel_at(ppu, 71, 121)) << "flipped: rightmost column is now opaque";
}

GTEST_TEST(testSprites, vertical_flip_mirrors_the_row_within_an_8x8_sprite)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu);
    write_tile(ppu, 0x0000, 2, kTopRowOnly);

    place_sprite(ppu, 0, 100, 2, 0x00, 64);          // unflipped
    place_sprite(ppu, 1, 120, 2, 0x80, 64);          // vertically flipped

    render_a_frame(ppu);

    EXPECT_EQ(kSpriteColour, pixel_at(ppu, 64, 101)) << "unflipped: the top row is opaque";
    EXPECT_EQ(kBackdrop, pixel_at(ppu, 64, 108)) << "unflipped: the bottom row is clear";

    EXPECT_EQ(kBackdrop, pixel_at(ppu, 64, 121)) << "flipped: the top row is now clear";
    EXPECT_EQ(kSpriteColour, pixel_at(ppu, 64, 128)) << "flipped: the bottom row is now opaque";
}

// The 8x16 case is a separate test because it is the one that pins down HOW the
// flip is done. Vertical flip mirrors within the whole sixteen-line sprite, so
// it swaps the two tiles as well as the rows inside them - which only falls out
// correctly if the row index is flipped BEFORE it is split into "which tile"
// and "which row of that tile".
//
// In 8x8 a height of 8 makes `height - 1 - row` and a hardcoded `7 - row`
// identical, so an 8x8 test alone cannot tell the two apart. Here it can.
GTEST_TEST(testSprites, vertical_flip_swaps_the_halves_of_an_8x16_sprite)
{
    Bus console;
    PPU& ppu = console.ppu;
    // Background tile 4, not the usual tile 0: this test needs tile 0 for the
    // sprite's top half and makes it SOLID, so leaving the nametable pointing
    // at tile 0 would fill the screen with it and every "transparent here"
    // check would read the background instead of the backdrop.
    set_up(ppu, 4, 0x20);  // PPUCTRL bit 5: 8x16 sprites
    write_tile(ppu, 0x0000, 4, kEmpty);

    // Tile $00 in 8x16 mode means tiles 0 and 1 of table $0000: solid top half,
    // empty bottom half.
    write_tile(ppu, 0x0000, 0, kSolid);
    write_tile(ppu, 0x0000, 1, kEmpty);

    place_sprite(ppu, 0, 100, 0x00, 0x00, 64);       // unflipped
    place_sprite(ppu, 1, 140, 0x00, 0x80, 64);       // vertically flipped

    render_a_frame(ppu);

    // Unflipped: lines 101-108 are the solid tile, 109-116 the empty one.
    EXPECT_EQ(kSpriteColour, pixel_at(ppu, 64, 101)) << "unflipped: top half is solid";
    EXPECT_EQ(kSpriteColour, pixel_at(ppu, 64, 108)) << "unflipped: last line of the top half";
    EXPECT_EQ(kBackdrop, pixel_at(ppu, 64, 109)) << "unflipped: bottom half is empty";
    EXPECT_EQ(kBackdrop, pixel_at(ppu, 64, 116)) << "unflipped: last line of the bottom half";

    // Flipped: the solid tile must move to the BOTTOM eight lines, 149-156.
    EXPECT_EQ(kBackdrop, pixel_at(ppu, 64, 141)) << "flipped: the empty half is now on top";
    EXPECT_EQ(kBackdrop, pixel_at(ppu, 64, 148)) << "flipped: last line of the empty half";
    EXPECT_EQ(kSpriteColour, pixel_at(ppu, 64, 149)) << "flipped: the solid half moved down";
    EXPECT_EQ(kSpriteColour, pixel_at(ppu, 64, 156)) << "flipped: last line of the solid half";
}

// --- priority --------------------------------------------------------------

GTEST_TEST(testSprites, a_front_priority_sprite_covers_an_opaque_background)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu, 1);  // solid background everywhere
    place_sprite(ppu, 0, 100, 1, 0x00, 64);

    render_a_frame(ppu);

    EXPECT_EQ(kSpriteColour, pixel_at(ppu, 64, 101)) << "attribute bit 5 clear: sprite in front";
    EXPECT_EQ(kBackgroundColour, pixel_at(ppu, 80, 101)) << "away from the sprite, background shows";
}

GTEST_TEST(testSprites, a_back_priority_sprite_is_hidden_by_an_opaque_background)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu, 1);  // solid background everywhere
    place_sprite(ppu, 0, 100, 1, 0x20, 64);  // attribute bit 5: behind background

    render_a_frame(ppu);

    EXPECT_EQ(kBackgroundColour, pixel_at(ppu, 64, 101))
        << "attribute bit 5 set: an opaque background pixel wins";
}

// The case that stops "behind background" being implemented as "never drawn".
// A back-priority sprite is still drawn wherever the background is transparent,
// which is the whole point of the bit.
GTEST_TEST(testSprites, a_back_priority_sprite_still_shows_through_a_transparent_background)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu, 0);  // transparent background everywhere
    place_sprite(ppu, 0, 100, 1, 0x20, 64);

    render_a_frame(ppu);

    EXPECT_EQ(kSpriteColour, pixel_at(ppu, 64, 101))
        << "the background is transparent here, so even a back-priority sprite is drawn";
}

// --- sprite versus sprite --------------------------------------------------

// Priority between sprites is not the attribute bit at all: it is OAM order.
// Secondary OAM is filled by scanning primary OAM upwards, so the lowest OAM
// index with an opaque pixel wins, whatever either sprite's priority bit says.
GTEST_TEST(testSprites, the_lower_oam_index_wins_between_two_overlapping_sprites)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu);

    // Both solid, both at the same place; sprite 1 uses palette 1 so the winner
    // is identifiable by colour.
    place_sprite(ppu, 0, 100, 1, 0x00, 64);
    place_sprite(ppu, 1, 100, 1, 0x01, 64);

    render_a_frame(ppu);

    EXPECT_EQ(kSpriteColour, pixel_at(ppu, 64, 101)) << "sprite 0 is in front of sprite 1";
}

GTEST_TEST(testSprites, a_lower_index_back_priority_sprite_still_beats_a_higher_index_one)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu);

    // Sprite 0 is behind the background, sprite 1 in front of it. Over a
    // TRANSPARENT background neither is occluded by it, so OAM order alone
    // decides - and sprite 0 still wins despite the "lower" priority bit.
    place_sprite(ppu, 0, 100, 1, 0x20, 64);
    place_sprite(ppu, 1, 100, 1, 0x01, 64);

    render_a_frame(ppu);

    EXPECT_EQ(kSpriteColour, pixel_at(ppu, 64, 101))
        << "the priority bit orders a sprite against the BACKGROUND, never against another sprite";
}

// --- secondary OAM --------------------------------------------------------

GTEST_TEST(testSprites, secondary_oam_is_cleared_to_ff_before_evaluation)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu);
    place_sprite(ppu, 0, 100, 1, 0x00, 64);

    clock_until(ppu, PPU::pre_render_scanline, 1);

    // Dots 1-64 write $FF into all 32 bytes; by dot 65 the clear is complete
    // and evaluation has not yet written anything.
    clock_until(ppu, 50, 65);
    for (int i = 0; i < 32; ++i) {
        EXPECT_EQ(0xFF, ppu.secondary_oam[i]) << "secondary OAM byte " << i << " at dot 65";
    }
}

GTEST_TEST(testSprites, an_out_of_range_sprite_never_reaches_the_units)
{
    Bus console;
    PPU& ppu = console.ppu;
    set_up(ppu);
    place_sprite(ppu, 0, 200, 1, 0x00, 64);

    clock_until(ppu, PPU::pre_render_scanline, 1);
    clock_until(ppu, 101, 100);

    EXPECT_EQ(0u, ppu.sprite_count) << "the only sprite is 100 lines below this one";
}

}  // namespace sprites
}  // namespace tests
