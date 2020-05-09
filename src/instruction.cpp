#include "instruction.h"






using c = CPU;
const std::valarray<Instruction> Instruction::instruction_set {
    {"BRK", AddressingTypes::addressing_implicit_type, &c::BRK, &c::addressing_implicit, 7},
    {"ORA", AddressingTypes::addressing_indexed_indirect_type, &c::ORA, &c::addressing_indexed_indirect, 6},
    {"STP", AddressingTypes::addressing_implicit_type, &c::STP, &c::addressing_implicit, 0},
    {"SLO", AddressingTypes::addressing_indexed_indirect_type, &c::SLO, &c::addressing_indexed_indirect, 8},
    {"NOP", AddressingTypes::addressing_zero_page_type, &c::NOP, &c::addressing_zero_page, 3},
    {"ORA", AddressingTypes::addressing_zero_page_type, &c::ORA, &c::addressing_zero_page, 3},
    {"ASL", AddressingTypes::addressing_zero_page_type, &c::ASL, &c::addressing_zero_page, 5},
    {"SLO", AddressingTypes::addressing_zero_page_type, &c::SLO, &c::addressing_zero_page, 5},
    {"PHP", AddressingTypes::addressing_implicit_type, &c::PHP, &c::addressing_implicit, 3},
    {"ORA", AddressingTypes::addressing_immediate_type, &c::ORA, &c::addressing_immediate, 2},
    {"ASL", AddressingTypes::addressing_implicit_type, &c::ASL, &c::addressing_implicit, 2},
    {"ANC", AddressingTypes::addressing_immediate_type, &c::ANC, &c::addressing_immediate, 2},
    {"NOP", AddressingTypes::addressing_absolute_type, &c::NOP, &c::addressing_absolute, 4},
    {"ORA", AddressingTypes::addressing_absolute_type, &c::ORA, &c::addressing_absolute, 4},
    {"ASL", AddressingTypes::addressing_absolute_type, &c::ASL, &c::addressing_absolute, 6},
    {"SLO", AddressingTypes::addressing_absolute_type, &c::SLO, &c::addressing_absolute, 6},
    {"BPL", AddressingTypes::addressing_relative_type, &c::BPL, &c::addressing_relative, 2}, // oops cycle
    {"ORA", AddressingTypes::addressing_indirect_indexed_type, &c::ORA, &c::addressing_indirect_indexed, 5}, // oops cycle
    {"STP", AddressingTypes::addressing_implicit_type, &c::STP, &c::addressing_implicit, 0},
    {"SLO", AddressingTypes::addressing_indirect_indexed_type, &c::SLO, &c::addressing_indirect_indexed, 8},
    {"NOP", AddressingTypes::addressing_zero_page_X_type, &c::NOP, &c::addressing_zero_page_X, 4},
    {"ORA", AddressingTypes::addressing_zero_page_X_type, &c::ORA, &c::addressing_zero_page_X, 4},
    {"ASL", AddressingTypes::addressing_zero_page_X_type, &c::ASL, &c::addressing_zero_page_X, 6},
    {"SLO", AddressingTypes::addressing_zero_page_X_type, &c::SLO, &c::addressing_zero_page_X, 6},
    {"CLC", AddressingTypes::addressing_implicit_type, &c::CLC, &c::addressing_implicit, 2},
    {"ORA", AddressingTypes::addressing_absolute_Y_type, &c::ORA, &c::addressing_absolute_Y, 4}, // oops cycle
    {"NOP", AddressingTypes::addressing_implicit_type, &c::NOP, &c::addressing_implicit, 2},
    {"SLO", AddressingTypes::addressing_absolute_Y_type, &c::SLO, &c::addressing_absolute_Y, 7},
    {"NOP", AddressingTypes::addressing_absolute_X_type, &c::NOP, &c::addressing_absolute_X, 4}, // oops cycle
    {"ORA", AddressingTypes::addressing_absolute_X_type, &c::ORA, &c::addressing_absolute_X, 4}, // oops cycle
    {"ASL", AddressingTypes::addressing_absolute_X_type, &c::ASL, &c::addressing_absolute_X, 7},
    {"SLO", AddressingTypes::addressing_absolute_X_type, &c::SLO, &c::addressing_absolute_X, 7},
    {"JSR", AddressingTypes::addressing_absolute_type, &c::JSR, &c::addressing_absolute, 6},
    {"AND", AddressingTypes::addressing_indexed_indirect_type, &c::AND, &c::addressing_indexed_indirect, 6},
    {"STP", AddressingTypes::addressing_implicit_type, &c::STP, &c::addressing_implicit, 0},
    {"RLA", AddressingTypes::addressing_indexed_indirect_type, &c::RLA, &c::addressing_indexed_indirect, 8},
    {"BIT", AddressingTypes::addressing_zero_page_type, &c::BIT, &c::addressing_zero_page, 3},
    {"AND", AddressingTypes::addressing_zero_page_type, &c::AND, &c::addressing_zero_page, 3},
    {"ROL", AddressingTypes::addressing_zero_page_type, &c::ROL, &c::addressing_zero_page, 5},
    {"RLA", AddressingTypes::addressing_zero_page_type, &c::RLA, &c::addressing_zero_page, 5},
    {"PLP", AddressingTypes::addressing_implicit_type, &c::PLP, &c::addressing_implicit, 4},
    {"AND", AddressingTypes::addressing_immediate_type, &c::AND, &c::addressing_immediate, 2},
    {"ROL", AddressingTypes::addressing_implicit_type, &c::ROL, &c::addressing_implicit, 2},
    {"ANC", AddressingTypes::addressing_immediate_type, &c::ANC, &c::addressing_immediate, 2},
    {"BIT", AddressingTypes::addressing_absolute_type, &c::BIT, &c::addressing_absolute, 4},
    {"AND", AddressingTypes::addressing_absolute_type, &c::AND, &c::addressing_absolute, 4},
    {"ROL", AddressingTypes::addressing_absolute_type, &c::ROL, &c::addressing_absolute, 6},
    {"RLA", AddressingTypes::addressing_absolute_type, &c::RLA, &c::addressing_absolute, 6},
    {"BMI", AddressingTypes::addressing_relative_type, &c::BMI, &c::addressing_relative, 2}, // oops cycle
    {"AND", AddressingTypes::addressing_indirect_indexed_type, &c::AND, &c::addressing_indirect_indexed, 5}, // oops cycle
    {"STP", AddressingTypes::addressing_implicit_type, &c::STP, &c::addressing_implicit, 0},
    {"RLA", AddressingTypes::addressing_indirect_indexed_type, &c::RLA, &c::addressing_indirect_indexed, 8},
    {"NOP", AddressingTypes::addressing_zero_page_X_type, &c::NOP, &c::addressing_zero_page_X, 4},
    {"AND", AddressingTypes::addressing_zero_page_X_type, &c::AND, &c::addressing_zero_page_X, 4},
    {"ROL", AddressingTypes::addressing_zero_page_X_type, &c::ROL, &c::addressing_zero_page_X, 6},
    {"RLA", AddressingTypes::addressing_zero_page_X_type, &c::RLA, &c::addressing_zero_page_X, 6},
    {"SEC", AddressingTypes::addressing_implicit_type, &c::SEC, &c::addressing_implicit, 2},
    {"AND", AddressingTypes::addressing_absolute_Y_type, &c::AND, &c::addressing_absolute_Y, 4}, // oops cycle
    {"NOP", AddressingTypes::addressing_implicit_type, &c::NOP, &c::addressing_implicit, 2},
    {"RLA", AddressingTypes::addressing_absolute_Y_type, &c::RLA, &c::addressing_absolute_Y, 7},
    {"NOP", AddressingTypes::addressing_absolute_X_type, &c::NOP, &c::addressing_absolute_X, 4}, // oops cycle
    {"AND", AddressingTypes::addressing_absolute_X_type, &c::AND, &c::addressing_absolute_X, 4}, // oops cycle
    {"ROL", AddressingTypes::addressing_absolute_X_type, &c::ROL, &c::addressing_absolute_X, 7},
    {"RLA", AddressingTypes::addressing_absolute_X_type, &c::RLA, &c::addressing_absolute_X, 7},
    {"RTI", AddressingTypes::addressing_implicit_type, &c::RTI, &c::addressing_implicit, 6},
    {"EOR", AddressingTypes::addressing_indexed_indirect_type, &c::EOR, &c::addressing_indexed_indirect, 6},
    {"STP", AddressingTypes::addressing_implicit_type, &c::STP, &c::addressing_implicit, 0},
    {"SRE", AddressingTypes::addressing_indexed_indirect_type, &c::SRE, &c::addressing_indexed_indirect, 8},
    {"NOP", AddressingTypes::addressing_zero_page_type, &c::NOP, &c::addressing_zero_page, 3},
    {"EOR", AddressingTypes::addressing_zero_page_type, &c::EOR, &c::addressing_zero_page, 3},
    {"LSR", AddressingTypes::addressing_zero_page_type, &c::LSR, &c::addressing_zero_page, 5},
    {"SRE", AddressingTypes::addressing_zero_page_type, &c::SRE, &c::addressing_zero_page, 5},
    {"PHA", AddressingTypes::addressing_implicit_type, &c::PHA, &c::addressing_implicit, 3},
    {"EOR", AddressingTypes::addressing_immediate_type, &c::EOR, &c::addressing_immediate, 2},
    {"LSR", AddressingTypes::addressing_implicit_type, &c::LSR, &c::addressing_implicit, 2},
    {"ALR", AddressingTypes::addressing_immediate_type, &c::ALR, &c::addressing_immediate, 2},
    {"JMP", AddressingTypes::addressing_absolute_type, &c::JMP, &c::addressing_absolute, 3},
    {"EOR", AddressingTypes::addressing_absolute_type, &c::EOR, &c::addressing_absolute, 4},
    {"LSR", AddressingTypes::addressing_absolute_type, &c::LSR, &c::addressing_absolute, 6},
    {"SRE", AddressingTypes::addressing_absolute_type, &c::SRE, &c::addressing_absolute, 6},
    {"BVC", AddressingTypes::addressing_relative_type, &c::BVC, &c::addressing_relative, 2}, // oops cycle
    {"EOR", AddressingTypes::addressing_indirect_indexed_type, &c::EOR, &c::addressing_indirect_indexed, 5}, // oops cycle
    {"STP", AddressingTypes::addressing_implicit_type, &c::STP, &c::addressing_implicit, 0},
    {"SRE", AddressingTypes::addressing_indirect_indexed_type, &c::SRE, &c::addressing_indirect_indexed, 8},
    {"NOP", AddressingTypes::addressing_zero_page_X_type, &c::NOP, &c::addressing_zero_page_X, 4},
    {"EOR", AddressingTypes::addressing_zero_page_X_type, &c::EOR, &c::addressing_zero_page_X, 4},
    {"LSR", AddressingTypes::addressing_zero_page_X_type, &c::LSR, &c::addressing_zero_page_X, 6},
    {"SRE", AddressingTypes::addressing_zero_page_X_type, &c::SRE, &c::addressing_zero_page_X, 6},
    {"CLI", AddressingTypes::addressing_implicit_type, &c::CLI, &c::addressing_implicit, 2},
    {"EOR", AddressingTypes::addressing_absolute_Y_type, &c::EOR, &c::addressing_absolute_Y, 4}, // oops cycle
    {"NOP", AddressingTypes::addressing_implicit_type, &c::NOP, &c::addressing_implicit, 2},
    {"SRE", AddressingTypes::addressing_absolute_Y_type, &c::SRE, &c::addressing_absolute_Y, 7},
    {"NOP", AddressingTypes::addressing_absolute_X_type, &c::NOP, &c::addressing_absolute_X, 4}, // oops cycle
    {"EOR", AddressingTypes::addressing_absolute_X_type, &c::EOR, &c::addressing_absolute_X, 4}, // oops cycle
    {"LSR", AddressingTypes::addressing_absolute_X_type, &c::LSR, &c::addressing_absolute_X, 7},
    {"SRE", AddressingTypes::addressing_absolute_X_type, &c::SRE, &c::addressing_absolute_X, 7},
    {"RTS", AddressingTypes::addressing_implicit_type, &c::RTS, &c::addressing_implicit, 6},
    {"ADC", AddressingTypes::addressing_indexed_indirect_type, &c::ADC, &c::addressing_indexed_indirect, 6},
    {"STP", AddressingTypes::addressing_implicit_type, &c::STP, &c::addressing_implicit, 0},
    {"RRA", AddressingTypes::addressing_indexed_indirect_type, &c::RRA, &c::addressing_indexed_indirect, 8},
    {"NOP", AddressingTypes::addressing_zero_page_type, &c::NOP, &c::addressing_zero_page, 3},
    {"ADC", AddressingTypes::addressing_zero_page_type, &c::ADC, &c::addressing_zero_page, 3},
    {"ROR", AddressingTypes::addressing_zero_page_type, &c::ROR, &c::addressing_zero_page, 5},
    {"RRA", AddressingTypes::addressing_zero_page_type, &c::RRA, &c::addressing_zero_page, 5},
    {"PLA", AddressingTypes::addressing_implicit_type, &c::PLA, &c::addressing_implicit, 4},
    {"ADC", AddressingTypes::addressing_immediate_type, &c::ADC, &c::addressing_immediate, 2},
    {"ROR", AddressingTypes::addressing_implicit_type, &c::ROR, &c::addressing_implicit, 2},
    {"ARR", AddressingTypes::addressing_immediate_type, &c::ARR, &c::addressing_immediate, 2},
    {"JMP", AddressingTypes::addressing_indirect_type, &c::JMP, &c::addressing_indirect, 5},
    {"ADC", AddressingTypes::addressing_absolute_type, &c::ADC, &c::addressing_absolute, 4},
    {"ROR", AddressingTypes::addressing_absolute_type, &c::ROR, &c::addressing_absolute, 6},
    {"RRA", AddressingTypes::addressing_absolute_type, &c::RRA, &c::addressing_absolute, 6},
    {"BVS", AddressingTypes::addressing_relative_type, &c::BVS, &c::addressing_relative, 2}, // oops cycle
    {"ADC", AddressingTypes::addressing_indirect_indexed_type, &c::ADC, &c::addressing_indirect_indexed, 5}, // oops cycle
    {"STP", AddressingTypes::addressing_implicit_type, &c::STP, &c::addressing_implicit, 0},
    {"RRA", AddressingTypes::addressing_indirect_indexed_type, &c::RRA, &c::addressing_indirect_indexed, 8},
    {"NOP", AddressingTypes::addressing_zero_page_X_type, &c::NOP, &c::addressing_zero_page_X, 4},
    {"ADC", AddressingTypes::addressing_zero_page_X_type, &c::ADC, &c::addressing_zero_page_X, 4},
    {"ROR", AddressingTypes::addressing_zero_page_X_type, &c::ROR, &c::addressing_zero_page_X, 6},
    {"RRA", AddressingTypes::addressing_zero_page_X_type, &c::RRA, &c::addressing_zero_page_X, 6},
    {"SEI", AddressingTypes::addressing_implicit_type, &c::SEI, &c::addressing_implicit, 2},
    {"ADC", AddressingTypes::addressing_absolute_Y_type, &c::ADC, &c::addressing_absolute_Y, 4}, // oops cycle
    {"NOP", AddressingTypes::addressing_implicit_type, &c::NOP, &c::addressing_implicit, 2},
    {"RRA", AddressingTypes::addressing_absolute_Y_type, &c::RRA, &c::addressing_absolute_Y, 7},
    {"NOP", AddressingTypes::addressing_absolute_X_type, &c::NOP, &c::addressing_absolute_X, 4}, // oops cycle
    {"ADC", AddressingTypes::addressing_absolute_X_type, &c::ADC, &c::addressing_absolute_X, 4}, // oops cycle
    {"ROR", AddressingTypes::addressing_absolute_X_type, &c::ROR, &c::addressing_absolute_X, 7},
    {"RRA", AddressingTypes::addressing_absolute_X_type, &c::RRA, &c::addressing_absolute_X, 7},
    {"NOP", AddressingTypes::addressing_immediate_type, &c::NOP, &c::addressing_immediate, 2},
    {"STA", AddressingTypes::addressing_indexed_indirect_type, &c::STA, &c::addressing_indexed_indirect, 6},
    {"NOP", AddressingTypes::addressing_immediate_type, &c::NOP, &c::addressing_immediate, 2},
    {"SAX", AddressingTypes::addressing_indexed_indirect_type, &c::SAX, &c::addressing_indexed_indirect, 6},
    {"STY", AddressingTypes::addressing_zero_page_type, &c::STY, &c::addressing_zero_page, 3},
    {"STA", AddressingTypes::addressing_zero_page_type, &c::STA, &c::addressing_zero_page, 3},
    {"STX", AddressingTypes::addressing_zero_page_type, &c::STX, &c::addressing_zero_page, 3},
    {"SAX", AddressingTypes::addressing_zero_page_type, &c::SAX, &c::addressing_zero_page, 3},
    {"DEY", AddressingTypes::addressing_implicit_type, &c::DEY, &c::addressing_implicit, 2},
    {"NOP", AddressingTypes::addressing_immediate_type, &c::NOP, &c::addressing_immediate, 2},
    {"TXA", AddressingTypes::addressing_implicit_type, &c::TXA, &c::addressing_implicit, 2},
    {"XAA", AddressingTypes::addressing_immediate_type, &c::XAA, &c::addressing_immediate, 2},
    {"STY", AddressingTypes::addressing_absolute_type, &c::STY, &c::addressing_absolute, 4},
    {"STA", AddressingTypes::addressing_absolute_type, &c::STA, &c::addressing_absolute, 4},
    {"STX", AddressingTypes::addressing_absolute_type, &c::STX, &c::addressing_absolute, 4},
    {"SAX", AddressingTypes::addressing_absolute_type, &c::SAX, &c::addressing_absolute, 4},
    {"BCC", AddressingTypes::addressing_relative_type, &c::BCC, &c::addressing_relative, 2}, // oops cycle
    {"STA", AddressingTypes::addressing_indirect_indexed_type, &c::STA, &c::addressing_indirect_indexed, 6},
    {"STP", AddressingTypes::addressing_implicit_type, &c::STP, &c::addressing_implicit, 0},
    {"AHX", AddressingTypes::addressing_indirect_indexed_type, &c::AHX, &c::addressing_indirect_indexed, 6},
    {"STY", AddressingTypes::addressing_zero_page_X_type, &c::STY, &c::addressing_zero_page_X, 4},
    {"STA", AddressingTypes::addressing_zero_page_X_type, &c::STA, &c::addressing_zero_page_X, 4},
    {"STX", AddressingTypes::addressing_zero_page_Y_type, &c::STX, &c::addressing_zero_page_Y, 4},
    {"SAX", AddressingTypes::addressing_zero_page_Y_type, &c::SAX, &c::addressing_zero_page_Y, 4},
    {"TYA", AddressingTypes::addressing_implicit_type, &c::TYA, &c::addressing_implicit, 2},
    {"STA", AddressingTypes::addressing_absolute_Y_type, &c::STA, &c::addressing_absolute_Y, 5},
    {"TXS", AddressingTypes::addressing_implicit_type, &c::TXS, &c::addressing_implicit, 2},
    {"TAS", AddressingTypes::addressing_absolute_Y_type, &c::TAS, &c::addressing_absolute_Y, 5},
    {"SHY", AddressingTypes::addressing_absolute_X_type, &c::SHY, &c::addressing_absolute_X, 5},
    {"STA", AddressingTypes::addressing_absolute_X_type, &c::STA, &c::addressing_absolute_X, 5},
    {"SHX", AddressingTypes::addressing_absolute_Y_type, &c::SHX, &c::addressing_absolute_Y, 5},
    {"AHX", AddressingTypes::addressing_absolute_Y_type, &c::AHX, &c::addressing_absolute_Y, 5},
    {"LDY", AddressingTypes::addressing_immediate_type, &c::LDY, &c::addressing_immediate, 2},
    {"LDA", AddressingTypes::addressing_indexed_indirect_type, &c::LDA, &c::addressing_indexed_indirect, 6},
    {"LDX", AddressingTypes::addressing_immediate_type, &c::LDX, &c::addressing_immediate, 2},
    {"LAX", AddressingTypes::addressing_indexed_indirect_type, &c::LAX, &c::addressing_indexed_indirect, 6},
    {"LDY", AddressingTypes::addressing_zero_page_type, &c::LDY, &c::addressing_zero_page, 3},
    {"LDA", AddressingTypes::addressing_zero_page_type, &c::LDA, &c::addressing_zero_page, 3},
    {"LDX", AddressingTypes::addressing_zero_page_type, &c::LDX, &c::addressing_zero_page, 3},
    {"LAX", AddressingTypes::addressing_zero_page_type, &c::LAX, &c::addressing_zero_page, 3},
    {"TAY", AddressingTypes::addressing_implicit_type, &c::TAY, &c::addressing_implicit, 2},
    {"LDA", AddressingTypes::addressing_immediate_type, &c::LDA, &c::addressing_immediate, 2},
    {"TAX", AddressingTypes::addressing_implicit_type, &c::TAX, &c::addressing_implicit, 2},
    {"LAX", AddressingTypes::addressing_immediate_type, &c::LAX, &c::addressing_immediate, 2},
    {"LDY", AddressingTypes::addressing_absolute_type, &c::LDY, &c::addressing_absolute, 4},
    {"LDA", AddressingTypes::addressing_absolute_type, &c::LDA, &c::addressing_absolute, 4},
    {"LDX", AddressingTypes::addressing_absolute_type, &c::LDX, &c::addressing_absolute, 4},
    {"LAX", AddressingTypes::addressing_absolute_type, &c::LAX, &c::addressing_absolute, 4},
    {"BCS", AddressingTypes::addressing_relative_type, &c::BCS, &c::addressing_relative, 2}, // oops cycle
    {"LDA", AddressingTypes::addressing_indirect_indexed_type, &c::LDA, &c::addressing_indirect_indexed, 5}, // oops cycle
    {"STP", AddressingTypes::addressing_implicit_type, &c::STP, &c::addressing_implicit, 0},
    {"LAX", AddressingTypes::addressing_indirect_indexed_type, &c::LAX, &c::addressing_indirect_indexed, 5}, // oops cycle
    {"LDY", AddressingTypes::addressing_zero_page_X_type, &c::LDY, &c::addressing_zero_page_X, 4},
    {"LDA", AddressingTypes::addressing_zero_page_X_type, &c::LDA, &c::addressing_zero_page_X, 4},
    {"LDX", AddressingTypes::addressing_zero_page_Y_type, &c::LDX, &c::addressing_zero_page_Y, 4},
    {"LAX", AddressingTypes::addressing_zero_page_Y_type, &c::LAX, &c::addressing_zero_page_Y, 4},
    {"CLV", AddressingTypes::addressing_implicit_type, &c::CLV, &c::addressing_implicit, 2},
    {"LDA", AddressingTypes::addressing_absolute_Y_type, &c::LDA, &c::addressing_absolute_Y, 4}, // oops cycle
    {"TSX", AddressingTypes::addressing_implicit_type, &c::TSX, &c::addressing_implicit, 2},
    {"LAS", AddressingTypes::addressing_absolute_Y_type, &c::LAS, &c::addressing_absolute_Y, 4}, // oops cycle
    {"LDY", AddressingTypes::addressing_absolute_X_type, &c::LDY, &c::addressing_absolute_X, 4}, // oops cycle
    {"LDA", AddressingTypes::addressing_absolute_X_type, &c::LDA, &c::addressing_absolute_X, 4}, // oops cycle
    {"LDX", AddressingTypes::addressing_absolute_Y_type, &c::LDX, &c::addressing_absolute_Y, 4}, // oops cycle
    {"LAX", AddressingTypes::addressing_absolute_Y_type, &c::LAX, &c::addressing_absolute_Y, 4}, // oops cycle
    {"CPY", AddressingTypes::addressing_immediate_type, &c::CPY, &c::addressing_immediate, 2},
    {"CMP", AddressingTypes::addressing_indexed_indirect_type, &c::CMP, &c::addressing_indexed_indirect, 6},
    {"NOP", AddressingTypes::addressing_immediate_type, &c::NOP, &c::addressing_immediate, 2},
    {"DCP", AddressingTypes::addressing_indexed_indirect_type, &c::DCP, &c::addressing_indexed_indirect, 8},
    {"CPY", AddressingTypes::addressing_zero_page_type, &c::CPY, &c::addressing_zero_page, 3},
    {"CMP", AddressingTypes::addressing_zero_page_type, &c::CMP, &c::addressing_zero_page, 3},
    {"DEC", AddressingTypes::addressing_zero_page_type, &c::DEC, &c::addressing_zero_page, 5},
    {"DCP", AddressingTypes::addressing_zero_page_type, &c::DCP, &c::addressing_zero_page, 5},
    {"INY", AddressingTypes::addressing_implicit_type, &c::INY, &c::addressing_implicit, 2},
    {"CMP", AddressingTypes::addressing_immediate_type, &c::CMP, &c::addressing_immediate, 2},
    {"DEX", AddressingTypes::addressing_implicit_type, &c::DEX, &c::addressing_implicit, 2},
    {"AXS", AddressingTypes::addressing_immediate_type, &c::AXS, &c::addressing_immediate, 2},
    {"CPY", AddressingTypes::addressing_absolute_type, &c::CPY, &c::addressing_absolute, 4},
    {"CMP", AddressingTypes::addressing_absolute_type, &c::CMP, &c::addressing_absolute, 4},
    {"DEC", AddressingTypes::addressing_absolute_type, &c::DEC, &c::addressing_absolute, 6},
    {"DCP", AddressingTypes::addressing_absolute_type, &c::DCP, &c::addressing_absolute, 6},
    {"BNE", AddressingTypes::addressing_relative_type, &c::BNE, &c::addressing_relative, 2}, // oops cycle
    {"CMP", AddressingTypes::addressing_indirect_indexed_type, &c::CMP, &c::addressing_indirect_indexed, 5}, // oops cycle
    {"STP", AddressingTypes::addressing_implicit_type, &c::STP, &c::addressing_implicit, 0},
    {"DCP", AddressingTypes::addressing_indirect_indexed_type, &c::DCP, &c::addressing_indirect_indexed, 8},
    {"NOP", AddressingTypes::addressing_zero_page_X_type, &c::NOP, &c::addressing_zero_page_X, 4},
    {"CMP", AddressingTypes::addressing_zero_page_X_type, &c::CMP, &c::addressing_zero_page_X, 4},
    {"DEC", AddressingTypes::addressing_zero_page_X_type, &c::DEC, &c::addressing_zero_page_X, 6},
    {"DCP", AddressingTypes::addressing_zero_page_X_type, &c::DCP, &c::addressing_zero_page_X, 6},
    {"CLD", AddressingTypes::addressing_implicit_type, &c::CLD, &c::addressing_implicit, 2},
    {"CMP", AddressingTypes::addressing_absolute_Y_type, &c::CMP, &c::addressing_absolute_Y, 4}, // oops cycle
    {"NOP", AddressingTypes::addressing_implicit_type, &c::NOP, &c::addressing_implicit, 2},
    {"DCP", AddressingTypes::addressing_absolute_Y_type, &c::DCP, &c::addressing_absolute_Y, 7},
    {"NOP", AddressingTypes::addressing_absolute_X_type, &c::NOP, &c::addressing_absolute_X, 4}, // oops cycle
    {"CMP", AddressingTypes::addressing_absolute_X_type, &c::CMP, &c::addressing_absolute_X, 4}, // oops cycle
    {"DEC", AddressingTypes::addressing_absolute_X_type, &c::DEC, &c::addressing_absolute_X, 7},
    {"DCP", AddressingTypes::addressing_absolute_X_type, &c::DCP, &c::addressing_absolute_X, 7},
    {"CPX", AddressingTypes::addressing_immediate_type, &c::CPX, &c::addressing_immediate, 2},
    {"SBC", AddressingTypes::addressing_indexed_indirect_type, &c::SBC, &c::addressing_indexed_indirect, 6},
    {"NOP", AddressingTypes::addressing_immediate_type, &c::NOP, &c::addressing_immediate, 2},
    {"ISC", AddressingTypes::addressing_indexed_indirect_type, &c::ISC, &c::addressing_indexed_indirect, 8},
    {"CPX", AddressingTypes::addressing_zero_page_type, &c::CPX, &c::addressing_zero_page, 3},
    {"SBC", AddressingTypes::addressing_zero_page_type, &c::SBC, &c::addressing_zero_page, 3},
    {"INC", AddressingTypes::addressing_zero_page_type, &c::INC, &c::addressing_zero_page, 5},
    {"ISC", AddressingTypes::addressing_zero_page_type, &c::ISC, &c::addressing_zero_page, 5},
    {"INX", AddressingTypes::addressing_implicit_type, &c::INX, &c::addressing_implicit, 2},
    {"SBC", AddressingTypes::addressing_immediate_type, &c::SBC, &c::addressing_immediate, 2},
    {"NOP", AddressingTypes::addressing_implicit_type, &c::NOP, &c::addressing_implicit, 2},
    {"SBC", AddressingTypes::addressing_immediate_type, &c::SBC, &c::addressing_immediate, 2},
    {"CPX", AddressingTypes::addressing_absolute_type, &c::CPX, &c::addressing_absolute, 4},
    {"SBC", AddressingTypes::addressing_absolute_type, &c::SBC, &c::addressing_absolute, 4},
    {"INC", AddressingTypes::addressing_absolute_type, &c::INC, &c::addressing_absolute, 6},
    {"ISC", AddressingTypes::addressing_absolute_type, &c::ISC, &c::addressing_absolute, 6},
    {"BEQ", AddressingTypes::addressing_relative_type, &c::BEQ, &c::addressing_relative, 2}, // oops cycle
    {"SBC", AddressingTypes::addressing_indirect_indexed_type, &c::SBC, &c::addressing_indirect_indexed, 5}, // oops cycle
    {"STP", AddressingTypes::addressing_implicit_type, &c::STP, &c::addressing_implicit, 0},
    {"ISC", AddressingTypes::addressing_indirect_indexed_type, &c::ISC, &c::addressing_indirect_indexed, 8},
    {"NOP", AddressingTypes::addressing_zero_page_X_type, &c::NOP, &c::addressing_zero_page_X, 4},
    {"SBC", AddressingTypes::addressing_zero_page_X_type, &c::SBC, &c::addressing_zero_page_X, 4},
    {"INC", AddressingTypes::addressing_zero_page_X_type, &c::INC, &c::addressing_zero_page_X, 6},
    {"ISC", AddressingTypes::addressing_zero_page_X_type, &c::ISC, &c::addressing_zero_page_X, 6},
    {"SED", AddressingTypes::addressing_implicit_type, &c::SED, &c::addressing_implicit, 2},
    {"SBC", AddressingTypes::addressing_absolute_Y_type, &c::SBC, &c::addressing_absolute_Y, 4}, // oops cycle
    {"NOP", AddressingTypes::addressing_implicit_type, &c::NOP, &c::addressing_implicit, 2},
    {"ISC", AddressingTypes::addressing_absolute_Y_type, &c::ISC, &c::addressing_absolute_Y, 7},
    {"NOP", AddressingTypes::addressing_absolute_X_type, &c::NOP, &c::addressing_absolute_X, 4}, // oops cycle
    {"SBC", AddressingTypes::addressing_absolute_X_type, &c::SBC, &c::addressing_absolute_X, 4}, // oops cycle
    {"INC", AddressingTypes::addressing_absolute_X_type, &c::INC, &c::addressing_absolute_X, 7},
    {"ISC", AddressingTypes::addressing_absolute_X_type, &c::ISC, &c::addressing_absolute_X, 7}
};
