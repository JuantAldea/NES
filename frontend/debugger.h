// The debugger panels - the part that draws.
//
// Everything the debugger DECIDES lives in debugger_state.h, which includes no
// ImGui and is linked by the test suite. This file is the presentation half:
// it may call ImGui freely and is consequently not covered by any test, so
// keep decisions out of it. If a panel needs to work something out, the working
// out belongs next door where it can be asserted on.
//
// ONE RULE APPLIES THROUGHOUT: nothing here reads emulator state through
// Bus::read or PPU::read. Those are the hardware read ports, and several of
// them have side effects - $2002 clears the vblank flag and the $2005/$2006
// write toggle, $2007 advances the VRAM address and cycles the read buffer,
// $4015 clears the frame interrupt. A debugger that displayed those by reading
// them would silently change the run it is supposed to be observing, and the
// resulting bug would look like an emulation defect. Panels read the member
// arrays directly instead; the one deliberate exception is decoding the opcode
// at PC, which goes through safe_to_peek().
#pragma once

#include "debugger_state.h"

class Bus;

namespace nes_gui
{

void draw_controls_panel(Bus& console, FrontendState& state);
void draw_cpu_panel(Bus& console);
void draw_ppu_panel(Bus& console);
void draw_memory_panel(Bus& console);
void draw_palette_panel(Bus& console);

}  // namespace nes_gui
