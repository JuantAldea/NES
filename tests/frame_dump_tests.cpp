// Guards the PPM writer itself.
//
// It exists so that a failing render can be looked at, which means it is
// consulted exactly when something else is already wrong - the worst moment to
// discover the dump is misleading. So the bytes are read back and checked,
// rather than the writer being trusted because it is short.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../include/bus.h"
#include "../include/frame_dump.h"
#include "gtest/gtest.h"

namespace tests
{
namespace frame_dump
{
namespace
{

// Each test writes a uniquely named file, so they stay independent under
// ctest -j. Written to the system temp directory rather than the source tree:
// these are diagnostic artefacts, not fixtures.
std::string temp_path(const std::string& name)
{
    return (std::filesystem::temp_directory_path() / ("nes_frame_dump_" + name + ".ppm")).string();
}

std::vector<uint8_t> read_all(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

}  // namespace

GTEST_TEST(frameDump, writes_a_readable_ppm_with_the_right_pixels)
{
    // A 2x2 image with four different indices, and a palette whose entries are
    // trivially distinguishable so a channel swap is obvious rather than
    // plausible.
    const uint32_t palette[4] = {0x000000, 0xFF0000, 0x00FF00, 0x0000FF};
    const uint16_t indices[4] = {0, 1, 2, 3};

    const std::string path = temp_path("basic");
    ASSERT_TRUE(nes::write_ppm(path, indices, 2, 2, palette, 4));

    const std::vector<uint8_t> bytes = read_all(path);

    const std::string header = "P6\n2 2\n255\n";
    ASSERT_EQ(header.size() + 4 * 3, bytes.size()) << "header plus one RGB triple per pixel";
    EXPECT_EQ(header, std::string(bytes.begin(), bytes.begin() + header.size()));

    const uint8_t* rgb = bytes.data() + header.size();
    EXPECT_EQ(0x00, rgb[0]);  // index 0 -> black
    EXPECT_EQ(0x00, rgb[1]);
    EXPECT_EQ(0x00, rgb[2]);
    EXPECT_EQ(0xFF, rgb[3]);  // index 1 -> red, and R really is first
    EXPECT_EQ(0x00, rgb[4]);
    EXPECT_EQ(0x00, rgb[5]);
    EXPECT_EQ(0x00, rgb[6]);  // index 2 -> green
    EXPECT_EQ(0xFF, rgb[7]);
    EXPECT_EQ(0x00, rgb[8]);
    EXPECT_EQ(0x00, rgb[9]);  // index 3 -> blue
    EXPECT_EQ(0x00, rgb[10]);
    EXPECT_EQ(0xFF, rgb[11]);

    std::remove(path.c_str());
}

// Palette RAM is six bits wide, so bits above the index are never a palette
// address. Masking makes a caller bug a visible wrong colour instead of a read
// past the end of the 64-entry table.
//
// THIS TEST USED TO PASS $41 AND $FF and expect $01 and $3F. That contract is
// gone: bits 6-8 are now the emphasis field, so $41 legitimately means "index
// $01, red emphasised" and returns an attenuated colour rather than the plain
// one. It was rewritten rather than deleted, because the thing it guards - not
// indexing past the table - is still worth guarding; only the choice of input
// changed, to bits above the whole 9-bit pixel.
GTEST_TEST(frameDump, ignores_bits_above_the_nine_bit_pixel)
{
    EXPECT_EQ(PPU::nes_palette[0x01], nes::palette_index_to_rgb(0x0201, PPU::nes_palette, 64))
        << "bit 9 is not part of the pixel and must not reach the table index";
    EXPECT_EQ(PPU::nes_palette[0x3F], nes::palette_index_to_rgb(0xFC3F, PPU::nes_palette, 64))
        << "$3F is column $xF, which emphasis leaves alone, so the colour is unchanged";
}

// The emphasis field itself, at the boundary: $3F is immune to emphasis, so it
// is the one index where a stray high bit CANNOT be caught by colour alone.
// $01 is not immune, which is what makes the pair above meaningful.
GTEST_TEST(frameDump, the_emphasis_field_is_bits_six_to_eight)
{
    EXPECT_NE(nes::palette_index_to_rgb(0x01, PPU::nes_palette, 64),
              nes::palette_index_to_rgb(0x01 | (1 << 6), PPU::nes_palette, 64))
        << "bit 6 must be read as emphasis";
    EXPECT_EQ(nes::palette_index_to_rgb(0x01, PPU::nes_palette, 64),
              nes::palette_index_to_rgb(0x01 | (1 << 9), PPU::nes_palette, 64))
        << "bit 9 must be ignored entirely";
}

GTEST_TEST(frameDump, reports_failure_when_the_path_is_unwritable)
{
    const uint32_t palette[1] = {0};
    const uint16_t indices[1] = {0};

    // A dump that silently did not happen is worse than none, because someone
    // goes looking for the file.
    EXPECT_FALSE(nes::write_ppm("/nonexistent-directory-for-tests/x.ppm", indices, 1, 1, palette, 1));
}

// The real thing: a rendered frame round-trips at the right size, with the
// PPU's own palette.
GTEST_TEST(frameDump, dumps_a_rendered_frame_at_the_right_size)
{
    Bus console;
    PPU& ppu = console.ppu;

    while (ppu.in_reset_write_lockout()) {
        ppu.clock();
    }
    ppu.write(PPU::PPUMASK, 0x08);  // background on
    console.run_frame();

    const std::string path = temp_path("frame");
    ASSERT_TRUE(nes::write_ppm(path, ppu.framebuffer, PPU::screen_width, PPU::screen_height, PPU::nes_palette, 64));

    const std::vector<uint8_t> bytes = read_all(path);
    const std::string header = "P6\n256 240\n255\n";
    EXPECT_EQ(header, std::string(bytes.begin(), bytes.begin() + header.size()));
    EXPECT_EQ(header.size() + static_cast<size_t>(PPU::screen_width) * PPU::screen_height * 3, bytes.size());

    std::remove(path.c_str());
}

}  // namespace frame_dump
// --- colour emphasis ---------------------------------------------------------
//
// The framebuffer cannot reach this: it stores an index plus three emphasis
// bits, and the attenuation happens here, on the way to RGB. So the ROM oracle
// proves emphasis is CAPTURED and these prove it is APPLIED.

GTEST_TEST(frameDumpEmphasis, no_emphasis_leaves_the_colour_alone)
{
    const uint32_t white = 0xFFFFFF;
    EXPECT_EQ(white, nes::apply_emphasis(white, 0x01, 0));
}

// Bit 0 emphasises red on the NTSC 2C02, so red survives and the other two are
// attenuated. Boosting red instead would be RGB-PPU (2C03/04/05) behaviour -
// see the divergence note in frame_dump.h.
GTEST_TEST(frameDumpEmphasis, emphasising_red_attenuates_only_green_and_blue)
{
    const uint32_t out = nes::apply_emphasis(0xFFFFFF, 0x01, 1);
    const uint8_t r = static_cast<uint8_t>(out >> 16);
    const uint8_t g = static_cast<uint8_t>(out >> 8);
    const uint8_t b = static_cast<uint8_t>(out);

    EXPECT_EQ(0xFF, r) << "the emphasised channel must not be attenuated - nor boosted";
    EXPECT_EQ(static_cast<uint8_t>(0xFF * nes::emphasis_attenuation), g);
    EXPECT_EQ(g, b) << "both unemphasised channels get the same factor";
}

// The case the nesdev thread "Most emulators i've tested don't do dual emphasis
// bits right" exists about: the factors COMPOUND per channel, so with all three
// bits set every channel is attenuated twice and the picture darkens. An
// implementation applying attenuation at most once would leave this at 0xFF *
// 0.816, and one that skipped the multi-bit case entirely would leave it white.
GTEST_TEST(frameDumpEmphasis, all_three_darkens_every_channel_twice_over)
{
    const uint32_t out = nes::apply_emphasis(0xFFFFFF, 0x01, 7);
    const uint8_t expected = static_cast<uint8_t>(0xFF * nes::emphasis_attenuation * nes::emphasis_attenuation);

    EXPECT_EQ(expected, static_cast<uint8_t>(out >> 16));
    EXPECT_EQ(expected, static_cast<uint8_t>(out >> 8));
    EXPECT_EQ(expected, static_cast<uint8_t>(out));
    EXPECT_LT(expected, static_cast<uint8_t>(0xFF * nes::emphasis_attenuation))
        << "all three set must be darker than one set, or the compounding is missing";
}

// "$1D black is affected by color emphasis, but $0F black is not." Columns $xE
// and $xF are left alone whatever the emphasis bits say - which is also why
// blargg's grid stops at $xD.
GTEST_TEST(frameDumpEmphasis, columns_xE_and_xF_are_never_attenuated)
{
    for (uint8_t index : {0x0E, 0x0F, 0x1E, 0x1F, 0x2E, 0x2F, 0x3E, 0x3F}) {
        EXPECT_EQ(0xFFFFFFu, nes::apply_emphasis(0xFFFFFF, index, 7))
            << "index $" << std::hex << static_cast<int>(index) << " must be immune to emphasis";
    }
    EXPECT_NE(0xFFFFFFu, nes::apply_emphasis(0xFFFFFF, 0x1D, 7)) << "$1D is NOT immune - it is column $xD";
}

// The encoding the PPU writes, round-tripped through the conversion the display
// uses, so a change to either half breaks here rather than silently disagreeing.
GTEST_TEST(frameDumpEmphasis, a_framebuffer_pixel_carries_its_emphasis_into_the_rgb)
{
    uint32_t palette[64];
    for (uint32_t& entry : palette) {
        entry = 0xFFFFFF;
    }
    const uint16_t plain = 0x01;                                         // index $01, no emphasis
    const uint16_t emphasised = static_cast<uint16_t>(0x01 | (1 << 6));  // index $01, red emphasis

    EXPECT_EQ(0xFFFFFFu, nes::palette_index_to_rgb(plain, palette, 64));
    EXPECT_NE(0xFFFFFFu, nes::palette_index_to_rgb(emphasised, palette, 64))
        << "the high bits of the pixel were dropped on the way to RGB";
}

// The lookup table the frontend uses must agree with the function everything
// else uses, for all 512 pixel values. Two paths to one answer is exactly how a
// display quietly starts disagreeing with a PPM dump of the same frame - and
// the table exists only as an optimisation, so any divergence is a bug in it
// rather than a difference of intent.
GTEST_TEST(frameDumpEmphasis, the_lookup_table_matches_the_function_for_every_pixel)
{
    const nes::EmphasisTable table(PPU::nes_palette, 64);

    for (uint16_t pixel = 0; pixel < 512; ++pixel) {
        ASSERT_EQ(nes::palette_index_to_rgb(pixel, PPU::nes_palette, 64), table.rgb[pixel])
            << "table and function disagree for pixel $" << std::hex << pixel;
    }
}

}  // namespace tests
