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

#include "gtest/gtest.h"

#include "../include/bus.h"
#include "../include/frame_dump.h"

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
    const uint8_t indices[4] = {0, 1, 2, 3};

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

// Palette RAM is six bits wide, so an index above $3F is a caller bug. Masking
// it makes that show up as a visible wrong colour instead of a read past the
// end of the 64-entry table.
GTEST_TEST(frameDump, masks_the_index_to_six_bits)
{
    EXPECT_EQ(PPU::nes_palette[0x01], nes::palette_index_to_rgb(0x41, PPU::nes_palette, 64))
        << "$41 must fold onto $01, not index past the table";
    EXPECT_EQ(PPU::nes_palette[0x3F], nes::palette_index_to_rgb(0xFF, PPU::nes_palette, 64));
}

GTEST_TEST(frameDump, reports_failure_when_the_path_is_unwritable)
{
    const uint32_t palette[1] = {0};
    const uint8_t indices[1] = {0};

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
}  // namespace tests
