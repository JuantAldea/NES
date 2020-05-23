#include "cpu.h"

#include <iostream>

#include "instruction.h"

uint8_t low_byte(const uint16_t twobytes) { return static_cast<uint8_t>(twobytes); }

uint8_t high_byte(const uint16_t twobytes) { return static_cast<uint8_t>(twobytes >> 8); }

CPU::CPU(std::function<uint8_t(uint16_t)> read_callback, std::function<void(uint16_t, uint8_t)> write_callback)
    : read{read_callback}, write{write_callback}
{
}

void CPU::register_update_signal_callback(std::function<void(void)> callback)
{
    signal_update = callback != nullptr ? callback : [] {};
}

void CPU::raise_NMI() { nmi_requested = true; }

void CPU::raise_IRQ() { irq_requested = true; }

void CPU::execute_current_instruction(const bool update_debugger)
{
    //std::cout << std::hex << "PC before executing " << (unsigned)current_op_code << "->" << registers.PC <<std::endl;

    // execute
    current_instruction->operation(*this);
    //std::cout << std::hex << "PC after executing " << (unsigned)current_op_code << "->" << registers.PC <<std::endl;

    set_flag(FLAGS::U, true);

    if (update_debugger) {
        signal_update();
    }
}

bool CPU::clock(bool trace)
{
    ++total_cycles;

    // fetch & decode
    if (cycles_left == 0) {
        if (nmi_requested) {
            current_instruction = &InstructionSet::NMI;
            nmi_requested = false;
        } else if (irq_requested && !get_flag(FLAGS::I)) {
            current_instruction = &InstructionSet::IRQ;
            irq_requested = false;
        } else {
            current_op_code = read(registers.PC++);
            current_instruction = &InstructionSet::Table[current_op_code];
            current_instruction->addressing(*this);
        }

        cycles_left = current_instruction->cycles;
    }

    --cycles_left;

    //std::cout << current_instruction->name << "(" << (unsigned)current_instruction->cycles - cycles_left << "/" << (unsigned)current_instruction->cycles << ") -> " << std::hex << (unsigned)current_op_code << " " << std::hex << (unsigned)registers.PC << std::endl;

    if (cycles_left != 0) {
        return false;
    }

    //execute during the last cycle
    execute_current_instruction(trace);

    return true;
}

void CPU::reset()
{
    registers = {0};
    registers.PC = 0x400;
    registers.SP = 0xff;
    registers.P = 0x34;  // U, B & I << WHY B is set on reset actually it does not exist?
    current_op_code = read(registers.PC);

    signal_update();
}

uint8_t CPU::fetch_byte() { return read(registers.PC++); }

uint16_t CPU::fetch_2bytes() { return fetch_byte() | (static_cast<uint16_t>(fetch_byte()) << 8); }

void CPU::push_stack(const uint8_t byte) { write(STACK_BASE_ADDR + (registers.SP--), byte); }

uint8_t CPU::pop_stack() { return read(STACK_BASE_ADDR + (++registers.SP)); }

void CPU::set_flag(const FLAGS flag, const bool value)
{
    if (value) {
        registers.P |= static_cast<uint8_t>(flag);
    } else {
        registers.P &= ~static_cast<uint8_t>(flag);
    }
}

bool CPU::get_flag(const FLAGS flag) const { return registers.P & static_cast<uint8_t>(flag); }

/* OP addressing modes */
// nothing to do here. Operand implied by the operation.
void CPU::addressing_implicit() { ; }

void CPU::addressing_immediate() { fetched_operand = register_PC()++; }

void CPU::addressing_zero_page() { fetched_operand = fetch_byte(); }

void CPU::addressing_zero_page_X() { fetched_operand = (fetch_byte() + registers.X) % 256; }

void CPU::addressing_zero_page_Y() { fetched_operand = (fetch_byte() + registers.Y) % 256; }

void CPU::addressing_relative()
{
    fetched_operand = fetch_byte();
    if (fetched_operand & 0x80) {
        fetched_operand |= 0xff00;
    }
}

void CPU::addressing_absolute() { fetched_operand = fetch_2bytes(); }

void CPU::addressing_absolute_X()
{
    uint16_t base_ptr = fetch_2bytes();
    fetched_operand = base_ptr + registers.X;

    if ((fetched_operand & 0xFF00) != (base_ptr & 0xFF00)) {
        cycles_left += 1;
    }
}

void CPU::addressing_absolute_Y()
{
    uint16_t base_ptr = fetch_2bytes();
    fetched_operand = base_ptr + registers.Y;

    if ((fetched_operand & 0xFF00) != (base_ptr & 0xFF00)) {
        cycles_left += 1;
    }
}

void CPU::addressing_indirect()
{
    // only used for JMP, also buggy
    const uint16_t ptr = fetch_2bytes();
    const uint8_t low = read(ptr);
    const uint8_t hi = ((ptr & 0x00FF) == 0xFF) ? read(ptr & 0xFF00) : read(ptr + 1);
    fetched_operand = (static_cast<uint16_t>(hi) << 8) | low;
}

void CPU::addressing_indexed_indirect()
{
    const uint16_t pointer = fetch_byte() + registers.X;
    fetched_operand = read((pointer + 1) % 256) * 256 + read(pointer % 256);
}

void CPU::addressing_indirect_indexed()
{
    const uint16_t pointer = fetch_byte();
    const uint16_t base_ptr = (read(pointer + 1) % 256) * 256 + read(pointer);
    fetched_operand = base_ptr + registers.Y;

    if ((fetched_operand & 0xFF00) != (base_ptr & 0xFF00)) {
        cycles_left += 1;
    }
}

/* Operations */

void CPU::ADC() { ADC_SBC_internal(read(fetched_operand)); }

void CPU::SBC() { ADC_SBC_internal(read(fetched_operand) ^ 0xFF); }

void CPU::ADC_SBC_internal(const uint8_t value)
{
    const uint16_t temp = static_cast<uint16_t>(registers.A) + value + get_flag(FLAGS::C);
    const uint8_t result = temp & 0xFF;
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::C, temp > 0xFF);
    set_flag(FLAGS::V, (registers.A ^ result) & (value ^ result) & 0x80);
    registers.A = result;
}

/*
void CPU::ADC()
{
    const uint8_t value = read(fetched_operand);
    uint16_t temp = static_cast<uint16_t>(registers.A) + value + get_flag(FLAGS::C);

    if (get_flag(FLAGS::D)) {
        if (((registers.A & 0xF) + (value & 0xF) + (get_flag(FLAGS::C) ? 1 : 0)) > 9) {
            temp += 0x6;
        }

                if (temp > 0x99) {
                        temp += 0x60;
                }

                set_flag(FLAGS::C, temp > 0x99);
    } else {
        set_flag(FLAGS::C, temp > 0xFF);
    }

    const uint8_t result = temp & 0xFF;
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::V, (registers.A ^ result) & (value ^ result) & 0x80);
    set_flag(FLAGS::N, result & 0x80);
    registers.A = result;
}

void CPU::SBC()
{
    const uint8_t value = read(fetched_operand) ^ 0xFF;
    //same as ADC
    uint16_t temp = static_cast<uint16_t>(registers.A) + value + get_flag(FLAGS::C);
    const uint8_t result = temp & 0xFF;
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::C, temp > 0xFF);
    set_flag(FLAGS::V, (registers.A ^ result) & (value ^ result) & 0x80);

        if (get_flag(FLAGS::D))
        {
                if (((registers.A & 0x0F) - (get_flag(FLAGS::C) ? 1 : 0)) < (value & 0x0F)) {
            temp -= 0x6;
                if (temp > 0x99)
                {
                        temp -= 0x60;
                }
        }

    registers.A = temp & 0xFF;
}
*/

void CPU::AND()
{
    registers.A &= read(fetched_operand);
    set_flag(FLAGS::Z, !registers.A);
    set_flag(FLAGS::N, registers.A & 0x80);
}

void CPU::ORA()
{
    registers.A |= read(fetched_operand);
    set_flag(FLAGS::Z, !registers.A);
    set_flag(FLAGS::N, registers.A & 0x80);
}

void CPU::EOR()
{
    registers.A ^= read(fetched_operand);
    set_flag(FLAGS::Z, !registers.A);
    set_flag(FLAGS::N, registers.A & 0x80);
}

void CPU::CMP()
{
    uint8_t result = registers.A - read(fetched_operand);
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::C, registers.A >= read(fetched_operand));
}

void CPU::CPX()
{
    uint8_t result = registers.X - read(fetched_operand);
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::C, registers.X >= read(fetched_operand));
}

void CPU::CPY()
{
    uint8_t data = read(fetched_operand);
    uint8_t result = registers.Y - data;
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::C, registers.Y >= data);
}

void CPU::DEC()
{
    uint8_t result = read(fetched_operand) - 1;
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    write(fetched_operand, result);
}

void CPU::DEX()
{
    --registers.X;
    set_flag(FLAGS::Z, !registers.X);
    set_flag(FLAGS::N, registers.X & 0x80);
}

void CPU::DEY()
{
    --registers.Y;
    set_flag(FLAGS::Z, !registers.Y);
    set_flag(FLAGS::N, registers.Y & 0x80);
}

void CPU::INC()
{
    uint8_t result = read(fetched_operand) + 1;
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    write(fetched_operand, result);
}

void CPU::INX()
{
    ++registers.X;
    set_flag(FLAGS::Z, !registers.X);
    set_flag(FLAGS::N, registers.X & 0x80);
}

void CPU::INY()
{
    ++registers.Y;
    set_flag(FLAGS::Z, !registers.Y);
    set_flag(FLAGS::N, registers.Y & 0x80);
}

void CPU::ASL()
{
    bool addressing_is_implicit = InstructionSet::Table[current_op_code].addr_type == Addressing::implicit;

    uint8_t value = addressing_is_implicit ? registers.A : read(fetched_operand);
    set_flag(FLAGS::C, value & 0x80);
    value <<= 1;
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::Z, value == 0);

    if (addressing_is_implicit) {
        registers.A = value;
    } else {
        write(fetched_operand, value);
    }
}

void CPU::ROL()
{
    bool addressing_is_implicit = InstructionSet::Table[current_op_code].addr_type == Addressing::implicit;

    uint8_t value = addressing_is_implicit ? registers.A : read(fetched_operand);

    auto carry = get_flag(FLAGS::C);
    set_flag(FLAGS::C, value & 0x80);
    value <<= 1;
    value |= carry;
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::Z, value == 0);

    if (addressing_is_implicit) {
        registers.A = value;
    } else {
        write(fetched_operand, value);
    }
}

void CPU::LSR()
{
    bool addressing_is_implicit = InstructionSet::Table[current_op_code].addr_type == Addressing::implicit;

    uint8_t value = addressing_is_implicit ? registers.A : read(fetched_operand);

    set_flag(FLAGS::C, value & 0x1);
    value >>= 1;
    set_flag(FLAGS::N, false);
    set_flag(FLAGS::Z, value == 0);

    if (addressing_is_implicit) {
        registers.A = value;
    } else {
        write(fetched_operand, value);
    }
}

void CPU::ROR()
{
    bool addressing_is_implicit = InstructionSet::Table[current_op_code].addr_type == Addressing::implicit;

    uint8_t value = addressing_is_implicit ? registers.A : read(fetched_operand);

    auto carry = get_flag(FLAGS::C);
    set_flag(FLAGS::C, value & 0x1);
    value >>= 1;
    value |= (carry << 7);
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::Z, value == 0);

    if (addressing_is_implicit) {
        registers.A = value;
    } else {
        write(fetched_operand, value);
    }
}

void CPU::LDA()
{
    registers.A = read(fetched_operand);
    set_flag(FLAGS::N, registers.A & 0x80);
    set_flag(FLAGS::Z, registers.A == 0);
}

void CPU::STA() { write(fetched_operand, registers.A); }

void CPU::LDX()
{
    registers.X = read(fetched_operand);
    set_flag(FLAGS::N, registers.X & 0x80);
    set_flag(FLAGS::Z, registers.X == 0);
}

void CPU::STX() { write(fetched_operand, registers.X); }

void CPU::LDY()
{
    registers.Y = read(fetched_operand);
    set_flag(FLAGS::N, registers.Y & 0x80);
    set_flag(FLAGS::Z, registers.Y == 0);
}

void CPU::STY() { write(fetched_operand, registers.Y); }

void CPU::TAX()
{
    registers.X = registers.A;
    set_flag(FLAGS::N, registers.A & 0x80);
    set_flag(FLAGS::Z, registers.A == 0);
}

void CPU::TXA()
{
    registers.A = registers.X;
    set_flag(FLAGS::N, registers.A & 0x80);
    set_flag(FLAGS::Z, registers.A == 0);
}

void CPU::TAY()
{
    registers.Y = registers.A;
    set_flag(FLAGS::N, registers.A & 0x80);
    set_flag(FLAGS::Z, registers.A == 0);
}

void CPU::TYA()
{
    registers.A = registers.Y;
    set_flag(FLAGS::N, registers.A & 0x80);
    set_flag(FLAGS::Z, registers.A == 0);
}

void CPU::TSX()
{
    registers.X = registers.SP;
    set_flag(FLAGS::N, registers.SP & 0x80);
    set_flag(FLAGS::Z, registers.SP == 0);
}

void CPU::TXS() { registers.SP = registers.X; }

void CPU::PLA()
{
    registers.A = pop_stack();
    set_flag(FLAGS::N, registers.A & 0x80);
    set_flag(FLAGS::Z, registers.A == 0);
}

void CPU::PHA() { push_stack(registers.A); }

void CPU::PLP() { registers.P = (pop_stack() & ~static_cast<uint8_t>(FLAGS::B)); }

void CPU::PHP() { push_stack(registers.P | static_cast<uint8_t>(FLAGS::B)); }

void CPU::BPL()
{
    if (!get_flag(FLAGS::N)) {
        registers.PC += static_cast<int16_t>(fetched_operand);
    }
}

void CPU::BMI()
{
    if (get_flag(FLAGS::N)) {
        registers.PC += static_cast<int16_t>(fetched_operand);
    }
}

void CPU::BVC()
{
    if (!get_flag(FLAGS::V)) {
        registers.PC += static_cast<int16_t>(fetched_operand);
    }
}

void CPU::BVS()
{
    if (get_flag(FLAGS::V)) {
        registers.PC += static_cast<int16_t>(fetched_operand);
    }
}

void CPU::BCC()
{
    if (!get_flag(FLAGS::C)) {
        registers.PC += static_cast<int16_t>(fetched_operand);
    }
}

void CPU::BCS()
{
    if (get_flag(FLAGS::C)) {
        registers.PC += static_cast<int16_t>(fetched_operand);
    }
}

void CPU::BNE()
{
    if (!get_flag(FLAGS::Z)) {
        registers.PC += static_cast<int16_t>(fetched_operand);
    }
}

void CPU::BEQ()
{
    if (get_flag(FLAGS::Z)) {
        registers.PC += static_cast<int16_t>(fetched_operand);
    }
}

void CPU::BRK()
{
    ++registers.PC;
    push_stack(static_cast<uint8_t>(registers.PC >> 8));
    push_stack(static_cast<uint8_t>(registers.PC & 0xFF));

    // Flag B only exists in the STACK, when pushed by BRK or PHP
    push_stack(registers.P | static_cast<uint8_t>(FLAGS::B));

    // BRK does set the interrupt-disable I flag like an IRQ does, and if you have the CMOS 6502 (65C02), it will also
    // clear the decimal D flag.
    set_flag(FLAGS::I, true);
    // set_flag(FLAGS::D, false);

    registers.PC = (static_cast<uint16_t>(read(0xFFFF)) << 8) | read(0xFFFE);
}

void CPU::RESET() { registers.PC = static_cast<uint16_t>(read(0xFFFD)) << 8 | read(0xFFFC); }

void CPU::NMI()
{
    push_stack(static_cast<uint8_t>(registers.PC >> 8));
    push_stack(static_cast<uint8_t>(registers.PC & 0xFF));
    push_stack(registers.P & ~static_cast<uint8_t>(FLAGS::B));
    set_flag(FLAGS::I, true);
    // set_flag(FLAGS::D, false);

    registers.PC = static_cast<uint16_t>(read(0xFFFB)) << 8 | read(0xFFFA);
}

void CPU::IRQ()
{
    push_stack(static_cast<uint8_t>(registers.PC >> 8));
    push_stack(static_cast<uint8_t>(registers.PC & 0xFF));
    push_stack(registers.P & ~static_cast<uint8_t>(FLAGS::B));
    set_flag(FLAGS::I, true);
    // set_flag(FLAGS::D, false);

    registers.PC = (static_cast<uint16_t>(read(0xFFFF)) << 8) | read(0xFFFE);
}

void CPU::RTI()
{
    registers.P = pop_stack();
    registers.PC = pop_stack() | (static_cast<uint16_t>(pop_stack()) << 8);
}

void CPU::JSR()
{
    // push return_address - 1!
    push_stack(high_byte(registers.PC - 1));
    push_stack(low_byte(registers.PC - 1));
    registers.PC = fetched_operand;
}

void CPU::RTS() { registers.PC = (pop_stack() | (static_cast<uint16_t>(pop_stack()) << 8)) + 1; }

void CPU::JMP() { registers.PC = fetched_operand; }

void CPU::BIT()
{
    uint8_t value = read(fetched_operand);
    set_flag(FLAGS::Z, !(registers.A & value));
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::V, value & 0x40);
}

void CPU::CLC() { set_flag(FLAGS::C, false); }

void CPU::SEC() { set_flag(FLAGS::C, true); }

void CPU::CLD() { set_flag(FLAGS::D, false); }

void CPU::SED() { set_flag(FLAGS::D, true); }

void CPU::CLI() { set_flag(FLAGS::I, false); }

void CPU::SEI() { set_flag(FLAGS::I, true); }

void CPU::CLV() { set_flag(FLAGS::V, false); }

void CPU::NOP() { ; }
void CPU::STP() { ; }
void CPU::SLO() { ; }
void CPU::ANC() { ; }
void CPU::RLA() { ; }
void CPU::ARL() { ; }
void CPU::LAX() { ; }
void CPU::AXS() { ; }
void CPU::DCP() { ; }
void CPU::SAX() { ; }
void CPU::RRA() { ; }
void CPU::SRE() { ; }
void CPU::ALR() { ; }
void CPU::ARR() { ; }
void CPU::ISC() { ; }
void CPU::AHX() { ; }
void CPU::SHX() { ; }
void CPU::SHY() { ; }
void CPU::TAS() { ; }
void CPU::XAA() { ; }
void CPU::LAS() { ; }

std::ostream& operator<<(std::ostream& os, const CPU& cpu)
{
    os << std::hex << "A:" << static_cast<unsigned>(cpu.registers.A) << " X:" << static_cast<unsigned>(cpu.registers.X)
       << " Y:" << static_cast<unsigned>(cpu.registers.Y) << " P:" << static_cast<unsigned>(cpu.registers.P)
       << " PC:" << cpu.registers.PC << " SP:" << static_cast<unsigned>(cpu.registers.SP) << std::endl;

    os << "C:" << (cpu.get_flag(CPU::FLAGS::C) ? "x" : "o") << " Z:" << (cpu.get_flag(CPU::FLAGS::Z) ? "x" : "o")
       << " I:" << (cpu.get_flag(CPU::FLAGS::I) ? "x" : "o") << " D:" << (cpu.get_flag(CPU::FLAGS::D) ? "x" : "o")
       << " B:" << (cpu.get_flag(CPU::FLAGS::B) ? "x" : "o") << " U:" << (cpu.get_flag(CPU::FLAGS::U) ? "x" : "o")
       << " V:" << (cpu.get_flag(CPU::FLAGS::V) ? "x" : "o") << " N:" << (cpu.get_flag(CPU::FLAGS::N) ? "x" : "o")
       << std::endl;

    os << "Instruction : " << InstructionSet::Table[cpu.read(cpu.registers.PC)].name
       << " Cycles left: " << static_cast<unsigned>(cpu.cycles_left) << std::endl;

    return os;
}
