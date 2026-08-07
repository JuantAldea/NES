// The external oracle for the standard controller: blargg's read_joy3
// test_buttons.
//
// controller_tests.cpp checks $4016 against the NESdev description, but those
// expectations and the implementation share an author, so a shared
// misunderstanding survives them. This ROM does not: it names a button, waits
// for a press AND a release, and only then moves on. Passing it means the
// controller works well enough for a real program written against real
// hardware, which is a different claim.
//
// It is also the emulator's first genuinely END-TO-END test. Everything has to
// work at once: the CPU executing the ROM, the controller shift register, the
// PPU drawing the prompt into the nametable, and the reader turning that back
// into text. Nothing else in the suite exercises input and output together.
//
// PROVEN NON-VACUOUS, which matters because the sibling ROM thorough_test is
// not: it reports "Passed" with no controller implemented at all. Measured
// against three deliberate defects, test_buttons detects every one:
//
//   controller always reports "not pressed"  -> stalls forever on prompt "A"
//   shift register never advances            -> "Failed"
//   strobe ignored (always reloading)        -> "Failed"
#include <cstdint>
#include <string>

#include "gtest/gtest.h"

#include "../include/bus.h"
#include "nametable_screen.h"

namespace tests
{
namespace controller_rom
{
namespace
{

using nametable_screen::read_text;
using nametable_screen::run_one_frame;

struct NamedButton {
    const char* name;
    uint8_t mask;
};

// "Select" must be tested before "Start"? No - but "A" and "B" are substrings
// of nothing here, and the prompts are exact words, so a plain search is safe.
constexpr NamedButton kButtons[] = {
    {"Up", Controllers::Up},         {"Down", Controllers::Down},
    {"Left", Controllers::Left},     {"Right", Controllers::Right},
    {"Select", Controllers::Select}, {"Start", Controllers::Start},
    {"A", Controllers::A},           {"B", Controllers::B},
};

// Measured: the ROM reaches its verdict at frame 156 when every button is
// delivered. The cap is generous because a REGRESSION here shows up as a stall
// rather than a failure - a controller that never reports a press leaves the
// ROM waiting forever - and the diagnosis wanted in that case is "stuck on
// prompt X", not a timeout at the first opportunity.
constexpr int kMaxFrames = 2000;

// The prompt is the last non-blank row on screen.
std::string last_non_blank_row(const std::string& screen)
{
    std::string out;
    size_t start = 0;
    while (start < screen.size()) {
        size_t end = screen.find('\n', start);
        if (end == std::string::npos) {
            end = screen.size();
        }
        const std::string row = screen.substr(start, end - start);
        if (!row.empty()) {
            out = row;
        }
        start = end + 1;
    }
    return out;
}

}  // namespace

GTEST_TEST(controllerRom, test_buttons_reports_passed_when_every_button_is_delivered)
{
    Bus console;
    const std::string path = std::string(NES_TEST_FILES_DIR) + "/read_joy3/test_buttons.nes";
    ASSERT_TRUE(console.load_cartridge(path))
        << "read_joy3 absent - run tests/test_files/fetch_read_joy3.sh";

    console.cpu.reset();

    std::string prompt;
    int presses = 0;

    for (int frame = 0; frame < kMaxFrames; ++frame) {
        const std::string screen = read_text(console);

        if (screen.find("Passed") != std::string::npos) {
            EXPECT_EQ(8, presses) << "all eight buttons should have been asked for";
            SUCCEED();
            return;
        }
        ASSERT_EQ(std::string::npos, screen.find("Failed"))
            << "the ROM rejected our controller at prompt '" << prompt << "':\n"
            << screen;

        const std::string row = last_non_blank_row(screen);

        uint8_t want = 0;
        for (const NamedButton& b : kButtons) {
            if (row.find(b.name) != std::string::npos) {
                want = b.mask;
                break;
            }
        }

        if (row != prompt && want != 0) {
            prompt = row;
            ++presses;
        }

        // Pressed for ten frames, released for ten. The ROM waits for the
        // release as well as the press, so holding a button down forever stalls
        // it just as surely as never pressing one.
        console.controllers.set_port(0, ((frame % 20) < 10) ? want : 0);

        run_one_frame(console);
    }

    FAIL() << "no verdict within " << kMaxFrames << " frames; stuck on prompt '" << prompt << "' after "
           << presses << " presses.\n"
              "  A stall here means the ROM asked for a button and never saw it: suspect $4016,\n"
              "  the shift register, or the strobe - not the PPU, since the prompt was read.";
}

}  // namespace controller_rom
}  // namespace tests
