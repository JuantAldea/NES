/*
http://www.nesdev.com/NESDoc.pdf
http://nesdev.com/6502.txt
http://nesdev.com/6502_cpu.txt
http://nesdev.com/6502bugs.txt
http://www.oxyron.de/html/opcodes02.html
https://www.masswerk.at/6502/6502_instruction_set.html
www.obelisk.me.uk/6502/reference.html
*/
#pragma once

#include <functional>
#include <vector>
#include "device.h"
#include "bus.h"

class CPU : public Device
{
public:
    CPU() = delete;
    CPU(Bus *b);
    void write(const uint16_t addr, const uint8_t data);
    uint8_t read(const uint16_t addr);

    void clock();

    enum class FLAGS {
        C = (1 << 0),  // carry bit
        Z = (1 << 1),  // zero
        I = (1 << 2),  // disable interrupts
        D = (1 << 3),  // decimal mode
        B0 = (1 << 4), // "B-flag 0"
        B1 = (1 << 5), // "B-Flag 1"
        V = (1 << 6),  // overflow
        N = (1 << 7),  // negative
    };

    static constexpr uint16_t STACK_BASE_ADDR = 0x0100;
    struct {
        uint8_t A;
        uint16_t X;
        uint8_t Y;
        uint8_t P;
        uint16_t PC;
        uint16_t SP;
    } registers;

    uint16_t fetched_operand;
    uint8_t current_op_code;
    uint8_t cycles_left;

protected:
    void LDA(); void LDX(); void LDY(); void STA(); void STX(); void STY();
    void TAX(); void TAY(); void TXA(); void TSX(); void TXS(); void TYA();

    void AND(); void ASL(); void BIT(); void EOR(); void LSR(); void ORA();
    void ROL(); void ROR();

    void CLC(); void CLD(); void CLI();void CLV(); void CMP(); void CPX();
    void CPY(); void SEC(); void SED(); void SEI();

    void ADC(); void DEC(); void DEX(); void DEY(); void INC(); void INX();
    void INY(); void SBC();

    void BCC(); void BCS(); void BEQ(); void BMI(); void BNE(); void BPL();
    void BVC(); void BVS();

    void JMP(); void JSR(); void RTI(); void RTS();

    void PHA(); void PHP(); void PLA(); void PLP();

    void BRK(); void NOP();

    //purely unoficial
    void STP(); void SLO(); void ANC(); void RLA();
    void ARL(); void LAX(); void AXS(); void DCP();
    void SAX(); void RRA(); void SRE(); void ALR();
    void ARR(); void ISC(); void AHX(); void SHX();
    void SHY(); void TAS(); void XAA(); void LAS();


    struct Instruction
    {
        std::string name;
        std::string description;
        //void (CPU::*operation)(void) = nullptr;
        //uint16_t (CPU::*addressing)(void) = nullptr;
        std::function<void(CPU&)> operation = nullptr;
        std::function<uint16_t(CPU&)> addressing = nullptr;
        uint8_t cycles;
    };

protected:
    uint16_t addressing_zero_page_X();
    uint16_t addressing_zero_page_Y();
    uint16_t addressing_absolute_X();
    uint16_t addressing_absolute_Y();
    uint16_t addressing_indexed_indirect();
    uint16_t addressing_indirect_indexed();

    uint16_t addressing_implicit();
    uint16_t address_accumulator();
    uint16_t addressing_immediate();
    uint16_t addressing_zero_page();
    uint16_t addressing_absolute();
    uint16_t addressing_relative();
    uint16_t addressing_indirect();

    void execute_next_instruction();
    uint8_t fetch_byte();
    uint16_t fetch_2bytes();
    void push_stack(const uint8_t byte);
    uint8_t pop_stack();
    void set_flag(const FLAGS flag, const bool value);
    bool get_flag(const FLAGS flag);

    using c = CPU;
    std::vector<Instruction> instruction_set = {
        {"BRK", "", &c::BRK, &c::addressing_implicit, 7},
        {"ORA", "", &c::ORA, &c::addressing_indexed_indirect, 6},
        {"STP", "", &c::STP, &c::addressing_implicit, 0},
        {"SLO", "", &c::SLO, &c::addressing_indexed_indirect, 8},
        {"NOP", "", &c::NOP, &c::addressing_zero_page, 3},
        {"ORA", "", &c::ORA, &c::addressing_zero_page, 3},
        {"ASL", "", &c::ASL, &c::addressing_zero_page, 5},
        {"SLO", "", &c::SLO, &c::addressing_zero_page, 5},
        {"PHP", "", &c::PHP, &c::addressing_implicit, 3},
        {"ORA", "", &c::ORA, &c::addressing_immediate, 2},
        {"ASL", "", &c::ASL, &c::addressing_implicit, 2},
        {"ANC", "", &c::ANC, &c::addressing_immediate, 2},
        {"NOP", "", &c::NOP, &c::addressing_absolute, 4},
        {"ORA", "", &c::ORA, &c::addressing_absolute, 4},
        {"ASL", "", &c::ASL, &c::addressing_absolute, 6},
        {"SLO", "", &c::SLO, &c::addressing_absolute, 6},
        {"BPL", "", &c::BPL, &c::addressing_relative, 2}, // oops cycle
        {"ORA", "", &c::ORA, &c::addressing_indirect_indexed, 5}, // oops cycle
        {"STP", "", &c::STP, &c::addressing_implicit, 0},
        {"SLO", "", &c::SLO, &c::addressing_indirect_indexed, 8},
        {"NOP", "", &c::NOP, &c::addressing_zero_page_X, 4},
        {"ORA", "", &c::ORA, &c::addressing_zero_page_X, 4},
        {"ASL", "", &c::ASL, &c::addressing_zero_page_X, 6},
        {"SLO", "", &c::SLO, &c::addressing_zero_page_X, 6},
        {"CLC", "", &c::CLC, &c::addressing_implicit, 2},
        {"ORA", "", &c::ORA, &c::addressing_absolute_Y, 4}, // oops cycle
        {"NOP", "", &c::NOP, &c::addressing_implicit, 2},
        {"SLO", "", &c::SLO, &c::addressing_absolute_Y, 7},
        {"NOP", "", &c::NOP, &c::addressing_absolute_X, 4}, // oops cycle
        {"ORA", "", &c::ORA, &c::addressing_absolute_X, 4}, // oops cycle
        {"ASL", "", &c::ASL, &c::addressing_absolute_X, 7},
        {"SLO", "", &c::SLO, &c::addressing_absolute_X, 7},
        {"JSR", "", &c::JSR, &c::addressing_absolute, 6},
        {"AND", "", &c::AND, &c::addressing_indexed_indirect, 6},
        {"STP", "", &c::STP, &c::addressing_implicit, 0},
        {"RLA", "", &c::RLA, &c::addressing_indexed_indirect, 8},
        {"BIT", "", &c::BIT, &c::addressing_zero_page, 3},
        {"AND", "", &c::AND, &c::addressing_zero_page, 3},
        {"ROL", "", &c::ROL, &c::addressing_zero_page, 5},
        {"RLA", "", &c::RLA, &c::addressing_zero_page, 5},
        {"PLP", "", &c::PLP, &c::addressing_implicit, 4},
        {"AND", "", &c::AND, &c::addressing_immediate, 2},
        {"ROL", "", &c::ROL, &c::addressing_implicit, 2},
        {"ANC", "", &c::ANC, &c::addressing_immediate, 2},
        {"BIT", "", &c::BIT, &c::addressing_absolute, 4},
        {"AND", "", &c::AND, &c::addressing_absolute, 4},
        {"ROL", "", &c::ROL, &c::addressing_absolute, 6},
        {"RLA", "", &c::RLA, &c::addressing_absolute, 6},
        {"BMI", "", &c::BMI, &c::addressing_relative, 2}, // oops cycle
        {"AND", "", &c::AND, &c::addressing_indirect_indexed, 5}, // oops cycle
        {"STP", "", &c::STP, &c::addressing_implicit, 0},
        {"RLA", "", &c::RLA, &c::addressing_indirect_indexed, 8},
        {"NOP", "", &c::NOP, &c::addressing_zero_page_X, 4},
        {"AND", "", &c::AND, &c::addressing_zero_page_X, 4},
        {"ROL", "", &c::ROL, &c::addressing_zero_page_X, 6},
        {"RLA", "", &c::RLA, &c::addressing_zero_page_X, 6},
        {"SEC", "", &c::SEC, &c::addressing_implicit, 2},
        {"AND", "", &c::AND, &c::addressing_absolute_Y, 4}, // oops cycle
        {"NOP", "", &c::NOP, &c::addressing_implicit, 2},
        {"RLA", "", &c::RLA, &c::addressing_absolute_Y, 7},
        {"NOP", "", &c::NOP, &c::addressing_absolute_X, 4}, // oops cycle
        {"AND", "", &c::AND, &c::addressing_absolute_X, 4}, // oops cycle
        {"ROL", "", &c::ROL, &c::addressing_absolute_X, 7},
        {"RLA", "", &c::RLA, &c::addressing_absolute_X, 7},
        {"RTI", "", &c::RTI, &c::addressing_implicit, 6},
        {"EOR", "", &c::EOR, &c::addressing_indexed_indirect, 6},
        {"STP", "", &c::STP, &c::addressing_implicit, 0},
        {"SRE", "", &c::SRE, &c::addressing_indexed_indirect, 8},
        {"NOP", "", &c::NOP, &c::addressing_zero_page, 3},
        {"EOR", "", &c::EOR, &c::addressing_zero_page, 3},
        {"LSR", "", &c::LSR, &c::addressing_zero_page, 5},
        {"SRE", "", &c::SRE, &c::addressing_zero_page, 5},
        {"PHA", "", &c::PHA, &c::addressing_implicit, 3},
        {"EOR", "", &c::EOR, &c::addressing_immediate, 2},
        {"LSR", "", &c::LSR, &c::addressing_implicit, 2},
        {"ALR", "", &c::ALR, &c::addressing_immediate, 2},
        {"JMP", "", &c::JMP, &c::addressing_absolute, 3},
        {"EOR", "", &c::EOR, &c::addressing_absolute, 4},
        {"LSR", "", &c::LSR, &c::addressing_absolute, 6},
        {"SRE", "", &c::SRE, &c::addressing_absolute, 6},
        {"BVC", "", &c::BVC, &c::addressing_relative, 2}, // oops cycle
        {"EOR", "", &c::EOR, &c::addressing_indirect_indexed, 5}, // oops cycle
        {"STP", "", &c::STP, &c::addressing_implicit, 0},
        {"SRE", "", &c::SRE, &c::addressing_indirect_indexed, 8},
        {"NOP", "", &c::NOP, &c::addressing_zero_page_X, 4},
        {"EOR", "", &c::EOR, &c::addressing_zero_page_X, 4},
        {"LSR", "", &c::LSR, &c::addressing_zero_page_X, 6},
        {"SRE", "", &c::SRE, &c::addressing_zero_page_X, 6},
        {"CLI", "", &c::CLI, &c::addressing_implicit, 2},
        {"EOR", "", &c::EOR, &c::addressing_absolute_Y, 4}, // oops cycle
        {"NOP", "", &c::NOP, &c::addressing_implicit, 2},
        {"SRE", "", &c::SRE, &c::addressing_absolute_Y, 7},
        {"NOP", "", &c::NOP, &c::addressing_absolute_X, 4}, // oops cycle
        {"EOR", "", &c::EOR, &c::addressing_absolute_X, 4}, // oops cycle
        {"LSR", "", &c::LSR, &c::addressing_absolute_X, 7},
        {"SRE", "", &c::SRE, &c::addressing_absolute_X, 7},
        {"RTS", "", &c::RTS, &c::addressing_implicit, 6},
        {"ADC", "", &c::ADC, &c::addressing_indexed_indirect, 6},
        {"STP", "", &c::STP, &c::addressing_implicit, 0},
        {"RRA", "", &c::RRA, &c::addressing_indexed_indirect, 8},
        {"NOP", "", &c::NOP, &c::addressing_zero_page, 3},
        {"ADC", "", &c::ADC, &c::addressing_zero_page, 3},
        {"ROR", "", &c::ROR, &c::addressing_zero_page, 5},
        {"RRA", "", &c::RRA, &c::addressing_zero_page, 5},
        {"PLA", "", &c::PLA, &c::addressing_implicit, 4},
        {"ADC", "", &c::ADC, &c::addressing_immediate, 2},
        {"ROR", "", &c::ROR, &c::addressing_implicit, 2},
        {"ARR", "", &c::ARR, &c::addressing_immediate, 2},
        {"JMP", "", &c::JMP, &c::addressing_indirect, 5},
        {"ADC", "", &c::ADC, &c::addressing_absolute, 4},
        {"ROR", "", &c::ROR, &c::addressing_absolute, 6},
        {"RRA", "", &c::RRA, &c::addressing_absolute, 6},
        {"BVS", "", &c::BVS, &c::addressing_relative, 2}, // oops cycle
        {"ADC", "", &c::ADC, &c::addressing_indirect_indexed, 5}, // oops cycle
        {"STP", "", &c::STP, &c::addressing_implicit, 0},
        {"RRA", "", &c::RRA, &c::addressing_indirect_indexed, 8},
        {"NOP", "", &c::NOP, &c::addressing_zero_page_X, 4},
        {"ADC", "", &c::ADC, &c::addressing_zero_page_X, 4},
        {"ROR", "", &c::ROR, &c::addressing_zero_page_X, 6},
        {"RRA", "", &c::RRA, &c::addressing_zero_page_X, 6},
        {"SEI", "", &c::SEI, &c::addressing_implicit, 2},
        {"ADC", "", &c::ADC, &c::addressing_absolute_Y, 4}, // oops cycle
        {"NOP", "", &c::NOP, &c::addressing_implicit, 2},
        {"RRA", "", &c::RRA, &c::addressing_absolute_Y, 7},
        {"NOP", "", &c::NOP, &c::addressing_absolute_X, 4}, // oops cycle
        {"ADC", "", &c::ADC, &c::addressing_absolute_X, 4}, // oops cycle
        {"ROR", "", &c::ROR, &c::addressing_absolute_X, 7},
        {"RRA", "", &c::RRA, &c::addressing_absolute_X, 7},
        {"NOP", "", &c::NOP, &c::addressing_immediate, 2},
        {"STA", "", &c::STA, &c::addressing_indexed_indirect, 6},
        {"NOP", "", &c::NOP, &c::addressing_immediate, 2},
        {"SAX", "", &c::SAX, &c::addressing_indexed_indirect, 6},
        {"STY", "", &c::STY, &c::addressing_zero_page, 3},
        {"STA", "", &c::STA, &c::addressing_zero_page, 3},
        {"STX", "", &c::STX, &c::addressing_zero_page, 3},
        {"SAX", "", &c::SAX, &c::addressing_zero_page, 3},
        {"DEY", "", &c::DEY, &c::addressing_implicit, 2},
        {"NOP", "", &c::NOP, &c::addressing_immediate, 2},
        {"TXA", "", &c::TXA, &c::addressing_implicit, 2},
        {"XAA", "", &c::XAA, &c::addressing_immediate, 2},
        {"STY", "", &c::STY, &c::addressing_absolute, 4},
        {"STA", "", &c::STA, &c::addressing_absolute, 4},
        {"STX", "", &c::STX, &c::addressing_absolute, 4},
        {"SAX", "", &c::SAX, &c::addressing_absolute, 4},
        {"BCC", "", &c::BCC, &c::addressing_relative, 2}, // oops cycle
        {"STA", "", &c::STA, &c::addressing_indirect_indexed, 6},
        {"STP", "", &c::STP, &c::addressing_implicit, 0},
        {"AHX", "", &c::AHX, &c::addressing_indirect_indexed, 6},
        {"STY", "", &c::STY, &c::addressing_zero_page_X, 4},
        {"STA", "", &c::STA, &c::addressing_zero_page_X, 4},
        {"STX", "", &c::STX, &c::addressing_zero_page_Y, 4},
        {"SAX", "", &c::SAX, &c::addressing_zero_page_Y, 4},
        {"TYA", "", &c::TYA, &c::addressing_implicit, 2},
        {"STA", "", &c::STA, &c::addressing_absolute_Y, 5},
        {"TXS", "", &c::TXS, &c::addressing_implicit, 2},
        {"TAS", "", &c::TAS, &c::addressing_absolute_Y, 5},
        {"SHY", "", &c::SHY, &c::addressing_absolute_X, 5},
        {"STA", "", &c::STA, &c::addressing_absolute_X, 5},
        {"SHX", "", &c::SHX, &c::addressing_absolute_Y, 5},
        {"AHX", "", &c::AHX, &c::addressing_absolute_Y, 5},
        {"LDY", "", &c::LDY, &c::addressing_immediate, 2},
        {"LDA", "", &c::LDA, &c::addressing_indexed_indirect, 6},
        {"LDX", "", &c::LDX, &c::addressing_immediate, 2},
        {"LAX", "", &c::LAX, &c::addressing_indexed_indirect, 6},
        {"LDY", "", &c::LDY, &c::addressing_zero_page, 3},
        {"LDA", "", &c::LDA, &c::addressing_zero_page, 3},
        {"LDX", "", &c::LDX, &c::addressing_zero_page, 3},
        {"LAX", "", &c::LAX, &c::addressing_zero_page, 3},
        {"TAY", "", &c::TAY, &c::addressing_implicit, 2},
        {"LDA", "", &c::LDA, &c::addressing_immediate, 2},
        {"TAX", "", &c::TAX, &c::addressing_implicit, 2},
        {"LAX", "", &c::LAX, &c::addressing_immediate, 2},
        {"LDY", "", &c::LDY, &c::addressing_absolute, 4},
        {"LDA", "", &c::LDA, &c::addressing_absolute, 4},
        {"LDX", "", &c::LDX, &c::addressing_absolute, 4},
        {"LAX", "", &c::LAX, &c::addressing_absolute, 4},
        {"BCS", "", &c::BCS, &c::addressing_relative, 2}, // oops cycle
        {"LDA", "", &c::LDA, &c::addressing_indirect_indexed, 5}, // oops cycle
        {"STP", "", &c::STP, &c::addressing_implicit, 0},
        {"LAX", "", &c::LAX, &c::addressing_indirect_indexed, 5}, // oops cycle
        {"LDY", "", &c::LDY, &c::addressing_zero_page_X, 4},
        {"LDA", "", &c::LDA, &c::addressing_zero_page_X, 4},
        {"LDX", "", &c::LDX, &c::addressing_zero_page_Y, 4},
        {"LAX", "", &c::LAX, &c::addressing_zero_page_Y, 4},
        {"CLV", "", &c::CLV, &c::addressing_implicit, 2},
        {"LDA", "", &c::LDA, &c::addressing_absolute_Y, 4}, // oops cycle
        {"TSX", "", &c::TSX, &c::addressing_implicit, 2},
        {"LAS", "", &c::LAS, &c::addressing_absolute_Y, 4}, // oops cycle
        {"LDY", "", &c::LDY, &c::addressing_absolute_X, 4}, // oops cycle
        {"LDA", "", &c::LDA, &c::addressing_absolute_X, 4}, // oops cycle
        {"LDX", "", &c::LDX, &c::addressing_absolute_Y, 4}, // oops cycle
        {"LAX", "", &c::LAX, &c::addressing_absolute_Y, 4}, // oops cycle
        {"CPY", "", &c::CPY, &c::addressing_immediate, 2},
        {"CMP", "", &c::CMP, &c::addressing_indexed_indirect, 6},
        {"NOP", "", &c::NOP, &c::addressing_immediate, 2},
        {"DCP", "", &c::DCP, &c::addressing_indexed_indirect, 8},
        {"CPY", "", &c::CPY, &c::addressing_zero_page, 3},
        {"CMP", "", &c::CMP, &c::addressing_zero_page, 3},
        {"DEC", "", &c::DEC, &c::addressing_zero_page, 5},
        {"DCP", "", &c::DCP, &c::addressing_zero_page, 5},
        {"INY", "", &c::INY, &c::addressing_implicit, 2},
        {"CMP", "", &c::CMP, &c::addressing_immediate, 2},
        {"DEX", "", &c::DEX, &c::addressing_implicit, 2},
        {"AXS", "", &c::AXS, &c::addressing_immediate, 2},
        {"CPY", "", &c::CPY, &c::addressing_absolute, 4},
        {"CMP", "", &c::CMP, &c::addressing_absolute, 4},
        {"DEC", "", &c::DEC, &c::addressing_absolute, 6},
        {"DCP", "", &c::DCP, &c::addressing_absolute, 6},
        {"BNE", "", &c::BNE, &c::addressing_relative, 2}, // oops cycle
        {"CMP", "", &c::CMP, &c::addressing_indirect_indexed, 5}, // oops cycle
        {"STP", "", &c::STP, &c::addressing_implicit, 0},
        {"DCP", "", &c::DCP, &c::addressing_indirect_indexed, 8},
        {"NOP", "", &c::NOP, &c::addressing_zero_page_X, 4},
        {"CMP", "", &c::CMP, &c::addressing_zero_page_X, 4},
        {"DEC", "", &c::DEC, &c::addressing_zero_page_X, 6},
        {"DCP", "", &c::DCP, &c::addressing_zero_page_X, 6},
        {"CLD", "", &c::CLD, &c::addressing_implicit, 2},
        {"CMP", "", &c::CMP, &c::addressing_absolute_Y, 4}, // oops cycle
        {"NOP", "", &c::NOP, &c::addressing_implicit, 2},
        {"DCP", "", &c::DCP, &c::addressing_absolute_Y, 7},
        {"NOP", "", &c::NOP, &c::addressing_absolute_X, 4}, // oops cycle
        {"CMP", "", &c::CMP, &c::addressing_absolute_X, 4}, // oops cycle
        {"DEC", "", &c::DEC, &c::addressing_absolute_X, 7},
        {"DCP", "", &c::DCP, &c::addressing_absolute_X, 7},
        {"CPX", "", &c::CPX, &c::addressing_immediate, 2},
        {"SBC", "", &c::SBC, &c::addressing_indexed_indirect, 6},
        {"NOP", "", &c::NOP, &c::addressing_immediate, 2},
        {"ISC", "", &c::ISC, &c::addressing_indexed_indirect, 8},
        {"CPX", "", &c::CPX, &c::addressing_zero_page, 3},
        {"SBC", "", &c::SBC, &c::addressing_zero_page, 3},
        {"INC", "", &c::INC, &c::addressing_zero_page, 5},
        {"ISC", "", &c::ISC, &c::addressing_zero_page, 5},
        {"INX", "", &c::INX, &c::addressing_implicit, 2},
        {"SBC", "", &c::SBC, &c::addressing_immediate, 2},
        {"NOP", "", &c::NOP, &c::addressing_implicit, 2},
        {"SBC", "", &c::SBC, &c::addressing_immediate, 2},
        {"CPX", "", &c::CPX, &c::addressing_absolute, 4},
        {"SBC", "", &c::SBC, &c::addressing_absolute, 4},
        {"INC", "", &c::INC, &c::addressing_absolute, 6},
        {"ISC", "", &c::ISC, &c::addressing_absolute, 6},
        {"BEQ", "", &c::BEQ, &c::addressing_relative, 2}, // oops cycle
        {"SBC", "", &c::SBC, &c::addressing_indirect_indexed, 5}, // oops cycle
        {"STP", "", &c::STP, &c::addressing_implicit, 0},
        {"ISC", "", &c::ISC, &c::addressing_indirect_indexed, 8},
        {"NOP", "", &c::NOP, &c::addressing_zero_page_X, 4},
        {"SBC", "", &c::SBC, &c::addressing_zero_page_X, 4},
        {"INC", "", &c::INC, &c::addressing_zero_page_X, 6},
        {"ISC", "", &c::ISC, &c::addressing_zero_page_X, 6},
        {"SED", "", &c::SED, &c::addressing_implicit, 2},
        {"SBC", "", &c::SBC, &c::addressing_absolute_Y, 4}, // oops cycle
        {"NOP", "", &c::NOP, &c::addressing_implicit, 2},
        {"ISC", "", &c::ISC, &c::addressing_absolute_Y, 7},
        {"NOP", "", &c::NOP, &c::addressing_absolute_X, 4}, // oops cycle
        {"SBC", "", &c::SBC, &c::addressing_absolute_X, 4}, // oops cycle
        {"INC", "", &c::INC, &c::addressing_absolute_X, 7},
        {"ISC", "", &c::ISC, &c::addressing_absolute_X,7}
    };
};
