// Reads a ROM's on-screen report out of the nametable, without any rendering.
//
// Blargg's 2005-era test ROMs predate his $6000 protocol and report "on screen
// and by beeping". That looked like it made them unusable headlessly, but the
// on-screen report is readable directly: the ROMs write their result into the
// nametable as tile indices, and the font is ASCII-mapped, so treating each
// byte at $2000 onwards as a character recovers the text.
//
// Two suites use this, and they do NOT report the same way:
//
//   blargg_ppu_tests_2005.09.15b  prints only a result code, "  $01"
//   sprite_hit_tests_2005.10.05   prints a TITLE line, then "PASSED"/"FAILED #N"
//
// So only the reading is shared here; each suite keeps its own idea of what a
// finished screen looks like. Sharing the terminal condition too would mean the
// sprite-hit reader stopping on "SPRITE HIT BASICS" and never seeing the
// verdict, which is exactly what a first attempt at it did.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../include/bus.h"

namespace tests
{
namespace nametable_screen
{

// One frame is 341 dots x 262 scanlines, and Bus::clock ticks the PPU every
// fourth bus cycle.
constexpr uint64_t kBusCyclesPerFrame = 341ull * 262ull * 4ull;

constexpr uint16_t kNametableBase = 0x2000;
constexpr uint16_t kRows = 30;
constexpr uint16_t kColumns = 32;

// Reads the nametable as 30 rows of text. Trailing spaces are trimmed, and a
// row with nothing printable on it comes back empty.
//
// "Printable" deliberately excludes the space itself when deciding whether a
// row has content: nametable RAM powers up as zeros, and 0x00 is not 0x20, so
// treating "not a space" as content reports a drawn screen on frame 0.
inline std::vector<std::string> read_rows(Bus& console)
{
    std::vector<std::string> rows;
    rows.reserve(kRows);

    for (uint16_t row = 0; row < kRows; ++row) {
        std::string line;
        bool any = false;

        for (uint16_t col = 0; col < kColumns; ++col) {
            const uint16_t addr = static_cast<uint16_t>(kNametableBase + row * kColumns + col);
            const uint8_t tile = console.ppu.ppu_bus_read(addr);
            line.push_back((tile >= 0x20 && tile < 0x7F) ? static_cast<char>(tile) : '.');
            if (tile > 0x20 && tile < 0x7F) {
                any = true;
            }
        }

        if (!any) {
            rows.emplace_back();
            continue;
        }
        const size_t end = line.find_last_not_of(' ');
        rows.push_back(end == std::string::npos ? std::string() : line.substr(0, end + 1));
    }

    return rows;
}

// The whole screen as one string, rows separated by newlines. Blank rows are
// kept so that row positions survive, and so that two screens compare equal
// only when they really are identical.
inline std::string read_text(Bus& console)
{
    std::string text;
    for (const std::string& row : read_rows(console)) {
        text += row;
        text.push_back('\n');
    }
    return text;
}

// The first row with anything printable on it. This is the whole report for the
// 2005 PPU ROMs; it is only the title for the sprite-hit ROMs.
inline std::string first_non_blank_row(Bus& console)
{
    for (const std::string& row : read_rows(console)) {
        if (!row.empty()) {
            return row;
        }
    }
    return {};
}

inline void run_one_frame(Bus& console)
{
    for (uint64_t i = 0; i < kBusCyclesPerFrame; ++i) {
        console.clock();
    }
}

}  // namespace nametable_screen
}  // namespace tests
