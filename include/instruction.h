#pragma once

#include <functional>
#include <string>
#include <valarray>
#include <vector>

#include "addressing_types.h"
#include "cpu.h"
#include "device.h"

// One row of the opcode table: what the instruction is called, how it finds its
// operand, and what it does with it.
//
// Deliberately absent: a cycle count, an addressing routine, and a flag for the
// "oops" cycle.
//
// Timing is not something an opcode declares. The 6502 drives the bus on every
// cycle it runs, so an instruction's length is exactly the number of accesses
// it makes - and those come from the cycle schedule CPU::step() runs, chosen
// from `addr_type` together with what the mnemonic does to memory. A count
// restated here would only be somewhere for the truth to be contradicted.
//
// Nor is there an addressing function, because addressing is not a step that
// happens before the instruction: it is spread across the instruction's first
// cycles, one byte per cycle, interleaved with the instruction's own work. JSR
// reads the low byte of its target on cycle 2 and the high byte on cycle 6,
// with three stack accesses in between; there is no moment at which "the
// addressing mode runs".
//
// The published per-opcode timings still exist as an oracle - in
// tests/cpu_cycle_tests.cpp, transcribed from a reference by hand, precisely so
// that they are not derived from this file.
struct Instruction {
    std::string name;
    Addressing addr_type;
    std::function<void(CPU&)> operation = nullptr;
};

struct InstructionSet {
    static const Instruction NMI;
    static const Instruction IRQ;
    static const std::valarray<Instruction> Table;
};
