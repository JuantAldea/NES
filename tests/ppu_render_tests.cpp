// The background pipeline, checked against a naive reference renderer.
//
// The reference below recomputes every pixel of a settled frame from first
// principles - absolute scroll position -> nametable -> tile -> attribute
// quadrant -> pattern bitplanes -> palette entry - without touching the
// pipeline's shift registers, latches, fine-X tap or v/t increments. The only
// thing it shares with the implementation is ppu_bus_read, which is the PPU's
// memory decode (mirroring, palette aliasing) rather than any part of the
// pipeline; duplicating THAT would be testing the wrong thing, and would let a
// mirroring change pass unnoticed on both sides.
//
// The pipeline is a per-dot state machine and the reference is a
// per-pixel formula. They agree only if the state machine is right.
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../include/bus.h"
#include "../include/frame_dump.h"
#include "gtest/gtest.h"

namespace tests
{
namespace render
{
namespace
{

constexpr int kWidth = 256;
constexpr int kHeight = 240;

void run_past_reset_lockout(PPU& ppu)
{
    while (ppu.in_reset_write_lockout()) {
        ppu.clock();
    }
}

// Clocks whole frames. The framebuffer is complete the moment the frame counter
// moves on, so this leaves the just-finished frame in place.
void run_frames(PPU& ppu, int frames)
{
    for (int i = 0; i < frames; ++i) {
        const uint64_t target = ppu.frame + 1;
        // A frame is 341 x 262 dots, one fewer on a skipped odd frame; the
        // bound is a runaway guard, not a measurement.
        for (uint64_t guard = 0; guard < 2ull * 341 * 262 && ppu.frame != target; ++guard) {
            ppu.clock();
        }
        ASSERT_EQ(target, ppu.frame) << "the frame counter did not advance";
    }
}

// A deterministic filling of CHR-RAM, the nametables and the palette, so that
// every tile, every attribute quadrant and every palette entry is distinct.
// A constant fill would let a pipeline that reads the wrong address pass.
//
// The generator is a plain LCG written out here rather than <random>, so the
// contents are fixed forever and a failure is reproducible from the seed.
uint8_t next_byte(uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return static_cast<uint8_t>(state >> 24);
}

void fill_memory(PPU& ppu, uint32_t seed)
{
    uint32_t state = seed;

    // CHR-RAM: both pattern tables.
    for (uint16_t addr = 0x0000; addr < 0x2000; ++addr) {
        ppu.ppu_bus_write(addr, next_byte(state));
    }

    // Nametables and their attribute tables. Written across the whole $2000
    // -$2FFF window; with only 2KB of RAM behind it the cartridge's mirroring
    // folds this down, which is exactly what the reference reads back through.
    for (uint16_t addr = 0x2000; addr < 0x3000; ++addr) {
        ppu.ppu_bus_write(addr, next_byte(state));
    }

    // Palette: 32 distinct entries, $01..$20, so a pixel that picks the wrong
    // entry cannot accidentally match. Entry $00 is the backdrop.
    for (uint16_t i = 0; i < 32; ++i) {
        ppu.ppu_bus_write(static_cast<uint16_t>(0x3F00 + i), static_cast<uint8_t>(i + 1));
    }
}

struct Config {
    uint8_t ppuctrl;  // nametable select (bits 0-1) and bg pattern table (bit 4)
    uint8_t scroll_x;
    uint8_t scroll_y;
    bool show_background = true;
    bool show_left_eight = true;
    bool greyscale = false;
};

uint8_t mask_byte(const Config& config)
{
    uint8_t mask = 0;
    if (config.greyscale) {
        mask |= 0x01;  // bit 0
    }
    if (config.show_left_eight) {
        mask |= 0x02;  // bit 1: show background in the leftmost 8 pixels
    }
    if (config.show_background) {
        mask |= 0x08;  // bit 3
    }
    return mask;
}

// Sets the PPU up and renders two frames: the first settles v from t, the
// second is the one under test.
void render_frame(PPU& ppu, const Config& config)
{
    run_past_reset_lockout(ppu);
    fill_memory(ppu, 0x1234567u);

    ppu.write(PPU::PPUCTRL, config.ppuctrl);
    ppu.write(PPU::PPUMASK, mask_byte(config));
    ppu.write(PPU::PPUSCROLL, config.scroll_x);
    ppu.write(PPU::PPUSCROLL, config.scroll_y);

    run_frames(ppu, 2);
}

// --- the reference renderer ------------------------------------------------
//
// Independent of the pipeline: no shift registers, no v, no dots. Given a
// screen pixel it works out which nametable, tile, attribute quadrant and
// bitplane bit it comes from, arithmetically.
uint8_t reference_pixel(PPU& ppu, const Config& config, int x, int y)
{
    const uint8_t backdrop = ppu.ppu_bus_read(0x3F00);

    if (!config.show_background || (x < 8 && !config.show_left_eight)) {
        return static_cast<uint8_t>(backdrop & (config.greyscale ? 0x30 : 0x3F));
    }

    // Where this pixel sits in the 512x480 space the four nametables cover.
    // A nametable is 256 pixels wide and 240 tall, so the scroll plus the
    // screen position picks both the nametable and the position within it.
    const int base_x = ((config.ppuctrl & 0x01) ? 256 : 0) + config.scroll_x;
    const int base_y = ((config.ppuctrl & 0x02) ? 240 : 0) + config.scroll_y;

    const int absolute_x = base_x + x;
    const int absolute_y = base_y + y;

    const int nametable_x = (absolute_x / 256) % 2;
    const int nametable_y = (absolute_y / 240) % 2;
    const uint16_t nametable = static_cast<uint16_t>(0x2000 + (nametable_y * 2 + nametable_x) * 0x400);

    const int within_x = absolute_x % 256;
    const int within_y = absolute_y % 240;

    const int tile_column = within_x / 8;
    const int tile_row = within_y / 8;
    const int pixel_column = within_x % 8;
    const int pixel_row = within_y % 8;

    const uint8_t tile = ppu.ppu_bus_read(static_cast<uint16_t>(nametable + tile_row * 32 + tile_column));

    // The attribute table is the last 64 bytes of the nametable: one byte per
    // 4x4 tiles, two bits per 2x2 tiles, packed from the low bits up as
    // top-left, top-right, bottom-left, bottom-right.
    const uint8_t attribute =
        ppu.ppu_bus_read(static_cast<uint16_t>(nametable + 0x3C0 + (tile_row / 4) * 8 + (tile_column / 4)));
    const int quadrant_shift = ((tile_row % 4) / 2) * 4 + ((tile_column % 4) / 2) * 2;
    const uint8_t palette = static_cast<uint8_t>((attribute >> quadrant_shift) & 0x03);

    // A tile is 16 bytes: eight rows of the low bitplane, then eight of the
    // high one.
    const uint16_t pattern = static_cast<uint16_t>(((config.ppuctrl & 0x10) ? 0x1000 : 0x0000) + tile * 16 + pixel_row);
    const uint8_t low = ppu.ppu_bus_read(pattern);
    const uint8_t high = ppu.ppu_bus_read(static_cast<uint16_t>(pattern + 8));

    // Bit 7 of each plane is the LEFTMOST pixel.
    const int bit = 7 - pixel_column;
    const uint8_t value = static_cast<uint8_t>(((low >> bit) & 0x01) | (((high >> bit) & 0x01) << 1));

    const uint8_t colour =
        value == 0 ? backdrop : ppu.ppu_bus_read(static_cast<uint16_t>(0x3F00 + palette * 4 + value));

    return static_cast<uint8_t>(colour & (config.greyscale ? 0x30 : 0x3F));
}

// A filename-safe label for the running test, so that tests dumping frames in
// parallel cannot overwrite each other's evidence.
std::string test_label()
{
    const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string label = info != nullptr ? std::string(info->name()) : std::string("unknown");
    for (char& c : label) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            c = '_';
        }
    }
    return label;
}

// Writes what the pipeline drew and what the reference expected, side by side
// on disk, so a failure can be LOOKED AT.
//
// Nothing displays the framebuffer yet, so without this a render bug is only
// ever a coordinate and two hex bytes. "First mismatch at (17, 43)" does not
// distinguish a one-pixel fine-X error from a completely scrambled screen, and
// those need entirely different investigations.
void dump_frames_for_inspection(PPU& ppu, const Config& config, const std::string& label)
{
    // uint16_t to match the framebuffer's index+emphasis encoding; the
    // reference is emphasis-free, so the high bits stay zero.
    std::vector<uint16_t> expected(static_cast<size_t>(kWidth) * kHeight);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            expected[static_cast<size_t>(y) * kWidth + x] = reference_pixel(ppu, config, x, y);
        }
    }

    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::string actual_path = (dir / ("nes_render_" + label + "_actual.ppm")).string();
    const std::string expected_path = (dir / ("nes_render_" + label + "_expected.ppm")).string();

    const bool wrote_actual = nes::write_ppm(actual_path, ppu.framebuffer, kWidth, kHeight, PPU::nes_palette, 64);
    const bool wrote_expected = nes::write_ppm(expected_path, expected.data(), kWidth, kHeight, PPU::nes_palette, 64);

    if (wrote_actual && wrote_expected) {
        ADD_FAILURE() << "wrote the two frames for inspection:\n"
                      << "  actual:   " << actual_path << "\n"
                      << "  expected: " << expected_path;
    } else {
        ADD_FAILURE() << "could not write the frame dumps to " << dir.string();
    }
}

// Compares the whole frame, reporting the first disagreement only - a broken
// pipeline disagrees about tens of thousands of pixels and printing them all
// buries the one that matters.
void expect_frame_matches_reference(PPU& ppu, const Config& config, const std::string& label)
{
    int mismatches = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const uint8_t expected = reference_pixel(ppu, config, x, y);
            const uint8_t actual = ppu.framebuffer[y * kWidth + x];
            if (expected != actual) {
                ++mismatches;
                if (mismatches == 1) {
                    ADD_FAILURE() << "first mismatch at (" << x << ", " << y << "): expected $" << std::hex
                                  << (unsigned)expected << ", got $" << (unsigned)actual << std::dec << "  [scroll "
                                  << (unsigned)config.scroll_x << "," << (unsigned)config.scroll_y << " ctrl $"
                                  << std::hex << (unsigned)config.ppuctrl << std::dec << "]";
                }
            }
        }
    }

    if (mismatches != 0) {
        dump_frames_for_inspection(ppu, config, label);
    }

    EXPECT_EQ(0, mismatches) << mismatches << " of " << (kWidth * kHeight) << " pixels disagree";
}

}  // namespace

// --- whole-frame agreement -------------------------------------------------

GTEST_TEST(testPPURender, background_matches_the_reference_renderer_unscrolled)
{
    Bus console;
    // Pattern table $1000, nametable 0, no scroll: the simplest case, so that a
    // failure here is about the fetch pattern rather than about scrolling.
    const Config config{0x10, 0, 0};
    render_frame(console.ppu, config);
    expect_frame_matches_reference(console.ppu, config, test_label());
}

GTEST_TEST(testPPURender, background_matches_the_reference_renderer_when_scrolled)
{
    Bus console;
    // X = 93 -> coarse X 11, fine X 5. 93 + 255 = 348, so the line runs off the
    // right of the nametable and the coarse X wrap into the next one is
    // exercised on every scanline.
    //
    // Y = 154 -> coarse Y 19, fine Y 2. 154 + 239 = 393, so the frame crosses
    // row 240 and the coarse Y 29 -> 0 wrap with its nametable flip is
    // exercised too.
    const Config config{0x10, 93, 154};
    render_frame(console.ppu, config);
    expect_frame_matches_reference(console.ppu, config, test_label());
}

GTEST_TEST(testPPURender, background_matches_the_reference_renderer_from_the_far_nametable)
{
    Bus console;
    // $2000 bits 0-1 = 3: start in the bottom-right nametable, and scroll so
    // that the frame wraps back through all four.
    const Config config{0x13, 200, 200};
    render_frame(console.ppu, config);
    expect_frame_matches_reference(console.ppu, config, test_label());
}

GTEST_TEST(testPPURender, every_fine_x_matches_the_reference_renderer)
{
    // Fine X taps the shift registers 0-7 pixels along. Each value is a
    // different tap, and a pipeline that ignores it agrees with the reference
    // only at 0.
    for (int fine = 0; fine < 8; ++fine) {
        Bus console;
        const Config config{0x10, static_cast<uint8_t>(0x40 + fine), 0};
        render_frame(console.ppu, config);
        ASSERT_EQ(fine, console.ppu.fine_x) << "setup: scroll byte should give fine X " << fine;
        expect_frame_matches_reference(console.ppu, config, test_label());
    }
}

GTEST_TEST(testPPURender, leftmost_eight_pixels_can_be_blanked)
{
    Bus console;
    Config config{0x10, 93, 154};
    config.show_left_eight = false;
    render_frame(console.ppu, config);
    expect_frame_matches_reference(console.ppu, config, test_label());

    // ...and it really is a blanking, not a coincidence: the backdrop is in
    // those columns and the reference says so for a different reason.
    const uint8_t backdrop = console.ppu.ppu_bus_read(0x3F00);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < 8; ++x) {
            ASSERT_EQ(backdrop, console.ppu.framebuffer[y * kWidth + x]) << "at (" << x << ", " << y << ")";
        }
    }
}

GTEST_TEST(testPPURender, disabling_the_background_leaves_the_backdrop)
{
    Bus console;
    Config config{0x10, 93, 154};
    config.show_background = false;
    render_frame(console.ppu, config);

    // With rendering off the PPU still drives the backdrop colour, so the
    // framebuffer is a flat field of $3F00 rather than stale or undefined.
    const uint8_t backdrop = console.ppu.ppu_bus_read(0x3F00);
    ASSERT_NE(0x00, backdrop) << "setup: the backdrop should be distinguishable from a cleared framebuffer";
    for (int i = 0; i < kWidth * kHeight; ++i) {
        ASSERT_EQ(backdrop, console.ppu.framebuffer[i]) << "at pixel " << i;
    }
}

GTEST_TEST(testPPURender, greyscale_collapses_the_index_onto_the_grey_column)
{
    Bus console;
    Config config{0x10, 93, 154};
    config.greyscale = true;
    render_frame(console.ppu, config);
    expect_frame_matches_reference(console.ppu, config, test_label());

    // Greyscale forces the low four bits of the palette INDEX, so every pixel
    // has to be one of $00, $10, $20, $30.
    for (int i = 0; i < kWidth * kHeight; ++i) {
        ASSERT_EQ(0x00, console.ppu.framebuffer[i] & 0x0F) << "at pixel " << i;
    }
}

// --- the attribute quadrants, against literal expectations -----------------
//
// The whole-frame comparison above would catch a broken quadrant selection, but
// it would report it as "43,000 pixels disagree". This pins the rule itself.

GTEST_TEST(testPPURender, attribute_quadrants_select_the_palette)
{
    Bus console;
    PPU& ppu = console.ppu;
    run_past_reset_lockout(ppu);

    // Tile 1: every pixel is colour 1 (low bitplane all ones, high all zeros),
    // so the only thing that can vary across the screen is the palette.
    for (int row = 0; row < 8; ++row) {
        ppu.ppu_bus_write(static_cast<uint16_t>(0x0010 + row), 0xFF);
        ppu.ppu_bus_write(static_cast<uint16_t>(0x0018 + row), 0x00);
    }

    // Nametable 0: tile 1 everywhere. 30 rows of 32.
    for (uint16_t i = 0; i < 32 * 30; ++i) {
        ppu.ppu_bus_write(static_cast<uint16_t>(0x2000 + i), 0x01);
    }

    // One attribute byte covers 4x4 tiles = 32x32 pixels, two bits per 2x2
    // tiles = 16x16 pixels, packed low-to-high as top-left, top-right,
    // bottom-left, bottom-right. $E4 = 11 10 01 00.
    ppu.ppu_bus_write(0x23C0, 0xE4);

    // Colour 1 of each of the four palettes, made distinguishable.
    ppu.ppu_bus_write(0x3F00, 0x0F);  // backdrop
    ppu.ppu_bus_write(0x3F01, 0x11);  // palette 0
    ppu.ppu_bus_write(0x3F05, 0x22);  // palette 1
    ppu.ppu_bus_write(0x3F09, 0x33);  // palette 2
    ppu.ppu_bus_write(0x3F0D, 0x03);  // palette 3

    ppu.write(PPU::PPUCTRL, 0x00);  // pattern table $0000, nametable 0
    ppu.write(PPU::PPUMASK, 0x0A);  // show background, including the left eight
    ppu.write(PPU::PPUSCROLL, 0x00);
    ppu.write(PPU::PPUSCROLL, 0x00);

    run_frames(ppu, 2);

    // The four 16x16 quadrants of the first 32x32 block. Sampled at the middle
    // of each so an off-by-one in the quadrant boundary is unambiguous.
    EXPECT_EQ(0x11, ppu.framebuffer[8 * kWidth + 8]) << "top-left quadrant -> palette 0";
    EXPECT_EQ(0x22, ppu.framebuffer[8 * kWidth + 24]) << "top-right quadrant -> palette 1";
    EXPECT_EQ(0x33, ppu.framebuffer[24 * kWidth + 8]) << "bottom-left quadrant -> palette 2";
    EXPECT_EQ(0x03, ppu.framebuffer[24 * kWidth + 24]) << "bottom-right quadrant -> palette 3";

    // The boundaries are at 16 pixels exactly, not 8 and not 32.
    EXPECT_EQ(0x11, ppu.framebuffer[0 * kWidth + 15]) << "x=15 is still the left half";
    EXPECT_EQ(0x22, ppu.framebuffer[0 * kWidth + 16]) << "x=16 is the right half";
    EXPECT_EQ(0x11, ppu.framebuffer[15 * kWidth + 0]) << "y=15 is still the top half";
    EXPECT_EQ(0x33, ppu.framebuffer[16 * kWidth + 0]) << "y=16 is the bottom half";

    // The next block along uses attribute byte $23C1, which was never written
    // and so is 0: palette 0 again.
    EXPECT_EQ(0x11, ppu.framebuffer[8 * kWidth + 40]) << "second block, attribute byte $00";
}

// --- transparency ----------------------------------------------------------

GTEST_TEST(testPPURender, colour_zero_reads_through_to_the_backdrop)
{
    Bus console;
    PPU& ppu = console.ppu;
    run_past_reset_lockout(ppu);

    // Tile 1 is half transparent: the left four pixels of every row are colour
    // 0, the right four colour 3.
    for (int row = 0; row < 8; ++row) {
        ppu.ppu_bus_write(static_cast<uint16_t>(0x0010 + row), 0x0F);
        ppu.ppu_bus_write(static_cast<uint16_t>(0x0018 + row), 0x0F);
    }
    for (uint16_t i = 0; i < 32 * 30; ++i) {
        ppu.ppu_bus_write(static_cast<uint16_t>(0x2000 + i), 0x01);
    }

    ppu.ppu_bus_write(0x3F00, 0x0F);  // backdrop
    // Palette 0 entry 3, and the unused entry at $3F04 set to something else
    // entirely: a transparent pixel must reach $3F00, not "entry 0 of the
    // tile's palette".
    ppu.ppu_bus_write(0x3F03, 0x2A);
    ppu.ppu_bus_write(0x3F04, 0x16);

    ppu.write(PPU::PPUCTRL, 0x00);
    ppu.write(PPU::PPUMASK, 0x0A);
    ppu.write(PPU::PPUSCROLL, 0x00);
    ppu.write(PPU::PPUSCROLL, 0x00);

    run_frames(ppu, 2);

    EXPECT_EQ(0x0F, ppu.framebuffer[0 * kWidth + 0]) << "colour 0 -> backdrop";
    EXPECT_EQ(0x0F, ppu.framebuffer[0 * kWidth + 3]);
    EXPECT_EQ(0x2A, ppu.framebuffer[0 * kWidth + 4]) << "colour 3 -> $3F03";
    EXPECT_EQ(0x2A, ppu.framebuffer[0 * kWidth + 7]);
}

// --- the colour table ------------------------------------------------------

GTEST_TEST(testPPURender, the_palette_table_has_64_entries_of_24_bit_colour)
{
    // The framebuffer holds a 6-bit index, so the table it indexes must have 64
    // entries and no entry may exceed 24 bits.
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(0u, PPU::nes_palette[i] & 0xFF000000u) << "entry " << i;
    }
    // Spot checks against the 2C02 palette: $0F is black, $30 is white, $20 is
    // the lighter white the greyscale column tops out at.
    EXPECT_EQ(0x000000u, PPU::nes_palette[0x0F]);
    EXPECT_EQ(0xFFFEFFu, PPU::nes_palette[0x30]);
    EXPECT_EQ(0xFFFEFFu, PPU::nes_palette[0x20]);
}

// --- the dot-1 nametable fetch -------------------------------------------
//
// The background fetch region runs from dot 2, so fetch_background_byte() is
// never entered with (cycle-1)%8 == 0 at dot 1: the nametable byte for the tile
// covering pixels 16-23 is never read on this line. What makes the picture come
// out right anyway is an accident - the dot-338/340 reads on the PREVIOUS line
// leave tile_address() in nametable_latch, and v does not move between the
// dot-336 coarse-X increment and this line's dot-8 one, so the stale latch
// happens to hold the right byte.
//
// It stops being right the moment v moves inside that window, because the
// attribute and pattern fetches at dots 3, 5 and 7 read v LIVE while the tile
// index does not. A mid-line $2006 write is exactly what raster-split code
// does.
//
// Set up two screens of solid, distinguishable tiles and switch nametable at
// dot 0 of scanline 100. The first two tiles of the line were prefetched during
// line 99 and must still be the OLD screen; from pixel 16 the new screen must
// show, because that tile's nametable byte is read at dot 1 - after the switch.
GTEST_TEST(testPPURender, a_nametable_switch_at_dot_0_takes_effect_at_pixel_16)
{
    Bus console;
    PPU& ppu = console.ppu;
    run_past_reset_lockout(ppu);

    // Tile 1: low bitplane only  -> every pixel is colour index 1.
    // Tile 2: high bitplane only -> every pixel is colour index 2.
    for (uint16_t row = 0; row < 8; ++row) {
        ppu.ppu_bus_write(static_cast<uint16_t>(0x0010 + row), 0xFF);  // tile 1, low
        ppu.ppu_bus_write(static_cast<uint16_t>(0x0018 + row), 0x00);  // tile 1, high
        ppu.ppu_bus_write(static_cast<uint16_t>(0x0020 + row), 0x00);  // tile 2, low
        ppu.ppu_bus_write(static_cast<uint16_t>(0x0028 + row), 0xFF);  // tile 2, high
    }

    // Screen A ($2000) is all tile 1; screen B ($2800) is all tile 2. Attribute
    // tables stay zero, so both use palette 0.
    for (uint16_t i = 0; i < 0x03C0; ++i) {
        ppu.ppu_bus_write(static_cast<uint16_t>(0x2000 + i), 0x01);
        ppu.ppu_bus_write(static_cast<uint16_t>(0x2800 + i), 0x02);
    }

    ppu.ppu_bus_write(0x3F01, 0x11);
    ppu.ppu_bus_write(0x3F02, 0x22);

    ppu.write(PPU::PPUCTRL, 0x00);  // nametable $2000, bg patterns at $0000
    ppu.write(PPU::PPUMASK, 0x0A);  // background on, including the left eight
    ppu.write(PPU::PPUSCROLL, 0x00);
    ppu.write(PPU::PPUSCROLL, 0x00);

    run_frames(ppu, 2);

    // Park on dot 0 of scanline 100 and switch v to the other nametable.
    while (!(ppu.scanline == 100 && ppu.cycle == 0)) {
        ppu.clock();
    }
    ppu.registers.PPUADDR = static_cast<uint16_t>(ppu.registers.PPUADDR | 0x0800);

    while (ppu.scanline == 100) {
        ppu.clock();
    }

    const uint16_t* line = &ppu.framebuffer[100 * PPU::screen_width];

    // Pixels 0-15: the two tiles fetched during line 99's dots 321-336, before
    // the switch. Still screen A.
    EXPECT_EQ(0x11, line[0]) << "pixels 0-7 were prefetched on the previous line";
    EXPECT_EQ(0x11, line[8]) << "pixels 8-15 were prefetched on the previous line";

    // Pixel 16 onwards: fetched by THIS line, starting with the nametable read
    // at dot 1, which happens after the switch.
    EXPECT_EQ(0x22, line[16]) << "the tile covering pixels 16-23 has its nametable byte read at "
                                 "dot 1 of this line, so the switch must already be visible here";
    EXPECT_EQ(0x22, line[24]);
    EXPECT_EQ(0x22, line[248]);
}

}  // namespace render
}  // namespace tests
