#include <iostream>

#include "../include/cpu.h"
#include "../include/instruction.h"

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

// True when the current instruction is one of the eight conditional branches
// and its condition holds, i.e. the branch will be taken.
//
// The branches share a single encoding: bits 7-6 of the opcode select which
// flag is tested and bit 5 selects the value it is compared against.
//
//     opcode      76  5    taken when
//     $10 BPL     00  0    N == 0        $90 BCC     10  0    C == 0
//     $30 BMI     00  1    N == 1        $B0 BCS     10  1    C == 1
//     $50 BVC     01  0    V == 0        $D0 BNE     11  0    Z == 0
//     $70 BVS     01  1    V == 1        $F0 BEQ     11  1    Z == 1
//
// Evaluating the condition here, at decode time, is equivalent to evaluating it
// when the operation runs: no branch reads or writes anything but PC, so the
// tested flag cannot change in between. That equivalence is what lets the whole
// cycle count be known up front - see clock() for why that matters.
//
// Only ever called for opcodes whose addressing mode is `relative`, which is
// exactly this set of eight. cpu_cycle_tests.cpp pins this against the
// BPL()/BMI()/... implementations so the two cannot drift apart.
bool CPU::branch_is_taken() const
{
    static constexpr FLAGS flag_under_test[4] = {FLAGS::N, FLAGS::V, FLAGS::C, FLAGS::Z};

    const FLAGS flag = flag_under_test[(current_op_code >> 6) & 0x03];
    const bool branch_on = (current_op_code & 0x20) != 0;

    return get_flag(flag) == branch_on;
}

// The variable part of the current instruction's cost, in cycles, computed at
// decode time. Zero unless the instruction is flagged as paying the "oops"
// cycle (see Instruction::extra_cycle_on_page_cross).
uint8_t CPU::extra_cycles_for_current_instruction() const
{
    if (!current_instruction->extra_cycle_on_page_cross) {
        return 0;
    }

    // A taken branch costs one extra cycle, and one more if the branch target
    // lands on a different page than the instruction that follows the branch.
    // A branch that is not taken costs nothing extra.
    if (current_instruction->addr_type == Addressing::relative) {
        if (!branch_is_taken()) {
            return 0;
        }

        const uint16_t target = registers.PC + static_cast<int16_t>(fetched_operand);
        return ((target & 0xFF00) != (registers.PC & 0xFF00)) ? 2 : 1;
    }

    return page_crossed ? 1 : 0;
}

bool CPU::clock(bool trace)
{
    ++total_cycles;

    // fetch & decode
    if (cycles_left == 0) {
        // Cleared before addressing runs; the indexed modes set it.
        page_crossed = false;

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

        // The full cost of the instruction has to be known now, before any
        // cycle of it is counted, because the operation runs on the *last*
        // cycle. An operation that tried to extend its own budget by bumping
        // cycles_left would push it from 0 back to 1 after having already
        // executed; the next clock() would then see a non-zero cycles_left,
        // skip fetch/decode, and silently run the same instruction a second
        // time. Computing it up front makes that unrepresentable.
        cycles_left = current_instruction->cycles + extra_cycles_for_current_instruction();
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

    // The reset sequence performs three dummy stack accesses that decrement SP
    // without writing anything, so SP settles at $FD rather than $FF.
    registers.SP = 0xFD;

    // U (always set) and I (interrupts masked on reset). B is deliberately not
    // set: it is not a real bit of P, only a value that appears in the copy
    // pushed to the stack by BRK/PHP.
    registers.P = static_cast<uint8_t>(FLAGS::U) | static_cast<uint8_t>(FLAGS::I);
    registers.PC = static_cast<uint16_t>(read(0xFFFC)) | (static_cast<uint16_t>(read(0xFFFD)) << 8);
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
    const uint16_t base_ptr = fetch_2bytes();
    fetched_operand = base_ptr + registers.X;

    // Recorded, not charged: whether crossing a page actually costs a cycle
    // depends on the instruction, not the addressing mode. clock() decides.
    page_crossed = (fetched_operand & 0xFF00) != (base_ptr & 0xFF00);
}

void CPU::addressing_absolute_Y()
{
    const uint16_t base_ptr = fetch_2bytes();
    fetched_operand = base_ptr + registers.Y;

    page_crossed = (fetched_operand & 0xFF00) != (base_ptr & 0xFF00);
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

    // The pointer is a zero-page address and its high byte is fetched from the
    // *next* zero-page location, wrapping within the page: for a pointer of
    // $FF the high byte comes from $00, not $0100. The wrap therefore belongs
    // on the address being read, not on the byte that comes back.
    const uint16_t base_ptr = read((pointer + 1) % 256) * 256 + read(pointer);
    fetched_operand = base_ptr + registers.Y;

    page_crossed = (fetched_operand & 0xFF00) != (base_ptr & 0xFF00);
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

void CPU::PLP() { registers.P = (pop_stack() & ~static_cast<uint8_t>(FLAGS::B)) | static_cast<uint8_t>(FLAGS::U); }

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
    // B does not exist as a real bit in P; it only ever appears in the copy
    // pushed onto the stack. Pulling it back has to discard it and leave U set,
    // exactly as PLP does.
    registers.P = (pop_stack() & ~static_cast<uint8_t>(FLAGS::B)) | static_cast<uint8_t>(FLAGS::U);
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

/* Undocumented ("illegal") opcodes.
 *
 * The ones implemented below are the combined read-modify-write / load forms
 * that fall out of the 6502's decoding naturally: the ALU and the memory
 * pipeline both run, so the instruction behaves as two documented instructions
 * applied to the same effective address. They are stable across every NMOS
 * 6502, and nestest exercises all of them.
 *
 * Each is written as the composition it actually is, reusing the documented
 * implementations, so the flag handling cannot drift from the real opcodes.
 * The addressing mode has already run, so fetched_operand is shared.
 */

// ASL then ORA on the result.
void CPU::SLO()
{
    ASL();
    ORA();
}

// ROL then AND on the result.
void CPU::RLA()
{
    ROL();
    AND();
}

// LSR then EOR on the result.
void CPU::SRE()
{
    LSR();
    EOR();
}

// ROR then ADC on the result.
void CPU::RRA()
{
    ROR();
    ADC();
}

// Stores A & X. Affects no flags - it is a store, and neither operand is
// modified.
void CPU::SAX() { write(fetched_operand, registers.A & registers.X); }

// Loads both A and X with the same value.
void CPU::LAX()
{
    LDA();
    registers.X = registers.A;
}

// DEC then CMP against the decremented value.
void CPU::DCP()
{
    DEC();
    CMP();
}

// INC then SBC by the incremented value. Also known as ISB in some references
// (including nestest.log); same opcode.
void CPU::ISC()
{
    INC();
    SBC();
}

// AND then copy bit 7 into carry, i.e. the carry ends up matching N.
void CPU::ANC()
{
    AND();
    set_flag(FLAGS::C, get_flag(FLAGS::N));
}

// AND then LSR on the accumulator. The carry takes the bit shifted out, which
// is bit 0 of the AND result - not zero.
void CPU::ALR()
{
    AND();
    set_flag(FLAGS::C, registers.A & 0x01);
    registers.A >>= 1;
    set_flag(FLAGS::N, false);
    set_flag(FLAGS::Z, registers.A == 0);
}

/* The remainder are deliberately left unimplemented.
 *
 * These are unstable on real hardware: their results depend on analogue effects
 * (a "magic constant" that varies between chips, temperature and the data left
 * floating on the bus), so there is no single correct behaviour to encode and
 * no oracle to test against. nestest does not execute any of them. Guessing
 * would produce code that looks authoritative while being wrong on some
 * fraction of real machines, so they stay as documented no-ops.
 */

// Halts the CPU until reset ("jam"/"KIL"). Modelled as a no-op rather than
// hanging the emulator.
void CPU::STP() { ; }

// (A | magic) & X & immediate - magic constant is chip-dependent.
void CPU::XAA() { ; }

// Stores A & X & (high byte of address + 1); the +1 is skipped when the
// address computation crossed a page, and the store target itself can change.
void CPU::AHX() { ; }

// As AHX, with X and Y respectively.
void CPU::SHX() { ; }
void CPU::SHY() { ; }

// Sets SP to A & X, then stores like AHX.
void CPU::TAS() { ; }

// Loads A, X and SP with (memory & SP); unstable on some units.
void CPU::LAS() { ; }

// ARR and AXS are stable, but nestest does not reach them and they are not
// required here; left unimplemented rather than added without an oracle.
void CPU::ARR() { ; }
void CPU::AXS() { ; }

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
