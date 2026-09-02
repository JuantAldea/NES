// End-to-end test against a commercial game, if one is present.
//
// Everything else in this suite is a test ROM - written to isolate one
// behaviour and report a verdict - plus two homebrew programs. A retail game is
// a different kind of load: it scrolls continuously, splits the screen with
// sprite 0 every frame, runs OAM DMA every frame, and does all of it at once
// for minutes. The README recorded that as an open verification gap for exactly
// that reason.
//
// THIS TEST SKIPS WHEN THE ROM IS ABSENT, AND THAT IS NOT A PASS.
//
// It cannot be otherwise: nothing here will ever download a commercial game, so
// no other checkout and no CI run can have the file. But `GTEST_SKIP` exits 0,
// which means ctest counts a skipped test as a passing one - the same reason
// CI's headline figure overstates what it verified. The skip message says which
// file is missing, and the honest number for any run is "executed", never
// "passed".
//
// To enable it, put a dump of a cartridge you own at
// tests/test_files/local/smb.nes. See that directory's README.
//
// EVERY THRESHOLD BELOW WAS MEASURED, not guessed:
//
//   title screen reaches 8+ distinct colours by frame 33
//   300 gameplay frames holding Right: coarse X takes 26 distinct values
//   sprite 0 hit fires on 249 of those 300 frames
//
// The assertions sit well below those figures, because they exist to catch
// "scrolling stopped working", not to pin the game's exact behaviour.
#include <cstdint>
#include <cstring>
#include <string>

#include "../frontend/debugger_state.h"
#include "../include/bus.h"
#include "gtest/gtest.h"
#include "rom_fixture.h"

namespace tests
{
namespace local_rom
{
namespace
{

using tests::fixture::distinct_indices;

std::string smb_path() { return std::string(NES_TEST_FILES_DIR) + "/local/smb.nes"; }

constexpr const char* kHowToGetIt =
    "This test needs a dump of a cartridge you own; nothing here fetches one."
    "\n  See tests/test_files/local/README.md.";

// Loads the cartridge. Asserted rather than returning a bool: presence is
// established by SKIP_IF_ROM_ABSENT before this is called, so a failure here
// means the file is there and will not parse - a broken dump, not a missing
// one, and worth stopping for rather than skipping past.
void load_smb(Bus& console)
{
    ASSERT_TRUE(console.load_cartridge(smb_path())) << "the ROM is present but did not load: " << smb_path();

    // power_on(), not reset(): Bus's constructor already reset this CPU before a
    // cartridge was mapped, and a reset subtracts 3 from S rather than
    // reloading it.
    console.cpu.power_on();
}

// Boots to the title screen, presses Start, and waits out the "WORLD 1-1" card.
void start_a_game(Bus& console)
{
    for (int f = 0; f < 120; ++f) {
        console.run_frame();
    }
    for (int f = 0; f < 10; ++f) {
        console.controllers.set_port(0, Controllers::Start);
        console.run_frame();
    }
    console.controllers.set_port(0, 0);
    for (int f = 0; f < 100; ++f) {
        console.run_frame();
    }
}

}  // namespace

GTEST_TEST(commercialRom, boots_to_a_drawn_title_screen)
{
    Bus console;
    SKIP_IF_ROM_ABSENT(smb_path(), kHowToGetIt);
    load_smb(console);

    for (int f = 0; f < 120; ++f) {
        console.run_frame();
    }

    // Measured: 8+ colours by frame 33, settling at 10-11. A blank or
    // single-colour screen is the failure this catches.
    EXPECT_GT(distinct_indices(console.ppu), 5) << "the title screen never drew";
    EXPECT_NE(0, console.ppu.registers.PPUMASK & 0x18) << "rendering was never enabled";
}

// The same boot, but driven the way the debugger's Run button drives it.
//
// That is a DIFFERENT loop from the one every other test here uses:
// Bus::run_frame clocks to the next frame, while the frontend free-runs through
// nes_gui::step_instruction and stops the moment it decides the CPU has hung.
// Nothing exercised that decision against a real game, and it was wrong - SMB's
// init ends on `jmp $8057` and the entire game runs out of the NMI handler, so
// the debugger halted on the last instruction before the title screen. Measured
// at the halt: frame 3, PPUCTRL $90, PPUMASK $06, screen blank.
//
// A green suite plus a tool that cannot run a game is exactly the gap this file
// was added to close, so the assertion is that the picture arrives, not merely
// that nothing tripped.
GTEST_TEST(commercialRom, boots_under_the_debuggers_own_step_loop)
{
    Bus console;
    SKIP_IF_ROM_ABSENT(smb_path(), kHowToGetIt);
    load_smb(console);

    nes_gui::FrontendState state;
    state.running = true;

    // Measured: rendering is on by frame 12 and the title screen has drawn well
    // before frame 120, which is ~360k instructions. The cap is a guard against
    // spinning, not a threshold.
    for (int i = 0; i < 600000 && state.running; ++i) {
        nes_gui::step_instruction(console, state);
    }

    EXPECT_FALSE(state.trapped) << "halted at PC " << state.trap_pc
                                << " - an idle loop an NMI will break is not a hang";
    EXPECT_TRUE(state.running) << "the free-run stopped before the game started";
    EXPECT_NE(0, console.ppu.registers.PPUMASK & 0x18) << "rendering was never enabled";
    EXPECT_GT(distinct_indices(console.ppu), 5) << "the title screen never drew under the step loop";
}

// The whole machine at once: the controller reaching the game, the game
// responding, and the picture changing as a result.
GTEST_TEST(commercialRom, pressing_start_begins_the_game)
{
    Bus console;
    SKIP_IF_ROM_ABSENT(smb_path(), kHowToGetIt);
    load_smb(console);

    for (int f = 0; f < 120; ++f) {
        console.run_frame();
    }
    // uint16_t, matching the framebuffer's element type - NOT uint8_t.
    //
    // It was uint8_t until the framebuffer widened to carry emphasis, and
    // nothing caught the mismatch: memcpy and memcmp both take void*, so the
    // compiler had nothing to object to, and sizeof(title) silently became half
    // the frame. The test kept passing because the top half of the screen does
    // change, so it was comparing 30,720 pixels' worth of bytes and calling it
    // the frame. Keep this type tracking PPU::framebuffer's.
    uint16_t title[PPU::screen_width * PPU::screen_height];
    static_assert(sizeof(title) == sizeof(console.ppu.framebuffer), "title must cover the WHOLE framebuffer");

    std::memcpy(title, console.ppu.framebuffer, sizeof(title));

    for (int f = 0; f < 10; ++f) {
        console.controllers.set_port(0, Controllers::Start);
        console.run_frame();
    }
    console.controllers.set_port(0, 0);
    for (int f = 0; f < 100; ++f) {
        console.run_frame();
    }

    EXPECT_NE(0, std::memcmp(title, console.ppu.framebuffer, sizeof(title)))
        << "the screen never changed after Start; the controller is not reaching the game";
}

// Sustained mid-frame scrolling, which no test ROM in this suite exercises for
// any length of time.
GTEST_TEST(commercialRom, holding_right_scrolls_the_playfield)
{
    Bus console;
    SKIP_IF_ROM_ABSENT(smb_path(), kHowToGetIt);
    load_smb(console);
    start_a_game(console);

    bool coarse_seen[32] = {false};
    int distinct = 0;

    for (int f = 0; f < 300; ++f) {
        console.controllers.set_port(0, Controllers::Right | Controllers::B);

        const uint64_t started_on = console.ppu.frame;
        int coarse_mid = -1;
        while (console.ppu.frame == started_on) {
            console.clock();
            // Sampled BELOW the status-bar split. At the frame boundary the
            // scroll reads 0, because the status bar is unscrolled and the
            // game has already rewritten t for it - measuring there says
            // "nothing ever scrolls".
            if (console.ppu.scanline == 150 && console.ppu.cycle == 100) {
                coarse_mid = console.ppu.registers.PPUADDR & 0x1F;
            }
        }

        if (coarse_mid >= 0 && !coarse_seen[coarse_mid]) {
            coarse_seen[coarse_mid] = true;
            ++distinct;
        }
    }

    // Measured: 26 of a possible 32.
    EXPECT_GT(distinct, 8) << "the playfield did not scroll; coarse X took only " << distinct << " values";
}

// The sprite-0 status bar split, driven every frame by a real game rather than
// as an isolated hit in a test ROM.
GTEST_TEST(commercialRom, the_status_bar_split_fires_every_frame)
{
    Bus console;
    SKIP_IF_ROM_ABSENT(smb_path(), kHowToGetIt);
    load_smb(console);
    start_a_game(console);

    int frames_with_hit = 0;
    for (int f = 0; f < 300; ++f) {
        console.controllers.set_port(0, Controllers::Right | Controllers::B);

        const uint64_t started_on = console.ppu.frame;
        bool hit = false;
        while (console.ppu.frame == started_on) {
            console.clock();
            if (console.ppu.registers.PPUSTATUS & 0x40) {
                hit = true;
            }
        }
        if (hit) {
            ++frames_with_hit;
        }
    }

    // Measured: 249 of 300. Not every frame - the game stops splitting during
    // the death sequence and the level card, and a Goomba reliably kills Mario
    // somewhere in these 300 frames. The bound is well under that, since this
    // exists to catch "sprite 0 stopped firing", not to pin the exact count.
    EXPECT_GT(frames_with_hit, 100) << "the sprite-0 split fired on only " << frames_with_hit << " of 300 frames";
}

}  // namespace local_rom
}  // namespace tests
