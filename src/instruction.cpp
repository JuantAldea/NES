#include "instruction.h"

// clang-format off
using c = CPU;
const Instruction InstructionSet::NMI{"NMI", Addressing::implicit, &c::NMI, &c::addressing_implicit, 8};
const Instruction InstructionSet::IRQ{"IRQ", Addressing::implicit, &c::IRQ, &c::addressing_implicit, 7};

//Aligned to 10 so that they are easier to count
const std::valarray<Instruction> InstructionSet::Table{
    {"BRK", Addressing::implicit, &c::BRK, &c::addressing_implicit, 7},
    {"ORA", Addressing::indexed_indirect, &c::ORA, &c::addressing_indexed_indirect, 6},
    {"STP", Addressing::implicit, &c::STP, &c::addressing_implicit, 0},
    {"SLO", Addressing::indexed_indirect, &c::SLO, &c::addressing_indexed_indirect, 8},
    {"NOP", Addressing::zero_page, &c::NOP, &c::addressing_zero_page, 3},
    {"ORA", Addressing::zero_page, &c::ORA, &c::addressing_zero_page, 3},
    {"ASL", Addressing::zero_page, &c::ASL, &c::addressing_zero_page, 5},
    {"SLO", Addressing::zero_page, &c::SLO, &c::addressing_zero_page, 5},
    {"PHP", Addressing::implicit, &c::PHP, &c::addressing_implicit, 3},
    {"ORA", Addressing::immediate, &c::ORA, &c::addressing_immediate, 2},
    {"ASL", Addressing::implicit, &c::ASL, &c::addressing_implicit, 2},
    {"ANC", Addressing::immediate, &c::ANC, &c::addressing_immediate, 2},
    {"NOP", Addressing::absolute, &c::NOP, &c::addressing_absolute, 4},
    {"ORA", Addressing::absolute, &c::ORA, &c::addressing_absolute, 4},
    {"ASL", Addressing::absolute, &c::ASL, &c::addressing_absolute, 6},
    {"SLO", Addressing::absolute, &c::SLO, &c::addressing_absolute, 6},
    {"BPL", Addressing::relative, &c::BPL, &c::addressing_relative, 2},                  // oops cycle
    {"ORA", Addressing::indirect_indexed, &c::ORA, &c::addressing_indirect_indexed, 5},  // oops cycle
    {"STP", Addressing::implicit, &c::STP, &c::addressing_implicit, 0},
    {"SLO", Addressing::indirect_indexed, &c::SLO, &c::addressing_indirect_indexed, 8},
    {"NOP", Addressing::zero_page_X, &c::NOP, &c::addressing_zero_page_X, 4},
    {"ORA", Addressing::zero_page_X, &c::ORA, &c::addressing_zero_page_X, 4},
    {"ASL", Addressing::zero_page_X, &c::ASL, &c::addressing_zero_page_X, 6},
    {"SLO", Addressing::zero_page_X, &c::SLO, &c::addressing_zero_page_X, 6},
    {"CLC", Addressing::implicit, &c::CLC, &c::addressing_implicit, 2},
    {"ORA", Addressing::absolute_Y, &c::ORA, &c::addressing_absolute_Y, 4},  // oops cycle
    {"NOP", Addressing::implicit, &c::NOP, &c::addressing_implicit, 2},
    {"SLO", Addressing::absolute_Y, &c::SLO, &c::addressing_absolute_Y, 7},
    {"NOP", Addressing::absolute_X, &c::NOP, &c::addressing_absolute_X, 4},  // oops cycle
    {"ORA", Addressing::absolute_X, &c::ORA, &c::addressing_absolute_X, 4},  // oops cycle
    {"ASL", Addressing::absolute_X, &c::ASL, &c::addressing_absolute_X, 7},
    {"SLO", Addressing::absolute_X, &c::SLO, &c::addressing_absolute_X, 7},
    {"JSR", Addressing::absolute, &c::JSR, &c::addressing_absolute, 6},
    {"AND", Addressing::indexed_indirect, &c::AND, &c::addressing_indexed_indirect, 6},
    {"STP", Addressing::implicit, &c::STP, &c::addressing_implicit, 0},
    {"RLA", Addressing::indexed_indirect, &c::RLA, &c::addressing_indexed_indirect, 8},
    {"BIT", Addressing::zero_page, &c::BIT, &c::addressing_zero_page, 3},
    {"AND", Addressing::zero_page, &c::AND, &c::addressing_zero_page, 3},
    {"ROL", Addressing::zero_page, &c::ROL, &c::addressing_zero_page, 5},
    {"RLA", Addressing::zero_page, &c::RLA, &c::addressing_zero_page, 5},
    {"PLP", Addressing::implicit, &c::PLP, &c::addressing_implicit, 4},
    {"AND", Addressing::immediate, &c::AND, &c::addressing_immediate, 2},
    {"ROL", Addressing::implicit, &c::ROL, &c::addressing_implicit, 2},
    {"ANC", Addressing::immediate, &c::ANC, &c::addressing_immediate, 2},
    {"BIT", Addressing::absolute, &c::BIT, &c::addressing_absolute, 4},
    {"AND", Addressing::absolute, &c::AND, &c::addressing_absolute, 4},
    {"ROL", Addressing::absolute, &c::ROL, &c::addressing_absolute, 6},
    {"RLA", Addressing::absolute, &c::RLA, &c::addressing_absolute, 6},
    {"BMI", Addressing::relative, &c::BMI, &c::addressing_relative, 2},                  // oops cycle
    {"AND", Addressing::indirect_indexed, &c::AND, &c::addressing_indirect_indexed, 5},  // oops cycle
    {"STP", Addressing::implicit, &c::STP, &c::addressing_implicit, 0},
    {"RLA", Addressing::indirect_indexed, &c::RLA, &c::addressing_indirect_indexed, 8},
    {"NOP", Addressing::zero_page_X, &c::NOP, &c::addressing_zero_page_X, 4},
    {"AND", Addressing::zero_page_X, &c::AND, &c::addressing_zero_page_X, 4},
    {"ROL", Addressing::zero_page_X, &c::ROL, &c::addressing_zero_page_X, 6},
    {"RLA", Addressing::zero_page_X, &c::RLA, &c::addressing_zero_page_X, 6},
    {"SEC", Addressing::implicit, &c::SEC, &c::addressing_implicit, 2},
    {"AND", Addressing::absolute_Y, &c::AND, &c::addressing_absolute_Y, 4},  // oops cycle
    {"NOP", Addressing::implicit, &c::NOP, &c::addressing_implicit, 2},
    {"RLA", Addressing::absolute_Y, &c::RLA, &c::addressing_absolute_Y, 7},
    {"NOP", Addressing::absolute_X, &c::NOP, &c::addressing_absolute_X, 4},  // oops cycle
    {"AND", Addressing::absolute_X, &c::AND, &c::addressing_absolute_X, 4},  // oops cycle
    {"ROL", Addressing::absolute_X, &c::ROL, &c::addressing_absolute_X, 7},
    {"RLA", Addressing::absolute_X, &c::RLA, &c::addressing_absolute_X, 7},
    {"RTI", Addressing::implicit, &c::RTI, &c::addressing_implicit, 6},
    {"EOR", Addressing::indexed_indirect, &c::EOR, &c::addressing_indexed_indirect, 6},
    {"STP", Addressing::implicit, &c::STP, &c::addressing_implicit, 0},
    {"SRE", Addressing::indexed_indirect, &c::SRE, &c::addressing_indexed_indirect, 8},
    {"NOP", Addressing::zero_page, &c::NOP, &c::addressing_zero_page, 3},
    {"EOR", Addressing::zero_page, &c::EOR, &c::addressing_zero_page, 3},
    {"LSR", Addressing::zero_page, &c::LSR, &c::addressing_zero_page, 5},
    {"SRE", Addressing::zero_page, &c::SRE, &c::addressing_zero_page, 5},
    {"PHA", Addressing::implicit, &c::PHA, &c::addressing_implicit, 3},
    {"EOR", Addressing::immediate, &c::EOR, &c::addressing_immediate, 2},
    {"LSR", Addressing::implicit, &c::LSR, &c::addressing_implicit, 2},
    {"ALR", Addressing::immediate, &c::ALR, &c::addressing_immediate, 2},
    {"JMP", Addressing::absolute, &c::JMP, &c::addressing_absolute, 3},
    {"EOR", Addressing::absolute, &c::EOR, &c::addressing_absolute, 4},
    {"LSR", Addressing::absolute, &c::LSR, &c::addressing_absolute, 6},
    {"SRE", Addressing::absolute, &c::SRE, &c::addressing_absolute, 6},
    {"BVC", Addressing::relative, &c::BVC, &c::addressing_relative, 2},                  // oops cycle
    {"EOR", Addressing::indirect_indexed, &c::EOR, &c::addressing_indirect_indexed, 5},  // oops cycle
    {"STP", Addressing::implicit, &c::STP, &c::addressing_implicit, 0},
    {"SRE", Addressing::indirect_indexed, &c::SRE, &c::addressing_indirect_indexed, 8},
    {"NOP", Addressing::zero_page_X, &c::NOP, &c::addressing_zero_page_X, 4},
    {"EOR", Addressing::zero_page_X, &c::EOR, &c::addressing_zero_page_X, 4},
    {"LSR", Addressing::zero_page_X, &c::LSR, &c::addressing_zero_page_X, 6},
    {"SRE", Addressing::zero_page_X, &c::SRE, &c::addressing_zero_page_X, 6},
    {"CLI", Addressing::implicit, &c::CLI, &c::addressing_implicit, 2},
    {"EOR", Addressing::absolute_Y, &c::EOR, &c::addressing_absolute_Y, 4},  // oops cycle
    {"NOP", Addressing::implicit, &c::NOP, &c::addressing_implicit, 2},
    {"SRE", Addressing::absolute_Y, &c::SRE, &c::addressing_absolute_Y, 7},
    {"NOP", Addressing::absolute_X, &c::NOP, &c::addressing_absolute_X, 4},  // oops cycle
    {"EOR", Addressing::absolute_X, &c::EOR, &c::addressing_absolute_X, 4},  // oops cycle
    {"LSR", Addressing::absolute_X, &c::LSR, &c::addressing_absolute_X, 7},
    {"SRE", Addressing::absolute_X, &c::SRE, &c::addressing_absolute_X, 7},
    {"RTS", Addressing::implicit, &c::RTS, &c::addressing_implicit, 6},
    {"ADC", Addressing::indexed_indirect, &c::ADC, &c::addressing_indexed_indirect, 6},
    {"STP", Addressing::implicit, &c::STP, &c::addressing_implicit, 0},
    {"RRA", Addressing::indexed_indirect, &c::RRA, &c::addressing_indexed_indirect, 8},
    {"NOP", Addressing::zero_page, &c::NOP, &c::addressing_zero_page, 3},
    {"ADC", Addressing::zero_page, &c::ADC, &c::addressing_zero_page, 3},
    {"ROR", Addressing::zero_page, &c::ROR, &c::addressing_zero_page, 5},
    {"RRA", Addressing::zero_page, &c::RRA, &c::addressing_zero_page, 5},
    {"PLA", Addressing::implicit, &c::PLA, &c::addressing_implicit, 4},
    {"ADC", Addressing::immediate, &c::ADC, &c::addressing_immediate, 2},
    {"ROR", Addressing::implicit, &c::ROR, &c::addressing_implicit, 2},
    {"ARR", Addressing::immediate, &c::ARR, &c::addressing_immediate, 2},
    {"JMP", Addressing::addressing_indirect, &c::JMP, &c::addressing_indirect, 5},
    {"ADC", Addressing::absolute, &c::ADC, &c::addressing_absolute, 4},
    {"ROR", Addressing::absolute, &c::ROR, &c::addressing_absolute, 6},
    {"RRA", Addressing::absolute, &c::RRA, &c::addressing_absolute, 6},
    {"BVS", Addressing::relative, &c::BVS, &c::addressing_relative, 2},                  // oops cycle
    {"ADC", Addressing::indirect_indexed, &c::ADC, &c::addressing_indirect_indexed, 5},  // oops cycle
    {"STP", Addressing::implicit, &c::STP, &c::addressing_implicit, 0},
    {"RRA", Addressing::indirect_indexed, &c::RRA, &c::addressing_indirect_indexed, 8},
    {"NOP", Addressing::zero_page_X, &c::NOP, &c::addressing_zero_page_X, 4},
    {"ADC", Addressing::zero_page_X, &c::ADC, &c::addressing_zero_page_X, 4},
    {"ROR", Addressing::zero_page_X, &c::ROR, &c::addressing_zero_page_X, 6},
    {"RRA", Addressing::zero_page_X, &c::RRA, &c::addressing_zero_page_X, 6},
    {"SEI", Addressing::implicit, &c::SEI, &c::addressing_implicit, 2},
    {"ADC", Addressing::absolute_Y, &c::ADC, &c::addressing_absolute_Y, 4},  // oops cycle
    {"NOP", Addressing::implicit, &c::NOP, &c::addressing_implicit, 2},
    {"RRA", Addressing::absolute_Y, &c::RRA, &c::addressing_absolute_Y, 7},
    {"NOP", Addressing::absolute_X, &c::NOP, &c::addressing_absolute_X, 4},  // oops cycle
    {"ADC", Addressing::absolute_X, &c::ADC, &c::addressing_absolute_X, 4},  // oops cycle
    {"ROR", Addressing::absolute_X, &c::ROR, &c::addressing_absolute_X, 7},
    {"RRA", Addressing::absolute_X, &c::RRA, &c::addressing_absolute_X, 7},
    {"NOP", Addressing::immediate, &c::NOP, &c::addressing_immediate, 2},
    {"STA", Addressing::indexed_indirect, &c::STA, &c::addressing_indexed_indirect, 6},
    {"NOP", Addressing::immediate, &c::NOP, &c::addressing_immediate, 2},
    {"SAX", Addressing::indexed_indirect, &c::SAX, &c::addressing_indexed_indirect, 6},
    {"STY", Addressing::zero_page, &c::STY, &c::addressing_zero_page, 3},
    {"STA", Addressing::zero_page, &c::STA, &c::addressing_zero_page, 3},
    {"STX", Addressing::zero_page, &c::STX, &c::addressing_zero_page, 3},
    {"SAX", Addressing::zero_page, &c::SAX, &c::addressing_zero_page, 3},
    {"DEY", Addressing::implicit, &c::DEY, &c::addressing_implicit, 2},
    {"NOP", Addressing::immediate, &c::NOP, &c::addressing_immediate, 2},
    {"TXA", Addressing::implicit, &c::TXA, &c::addressing_implicit, 2},
    {"XAA", Addressing::immediate, &c::XAA, &c::addressing_immediate, 2},
    {"STY", Addressing::absolute, &c::STY, &c::addressing_absolute, 4},
    {"STA", Addressing::absolute, &c::STA, &c::addressing_absolute, 4},
    {"STX", Addressing::absolute, &c::STX, &c::addressing_absolute, 4},
    {"SAX", Addressing::absolute, &c::SAX, &c::addressing_absolute, 4},
    {"BCC", Addressing::relative, &c::BCC, &c::addressing_relative, 2},  // oops cycle
    {"STA", Addressing::indirect_indexed, &c::STA, &c::addressing_indirect_indexed, 6},
    {"STP", Addressing::implicit, &c::STP, &c::addressing_implicit, 0},
    {"AHX", Addressing::indirect_indexed, &c::AHX, &c::addressing_indirect_indexed, 6},
    {"STY", Addressing::zero_page_X, &c::STY, &c::addressing_zero_page_X, 4},
    {"STA", Addressing::zero_page_X, &c::STA, &c::addressing_zero_page_X, 4},
    {"STX", Addressing::zero_page_Y, &c::STX, &c::addressing_zero_page_Y, 4},
    {"SAX", Addressing::zero_page_Y, &c::SAX, &c::addressing_zero_page_Y, 4},
    {"TYA", Addressing::implicit, &c::TYA, &c::addressing_implicit, 2},
    {"STA", Addressing::absolute_Y, &c::STA, &c::addressing_absolute_Y, 5},
    {"TXS", Addressing::implicit, &c::TXS, &c::addressing_implicit, 2},
    {"TAS", Addressing::absolute_Y, &c::TAS, &c::addressing_absolute_Y, 5},
    {"SHY", Addressing::absolute_X, &c::SHY, &c::addressing_absolute_X, 5},
    {"STA", Addressing::absolute_X, &c::STA, &c::addressing_absolute_X, 5},
    {"SHX", Addressing::absolute_Y, &c::SHX, &c::addressing_absolute_Y, 5},
    {"AHX", Addressing::absolute_Y, &c::AHX, &c::addressing_absolute_Y, 5},
    {"LDY", Addressing::immediate, &c::LDY, &c::addressing_immediate, 2},
    {"LDA", Addressing::indexed_indirect, &c::LDA, &c::addressing_indexed_indirect, 6},
    {"LDX", Addressing::immediate, &c::LDX, &c::addressing_immediate, 2},
    {"LAX", Addressing::indexed_indirect, &c::LAX, &c::addressing_indexed_indirect, 6},
    {"LDY", Addressing::zero_page, &c::LDY, &c::addressing_zero_page, 3},
    {"LDA", Addressing::zero_page, &c::LDA, &c::addressing_zero_page, 3},
    {"LDX", Addressing::zero_page, &c::LDX, &c::addressing_zero_page, 3},
    {"LAX", Addressing::zero_page, &c::LAX, &c::addressing_zero_page, 3},
    {"TAY", Addressing::implicit, &c::TAY, &c::addressing_implicit, 2},
    {"LDA", Addressing::immediate, &c::LDA, &c::addressing_immediate, 2},
    {"TAX", Addressing::implicit, &c::TAX, &c::addressing_implicit, 2},
    {"LAX", Addressing::immediate, &c::LAX, &c::addressing_immediate, 2},
    {"LDY", Addressing::absolute, &c::LDY, &c::addressing_absolute, 4},
    {"LDA", Addressing::absolute, &c::LDA, &c::addressing_absolute, 4},
    {"LDX", Addressing::absolute, &c::LDX, &c::addressing_absolute, 4},
    {"LAX", Addressing::absolute, &c::LAX, &c::addressing_absolute, 4},
    {"BCS", Addressing::relative, &c::BCS, &c::addressing_relative, 2},                  // oops cycle
    {"LDA", Addressing::indirect_indexed, &c::LDA, &c::addressing_indirect_indexed, 5},  // oops cycle
    {"STP", Addressing::implicit, &c::STP, &c::addressing_implicit, 0},
    {"LAX", Addressing::indirect_indexed, &c::LAX, &c::addressing_indirect_indexed, 5},  // oops cycle
    {"LDY", Addressing::zero_page_X, &c::LDY, &c::addressing_zero_page_X, 4},
    {"LDA", Addressing::zero_page_X, &c::LDA, &c::addressing_zero_page_X, 4},
    {"LDX", Addressing::zero_page_Y, &c::LDX, &c::addressing_zero_page_Y, 4},
    {"LAX", Addressing::zero_page_Y, &c::LAX, &c::addressing_zero_page_Y, 4},
    {"CLV", Addressing::implicit, &c::CLV, &c::addressing_implicit, 2},
    {"LDA", Addressing::absolute_Y, &c::LDA, &c::addressing_absolute_Y, 4},  // oops cycle
    {"TSX", Addressing::implicit, &c::TSX, &c::addressing_implicit, 2},
    {"LAS", Addressing::absolute_Y, &c::LAS, &c::addressing_absolute_Y, 4},  // oops cycle
    {"LDY", Addressing::absolute_X, &c::LDY, &c::addressing_absolute_X, 4},  // oops cycle
    {"LDA", Addressing::absolute_X, &c::LDA, &c::addressing_absolute_X, 4},  // oops cycle
    {"LDX", Addressing::absolute_Y, &c::LDX, &c::addressing_absolute_Y, 4},  // oops cycle
    {"LAX", Addressing::absolute_Y, &c::LAX, &c::addressing_absolute_Y, 4},  // oops cycle
    {"CPY", Addressing::immediate, &c::CPY, &c::addressing_immediate, 2},
    {"CMP", Addressing::indexed_indirect, &c::CMP, &c::addressing_indexed_indirect, 6},
    {"NOP", Addressing::immediate, &c::NOP, &c::addressing_immediate, 2},
    {"DCP", Addressing::indexed_indirect, &c::DCP, &c::addressing_indexed_indirect, 8},
    {"CPY", Addressing::zero_page, &c::CPY, &c::addressing_zero_page, 3},
    {"CMP", Addressing::zero_page, &c::CMP, &c::addressing_zero_page, 3},
    {"DEC", Addressing::zero_page, &c::DEC, &c::addressing_zero_page, 5},
    {"DCP", Addressing::zero_page, &c::DCP, &c::addressing_zero_page, 5},
    {"INY", Addressing::implicit, &c::INY, &c::addressing_implicit, 2},
    {"CMP", Addressing::immediate, &c::CMP, &c::addressing_immediate, 2},
    {"DEX", Addressing::implicit, &c::DEX, &c::addressing_implicit, 2},
    {"AXS", Addressing::immediate, &c::AXS, &c::addressing_immediate, 2},
    {"CPY", Addressing::absolute, &c::CPY, &c::addressing_absolute, 4},
    {"CMP", Addressing::absolute, &c::CMP, &c::addressing_absolute, 4},
    {"DEC", Addressing::absolute, &c::DEC, &c::addressing_absolute, 6},
    {"DCP", Addressing::absolute, &c::DCP, &c::addressing_absolute, 6},
    {"BNE", Addressing::relative, &c::BNE, &c::addressing_relative, 2},                  // oops cycle
    {"CMP", Addressing::indirect_indexed, &c::CMP, &c::addressing_indirect_indexed, 5},  // oops cycle
    {"STP", Addressing::implicit, &c::STP, &c::addressing_implicit, 0},
    {"DCP", Addressing::indirect_indexed, &c::DCP, &c::addressing_indirect_indexed, 8},
    {"NOP", Addressing::zero_page_X, &c::NOP, &c::addressing_zero_page_X, 4},
    {"CMP", Addressing::zero_page_X, &c::CMP, &c::addressing_zero_page_X, 4},
    {"DEC", Addressing::zero_page_X, &c::DEC, &c::addressing_zero_page_X, 6},
    {"DCP", Addressing::zero_page_X, &c::DCP, &c::addressing_zero_page_X, 6},
    {"CLD", Addressing::implicit, &c::CLD, &c::addressing_implicit, 2},
    {"CMP", Addressing::absolute_Y, &c::CMP, &c::addressing_absolute_Y, 4},  // oops cycle
    {"NOP", Addressing::implicit, &c::NOP, &c::addressing_implicit, 2},
    {"DCP", Addressing::absolute_Y, &c::DCP, &c::addressing_absolute_Y, 7},
    {"NOP", Addressing::absolute_X, &c::NOP, &c::addressing_absolute_X, 4},  // oops cycle
    {"CMP", Addressing::absolute_X, &c::CMP, &c::addressing_absolute_X, 4},  // oops cycle
    {"DEC", Addressing::absolute_X, &c::DEC, &c::addressing_absolute_X, 7},
    {"DCP", Addressing::absolute_X, &c::DCP, &c::addressing_absolute_X, 7},
    {"CPX", Addressing::immediate, &c::CPX, &c::addressing_immediate, 2},
    {"SBC", Addressing::indexed_indirect, &c::SBC, &c::addressing_indexed_indirect, 6},
    {"NOP", Addressing::immediate, &c::NOP, &c::addressing_immediate, 2},
    {"ISC", Addressing::indexed_indirect, &c::ISC, &c::addressing_indexed_indirect, 8},
    {"CPX", Addressing::zero_page, &c::CPX, &c::addressing_zero_page, 3},
    {"SBC", Addressing::zero_page, &c::SBC, &c::addressing_zero_page, 3},
    {"INC", Addressing::zero_page, &c::INC, &c::addressing_zero_page, 5},
    {"ISC", Addressing::zero_page, &c::ISC, &c::addressing_zero_page, 5},
    {"INX", Addressing::implicit, &c::INX, &c::addressing_implicit, 2},
    {"SBC", Addressing::immediate, &c::SBC, &c::addressing_immediate, 2},
    {"NOP", Addressing::implicit, &c::NOP, &c::addressing_implicit, 2},
    {"SBC", Addressing::immediate, &c::SBC, &c::addressing_immediate, 2},
    {"CPX", Addressing::absolute, &c::CPX, &c::addressing_absolute, 4},
    {"SBC", Addressing::absolute, &c::SBC, &c::addressing_absolute, 4},
    {"INC", Addressing::absolute, &c::INC, &c::addressing_absolute, 6},
    {"ISC", Addressing::absolute, &c::ISC, &c::addressing_absolute, 6},
    {"BEQ", Addressing::relative, &c::BEQ, &c::addressing_relative, 2},                  // oops cycle
    {"SBC", Addressing::indirect_indexed, &c::SBC, &c::addressing_indirect_indexed, 5},  // oops cycle
    {"STP", Addressing::implicit, &c::STP, &c::addressing_implicit, 0},
    {"ISC", Addressing::indirect_indexed, &c::ISC, &c::addressing_indirect_indexed, 8},
    {"NOP", Addressing::zero_page_X, &c::NOP, &c::addressing_zero_page_X, 4},
    {"SBC", Addressing::zero_page_X, &c::SBC, &c::addressing_zero_page_X, 4},
    {"INC", Addressing::zero_page_X, &c::INC, &c::addressing_zero_page_X, 6},
    {"ISC", Addressing::zero_page_X, &c::ISC, &c::addressing_zero_page_X, 6},
    {"SED", Addressing::implicit, &c::SED, &c::addressing_implicit, 2},
    {"SBC", Addressing::absolute_Y, &c::SBC, &c::addressing_absolute_Y, 4},  // oops cycle
    {"NOP", Addressing::implicit, &c::NOP, &c::addressing_implicit, 2},
    {"ISC", Addressing::absolute_Y, &c::ISC, &c::addressing_absolute_Y, 7},
    {"NOP", Addressing::absolute_X, &c::NOP, &c::addressing_absolute_X, 4},  // oops cycle
    {"SBC", Addressing::absolute_X, &c::SBC, &c::addressing_absolute_X, 4},  // oops cycle
    {"INC", Addressing::absolute_X, &c::INC, &c::addressing_absolute_X, 7},
    {"ISC", Addressing::absolute_X, &c::ISC, &c::addressing_absolute_X, 7}};

// clang-format off
