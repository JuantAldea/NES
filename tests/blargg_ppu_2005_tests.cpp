// Blargg's 2005 PPU tests: palette RAM, VRAM access through $2006/$2007, OAM.
//
// These predate his $6000 reporting protocol and report "on screen and by
// beeping", which is why they sat unused - a $6000 harness reads them as
// "timed out without initialising" no matter how correct the emulator is.
//
// But the on-screen report is readable without any rendering. The ROMs write
// their result into the NAMETABLE as tile indices, and the font is ASCII
// mapped, so reading $2000 onwards and treating each byte as a character
// recovers the text directly:
//
//     |  $01                           |
//
// Per blargg's readme, "A result code of 1 always indicates that all tests were
// passed". Anything else is a specific failure, listed per ROM below.
//
// This matters because it is an EXTERNAL check on the PPU address space, which
// until now was covered only by unit tests written alongside the implementation
// - the situation where a shared misunderstanding survives.
#include <cstdint>
#include <string>

#include "gtest/gtest.h"

#include "../include/bus.h"

namespace tests
{
namespace blargg_2005
{

// Enough for these ROMs, which settle within ~120 frames; measured below.
constexpr uint64_t kMaxFrames = 400;
constexpr uint64_t kBusCyclesPerFrame = 341ull * 262ull * 4ull;

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/blargg_ppu_2005/" + name + ".nes";
}

// Reads the first nametable row that has anything on it, as text.
std::string read_screen_line(Bus& console)
{
    for (uint16_t row = 0; row < 30; ++row) {
        std::string line;
        bool any = false;

        for (uint16_t col = 0; col < 32; ++col) {
            const uint8_t tile = console.ppu.ppu_bus_read(static_cast<uint16_t>(0x2000 + row * 32 + col));
            line.push_back((tile >= 0x20 && tile < 0x7F) ? static_cast<char>(tile) : '.');

            // A row counts as drawn only once it holds a PRINTABLE, non-space
            // character. Treating "not a space" as content instead reports
            // success on frame 0, because nametable RAM powers up as zeros and
            // 0x00 is not 0x20.
            if (tile > 0x20 && tile < 0x7F) {
                any = true;
            }
        }

        if (any) {
            // Trim the trailing run of spaces.
            const size_t end = line.find_last_not_of(' ');
            return end == std::string::npos ? std::string() : line.substr(0, end + 1);
        }
    }
    return {};
}

// Runs until the ROM has drawn something, then returns it.
std::string run_and_read_result(const std::string& name)
{
    Bus console;
    if (!console.load_cartridge(rom_path(name))) {
        ADD_FAILURE() << "could not load " << rom_path(name)
                      << " - run tests/test_files/fetch_blargg_ppu_2005.sh";
        return {};
    }

    console.cpu.reset();

    // Wait for the line to SETTLE, not merely to appear. The ROM writes its
    // result one tile at a time, so sampling the first frame with any text on
    // it catches "  $" before the digits arrive.
    std::string previous;
    uint64_t stable_frames = 0;

    for (uint64_t frame = 0; frame < kMaxFrames; ++frame) {
        for (uint64_t i = 0; i < kBusCyclesPerFrame; ++i) {
            console.clock();
        }

        const std::string line = read_screen_line(console);
        if (line.empty()) {
            continue;
        }

        stable_frames = (line == previous) ? stable_frames + 1 : 0;
        previous = line;

        // Three frames unchanged is far longer than the handful of scanlines a
        // write takes, and far shorter than the cap.
        if (stable_frames >= 3) {
            return line;
        }
    }

    return previous;
}

class Blargg2005PpuRoms : public ::testing::TestWithParam<std::string>
{
};

TEST_P(Blargg2005PpuRoms, reports_pass)
{
    const std::string name = GetParam();
    const std::string result = run_and_read_result(name);

    ASSERT_FALSE(result.empty()) << name << ": the ROM drew nothing within " << kMaxFrames
                                 << " frames. It reports by writing tile indices into the nametable, so an "
                                    "empty screen means it never got that far - suspect the CPU or the "
                                    "cartridge mapping rather than the PPU.";

    // "$01" is blargg's pass code. The leading spaces are part of the layout.
    EXPECT_NE(std::string::npos, result.find("$01"))
        << name << " reported \"" << result << "\" rather than $01.\n"
        << "  palette_ram:  2 palette read shouldn't be buffered   3 write/read broken\n"
        << "                4 not mirrored within $3F00-$3FFF      5 $10 not mirrored at $00\n"
        << "                6 $00 not mirrored at $10\n"
        << "  vram_access:  2 VRAM reads should be delayed one read\n"
        << "                3 basic write/read broken              4 $2007 should not increment\n"
        << "                                                         when rendering is on\n"
        << "  sprite_ram:   2 basic read/write broken              3 address should increment on write\n"
        << "                4 address should not increment on read 5 third sprite byte masks low bits";
}

INSTANTIATE_TEST_SUITE_P(Blargg2005Ppu, Blargg2005PpuRoms,
                         ::testing::Values("palette_ram", "vram_access", "sprite_ram"),
                         [](const ::testing::TestParamInfo<std::string>& info) { return info.param; });

// Guards the reading mechanism itself. If the font stopped being ASCII-mapped,
// or the nametable decode regressed, every test above would fail identically
// with an unreadable string and it would not be obvious which.
GTEST_TEST(blargg2005Harness, the_screen_reader_recovers_ascii_from_the_nametable)
{
    Bus console;

    const char* message = "PASS $01";
    for (uint16_t i = 0; message[i] != '\0'; ++i) {
        console.ppu.ppu_bus_write(static_cast<uint16_t>(0x2000 + i), static_cast<uint8_t>(message[i]));
    }
    // The rest of the row is spaces, as the ROMs leave it.
    for (uint16_t i = 8; i < 32; ++i) {
        console.ppu.ppu_bus_write(static_cast<uint16_t>(0x2000 + i), 0x20);
    }

    EXPECT_EQ("PASS $01", read_screen_line(console));
}

}  // namespace blargg_2005
}  // namespace tests
