#pragma once

#include <functional>
#include <string>
#include <valarray>
#include <vector>

#include "addressing_types.h"
#include "cpu.h"
#include "device.h"

struct Instruction {
    std::string name;
    Addressing addr_type;
    std::function<void(CPU&)> operation = nullptr;
    std::function<void(CPU&)> addressing = nullptr;
    uint8_t cycles;

    // The "oops" cycle: some instructions spend one extra cycle when the
    // effective address computation crosses a page boundary, because the CPU
    // speculatively reads from the un-carried address first and then has to
    // repeat the read once the high byte is fixed up.
    //
    // This is NOT universal. It applies only to instructions that *read* their
    // operand (LDA abs,X and friends). Stores and read-modify-writes always
    // perform the fix-up read, so their cost is fixed and already includes it:
    // STA abs,X is 5 cycles whether or not it crosses a page, and ASL abs,X is
    // always 7. Marking those would over-count.
    //
    // The eight conditional branches reuse this flag for their own variable
    // cost (+1 when taken, +1 more when the target is on a different page);
    // see CPU::extra_cycles_for_current_instruction().
    //
    // Defaulted to false so that only the instructions that actually pay the
    // penalty have to name it, rather than every one of the 256 table rows.
    bool extra_cycle_on_page_cross = false;
};

struct InstructionSet {
    static const Instruction NMI;
    static const Instruction IRQ;
    static const std::valarray<Instruction> Table;
};
