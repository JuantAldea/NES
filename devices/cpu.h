#pragma once
#include "device.h"
#include <functional>

class CPU : public Device
{
public:
    CPU() = delete;
    CPU(Bus &b);
    void write(uint16_t addr, uint8_t data) { };
    uint8_t read(uint16_t addr) {};

    void CPU::clock();

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
    uint8_t acc;
    uint16_t x;
    uint8_t y;
    uint8_t p;
    uint16_t pc;
    uint16_t sp;
    uint16_t target_address;

    uint8_t cycles_left;

protected:
    void LDA(); void LDX(); void LDY(); void STA(); void STX(); void STY();
    void TAX(); void TAY(); void TXA(); void TSX(); void TXS(); void TYA();

    void AND(); void ASL(); void BIT(); void EOR(); void LSR(); void ORA();
    void ROL(); void ROR();

    void CLC(); void CLD(); void CLI(); void CMP(); void CPX(); void CPY();
    void SEC(); void SED(); void SEI();

    void ADC(); void DEC(); void DEX(); void DEY(); void INC(); void INX();
    void INY(); void SBC();

    void BCC(); void BCS(); void BEQ(); void BMI(); void BNE(); void BPL();
    void BVC(); void BVS();

    void JMP(); void JSR(); void RTI(); void RTS();

    void PHA(); void PHP(); void PLA(); void PLP();
    void PHA_PHP(); void PLA_PLP(); // watch out!

    void BRK(); void NOP();
    //purely unoficial
    void STP(); void SLO(); void ANC(); void RLA();
    void ARL(); void LAX(); void AXS(); void DCP();
    void SAX(); void RRA(); void SRE(); void ALR();
    void ARR(); void ISC(); void AHX(); void SHX();
    void SHY(); void TAS(); void XAA(); void LAS();
    void CLV();

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
    uint16_t address_zero_page_X();
    uint16_t address_zero_page_Y();
    uint16_t address_absolute_X();
    uint16_t address_absolute_Y();
    uint16_t address_indexed_indirect();
    uint16_t address_indirect_indexed();

    uint16_t address_implicit();
    uint16_t address_accumulator();
    uint16_t address_immediate();
    uint16_t address_zero_page();
    uint16_t address_absolute();
    uint16_t address_relative();
    uint16_t address_indirect();

    uint16_t address_none();

    void execute_next_instruction();
    uint8_t fetch_byte();
    uint16_t fetch_2bytes();
    void push_stack(const uint8_t byte);
    uint8_t pop_stack();
    void set_flag(const FLAGS flag, const bool value);

    using c = CPU;
    std::vector<Instruction> instruction_set = {
        {"BRK", "", &c::BRK, &c::address_implicit, 7},
        {"ORA", "", &c::ORA, &c::address_indexed_indirect, 6},
        {"STP", "", &c::STP, &c::address_none, 0},
        {"SLO", "", &c::SLO, &c::address_indexed_indirect, 8},
        {"NOP", "", &c::NOP, &c::address_zero_page, 3},
        {"ORA", "", &c::ORA, &c::address_zero_page, 3},
        {"ASL", "", &c::ASL, &c::address_zero_page, 5},
        {"SLO", "", &c::SLO, &c::address_zero_page, 5},
        {"PHP", "", &c::PHP, &c::address_implicit, 3},
        {"ORA", "", &c::ORA, &c::address_immediate, 2},
        {"ASL", "", &c::ASL, &c::address_implicit, 2},
        {"ANC", "", &c::ANC, &c::address_immediate, 2},
        {"NOP", "", &c::NOP, &c::address_absolute, 4},
        {"ORA", "", &c::ORA, &c::address_absolute, 4},
        {"ASL", "", &c::ASL, &c::address_absolute, 6},
        {"SLO", "", &c::SLO, &c::address_absolute, 6},
        {"BPL", "", &c::BPL, &c::address_relative, 2}, // oops cycle
        {"ORA", "", &c::ORA, &c::address_indirect_indexed, 5}, // oops cycle
        {"STP", "", &c::STP, &c::address_none, 0},
        {"SLO", "", &c::SLO, &c::address_indirect_indexed, 8},
        {"NOP", "", &c::NOP, &c::address_zero_page_X, 4},
        {"ORA", "", &c::ORA, &c::address_zero_page_X, 4},
        {"ASL", "", &c::ASL, &c::address_zero_page_X, 6},
        {"SLO", "", &c::SLO, &c::address_zero_page_X, 6},
        {"CLC", "", &c::CLC, &c::address_implicit, 2},
        {"ORA", "", &c::ORA, &c::address_absolute_Y, 4}, // oops cycle
        {"NOP", "", &c::NOP, &c::address_implicit, 2},
        {"SLO", "", &c::SLO, &c::address_absolute_Y, 7},
        {"NOP", "", &c::NOP, &c::address_absolute_X, 4}, // oops cycle
        {"ORA", "", &c::ORA, &c::address_absolute_X, 4}, // oops cycle
        {"ASL", "", &c::ASL, &c::address_absolute_X, 7},
        {"SLO", "", &c::SLO, &c::address_absolute_X, 7},
        {"JSR", "", &c::JSR, &c::address_absolute, 6},
        {"AND", "", &c::AND, &c::address_indexed_indirect, 6},
        {"STP", "", &c::STP, &c::address_none, 0},
        {"RLA", "", &c::RLA, &c::address_indexed_indirect, 8},
        {"BIT", "", &c::BIT, &c::address_zero_page, 3},
        {"AND", "", &c::AND, &c::address_zero_page, 3},
        {"ROL", "", &c::ROL, &c::address_zero_page, 5},
        {"RLA", "", &c::RLA, &c::address_zero_page, 5},
        {"PLP", "", &c::PLP, &c::address_implicit, 4},
        {"AND", "", &c::AND, &c::address_immediate, 2},
        {"ROL", "", &c::ROL, &c::address_none, 2},
        {"ANC", "", &c::ANC, &c::address_immediate, 2},
        {"BIT", "", &c::BIT, &c::address_absolute, 4},
        {"AND", "", &c::AND, &c::address_absolute, 4},
        {"ROL", "", &c::ROL, &c::address_absolute, 6},
        {"RLA", "", &c::RLA, &c::address_absolute, 6},
        {"BMI", "", &c::BMI, &c::address_relative, 2}, // oops cycle
        {"AND", "", &c::AND, &c::address_indirect_indexed, 5}, // oops cycle
        {"STP", "", &c::STP, &c::address_none, 0},
        {"RLA", "", &c::RLA, &c::address_indirect_indexed, 8},
        {"NOP", "", &c::NOP, &c::address_zero_page_X, 4},
        {"AND", "", &c::AND, &c::address_zero_page_X, 4},
        {"ROL", "", &c::ROL, &c::address_zero_page_X, 6},
        {"RLA", "", &c::RLA, &c::address_zero_page_X, 6},
        {"SEC", "", &c::SEC, &c::address_implicit, 2},
        {"AND", "", &c::AND, &c::address_absolute_Y, 4}, // oops cycle
        {"NOP", "", &c::NOP, &c::address_implicit, 2},
        {"RLA", "", &c::RLA, &c::address_absolute_Y, 7},
        {"NOP", "", &c::NOP, &c::address_absolute_X, 4}, // oops cycle
        {"AND", "", &c::AND, &c::address_absolute_X, 4}, // oops cycle
        {"ROL", "", &c::ROL, &c::address_absolute_X, 7},
        {"RLA", "", &c::RLA, &c::address_absolute_X, 7},
        {"RTI", "", &c::RTI, &c::address_implicit, 6},
        {"EOR", "", &c::EOR, &c::address_indexed_indirect, 6},
        {"STP", "", &c::STP, &c::address_none, 0},
        {"SRE", "", &c::SRE, &c::address_indexed_indirect, 8},
        {"NOP", "", &c::NOP, &c::address_zero_page, 3},
        {"EOR", "", &c::EOR, &c::address_zero_page, 3},
        {"LSR", "", &c::LSR, &c::address_zero_page, 5},
        {"SRE", "", &c::SRE, &c::address_zero_page, 5},
        {"PHA", "", &c::PHA, &c::address_implicit, 3},
        {"EOR", "", &c::EOR, &c::address_immediate, 2},
        {"LSR", "", &c::LSR, &c::address_implicit, 2},
        {"ALR", "", &c::ALR, &c::address_immediate, 2},
        {"JMP", "", &c::JMP, &c::address_absolute, 3},
        {"EOR", "", &c::EOR, &c::address_absolute, 4},
        {"LSR", "", &c::LSR, &c::address_absolute, 6},
        {"SRE", "", &c::SRE, &c::address_absolute, 6},
        {"BVC", "", &c::BVC, &c::address_relative, 2}, // oops cycle
        {"EOR", "", &c::EOR, &c::address_indirect_indexed, 5}, // oops cycle
        {"STP", "", &c::STP, &c::address_none, 0},
        {"SRE", "", &c::SRE, &c::address_indirect_indexed, 8},
        {"NOP", "", &c::NOP, &c::address_zero_page_X, 4},
        {"EOR", "", &c::EOR, &c::address_zero_page_X, 4},
        {"LSR", "", &c::LSR, &c::address_zero_page_X, 6},
        {"SRE", "", &c::SRE, &c::address_zero_page_X, 6},
        {"CLI", "", &c::CLI, &c::address_implicit, 2},
        {"EOR", "", &c::EOR, &c::address_absolute_Y, 4}, // oops cycle
        {"NOP", "", &c::NOP, &c::address_implicit, 2},
        {"SRE", "", &c::SRE, &c::address_absolute_Y, 7},
        {"NOP", "", &c::NOP, &c::address_absolute_X, 4}, // oops cycle
        {"EOR", "", &c::EOR, &c::address_absolute_X, 4}, // oops cycle
        {"LSR", "", &c::LSR, &c::address_absolute_X, 7},
        {"SRE", "", &c::SRE, &c::address_absolute_X, 7},
        {"RTS", "", &c::RTS, &c::address_implicit, 6},
        {"ADC", "", &c::ADC, &c::address_indexed_indirect, 6},
        {"STP", "", &c::STP, &c::address_none, 0},
        {"RRA", "", &c::RRA, &c::address_indexed_indirect, 8},
        {"NOP", "", &c::NOP, &c::address_zero_page, 3},
        {"ADC", "", &c::ADC, &c::address_zero_page, 3},
        {"ROR", "", &c::ROR, &c::address_zero_page, 5},
        {"RRA", "", &c::RRA, &c::address_zero_page, 5},
        {"PLA", "", &c::PLA, &c::address_implicit, 4},
        {"ADC", "", &c::ADC, &c::address_immediate, 2},
        {"ROR", "", &c::ROR, &c::address_implicit, 2},
        {"ARR", "", &c::ARR, &c::address_immediate, 2},
        {"JMP", "", &c::JMP, &c::address_indirect, 5},
        {"ADC", "", &c::ADC, &c::address_absolute, 4},
        {"ROR", "", &c::ROR, &c::address_absolute, 6},
        {"RRA", "", &c::RRA, &c::address_absolute, 6},
        {"BVS", "", &c::BVS, &c::address_relative, 2}, // oops cycle
        {"ADC", "", &c::ADC, &c::address_indirect_indexed, 5}, // oops cycle
        {"STP", "", &c::STP, &c::address_none, 0},
        {"RRA", "", &c::RRA, &c::address_indirect_indexed, 8},
        {"NOP", "", &c::NOP, &c::address_zero_page_X, 4},
        {"ADC", "", &c::ADC, &c::address_zero_page_X, 4},
        {"ROR", "", &c::ROR, &c::address_zero_page_X, 6},
        {"RRA", "", &c::RRA, &c::address_zero_page_X, 6},
        {"SEI", "", &c::SEI, &c::address_implicit, 2},
        {"ADC", "", &c::ADC, &c::address_absolute_Y, 4}, // oops cycle
        {"NOP", "", &c::NOP, &c::address_implicit, 2},
        {"RRA", "", &c::RRA, &c::address_absolute_Y, 7},
        {"NOP", "", &c::NOP, &c::address_absolute_X, 4}, // oops cycle
        {"ADC", "", &c::ADC, &c::address_absolute_X, 4}, // oops cycle
        {"ROR", "", &c::ROR, &c::address_absolute_X, 7},
        {"RRA", "", &c::RRA, &c::address_absolute_X, 7},
        {"NOP", "", &c::NOP, &c::address_immediate, 2},
        {"STA", "", &c::STA, &c::address_indexed_indirect, 6},
        {"NOP", "", &c::NOP, &c::address_immediate, 2},
        {"SAX", "", &c::SAX, &c::address_indexed_indirect, 6},
        {"STY", "", &c::STY, &c::address_zero_page, 3},
        {"STA", "", &c::STA, &c::address_zero_page, 3},
        {"STX", "", &c::STX, &c::address_zero_page, 3},
        {"SAX", "", &c::SAX, &c::address_zero_page, 3},
        {"DEY", "", &c::DEY, &c::address_implicit, 2},
        {"NOP", "", &c::NOP, &c::address_immediate, 2},
        {"TXA", "", &c::TXA, &c::address_implicit, 2},
        {"XAA", "", &c::XAA, &c::address_immediate, 2},
        {"STY", "", &c::STY, &c::address_absolute, 4},
        {"STA", "", &c::STA, &c::address_absolute, 4},
        {"STX", "", &c::STX, &c::address_absolute, 4},
        {"SAX", "", &c::SAX, &c::address_absolute, 4},
        {"BCC", "", &c::BCC, &c::address_relative, 2}, // oops cycle
        {"STA", "", &c::STA, &c::address_indirect_indexed, 6},
        {"STP", "", &c::STP, &c::address_none, 0},
        {"AHX", "", &c::AHX, &c::address_indirect_indexed, 6},
        {"STY", "", &c::STY, &c::address_zero_page_X, 4},
        {"STA", "", &c::STA, &c::address_zero_page_X, 4},
        {"STX", "", &c::STX, &c::address_zero_page_Y, 4},
        {"SAX", "", &c::SAX, &c::address_zero_page_Y, 4},
        {"TYA", "", &c::TYA, &c::address_implicit, 2},
        {"STA", "", &c::STA, &c::address_absolute_Y, 5},
        {"TXS", "", &c::TXS, &c::address_implicit, 2},
        {"TAS", "", &c::TAS, &c::address_absolute_Y, 5},
        {"SHY", "", &c::SHY, &c::address_absolute_X, 5},
        {"STA", "", &c::STA, &c::address_absolute_X, 5},
        {"SHX", "", &c::SHX, &c::address_absolute_Y, 5},
        {"AHX", "", &c::AHX, &c::address_absolute_Y, 5},
        {"LDY", "", &c::LDY, &c::address_immediate, 2},
        {"LDA", "", &c::LDA, &c::address_indexed_indirect, 6},
        {"LDX", "", &c::LDX, &c::address_immediate, 2},
        {"LAX", "", &c::LAX, &c::address_indexed_indirect, 6},
        {"LDY", "", &c::LDY, &c::address_zero_page, 3},
        {"LDA", "", &c::LDA, &c::address_zero_page, 3},
        {"LDX", "", &c::LDX, &c::address_zero_page, 3},
        {"LAX", "", &c::LAX, &c::address_zero_page, 3},
        {"TAY", "", &c::TAY, &c::address_implicit, 2},
        {"LDA", "", &c::LDA, &c::address_immediate, 2},
        {"TAX", "", &c::TAX, &c::address_implicit, 2},
        {"LAX", "", &c::LAX, &c::address_immediate, 2},
        {"LDY", "", &c::LDY, &c::address_absolute, 4},
        {"LDA", "", &c::LDA, &c::address_absolute, 4},
        {"LDX", "", &c::LDX, &c::address_absolute, 4},
        {"LAX", "", &c::LAX, &c::address_absolute, 4},
        {"BCS", "", &c::BCS, &c::address_relative, 2}, // oops cycle
        {"LDA", "", &c::LDA, &c::address_indirect_indexed, 5}, // oops cycle
        {"STP", "", &c::STP, &c::address_none, 0},
        {"LAX", "", &c::LAX, &c::address_indirect_indexed, 5}, // oops cycle
        {"LDY", "", &c::LDY, &c::address_zero_page_X, 4},
        {"LDA", "", &c::LDA, &c::address_zero_page_X, 4},
        {"LDX", "", &c::LDX, &c::address_zero_page_Y, 4},
        {"LAX", "", &c::LAX, &c::address_zero_page_Y, 4},
        {"CLV", "", &c::CLV, &c::address_implicit, 2},
        {"LDA", "", &c::LDA, &c::address_absolute_Y, 4}, // oops cycle
        {"TSX", "", &c::TSX, &c::address_implicit, 2},
        {"LAS", "", &c::LAS, &c::address_absolute_Y, 4}, // oops cycle
        {"LDY", "", &c::LDY, &c::address_absolute_X, 4}, // oops cycle
        {"LDA", "", &c::LDA, &c::address_absolute_X, 4}, // oops cycle
        {"LDX", "", &c::LDX, &c::address_absolute_Y, 4}, // oops cycle
        {"LAX", "", &c::LAX, &c::address_absolute_Y, 4}, // oops cycle
        {"CPY", "", &c::CPY, &c::address_immediate, 2},
        {"CMP", "", &c::CMP, &c::address_indexed_indirect, 6},
        {"NOP", "", &c::NOP, &c::address_immediate, 2},
        {"DCP", "", &c::DCP, &c::address_indexed_indirect, 8},
        {"CPY", "", &c::CPY, &c::address_zero_page, 3},
        {"CMP", "", &c::CMP, &c::address_zero_page, 3},
        {"DEC", "", &c::DEC, &c::address_zero_page, 5},
        {"DCP", "", &c::DCP, &c::address_zero_page, 5},
        {"INY", "", &c::INY, &c::address_implicit, 2},
        {"CMP", "", &c::CMP, &c::address_immediate, 2},
        {"DEX", "", &c::DEX, &c::address_implicit, 2},
        {"AXS", "", &c::AXS, &c::address_immediate, 2},
        {"CPY", "", &c::CPY, &c::address_absolute, 4},
        {"CMP", "", &c::CMP, &c::address_absolute, 4},
        {"DEC", "", &c::DEC, &c::address_absolute, 6},
        {"DCP", "", &c::DCP, &c::address_absolute, 6},
        {"BNE", "", &c::BNE, &c::address_relative, 2}, // oops cycle
        {"CMP", "", &c::CMP, &c::address_indirect_indexed, 5}, // oops cycle
        {"STP", "", &c::STP, &c::address_none, 0},
        {"DCP", "", &c::DCP, &c::address_indirect_indexed, 8},
        {"NOP", "", &c::NOP, &c::address_zero_page_X, 4},
        {"CMP", "", &c::CMP, &c::address_zero_page_X, 4},
        {"DEC", "", &c::DEC, &c::address_zero_page_X, 6},
        {"DCP", "", &c::DCP, &c::address_zero_page_X, 6},
        {"CLD", "", &c::CLD, &c::address_implicit, 2},
        {"CMP", "", &c::CMP, &c::address_absolute_Y, 4}, // oops cycle
        {"NOP", "", &c::NOP, &c::address_implicit, 2},
        {"DCP", "", &c::DCP, &c::address_absolute_Y, 7},
        {"NOP", "", &c::NOP, &c::address_absolute_X, 4}, // oops cycle
        {"CMP", "", &c::CMP, &c::address_absolute_X, 4}, // oops cycle
        {"DEC", "", &c::DEC, &c::address_absolute_X, 7},
        {"DCP", "", &c::DCP, &c::address_absolute_X, 7},
        {"CPX", "", &c::CPX, &c::address_immediate, 2},
        {"SBC", "", &c::SBC, &c::address_indexed_indirect, 6},
        {"NOP", "", &c::NOP, &c::address_immediate, 2},
        {"ISC", "", &c::ISC, &c::address_indexed_indirect, 8},
        {"CPX", "", &c::CPX, &c::address_zero_page, 3},
        {"SBC", "", &c::SBC, &c::address_zero_page, 3},
        {"INC", "", &c::INC, &c::address_zero_page, 5},
        {"ISC", "", &c::ISC, &c::address_zero_page, 5},
        {"INX", "", &c::INX, &c::address_implicit, 2},
        {"SBC", "", &c::SBC, &c::address_immediate, 2},
        {"NOP", "", &c::NOP, &c::address_implicit, 2},
        {"SBC", "", &c::SBC, &c::address_immediate, 2},
        {"CPX", "", &c::CPX, &c::address_absolute, 4},
        {"SBC", "", &c::SBC, &c::address_absolute, 4},
        {"INC", "", &c::INC, &c::address_absolute, 6},
        {"ISC", "", &c::ISC, &c::address_absolute, 6},
        {"BEQ", "", &c::BEQ, &c::address_relative, 2}, // oops cycle
        {"SBC", "", &c::SBC, &c::address_indirect_indexed, 5}, // oops cycle
        {"STP", "", &c::STP, &c::address_none, 0},
        {"ISC", "", &c::ISC, &c::address_indirect_indexed, 8},
        {"NOP", "", &c::NOP, &c::address_zero_page_X, 4},
        {"SBC", "", &c::SBC, &c::address_zero_page_X, 4},
        {"INC", "", &c::INC, &c::address_zero_page_X, 6},
        {"ISC", "", &c::ISC, &c::address_zero_page_X, 6},
        {"SED", "", &c::SED, &c::address_implicit, 2},
        {"SBC", "", &c::SBC, &c::address_absolute_Y, 4}, // oops cycle
        {"NOP", "", &c::NOP, &c::address_implicit, 2},
        {"ISC", "", &c::ISC, &c::address_absolute_Y, 7},
        {"NOP", "", &c::NOP, &c::address_absolute_X, 4}, // oops cycle
        {"SBC", "", &c::SBC, &c::address_absolute_X, 4}, // oops cycle
        {"INC", "", &c::INC, &c::address_absolute_X, 7},
        {"ISC", "", &c::ISC, &c::address_absolute_X,7}
    };
};
