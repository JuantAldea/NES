#include "debugger_state.h"

#include "../include/bus.h"
#include "../include/instruction.h"

namespace nes_gui
{

bool safe_to_peek(const uint16_t addr) { return addr < 0x2000 || addr > 0x401F; }

const char* instruction_name_at_pc(Bus& console)
{
    const uint16_t pc = console.cpu.registers.PC;
    if (!safe_to_peek(pc)) {
        return "(unreadable)";
    }
    return InstructionSet::Table[console.read(pc)].name.c_str();
}

bool step_instruction(Bus& console, FrontendState& state)
{
    const uint16_t previous_pc = console.cpu.registers.PC;

    if (!console.step_instruction()) {
        state.trapped = true;
        state.trap_pc = previous_pc;
        state.running = false;
        state.status = "CPU never completed an instruction - jammed opcode?";
        return false;
    }

    // Every one of the test ROMs ends on a branch to itself, and so does a
    // runaway program that fell into one. Catching it is the difference between
    // "the emulator stopped" and a spinning UI.
    //
    // But a self-jump is only a hang if nothing can leave it, and that
    // distinction is not academic: Super Mario Bros' reset routine ENDS on one.
    // Measured on tests/test_files/local/smb.nes - the last three bytes of init
    // are `4C 57 80`, `jmp $8057` at $8057, reached at frame 3 with PPUCTRL
    // $90, and the whole game runs out of the NMI handler from there. Treating
    // that as a trap stopped the debugger on the last instruction before the
    // game starts, i.e. it could not run a commercial game at all. That is
    // most of them: an idle loop woken by NMI is the standard main-loop shape.
    //
    // So it is a trap only when no interrupt can break it - NMI off in PPUCTRL,
    // and I set against every IRQ source. Reading both non-destructively
    // matters as much here as in the panels: MMI_on_V_Blank is the decoded
    // flag, not a $2002 read that would clear vblank underneath the run.
    //
    // The asymmetry is deliberate. A missed trap costs a free-run that spins
    // until Pause, which the UI survives; a false trap costs the tool the only
    // thing it is for. When in doubt, keep running.
    const bool nmi_can_break_it = console.ppu.MMI_on_V_Blank;
    const bool irq_can_break_it = (console.cpu.registers.P & static_cast<uint8_t>(CPU::FLAGS::I)) == 0;

    if (console.cpu.registers.PC == previous_pc && !nmi_can_break_it && !irq_can_break_it) {
        state.trapped = true;
        state.trap_pc = previous_pc;
        state.running = false;
        return false;
    }

    state.trapped = false;
    return true;
}

bool load_cartridge(Bus& console, FrontendState& state, const std::string& path)
{
    if (path.empty()) {
        state.status = "No ROM path given";
        return false;
    }

    if (!console.load_cartridge(path)) {
        state.status = "Failed to load " + path;
        return false;
    }

    // The reset vector lives in the cartridge, so it can only be fetched once
    // the cartridge is mapped.
    //
    // power_on(), not reset(), for the reason local_rom_tests.cpp spells out at
    // its own call site: Bus's constructor already reset this CPU before any
    // cartridge was mapped, and reset() "subtracts 3 from S, nothing more"
    // rather than reloading it. A second reset therefore leaves S at $FA
    // instead of $FD, and because this function also serves the drag-and-drop
    // path, S walked down 3 more on every ROM dropped onto the window - $FD,
    // $FA, $F7. Inserting a cartridge is a power-on, so say so.
    console.cpu.power_on();
    state.trapped = false;
    state.running = false;
    state.status = "Loaded " + path;
    return true;
}

}  // namespace nes_gui
