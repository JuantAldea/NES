// Shared helpers for the ROM-backed suites.
//
// This exists because three suites had grown their own copy of the same two
// things, and the copies had drifted in a way that mattered.
//
// THE MACRO NAME NOW MATCHES WHAT IT DOES. `SKIP_IF_ABSENT` existed in three
// incompatible forms: one took a path and only checked for the file, one took a
// console and loaded the cartridge, and one took a console and a name and
// loaded it AND EMULATED 300 FRAMES. A reader moving between suites got a
// different contract each time, and the last one hid roughly 2,100 frames of
// emulation across six tests behind a name that promised a file check.
//
// So the macro here checks presence and nothing else. Loading and running are
// ordinary statements at the call site, where their cost is visible.
//
// THERE ARE TWO OF THEM, AND CHOOSING WRONG IS THE FAILURE THIS REPO EXISTS TO
// PREVENT. `REQUIRE_ROM` fails; `SKIP_IF_ROM_ABSENT` skips. Which one a fixture
// gets is decided by ONE question - does anything fetch it?
//
//   Fetched by a tests/test_files/fetch_*.sh script  ->  REQUIRE_ROM
//   Nothing fetches it, ever                         ->  SKIP_IF_ROM_ABSENT
//
// A fetched ROM is absent only when the fetch step broke, and `GTEST_SKIP`
// exits 0, so skipping there turns a broken CI run green while verifying
// nothing. Eleven tests across full_palette and apu_test were on the wrong side
// of this line.
#pragma once

#include <cstdint>
#include <fstream>
#include <string>

#include "../include/ppu.h"
#include "gtest/gtest.h"

namespace tests
{
namespace fixture
{

// Presence only. No loading, no emulation, no side effects.
inline bool rom_present(const std::string& path) { return std::ifstream(path).good(); }

// The skip message, in one place so every suite says the same thing.
//
// The last line is not boilerplate: `GTEST_SKIP` exits 0, so ctest scores a
// skip as a pass, and a run that reports "100% passed" while quietly skipping
// half its suites is the failure this repository is most concerned with. See
// "Skipped is not passed" in CLAUDE.md.
inline std::string absent_message(const std::string& path, const std::string& how_to_get_it)
{
    return "no ROM at " + path + "\n  " + how_to_get_it + "\n  NOTE: ctest counts this skip as a pass. It is not one.";
}

// The failure message for a fetched ROM that is not there.
//
// The wording is fixed at "could not load <path> - <command>" because of who
// reads it: someone looking at a red CI log who needs the exact file and the
// exact command, not a diagnosis. It deliberately does NOT hedge about this
// maybe being an emulation bug - naming the script is what rules that out.
inline std::string required_message(const std::string& path, const std::string& how_to_get_it)
{
    return "could not load " + path + " - " + how_to_get_it;
}

// Distinct 6-bit palette indices in a finished frame.
//
// Masked to six bits deliberately: the framebuffer holds an index plus three
// emphasis bits, and this counts what the PPU SELECTED, not what a display
// would make of it. Use distinct_pixels when emphasis is the thing under test -
// this function is blind to it by construction.
inline int distinct_indices(const PPU& ppu)
{
    bool seen[64] = {false};
    int n = 0;
    for (int i = 0; i < PPU::screen_width * PPU::screen_height; ++i) {
        const uint8_t index = PPU::pixel_index(ppu.framebuffer[i]);
        if (!seen[index]) {
            seen[index] = true;
            ++n;
        }
    }
    return n;
}

// Distinct index+emphasis pairs - the whole 9-bit pixel. 512 possible values,
// 64 colours under 8 emphasis states.
inline int distinct_pixels(const PPU& ppu)
{
    bool seen[512] = {false};
    int n = 0;
    for (int i = 0; i < PPU::screen_width * PPU::screen_height; ++i) {
        const uint16_t pixel = static_cast<uint16_t>(ppu.framebuffer[i] & 0x1FF);
        if (!seen[pixel]) {
            seen[pixel] = true;
            ++n;
        }
    }
    return n;
}

}  // namespace fixture
}  // namespace tests

// Fails the current test when a FETCHED ROM is missing. This is the default for
// anything a tests/test_files/fetch_*.sh script produces. `how_to_get_it` is
// the bare command, e.g. "run tests/test_files/fetch_apu_test.sh", so the
// assembled message reads as one line.
#define REQUIRE_ROM(path, how_to_get_it)                                                \
    do {                                                                                \
        const std::string required_path = (path);                                       \
        if (!tests::fixture::rom_present(required_path)) {                              \
            FAIL() << tests::fixture::required_message(required_path, (how_to_get_it)); \
        }                                                                               \
    } while (0)

// Skips the current test when the ROM is missing. `how_to_get_it` is the one
// sentence that tells the reader what to run - a fetch script, or where to put
// their own dump.
//
// ONLY for fixtures that CANNOT be fetched, of which this repo has exactly two
// kinds: the cartridge dumps under tests/test_files/local/, which nothing
// fetches because they are the user's own, and the 1.1 GB SingleStepTests
// vectors, which CI deliberately does not pull. Both are absent on every clean
// checkout and always will be, so failing on them would mean a permanently red
// suite - which is the only thing that justifies the exit-0 hazard above.
//
// If a fetch_*.sh script produces the file, this is the wrong macro. Use
// REQUIRE_ROM.
#define SKIP_IF_ROM_ABSENT(path, how_to_get_it)                                         \
    do {                                                                                \
        const std::string skip_path = (path);                                           \
        if (!tests::fixture::rom_present(skip_path)) {                                  \
            GTEST_SKIP() << tests::fixture::absent_message(skip_path, (how_to_get_it)); \
        }                                                                               \
    } while (0)
