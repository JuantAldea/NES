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
    if (console.cpu.registers.PC == previous_pc) {
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
    console.cpu.reset();
    state.trapped = false;
    state.running = false;
    state.status = "Loaded " + path;
    return true;
}

}  // namespace nes_gui
