#include "bus.h"

uint8_t low_byte(const uint16_t twobytes)
{
    return static_cast<uint8_t>(twobytes);
}

uint8_t high_byte(const uint16_t twobytes)
{
    return static_cast<uint8_t>(twobytes >> 8);
}

CPU::CPU(Bus *b) : Device{b} {}

void CPU::clock()
{
    if (cycles_left > 0)
    {
        --cycles_left;
        return;
    }

    const uint8_t opcode = read(registers.PC);
    auto instruction = instruction_set[opcode];
    auto op_function = instruction.operation;
    auto op_addresing = instruction.addressing;
    cycles_left = instruction.cycles;
    registers.PC++;
}

uint8_t CPU::fetch_byte()
{
    return read(registers.PC++);
}

uint16_t CPU::fetch_2bytes()
{
    return fetch_byte() | (static_cast<uint16_t>(fetch_byte()) << 8);
}

void CPU::write(const uint16_t addr, const uint8_t data)
{
    bus->write(addr, data);
}

uint8_t CPU::read(const uint16_t addr)
{
    return bus->read(addr);
}

void CPU::push_stack(const uint8_t byte)
{
    write(STACK_BASE_ADDR + registers.SP, byte);
    --registers.SP;
}

uint8_t CPU::pop_stack()
{
    ++registers.SP;
    return read(STACK_BASE_ADDR + registers.SP);
}

void CPU::set_flag(const FLAGS flag, const bool value)
{
    if (value)
    {
        registers.P |= static_cast<uint8_t>(flag);
    }
    else
    {
        registers.P &= ~static_cast<uint8_t>(flag);
    }
}

bool CPU::get_flag(const FLAGS flag)
{
    return registers.P & static_cast<uint8_t>(flag);
}

/* OP addressing modes */
uint16_t CPU::addressing_implicit()
{
    //nothing to do here. Operand implied by the operation.
    return 0;
}

uint16_t CPU::addressing_immediate()
{
    //Uses the 8-bit operand itself as the value for the operation, rather than fetching a value from a memory address.
    fetched_operand = registers.PC++;
    return 0;
}

uint16_t CPU::addressing_zero_page()
{
    fetched_operand = fetch_byte();
    return 0;
}

uint16_t CPU::addressing_zero_page_X()
{
    fetched_operand = (fetch_byte() + registers.X) % 256;
    return 0;
}

uint16_t CPU::addressing_zero_page_Y()
{
    fetched_operand = (fetch_byte() + registers.Y) % 256;
    return 0;
}

uint16_t CPU::addressing_relative()
{
    fetched_operand = fetch_byte();
    return 0;
}

uint16_t CPU::addressing_absolute()
{
    //Fetches the value from a 16-bit address anywhere in memory.
    fetched_operand = fetch_2bytes();
    return 0;
}

uint16_t CPU::addressing_absolute_X()
{
    fetched_operand = fetch_2bytes() + registers.X;
    return 0;
}

uint16_t CPU::addressing_absolute_Y()
{
    fetched_operand = fetch_2bytes() + registers.Y;
    return 0;
}

uint16_t CPU::addressing_indirect()
{
    fetched_operand = read(fetch_2bytes());
    return 0;
}

uint16_t CPU::addressing_indexed_indirect()
{
    const uint8_t pointer = (fetch_byte() + registers.X) % 256;
    fetched_operand = read((pointer + 1) % 256) << 8 | read(pointer);
    return 0;
}

uint16_t CPU::addressing_indirect_indexed()
{
    const uint8_t pointer = fetch_byte();
    fetched_operand = (read(pointer + 1) % 256) << 8 | read(pointer) + registers.Y;
    return 0;
}

/* Operations */

void CPU::ADC()
{
    const uint8_t value = read(fetched_operand);
    const uint16_t temp = static_cast<uint16_t>(registers.A) + value + get_flag(FLAGS::C);
    const uint8_t result = temp & 0xFF;
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::C, temp > 0xFF);
    set_flag(FLAGS::V, (registers.A ^ result) & (value ^ result) & 0x80);
    registers.A = result;
}

void CPU::SBC()
{
    const uint8_t value = read(fetched_operand) ^ 0xFF;
    //same as ADC
    const uint16_t temp = static_cast<uint16_t>(registers.A) + value + get_flag(FLAGS::C);
    const uint8_t result = temp & 0xFF;
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::C, temp > 0xFF);
    set_flag(FLAGS::V, (registers.A ^ result) & (value ^ result) & 0x80);
    registers.A = result;
}

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
    uint8_t result = registers.Y - read(fetched_operand);
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::C, registers.Y >= read(fetched_operand));
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
    auto value = read(fetched_operand);
    set_flag(FLAGS::C, value & 0x80);
    value <<= 1;
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::Z, value == 0);
    write(fetched_operand, value);
}

void CPU::ROL()
{
    auto value = read(fetched_operand);
    auto carry = get_flag(FLAGS::C);
    set_flag(FLAGS::C, value & 0x80);
    value <<= 1;
    value |= carry;
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::Z, value == 0);
    write(fetched_operand, value);
}

void CPU::LSR()
{
    auto value = read(fetched_operand);
    set_flag(FLAGS::C, value & 0x1);
    value >>= 1;
    set_flag(FLAGS::N, false);
    set_flag(FLAGS::Z, value == 0);
    write(fetched_operand, value);
}

void CPU::ROR()
{
    auto value = read(fetched_operand);
    auto carry = get_flag(FLAGS::C);
    set_flag(FLAGS::C, value & 0x1);
    value >>= 1;
    value |= (carry << 7);
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::Z, value == 0);
    write(fetched_operand, value);
}

void CPU::LDA()
{
    registers.A = read(fetched_operand);
    set_flag(FLAGS::N, registers.A & 0x80);
    set_flag(FLAGS::Z, registers.A == 0);
}

void CPU::STA()
{
    write(fetched_operand, registers.A);
}

void CPU::LDX()
{
    registers.X = read(fetched_operand);
    set_flag(FLAGS::N, registers.X & 0x80);
    set_flag(FLAGS::Z, registers.X == 0);
}

void CPU::STX()
{
    write(fetched_operand, registers.X);
}

void CPU::LDY()
{
    registers.Y = read(fetched_operand);
    set_flag(FLAGS::N, registers.Y & 0x80);
    set_flag(FLAGS::Z, registers.Y == 0);
}

void CPU::STY()
{
    write(fetched_operand, registers.Y);
}

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

void CPU::TXS()
{
    registers.SP = registers.X;
}

void CPU::PLA()
{
    registers.A = pop_stack();
    set_flag(FLAGS::N, registers.A & 0x80);
    set_flag(FLAGS::Z, registers.A == 0);
}

void CPU::PHA()
{
    push_stack(registers.A);
}

void CPU::PLP()
{
    registers.P = pop_stack();
}

void CPU::PHP()
{
    push_stack(registers.P | static_cast<uint8_t>(FLAGS::B0));
}

void CPU::BPL()
{
    if (!get_flag(FLAGS::V))
    {
        registers.PC += fetched_operand;
    }
}

void CPU::BMI()
{
    if (get_flag(FLAGS::N))
    {
        registers.PC += fetched_operand;
    }
}

void CPU::BVC()
{
    if (!get_flag(FLAGS::V))
    {
        registers.PC += fetched_operand;
    }
}

void CPU::BVS()
{
    if (get_flag(FLAGS::V))
    {
        registers.PC += fetched_operand;
    }
}

void CPU::BCC()
{
    if (!get_flag(FLAGS::C))
    {
        registers.PC += fetched_operand;
    }
}

void CPU::BCS()
{
    if (get_flag(FLAGS::C))
    {
        registers.PC += fetched_operand;
    }
}

void CPU::BNE()
{
    if (!get_flag(FLAGS::Z))
    {
        registers.PC += fetched_operand;
    }
}

void CPU::BEQ()
{
    if (get_flag(FLAGS::Z))
    {
        registers.PC += fetched_operand;
    }
}

void CPU::BRK()
{
    push_stack(static_cast<uint8_t>(registers.PC >> 8));
    push_stack(static_cast<uint8_t>(registers.PC & 0xFF));
    push_stack(registers.P);
    registers.PC = read(0xFFFF) << 8 | read(0xFFFE);
    set_flag(FLAGS::B0, 1);
}

void CPU::RTI()
{
    registers.P = pop_stack();
    registers.PC = pop_stack() | static_cast<uint16_t>(pop_stack()) << 8;
}

void CPU::JSR()
{
    fetched_operand = read(registers.PC);
    ++registers.PC;
    // push return_address - 1!
    push_stack(high_byte(registers.PC));
    push_stack(low_byte(registers.PC));
    registers.PC = static_cast<uint16_t>(read(registers.PC)) << 8 | fetched_operand;
}

void CPU::RTS()
{
    registers.PC = pop_stack() | pop_stack() << 8;
    ++registers.PC;
}

void CPU::JMP()
{
    registers.PC = fetch_2bytes();
}

void CPU::BIT()
{
    uint8_t value = fetch_byte();
    set_flag(FLAGS::Z, registers.A & value);
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::V, value & 0x40);
}

void CPU::CLC()
{
    set_flag(FLAGS::C, false);
}

void CPU::SEC()
{
    set_flag(FLAGS::C, true);
}

void CPU::CLD()
{
    set_flag(FLAGS::D, false);
}

void CPU::SED()
{
    set_flag(FLAGS::D, true);
}

void CPU::CLI()
{
    set_flag(FLAGS::I, false);
}

void CPU::SEI()
{
    set_flag(FLAGS::I, true);
}

void CPU::CLV()
{
    set_flag(FLAGS::V, false);
}

void CPU::NOP() { ; }

void CPU::STP() {;}
void CPU::SLO() {;}
void CPU::ANC() {;}
void CPU::RLA() {;}
void CPU::ARL() {;}
void CPU::LAX() {;}
void CPU::AXS() {;}
void CPU::DCP() {;}
void CPU::SAX() {;}
void CPU::RRA() {;}
void CPU::SRE() {;}
void CPU::ALR() {;}
void CPU::ARR() {;}
void CPU::ISC() {;}
void CPU::AHX() {;}
void CPU::SHX() {;}
void CPU::SHY() {;}
void CPU::TAS() {;}
void CPU::XAA() {;}
void CPU::LAS() {;}
