#include "bus.h"

CPU::CPU(Bus &b) : Device(b) {}

void CPU::clock()
{
    if (cycles_left > 0)
    {
        --cycles_left;
        return;
    }

    const uint8_t opcode = read(PC);
    auto instruction = instruction_set[opcode];
    auto op_function = instruction.operation;
    auto op_addresing = instruction.addressing;
    cycles_left = instruction.cycles;
    PC++;
}

uint8_t CPU::fetch_byte()
{
    return read(PC++);
}

uint16_t CPU::fetch_2bytes()
{
    return fetch_byte() | (static_cast<uint16_t>(fetch_byte()) << 8);
}

void CPU::write(const uint16_t addr, const uint8_t data)
{
    bus.write(addr, data);
}

uint8_t CPU::read(const uint16_t addr)
{
    return bus.read(addr);
}

void CPU::push_stack(const uint8_t byte)
{
    write(STACK_BASE_ADDR + SP, byte);
    --SP;
}

uint8_t CPU::pop_stack()
{
    ++SP;
    return read(STACK_BASE_ADDR + SP);
}

void CPU::set_flag(const FLAGS flag, const bool value)
{
    if (value)
    {
        P |= static_cast<uint8_t>(flag);
    }
    else
    {
        P &= ~static_cast<uint8_t>(flag);
    }
}

bool CPU::get_flag(const FLAGS flag)
{
    return P & static_cast<uint8_t>(flag);
}

uint16_t CPU::address_absolute()
{
    //Fetches the value from a 16-bit address anywhere in memory.
    fetched_operand = fetch_2bytes();
}

uint16_t CPU::address_immediate()
{
    //Uses the 8-bit operand itself as the value for the operation, rather than fetching a value from a memory address.
    fetched_operand = fetch_byte();
}

uint16_t CPU::address_zero_page()
{
    fetched_operand = fetch_byte();
}

uint16_t CPU::address_zero_page_X()
{
    fetched_operand = (fetch_byte() + X) % 256;
}

uint16_t CPU::address_zero_page_Y()
{
    fetched_operand = (fetch_byte() + Y) % 256;
}

uint16_t CPU::address_relative()
{
    fetched_operand = fetch_byte();
}

uint16_t CPU::address_absolute_X()
{
    fetched_operand = fetch_2bytes() + X;
}

uint16_t CPU::address_absolute_Y()
{
    fetched_operand = fetch_2bytes() + Y;
}

uint16_t CPU::address_indirect()
{
    fetched_operand = read(fetch_2bytes());
}

uint16_t CPU::address_indexed_indirect()
{
    fetched_operand = read((fetch_byte() + X) % 256);
}

uint16_t CPU::address_indirect_indexed()
{
    fetched_operand = (read(fetch_byte()) + Y) % 256;
}

void CPU::ADC()
{
    const uint8_t value = read(fetched_operand);
    const uint16_t temp = static_cast<uint16_t>(A) + value + get_flag(FLAGS::C);
    const uint8_t result = temp & 0xFF;
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::C, temp > 0xFF);
    set_flag(FLAGS::V, (A ^ result) & (value ^ result) & 0x80);
    A = result;
}

void CPU::SBC()
{
    const uint8_t value = read(fetched_operand) ^ 0xFF;
    //same as ADC
    const uint16_t temp = static_cast<uint16_t>(A) + value + get_flag(FLAGS::C);
    const uint8_t result = temp & 0xFF;
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::C, temp > 0xFF);
    set_flag(FLAGS::V, (A ^ result) & (value ^ result) & 0x80);
    A = result;
}

void CPU::AND()
{
    A &= read(fetched_operand);
    set_flag(FLAGS::Z, !A);
    set_flag(FLAGS::N, A & 0x80);
}

void CPU::ORA()
{
    A |= read(fetched_operand);
    set_flag(FLAGS::Z, !A);
    set_flag(FLAGS::N, A & 0x80);
}

void CPU::EOR()
{
    A ^= read(fetched_operand);
    set_flag(FLAGS::Z, !A);
    set_flag(FLAGS::N, A & 0x80);
}

void CPU::CMP()
{
    uint8_t result = A - read(fetched_operand);
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::C, A >= read(fetched_operand));
}

void CPU::CPX()
{
    uint8_t result = X - read(fetched_operand);
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::C, X >= read(fetched_operand));
}

void CPU::CPY()
{
    uint8_t result = Y - read(fetched_operand);
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::C, Y >= read(fetched_operand));
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
    --X;
    set_flag(FLAGS::Z, !X);
    set_flag(FLAGS::N, X & 0x80);
}

void CPU::DEY()
{
    --Y;
    set_flag(FLAGS::Z, !Y);
    set_flag(FLAGS::N, Y & 0x80);
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
    ++X;
    set_flag(FLAGS::Z, !X);
    set_flag(FLAGS::N, X & 0x80);
}

void CPU::INY()
{
    ++Y;
    set_flag(FLAGS::Z, !Y);
    set_flag(FLAGS::N, Y & 0x80);
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
    A = read(fetched_operand);
    set_flag(FLAGS::N, A & 0x80);
    set_flag(FLAGS::Z, A == 0);
}

void CPU::STA()
{
    write(fetched_operand, A);
}

void CPU::LDX()
{
    X = read(fetched_operand);
    set_flag(FLAGS::N, X & 0x80);
    set_flag(FLAGS::Z, X == 0);
}

void CPU::STX()
{
    write(fetched_operand, X);
}

void CPU::LDY()
{
    Y = read(fetched_operand);
    set_flag(FLAGS::N, Y & 0x80);
    set_flag(FLAGS::Z, Y == 0);
}

void CPU::STY()
{
    write(fetched_operand, Y);
}

void CPU::TAX()
{
    X = A;
    set_flag(FLAGS::N, A & 0x80);
    set_flag(FLAGS::Z, A == 0);
}

void CPU::TXA()
{
    A = X;
    set_flag(FLAGS::N, A & 0x80);
    set_flag(FLAGS::Z, A == 0);
}

void CPU::TAY()
{
    Y = A;
    set_flag(FLAGS::N, A & 0x80);
    set_flag(FLAGS::Z, A == 0);
}

void CPU::TYA()
{
    A = Y;
    set_flag(FLAGS::N, A & 0x80);
    set_flag(FLAGS::Z, A == 0);
}

void CPU::TSX()
{
    X = SP;
    set_flag(FLAGS::N, SP & 0x80);
    set_flag(FLAGS::Z, SP == 0);
}

void CPU::TXS()
{
    SP = X;
}

void CPU::PLA()
{
    A = pop_stack();
    set_flag(FLAGS::N, A & 0x80);
    set_flag(FLAGS::Z, A == 0);
}

void CPU::PHA()
{
    push_stack(A);
}

void CPU::PLP()
{
    P = pop_stack();
}

void CPU::PHP()
{
    push_stack(P | static_cast<uint8_t>(FLAGS::B0));
}

void CPU::BPL()
{
    if (!get_flag(FLAGS::V))
    {
        PC += fetched_operand;
    }
}

void CPU::BMI()
{
    if (get_flag(FLAGS::N))
    {
        PC += fetched_operand;
    }
}

void CPU::BVC()
{
    if (!get_flag(FLAGS::V))
    {
        PC += fetched_operand;
    }
}

void CPU::BVS()
{
    if (get_flag(FLAGS::V))
    {
        PC += fetched_operand;
    }
}

void CPU::BCC()
{
    if (!get_flag(FLAGS::C))
    {
        PC += fetched_operand;
    }
}

void CPU::BCS()
{
    if (get_flag(FLAGS::C))
    {
        PC += fetched_operand;
    }
}

void CPU::BNE()
{
    if (!get_flag(FLAGS::Z))
    {
        PC += fetched_operand;
    }
}

void CPU::BEQ()
{
    if (get_flag(FLAGS::Z))
    {
        PC += fetched_operand;
    }
}

void CPU::BRK()
{
    push_stack(static_cast<uint8_t>(PC >> 8));
    push_stack(static_cast<uint8_t>(PC & 0xFF));
    push_stack(P);
    PC = read(0xFFFE) | read(0xFFFF) << 8;
    set_flag(FLAGS::B0, 1);
}

void CPU::RTI()
{
    P = pop_stack();
    PC = pop_stack() | static_cast<uint16_t>(pop_stack()) << 8;
}

void CPU::JSR()
{
    fetched_operand = read(PC);
    ++PC;
    // push return_address - 1!
    push_stack(high_byte(PC));
    push_stack(low_byte(PC));
    PC = static_cast<uint16_t>(read(PC)) << 8 | fetched_operand;
}

void CPU::RTS()
{
    PC = pop_stack() | pop_stack() << 8;
    ++PC;
}

void CPU::JMP()
{
    PC = fetch_2bytes();
}

void CPU::BIT()
{
    uint8_t value = fetch_byte();
    set_flag(FLAGS::Z, A & value);
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

void CPU::CLV() { set_flag(FLAGS::V, false); }

void CPU::NOP() { ; }

uint8_t low_byte(const uint16_t twobytes)
{
    return static_cast<uint8_t>(twobytes);
}

uint8_t high_byte(const uint16_t twobytes)
{
    return static_cast<uint8_t>(twobytes >> 8);
}
