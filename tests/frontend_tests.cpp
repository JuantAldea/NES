// Guards the debugger's policy: the decisions it makes about the machine it is
// observing.
//
// This file exists because of a bug that shipped. The palette panel indexed
// palette RAM directly, so it displayed the four cells behind
// $3F10/$3F14/$3F18/$3F1C - storage the PPU never reads, because those
// addresses are aliases of $3F00/$04/$08/$0C. It showed four colours the
// hardware does not render, next to the four it does. Nothing caught it; it was
// found by probing a running ROM by hand. Every panel was in the same position:
// hand-written logic against live emulator state, with no assertion anywhere
// near it.
//
// The panels themselves still cannot be tested - ImGui needs a live context, a
// backend and a window. What CAN be tested is everything the debugger decides,
// which is why that was split into frontend/debugger_state.{h,cpp}: a
// translation unit with no ImGui and no SDL, linked here.
//
// The invariant worth the most is the one in safe_to_peek. A debugger that
// reads a hardware port to display it changes the run it exists to observe, and
// the resulting bug looks like an emulation defect rather than a tooling one.
#include <cstdint>
#include <string>

#include "../frontend/debugger_state.h"
#include "../include/bus.h"
#include "../include/frame_dump.h"
#include "gtest/gtest.h"

namespace tests
{
namespace frontend
{
namespace
{

// Puts a program in RAM and points the CPU at it. Avoids needing a cartridge
// for the cases that are only about instruction stepping.
void load_program(Bus& console, const uint16_t addr, const std::vector<uint8_t>& bytes)
{
    console.write_ram(addr, bytes.size(), bytes.data());
    console.cpu.registers.PC = addr;
    console.cpu.cycles_left = 0;  // force a fresh fetch rather than resuming a schedule
}

}  // namespace

// --- safe_to_peek: the invariant that keeps the debugger from lying ---------

GTEST_TEST(frontendSafeToPeek, rejects_every_address_with_read_side_effects)
{
    // $2000-$3FFF: the PPU registers and their mirrors. A read of $2002 clears
    // the vblank flag and the $2005/$2006 write toggle; a read of $2007
    // advances the VRAM address and cycles the read buffer.
    EXPECT_FALSE(nes_gui::safe_to_peek(0x2000));
    EXPECT_FALSE(nes_gui::safe_to_peek(0x2002));
    EXPECT_FALSE(nes_gui::safe_to_peek(0x2007));
    EXPECT_FALSE(nes_gui::safe_to_peek(0x3FFF)) << "the PPU register mirrors run to $3FFF";

    // $4000-$401F: APU and I/O. A read of $4015 acknowledges the frame
    // interrupt; $4016/$4017 clock the controller shift registers.
    EXPECT_FALSE(nes_gui::safe_to_peek(0x4000));
    EXPECT_FALSE(nes_gui::safe_to_peek(0x4015));
    EXPECT_FALSE(nes_gui::safe_to_peek(0x401F)) << "the I/O range ends at $401F inclusive";
}

GTEST_TEST(frontendSafeToPeek, allows_memory_that_is_free_to_read)
{
    EXPECT_TRUE(nes_gui::safe_to_peek(0x0000));
    EXPECT_TRUE(nes_gui::safe_to_peek(0x1FFF)) << "RAM and its mirrors end at $1FFF";
    EXPECT_TRUE(nes_gui::safe_to_peek(0x4020)) << "cartridge space starts one past the I/O range";
    EXPECT_TRUE(nes_gui::safe_to_peek(0x8000));
    EXPECT_TRUE(nes_gui::safe_to_peek(0xFFFF));
}

// The point of the guard, demonstrated rather than asserted about in the
// abstract: decoding the instruction at PC must not disturb the machine.
GTEST_TEST(frontendSafeToPeek, decoding_the_opcode_at_pc_does_not_touch_ppu_state)
{
    Bus console;
    PPU& ppu = console.ppu;

    while (ppu.in_reset_write_lockout()) {
        ppu.clock();
    }

    // Put the PPU in a state a read of $2002 would visibly destroy: vblank set,
    // and the $2005/$2006 write toggle part-way through a pair.
    ppu.set_vblank();
    ppu.write(PPU::PPUADDR, 0x21);  // first of two writes, so w is now 1
    ASSERT_TRUE(ppu.write_toggle_w()) << "precondition: the write toggle is mid-pair";
    ASSERT_NE(0, ppu.registers.PPUSTATUS & 0x80) << "precondition: vblank is set";

    // Aim PC at $2002 - which a real program would never do, and which is
    // exactly when the debugger must not read it.
    console.cpu.registers.PC = 0x2002;

    EXPECT_STREQ("(unreadable)", nes_gui::instruction_name_at_pc(console));

    EXPECT_NE(0, ppu.registers.PPUSTATUS & 0x80) << "vblank was cleared - the debugger read $2002";
    EXPECT_TRUE(ppu.write_toggle_w()) << "the write toggle was reset - the debugger read $2002";
}

GTEST_TEST(frontendSafeToPeek, decodes_a_real_opcode_when_pc_points_at_memory)
{
    Bus console;
    load_program(console, 0x0300, {0xEA});  // NOP
    console.cpu.registers.PC = 0x0300;

    EXPECT_STREQ("NOP", nes_gui::instruction_name_at_pc(console));
}

// --- step_instruction: what the Step button and the free-run loop share -----

GTEST_TEST(frontendStep, reports_a_trap_when_the_cpu_branches_to_itself)
{
    Bus console;
    nes_gui::FrontendState state;
    state.running = true;

    // JMP $0300 at $0300 - the idiom every Blargg ROM ends on.
    load_program(console, 0x0300, {0x4C, 0x00, 0x03});

    EXPECT_FALSE(nes_gui::step_instruction(console, state)) << "a self-jump must report as a trap";
    EXPECT_TRUE(state.trapped);
    EXPECT_EQ(0x0300, state.trap_pc);
    EXPECT_FALSE(state.running) << "a trap must stop the free-run loop, or the UI spins";
}

GTEST_TEST(frontendStep, an_ordinary_instruction_is_not_a_trap)
{
    Bus console;
    nes_gui::FrontendState state;
    state.trapped = true;  // stale from an earlier trap; stepping on must clear it

    load_program(console, 0x0300, {0xEA, 0xEA});  // NOP, NOP

    EXPECT_TRUE(nes_gui::step_instruction(console, state));
    EXPECT_FALSE(state.trapped) << "the latched trap must clear once the CPU moves again";
    EXPECT_NE(0x0300, console.cpu.registers.PC) << "PC must have advanced";
}

// A branch that jumps back to its own opcode is a trap; one that merely jumps
// backwards is not. Without this, any loop would be reported as a hang.
GTEST_TEST(frontendStep, a_backward_branch_that_is_not_self_targeted_is_not_a_trap)
{
    Bus console;
    nes_gui::FrontendState state;

    // $0300: NOP
    // $0301: BNE -3  -> back to $0300, not to $0301
    load_program(console, 0x0300, {0xEA, 0xD0, 0xFD});
    console.cpu.set_flag(CPU::FLAGS::Z, false);  // ensure the branch is taken

    ASSERT_TRUE(nes_gui::step_instruction(console, state));  // the NOP
    EXPECT_TRUE(nes_gui::step_instruction(console, state)) << "a loop is not a trap";
    EXPECT_FALSE(state.trapped);
    EXPECT_EQ(0x0300, console.cpu.registers.PC) << "the branch was taken";
}

// An idle loop waited on by an interrupt is not a hang, and this is the shape
// nearly every commercial game's main loop takes: init ends on `jmp *`, and the
// NMI handler does the work. Reporting that as a trap stopped the free-run on
// the last instruction before the game started - found by running SMB in the
// debugger, where it halted at $8057 on frame 3 with a blank screen.
GTEST_TEST(frontendStep, a_self_jump_is_not_a_trap_while_nmi_can_break_it)
{
    Bus console;
    nes_gui::FrontendState state;
    state.running = true;

    console.ppu.MMI_on_V_Blank = true;  // PPUCTRL bit 7, as SMB leaves it ($90)
    load_program(console, 0x0300, {0x4C, 0x00, 0x03});

    EXPECT_TRUE(nes_gui::step_instruction(console, state)) << "an NMI-woken idle loop is not a hang";
    EXPECT_FALSE(state.trapped);
    EXPECT_TRUE(state.running) << "the free-run must survive the game's main loop";
}

GTEST_TEST(frontendStep, a_self_jump_is_not_a_trap_while_irqs_are_enabled)
{
    Bus console;
    nes_gui::FrontendState state;
    state.running = true;

    console.ppu.MMI_on_V_Blank = false;
    console.cpu.set_flag(CPU::FLAGS::I, false);  // an IRQ source can still reach it
    load_program(console, 0x0300, {0x4C, 0x00, 0x03});

    EXPECT_TRUE(nes_gui::step_instruction(console, state));
    EXPECT_FALSE(state.trapped);
}

// The other half of the rule: with both interrupts shut out, the loop really is
// unbreakable, and must still be caught. This is the Blargg end-of-ROM idiom.
GTEST_TEST(frontendStep, a_self_jump_with_no_interrupt_left_is_still_a_trap)
{
    Bus console;
    nes_gui::FrontendState state;
    state.running = true;

    console.ppu.MMI_on_V_Blank = false;
    console.cpu.set_flag(CPU::FLAGS::I, true);
    load_program(console, 0x0300, {0x4C, 0x00, 0x03});

    EXPECT_FALSE(nes_gui::step_instruction(console, state));
    EXPECT_TRUE(state.trapped);
}

// --- the controller pad panel ----------------------------------------------

GTEST_TEST(frontendController, reports_the_buttons_that_are_held)
{
    Bus console;
    console.controllers.set_port(0, Controllers::A | Controllers::Right);
    console.controllers.set_port(1, Controllers::Start);

    EXPECT_EQ(Controllers::A | Controllers::Right, nes_gui::peek_controller(console, 0).held);
    EXPECT_EQ(Controllers::Start, nes_gui::peek_controller(console, 1).held) << "the ports must not be confused";
}

// THE test for this panel, and the reason peek_controller exists at all.
//
// $4016 is a hardware port: reading it shifts the register one place. A panel
// that displayed the pad by reading it would consume the bits the running game
// was about to read, so the game would drop inputs only while the panel was
// open - an emulation bug that is really a tooling one. Same class as the
// palette bug in this file's header.
GTEST_TEST(frontendController, peeking_does_not_shift_the_register_under_the_game)
{
    // The sequence a game uses: strobe high to latch, low, then eight reads.
    const auto read_pad = [](Bus& console, const bool peek_between) {
        console.write(0x4016, 1);
        console.write(0x4016, 0);

        uint8_t bits = 0;
        for (int i = 0; i < 8; ++i) {
            if (peek_between) {
                (void)nes_gui::peek_controller(console, 0);
            }
            bits |= static_cast<uint8_t>((console.read(0x4016) & 1) << i);
        }
        return bits;
    };

    Bus undisturbed;
    undisturbed.controllers.set_port(0, Controllers::A | Controllers::Right);
    const uint8_t expected = read_pad(undisturbed, false);

    Bus observed;
    observed.controllers.set_port(0, Controllers::A | Controllers::Right);
    const uint8_t actual = read_pad(observed, true);

    EXPECT_EQ(Controllers::A | Controllers::Right, expected) << "the read sequence itself is wrong; fix that first";
    EXPECT_EQ(expected, actual) << "the panel ate bits the game was about to read";
}

// The half of the snapshot that is not the picture: `shift` must track what the
// port will report next, or the panel would show a pad that is already spent as
// though it were still full.
GTEST_TEST(frontendController, the_snapshot_follows_the_register_as_it_clocks_out)
{
    Bus console;
    console.controllers.set_port(0, Controllers::A);

    console.write(0x4016, 1);
    EXPECT_TRUE(nes_gui::peek_controller(console, 0).strobe) << "strobe high must be visible - it pins reads to A";
    console.write(0x4016, 0);

    EXPECT_EQ(Controllers::A, nes_gui::peek_controller(console, 0).shift) << "latched, nothing read yet";
    (void)console.read(0x4016);
    EXPECT_EQ(0x80, nes_gui::peek_controller(console, 0).shift) << "one bit out, a 1 shifted in at the top";
    EXPECT_EQ(Controllers::A, nes_gui::peek_controller(console, 0).held) << "the latch is unchanged by reading";
}

// --- load_cartridge --------------------------------------------------------

// Bus's constructor resets the CPU before any cartridge is mapped, and reset()
// subtracts 3 from S rather than reloading it, so a second reset in the load
// path left S at $FA. It compounded: this function is also the drag-and-drop
// handler, so dropping three ROMs on the window walked S to $F7 and the panel
// reported it as fact. SMB hides this by doing `ldx #$FF / txs` itself.
GTEST_TEST(frontendLoad, loading_a_cartridge_powers_on_rather_than_resetting_again)
{
    Bus console;
    nes_gui::FrontendState state;

    const std::string path = std::string(NES_TEST_FILES_DIR) + "/sprite_hit/01.basics.nes";
    ASSERT_TRUE(nes_gui::load_cartridge(console, state, path))
        << "sprite_hit ROMs absent - run tests/test_files/fetch_sprite_hit.sh";
    EXPECT_EQ(0xFD, console.cpu.registers.SP) << "a freshly inserted cartridge is a power-on";

    ASSERT_TRUE(nes_gui::load_cartridge(console, state, path));
    EXPECT_EQ(0xFD, console.cpu.registers.SP) << "S must not walk down on every ROM loaded";
}

GTEST_TEST(frontendLoad, reports_a_missing_file_rather_than_failing_silently)
{
    Bus console;
    nes_gui::FrontendState state;

    EXPECT_FALSE(nes_gui::load_cartridge(console, state, "/nonexistent-directory/nope.nes"));
    EXPECT_NE(std::string::npos, state.status.find("Failed"))
        << "a load failure the user cannot see is worse than none: status was '" << state.status << "'";
}

GTEST_TEST(frontendLoad, an_empty_path_is_rejected_before_it_reaches_the_loader)
{
    Bus console;
    nes_gui::FrontendState state;

    EXPECT_FALSE(nes_gui::load_cartridge(console, state, ""));
    EXPECT_FALSE(state.status.empty()) << "pressing Load with an empty box must say something";
}

GTEST_TEST(frontendLoad, a_successful_load_resets_the_cpu_to_the_cartridge_vector)
{
    Bus console;
    nes_gui::FrontendState state;
    state.trapped = true;
    state.running = true;

    const std::string path = std::string(NES_TEST_FILES_DIR) + "/sprite_hit/01.basics.nes";
    ASSERT_TRUE(nes_gui::load_cartridge(console, state, path))
        << "sprite_hit ROMs absent - run tests/test_files/fetch_sprite_hit.sh";

    // The reset vector lives in the cartridge, so this only works if the reset
    // happens AFTER the mapping. Loading without it left the CPU at whatever
    // vector the previous cartridge had.
    const uint16_t vector = static_cast<uint16_t>(console.read(0xFFFC) | (console.read(0xFFFD) << 8));
    EXPECT_EQ(vector, console.cpu.registers.PC) << "PC must be the new cartridge's reset vector";

    EXPECT_FALSE(state.trapped) << "a fresh cartridge must clear a stale trap";
    EXPECT_FALSE(state.running) << "loading must not leave the machine running";
}

// --- the conversion the screen and the palette panel share ------------------

// Both the screen texture and the palette swatches go through
// palette_index_to_rgb. This pins the channel order, because a red/blue swap
// there would be consistent across the whole frontend and so invisible by eye.
GTEST_TEST(frontendPalette, index_to_rgb_keeps_the_channels_in_order)
{
    const uint32_t palette[4] = {0x000000, 0xFF0000, 0x00FF00, 0x0000FF};

    EXPECT_EQ(0xFF0000u, nes::palette_index_to_rgb(1, palette, 4));
    EXPECT_EQ(0x00FF00u, nes::palette_index_to_rgb(2, palette, 4));
    EXPECT_EQ(0x0000FFu, nes::palette_index_to_rgb(3, palette, 4));
}

// The bug this file was written for. $3F10/$3F14/$3F18/$3F1C are aliases, so
// the cells behind them are never written and never read; a viewer indexing
// palette RAM raw shows four colours the PPU does not use.
GTEST_TEST(frontendPalette, the_mirrored_entries_resolve_to_the_ones_the_ppu_renders)
{
    EXPECT_EQ(0x00, PPU::palette_offset(0x3F10)) << "$3F10 is an alias of $3F00";
    EXPECT_EQ(0x04, PPU::palette_offset(0x3F14));
    EXPECT_EQ(0x08, PPU::palette_offset(0x3F18));
    EXPECT_EQ(0x0C, PPU::palette_offset(0x3F1C));

    // The neighbours are NOT aliases and must be left alone - folding those too
    // would hide three quarters of each sprite palette.
    EXPECT_EQ(0x11, PPU::palette_offset(0x3F11));
    EXPECT_EQ(0x12, PPU::palette_offset(0x3F12));
    EXPECT_EQ(0x13, PPU::palette_offset(0x3F13));
}

// Demonstrated end to end: write distinguishable values through the alias and
// its target, then confirm the panel's lookup shows what rendering would use.
GTEST_TEST(frontendPalette, a_write_through_the_alias_is_what_the_panel_displays)
{
    Bus console;
    PPU& ppu = console.ppu;

    while (ppu.in_reset_write_lockout()) {
        ppu.clock();
    }

    // $3F00 and $3F10 are the same cell, so the second write wins for both.
    ppu.write(PPU::PPUADDR, 0x3F);
    ppu.write(PPU::PPUADDR, 0x00);
    ppu.write(PPU::PPUDATA, 0x21);

    ppu.write(PPU::PPUADDR, 0x3F);
    ppu.write(PPU::PPUADDR, 0x10);
    ppu.write(PPU::PPUDATA, 0x0F);

    EXPECT_EQ(0x0F, ppu.palette_ram[PPU::palette_offset(0x3F00)]);
    EXPECT_EQ(0x0F, ppu.palette_ram[PPU::palette_offset(0x3F10)])
        << "the alias and its target must display the same colour";
}

// MUTING AND PAUSING ARE NOT THE SAME SILENCE, and all four rows are asserted
// because three of them agree - only one separates the two ideas.
//
// This is policy in debugger_state for the reason the header of this file gives:
// main.cpp cannot be linked here, so a mute that quietly stopped working would
// be invisible until somebody noticed the noise. Which is how it was noticed the
// first time - run_functional.sh opens twelve frontend windows, each with a live
// audio device.
GTEST_TEST(frontendAudio, muting_stops_the_sampler_and_pausing_only_stops_the_device)
{
    using nes_gui::audio_intent;

    // Running and unmuted: the only combination that makes sound.
    EXPECT_TRUE(audio_intent(false, true).sampler_enabled);
    EXPECT_TRUE(audio_intent(false, true).device_running);

    // Paused. The device stops but the sampler stays ON - a paused machine is
    // not clocking, so nothing is produced anyway, and leaving it enabled is
    // what lets sound resume without a gap.
    EXPECT_TRUE(audio_intent(false, false).sampler_enabled) << "pausing must not disable the sampler";
    EXPECT_FALSE(audio_intent(false, false).device_running);

    // Muted while running. THE ROW THAT MATTERS: the machine is still clocking,
    // so the sampler has to stop too. Left enabled it would fill a ring nobody
    // drains, and every sample would be counted as dropped.
    EXPECT_FALSE(audio_intent(true, true).sampler_enabled) << "muting must stop the sampler, not just the device";
    EXPECT_FALSE(audio_intent(true, true).device_running);

    EXPECT_FALSE(audio_intent(true, false).sampler_enabled);
    EXPECT_FALSE(audio_intent(true, false).device_running);
}

GTEST_TEST(frontendAudio, a_frontend_starts_unmuted)
{
    // --mute is opt-in: somebody launching the emulator wants sound. The
    // functional pass passes it explicitly, which is the point of it being a
    // flag rather than a default.
    const nes_gui::FrontendState state;
    EXPECT_FALSE(state.muted);
}

}  // namespace frontend
}  // namespace tests
