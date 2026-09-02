// Reads a ROM's on-screen report out of the nametable, without any rendering.
//
// Blargg's 2005-era test ROMs predate his $6000 protocol and report "on screen
// and by beeping". That looked like it made them unusable headlessly, but the
// on-screen report is readable directly: the ROMs write their result into the
// nametable as tile indices, and the font is ASCII-mapped, so treating each
// byte at $2000 onwards as a character recovers the text.
//
// ONLY THE READING IS SHARED. The suites that take a verdict from the screen do
// not agree on what a finished one looks like:
//
//   blargg_ppu_tests_2005.09.15b  prints only a result code, "  $01"
//   blargg_apu_2005               likewise, read through first_non_blank_row
//   sprite_hit_tests_2005.10.05   prints a TITLE line, then "PASSED"/"FAILED #N"
//
// So each keeps its own terminal condition. Sharing that too makes the
// sprite-hit reader stop on "SPRITE HIT BASICS" and never see the verdict.
//
// Several other suites call read_text purely to PRINT the screen when they
// fail. Those assert on nothing here, so a change to the reading affects their
// diagnostics and not their results.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../include/bus.h"

namespace tests
{
namespace nametable_screen
{

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
    // Bus::run_frame watches the PPU's own frame counter. Clocking a fixed
    // 341*262*4 is the obvious alternative and overshoots by a dot on every odd
    // frame once rendering is enabled, because the pre-render line drops its
    // last dot.
    console.run_frame();
}

}  // namespace nametable_screen
}  // namespace tests
