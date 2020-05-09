/*
http://www.nesdev.com/NESDoc.pdf
http://nesdev.com/6502.txt
http://nesdev.com/6502_cpu.txt
http://nesdev.com/6502bugs.txt
http://www.oxyron.de/html/opcodes02.html
https://www.masswerk.at/6502/6502_instruction_set.html
http://www.obelisk.me.uk/6502/reference.html
*/
#pragma once

#include <functional>
#include <valarray>
#include "device.h"
#include "addressing_types.h"
#include <ostream>

#include <QObject>
class Bus;
class CPU : public QObject, public Device
{
   Q_OBJECT

public:
    CPU() = delete;
    CPU(Bus *b);
    void write(const uint16_t addr, const uint8_t data);
    uint8_t read(const uint16_t addr);
    uint8_t read(const uint16_t addr) const;

    enum class FLAGS {
        C = (1 << 0),  // carry bit
        Z = (1 << 1),  // zero
        I = (1 << 2),  // disable interrupts
        D = (1 << 3),  // decimal mode
        B = (1 << 4), // "B-flag "
        U = (1 << 5), // "Unused"
        V = (1 << 6),  // overflow
        N = (1 << 7),  // negative
    };

    static constexpr uint16_t STACK_BASE_ADDR = 0x0100;
    struct {
        uint8_t A = 0;
        uint8_t X = 0;
        uint8_t Y = 0;
        uint8_t P = 0;
        uint16_t PC = 0;
        uint8_t SP = 0;
    } registers;

    void set_flag(const FLAGS flag, const bool value);
    bool get_flag(const FLAGS flag) const;

    uint16_t fetched_operand = 0;
    uint8_t current_op_code = 0;
    uint8_t cycles_left = 0;
    uint16_t previous_pc = 0;

    friend std::ostream& operator<<(std::ostream& os, const CPU& cpu);


public slots:
    bool clock();
    void execute_next_instruction(const bool update_debugger);
    void reset();
    uint8_t& register_A() { return registers.A; };
    uint8_t& register_X() { return registers.X; };
    uint8_t& register_Y() { return registers.Y; };
    uint8_t& register_P() { return registers.P; };
    uint16_t& register_PC() { return registers.PC; };
    uint8_t& register_SP() { return registers.SP; };


signals:
    void updated();

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
private:
    void ADC_SBC_internal(const uint8_t value);

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

    uint8_t fetch_byte();
    uint16_t fetch_2bytes();
    void push_stack(const uint8_t byte);
    uint8_t pop_stack();

public:
    friend class Instruction;
};
