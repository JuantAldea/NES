// The debugger's policy: everything it decides, with nothing it draws.
//
// Split out of debugger.cpp so it can be tested. The panels call ImGui, which
// needs a live context, a backend and a window - so anything in the same
// translation unit as a draw call is unreachable from a headless test, and the
// whole frontend was consequently untested. A palette panel that displayed four
// colours the PPU never renders shipped because nothing could have caught it.
//
// The split is not only a testing convenience. It draws the line this frontend
// already wanted: this file knows about the emulator and nothing about the
// backend, and debugger.cpp knows about the backend and makes no decisions. The
// backend is the part expected to be replaced again.
//
// NOTHING HERE MAY INCLUDE IMGUI OR SDL. The test binary links this file and
// must build on a machine with neither installed.
#pragma once

#include <cstdint>
#include <string>

class Bus;

namespace nes_gui
{

// Everything the panels remember between frames. The main loop owns one and
// passes it to each panel; nothing here is global.
struct FrontendState {
    // Free-running: the main loop steps whole frames on a wall clock while this
    // is set. Cleared by Pause, by the step buttons, and by a CPU trap.
    bool running = false;

    // Passed through to Bus::trace_cpu, which prints a line per instruction to
    // stdout.
    bool trace = false;

    // Integer scale for the 256x240 screen. Integer only: a fractional scale
    // with nearest-neighbour filtering makes some scanlines taller than others,
    // which looks like a rendering bug and is not one.
    int screen_scale = 2;

    // Buffer behind the ROM path text field. Fixed-size because ImGui's
    // InputText wants a char buffer; 512 is longer than any sane path here.
    char rom_path[512] = {0};

    // Last thing that happened, shown under the controls. Holds load failures,
    // which would otherwise be invisible.
    std::string status;

    // Set when the CPU is caught in a loop nothing can break it out of: a
    // branch to itself with NMI disabled and I set. The self-jump alone is NOT
    // enough - it is also how most games idle between NMIs, SMB included - so
    // the interrupt test is what separates the Blargg end-of-ROM idiom from a
    // game running normally. Latched rather than re-detected so the message
    // survives the frames after the trap.
    bool trapped = false;
    uint16_t trap_pc = 0;
};

// Whether the debugger may read this address to look at it.
//
// $2000-$3FFF and $4000-$401F are hardware ports where a READ has effects: it
// clears the vblank flag, advances the VRAM address, or acknowledges an IRQ. A
// debugger that displayed them by reading them would change the run it exists
// to observe, and the resulting bug would look like an emulation defect.
bool safe_to_peek(const uint16_t addr);

// The mnemonic of the opcode at PC, or "(unreadable)" when PC points somewhere
// reading would have side effects. Code never legitimately executes from the
// register ranges, so that answer means the program has already gone wrong.
const char* instruction_name_at_pc(Bus& console);

// Steps one instruction, reporting whether the CPU trapped (branched to itself)
// or failed to complete one at all. Shared by the Step button and the free-run
// loop so the two cannot disagree about what a trap is.
bool step_instruction(Bus& console, FrontendState& state);

// Loads a cartridge and resets the machine. Returns false and fills
// state.status on failure.
bool load_cartridge(Bus& console, FrontendState& state, const std::string& path);

// One controller port, frozen for the pad diagram to draw.
//
// A struct rather than the panel reaching into Controllers itself, because of
// what the obvious alternative would be. The natural way to display a pad is to
// read $4016 - and that is a hardware port with two side effects: it shifts the
// register one place on every access, and reloads it continuously while the
// strobe is high. A panel drawn that way would eat the bits the running game
// was about to read, so inputs would drop only while the panel was open. That
// is the same shape as the palette bug this file was split out after.
struct ControllerSnapshot {
    // Buttons physically down, in Controllers::Button order (bit 0 = A). The
    // latch the frontend writes, not the shift register, so it stays truthful
    // mid-read: hardware samples the buttons at the latch, not at each read.
    uint8_t held = 0;

    // What the next eight reads of the port would report, LSB first. It
    // diverges from `held` the moment a game starts clocking bits out, which is
    // the reason to show both.
    uint8_t shift = 0;

    // Strobe high: the register is reloaded continuously, so every read returns
    // A. A game that leaves it set is the classic reason only A appears to
    // work, and it is invisible without a panel like this.
    bool strobe = false;
};

// Reads one port without disturbing it. `port` is 0 or 1; anything else returns
// an empty snapshot rather than indexing out of bounds.
ControllerSnapshot peek_controller(Bus& console, int port);

}  // namespace nes_gui
