#include "bus.h"

CPU::CPU(Bus &b) : Device(b) {}

void CPU::clock() {
    if (cycles_left > 0) {
        --cycles_left;
        return;
    }

    const uint8_t opcode = read(pc);
    auto instruction = instruction_set[opcode];
    auto op_function = instruction.operation;
    auto op_addresing = instruction.addressing;
    cycles_left = instruction.cycles;
    pc++;
}

uint8_t CPU::fetch_byte()
{
    return read(pc++);
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
    write(STACK_BASE_ADDR + sp, byte);
    --sp;
}

uint8_t CPU::pop_stack()
{
    ++sp;
    return read(STACK_BASE_ADDR + sp);
}

void CPU::set_flag(const FLAGS flag, const bool value)
{
    if (value) {
        p |= static_cast<uint8_t>(flag);
    } else {
        p &= ~static_cast<uint8_t>(flag);
    }
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
    target_address = (fetch_byte() + x) % 256;
}

uint16_t CPU::address_zero_page_Y()
{
    target_address = (fetch_byte() + y) % 256;
}

uint16_t CPU::address_relative()
{
    target_address = fetch_byte();
}

uint16_t CPU::address_absolute_X()
{
    target_address = fetch_2bytes() + x;
}

uint16_t CPU::address_absolute_Y()
{
    target_address = fetch_2bytes() + y;
}

uint16_t CPU::address_indirect()
{
    target_address = read(fetch_2bytes());
}

uint16_t CPU::address_indexed_indirect()
{
    target_address = read((fetch_byte() + x) % 256);
}

uint16_t CPU::address_indirect_indexed()
{
    target_address = (read(fetch_byte()) + y) % 256;
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
    //++pc;
    push_stack(static_cast<uint8_t>(pc >> 8));
    push_stack(static_cast<uint8_t>(pc & 0x00FF));
    push_stack(p);
    pc = read(0xFFFE) | read(0xFFFF) << 8;
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
    //++pc;
    p = pop_stack();
    pc = pop_stack() | pop_stack() << 8;
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
    //++pc;
    pc = pop_stack() | pop_stack() << 8;
    ++pc;
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
   ++pc;
   push_stack(p);
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
   ++pc;
   push_stack(acc);
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
   ++pc;
    p = pop_stack();
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
   ++pc;
    acc = pop_stack();
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
    target_address = read(pc);
    ++pc;
    // push return_address - 1
    push_stack(high_byte(pc));
    push_stack(low_byte(pc));
    pc = static_cast<uint16_t>(read(pc)) << 8 | target_address;
}

uint8_t low_byte(const uint16_t twobytes)
{
    return static_cast<uint8_t>(twobytes);
}

uint8_t high_byte(const uint16_t twobytes)
{
    return static_cast<uint8_t>(twobytes >> 8);
}
