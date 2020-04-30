#include "bus.h"

CPU::CPU(Bus &b) : Device(b) {}

void CPU::clock() {
    if (cycles_left > 0) {
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
    return (static_cast<uint16_t>(fetch_byte()) << 8) | fetch_byte();
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
    if (value) {
        P |= static_cast<uint8_t>(flag);
    } else {
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
    target_address = fetch_2bytes();
}

uint16_t CPU::address_immediate()
{
    //Uses the 8-bit operand itself as the value for the operation, rather than fetching a value from a memory address.
    target_address = fetch_byte();
}

uint16_t CPU::address_zero_page()
{
    target_address = fetch_byte();
}

uint16_t CPU::address_zero_page_X()
{
    target_address = (fetch_byte() + X) % 256;
}

uint16_t CPU::address_zero_page_Y()
{
    target_address = (fetch_byte() + Y) % 256;
}

uint16_t CPU::address_relative()
{
    target_address = fetch_byte();
}

uint16_t CPU::address_absolute_X()
{
    target_address = fetch_2bytes() + X;
}

uint16_t CPU::address_absolute_Y()
{
    target_address = fetch_2bytes() + Y;
}

uint16_t CPU::address_indirect()
{
    target_address = read(fetch_2bytes());
}

uint16_t CPU::address_indexed_indirect()
{
    target_address = read((fetch_byte() + X) % 256);
}

uint16_t CPU::address_indirect_indexed()
{
    target_address = (read(fetch_byte()) + Y) % 256;
}

void CPU::ADC()
{
    const uint8_t value = read(target_address);
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
    const uint8_t value = read(target_address) ^ 0xFF;
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
    A &= read(target_address);
    set_flag(FLAGS::Z, !A);
    set_flag(FLAGS::N, A & 0x80);
}

void CPU::ORA()
{
    A |= read(target_address);
    set_flag(FLAGS::Z, !A);
    set_flag(FLAGS::N, A & 0x80);
}

void CPU::BRK()
{
    /*     BRK

        #  address R/W description
       --- ------- --- -----------------------------------------------
        1    PC     R  fetch opcode, increment PC
        2    PC     R  read next instruction byte (and throw it away), //TODO uhm?
                       increment PC
        3  $0100,S  W  push PCH on stack (with B flag set), decrement S
        4  $0100,S  W  push PCL on stack, decrement S
        5  $0100,S  W  push P on stack, decrement S
        6   $FFFE   R  fetch PCL
        7   $FFFF   R  fetch PCH
    */
    push_stack(static_cast<uint8_t>(PC >> 8));
    push_stack(static_cast<uint8_t>(PC & 0x00FF));
    push_stack(P);
    PC = read(0xFFFE) | read(0xFFFF) << 8;
    set_flag(FLAGS::B0, 1);
}

void CPU::RTI()
{
    /* RTI

        #  address R/W description
       --- ------- --- -----------------------------------------------
        1    PC     R  fetch opcode, increment PC
        2    PC     R  read next instruction byte (and throw it away)
        3  $0100,S  R  increment S
        4  $0100,S  R  pull P from stack, increment S
        5  $0100,S  R  pull PCL from stack, increment S
        6  $0100,S  R  pull PCH from stack
    */
    P = pop_stack();
    PC = pop_stack() | pop_stack() << 8;
}

void CPU::RTS()
{
    /* RTS

        #  address R/W description
       --- ------- --- -----------------------------------------------
        1    PC     R  fetch opcode, increment PC
        2    PC     R  read next instruction byte (and throw it away)
        3  $0100,S  R  increment S
        4  $0100,S  R  pull PCL from stack, increment S
        5  $0100,S  R  pull PCH from stack
        6    PC     R  increment PC
    */
    PC = pop_stack() | pop_stack() << 8;
    ++PC;
}

void CPU::JSR()
{
    /* JSR

        #  address R/W description
       --- ------- --- -------------------------------------------------
        1    PC     R  fetch opcode, increment PC
        2    PC     R  fetch low address byte, increment PC
        3  $0100,S  R  internal operation (predecrement S?)
        4  $0100,S  W  push PCH on stack, decrement S
        5  $0100,S  W  push PCL on stack, decrement S
        6    PC     R  copy low address byte to PCL, fetch high address
                       byte to PCH
    */
    target_address = read(PC);
    ++PC;
    // push return_address - 1!
    push_stack(high_byte(PC));
    push_stack(low_byte(PC));
    PC = static_cast<uint16_t>(read(PC)) << 8 | target_address;
}

void CPU::EOR()
{
    A ^= read(target_address);
    set_flag(FLAGS::Z, !A);
    set_flag(FLAGS::N, A & 0x80);
}

void CPU::CMP()
{
    uint8_t result = A - read(target_address);
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::C, A >= read(target_address));
}

void CPU::CPX()
{
    uint8_t result = A - X;
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::C, A >= X);
}

void CPU::CPY()
{
    uint8_t result = A - Y;
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::C, A >= Y);
}

void CPU::DEC()
{
    uint8_t result = read(target_address) - 1;
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    write(target_address, result);
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
    uint8_t result = read(target_address) + 1;
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    write(target_address, result);
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
    auto value = read(target_address);
    set_flag(FLAGS::C, value & 0x80);
    value <<= 1;
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::Z, value == 0);
    write(target_address, value);
}

void CPU::ROL()
{
    auto value = read(target_address);
    auto carry = get_flag(FLAGS::C);
    set_flag(FLAGS::C, value & 0x80);
    value <<= 1;
    value |= carry;
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::Z, value == 0);
    write(target_address, value);
}

void CPU::LSR()
{
    auto value = read(target_address);
    set_flag(FLAGS::C, value & 0x1);
    value >>= 1;
    set_flag(FLAGS::N, false);
    set_flag(FLAGS::Z, value == 0);
    write(target_address, value);
}

void CPU::ROR()
{
    auto value = read(target_address);
    auto carry = get_flag(FLAGS::C);
    set_flag(FLAGS::C, value & 0x1);
    value >>= 1;
    value |= (carry << 7);
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::Z, value == 0);
    write(target_address, value);
}

void CPU::LDA()
{
    A = read(target_address);
    set_flag(FLAGS::N, A & 0x80);
    set_flag(FLAGS::Z, A == 0);
}

void CPU::STA()
{
    write(target_address, A);
}

void CPU::LDX()
{
    X = read(target_address);
    set_flag(FLAGS::N, X & 0x80);
    set_flag(FLAGS::Z, X == 0);
}

void CPU::STX()
{
    write(target_address, X);
}

void CPU::LDY()
{
    Y = read(target_address);
    set_flag(FLAGS::N, Y & 0x80);
    set_flag(FLAGS::Z, Y == 0);
}

void CPU::STY()
{
    write(target_address, Y);
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
    /*     PLA, PLP

        #  address R/W description
       --- ------- --- -----------------------------------------------
        1    PC     R  fetch opcode, increment PC
        2    PC     R  read next instruction byte (and throw it away)
        3  $0100,S  R  increment S
        4  $0100,S  R  pull register from stack
    */
    A = pop_stack();
    set_flag(FLAGS::N, A & 0x80);
    set_flag(FLAGS::Z, A == 0);
}

void CPU::PHA()
{
    /*     PHA, PHP

        #  address R/W description
       --- ------- --- -----------------------------------------------
        1    PC     R  fetch opcode, increment PC
        2    PC     R  read next instruction byte (and throw it away)
        3  $0100,S  W  push register on stack, decrement S
    */
   push_stack(A);
}

void CPU::PLP()
{
    /*     PLA, PLP

        #  address R/W description
       --- ------- --- -----------------------------------------------
        1    PC     R  fetch opcode, increment PC
        2    PC     R  read next instruction byte (and throw it away)
        3  $0100,S  R  increment S
        4  $0100,S  R  pull register from stack
    */
    P = pop_stack();
}

void CPU::PHP()
{
    /*     PHA, PHP

        #  address R/W description
       --- ------- --- -----------------------------------------------
        1    PC     R  fetch opcode, increment PC
        2    PC     R  read next instruction byte (and throw it away)
        3  $0100,S  W  push register on stack, decrement S
    */
   push_stack(P | static_cast<uint8_t>(FLAGS::B0));
}


uint8_t low_byte(const uint16_t twobytes)
{
    return static_cast<uint8_t>(twobytes);
}

uint8_t high_byte(const uint16_t twobytes)
{
    return static_cast<uint8_t>(twobytes >> 8);
}
