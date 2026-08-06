// The debugger panels.
//
// Split from main.cpp so that the SDL/ImGui bootstrap and the emulator-facing
// UI stay separable: the bootstrap is the only part that knows about windows,
// renderers and textures, and these functions are the only part that knows
// about the Bus. Swapping the backend should not touch this file, and adding a
// panel should not touch main.cpp.
//
// ONE RULE APPLIES THROUGHOUT: nothing here reads emulator state through
// Bus::read or PPU::read. Those are the hardware read ports, and several of
// them have side effects - $2002 clears the vblank flag and the $2005/$2006
// write toggle, $2007 advances the VRAM address and cycles the read buffer,
// $4015 clears the frame interrupt. A debugger that displayed those by reading
// them would silently change the run it is supposed to be observing, and the
// resulting bug would look like an emulation defect. Panels read the member
// arrays directly instead.
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

    // Passed straight through to CPU::clock, which prints a trace line per
    // instruction to stdout.
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

    // Set when the CPU is caught branching to itself - the idiom every test ROM
    // ends on. Latched rather than re-detected so the message survives the
    // frames after the trap.
    bool trapped = false;
    uint16_t trap_pc = 0;
};

// Steps one instruction, reporting whether the CPU trapped (branched to
// itself). Shared by the Step button and the free-run loop so the two cannot
// disagree about what a trap is.
bool step_instruction(Bus& console, FrontendState& state);

// Loads a cartridge and resets the machine. Returns false and fills
// state.status on failure.
bool load_cartridge(Bus& console, FrontendState& state, const std::string& path);

void draw_controls_panel(Bus& console, FrontendState& state);
void draw_cpu_panel(Bus& console);
void draw_ppu_panel(Bus& console);
void draw_memory_panel(Bus& console);
void draw_palette_panel(Bus& console);

}  // namespace nes_gui
