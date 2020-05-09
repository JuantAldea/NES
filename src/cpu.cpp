#include "bus.h"
#include <iostream>

uint8_t low_byte(const uint16_t twobytes)
{
    return static_cast<uint8_t>(twobytes);
}

uint8_t high_byte(const uint16_t twobytes)
{
    return static_cast<uint8_t>(twobytes >> 8);
}

CPU::CPU(Bus *b) : Device{b} {}

void CPU::execute_next_instruction(const bool update_debugger)
{
    current_op_code = read(registers.PC);
    auto instruction = instruction_set[current_op_code];
    auto fn_operation = instruction.operation;
    auto fn_addresing = instruction.addressing;
    cycles_left = instruction.cycles;
    //std::cout << "Instruction fetched: " << instruction.name << " " << (unsigned) current_op_code << std::endl;
    auto addr = registers.PC;
    registers.PC++;

    fn_addresing(*this);
    fn_operation(*this);

    set_flag(FLAGS::U, true);

    if (registers.PC == addr) {
        std::cout << "TRAP: " << std::hex << addr << std::endl;
        //exit(1);
    }

    if (update_debugger){
        emit updated();
    }
}

bool CPU::clock()
{
    if (cycles_left > 0)
    {
        --cycles_left;
        return false;
    }

    execute_next_instruction(false);
    return true;
}

void CPU::reset()
{
    registers = { 0 };
    registers.PC = 0x400;
    registers.SP = 0xff;
    registers.P = 0x34; //U, B & I
    current_op_code = read(registers.PC);
    //std::cout << *this;
    emit updated();
}

uint8_t CPU::fetch_byte()
{
    uint8_t byte = read(registers.PC++);
    //std::cout << "fetching byte from *" << std::hex << registers.PC << ": " << std::hex << (unsigned) byte << std::endl;
    return byte;
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

uint8_t CPU::read(const uint16_t addr) const
{
    return bus->read(addr);
}


void CPU::push_stack(const uint8_t byte)
{
    write(STACK_BASE_ADDR + (registers.SP--), byte);
}

uint8_t CPU::pop_stack()
{
    return read(STACK_BASE_ADDR + (++registers.SP));
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

bool CPU::get_flag(const FLAGS flag) const
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
    //std::cout << "Addresing Inmediate";
    //Uses the 8-bit operand itself as the value for the operation, rather than fetching a value from a memory address.
    fetched_operand = register_PC()++;
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
    if (fetched_operand & 0x80) {
        fetched_operand |= 0xff00;
    }
    //std::cout << "RELATIVE ADDR: " << std::hex << static_cast<int16_t>(fetched_operand) << std::endl;
    return 0;
}

uint16_t CPU::addressing_absolute()
{
    //Fetches the value from a 16-bit address anywhere in memory.

    fetched_operand = fetch_2bytes();
    //std::cout << "addressing absoute fetched:" << std::hex << (unsigned) fetched_operand << std::endl;
    return 0;
}

uint16_t CPU::addressing_absolute_X()
{
    fetched_operand = fetch_2bytes() + registers.X;
    //std::cout << "ABX: " << fetched_operand - registers.X << "+"<< (unsigned)registers.X << std::endl;
    return 0;
}

uint16_t CPU::addressing_absolute_Y()
{
    fetched_operand = fetch_2bytes() + registers.Y;
    //std::cout << "ABY: " << fetched_operand - registers.Y << "+"<< (unsigned)registers.Y << std::endl;
    return 0;
}

uint16_t CPU::addressing_indirect()
{
    // only used for JMP, also buggy
    const uint16_t ptr = fetch_2bytes();
    const uint8_t low = read(ptr);
    const uint8_t hi = ((ptr & 0x00FF) == 0xFF) ? read(ptr & 0xFF00) : read(ptr + 1);
    fetched_operand = (static_cast<uint16_t>(hi) << 8) | low;
    //std::cout << "INDIRECT: (" << std::hex << ptr << ") -> " << std::hex << fetched_operand << std::endl;
    return 0;
}

uint16_t CPU::addressing_indexed_indirect()
{
    const uint16_t pointer = fetch_byte() + registers.X;
    fetched_operand = read((pointer + 1) % 256) * 256 + read(pointer % 256);
    return 0;
}

uint16_t CPU::addressing_indirect_indexed()
{
    const uint16_t pointer = fetch_byte();
    fetched_operand = (read(pointer + 1) % 256) * 256 + read(pointer) + registers.Y;
    return 0;
}

/* Operations */

void CPU::ADC()
{
    ADC_SBC_internal(read(fetched_operand));
}

void CPU::SBC()
{
    ADC_SBC_internal(read(fetched_operand) ^ 0xFF);
}

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
    //std::cout << "CMP " << std::hex << (unsigned) registers.A << ", " << (unsigned)read(fetched_operand) << "(" << fetched_operand << ")\n";
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
    //std::cout << "CPY " << std::hex << (unsigned)registers.Y << ", "
    //    << std::hex << (unsigned) data << "(" <<  std::hex << (unsigned)fetched_operand << ")"<< std::endl;
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
    bool addressing_is_implicit =  instruction_set[current_op_code].addr_type == AddressingTypes::addressing_implicit_type;

    uint8_t value = addressing_is_implicit ? registers.A : read(fetched_operand);

    //std::cout << "ASL: (" << std::hex << (unsigned)fetched_operand << ") -> " << (unsigned)value << std::endl;
    set_flag(FLAGS::C, value & 0x80);
    value <<= 1;
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::Z, value == 0);

    if (addressing_is_implicit){
        registers.A = value;
    } else {
        write(fetched_operand, value);
    }
}


void CPU::ROL()
{
    bool addressing_is_implicit =  instruction_set[current_op_code].addr_type == AddressingTypes::addressing_implicit_type;

    uint8_t value = addressing_is_implicit ? registers.A : read(fetched_operand);

    auto carry = get_flag(FLAGS::C);
    set_flag(FLAGS::C, value & 0x80);
    value <<= 1;
    value |= carry;
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::Z, value == 0);

    if (addressing_is_implicit){
        registers.A = value;
    } else {
        write(fetched_operand, value);
    }
}

void CPU::LSR()
{    bool addressing_is_implicit =  instruction_set[current_op_code].addr_type == AddressingTypes::addressing_implicit_type;

    uint8_t value = addressing_is_implicit ? registers.A : read(fetched_operand);

    set_flag(FLAGS::C, value & 0x1);
    value >>= 1;
    set_flag(FLAGS::N, false);
    set_flag(FLAGS::Z, value == 0);

    if (addressing_is_implicit){
        registers.A = value;
    } else {
        write(fetched_operand, value);
    }
}

void CPU::ROR()
{
    bool addressing_is_implicit =  instruction_set[current_op_code].addr_type == AddressingTypes::addressing_implicit_type;

    uint8_t value = addressing_is_implicit ? registers.A : read(fetched_operand);
    auto carry = get_flag(FLAGS::C);
    set_flag(FLAGS::C, value & 0x1);
    value >>= 1;
    value |= (carry << 7);
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::Z, value == 0);

    if (addressing_is_implicit){
        registers.A = value;
    } else {
        write(fetched_operand, value);
    }
}

void CPU::LDA()
{
    registers.A = read(fetched_operand);
    if (fetched_operand == 0xe) {
        //std::cout << "LDA: (" << std::hex << (unsigned)fetched_operand << ") -> " << (unsigned)registers.A << std::endl;
    }
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
    push_stack(registers.P | static_cast<uint8_t>(FLAGS::B));
}

void CPU::BPL()
{
    if (!get_flag(FLAGS::N))
    {
        registers.PC += static_cast<int16_t>(fetched_operand);
    }
}

void CPU::BMI()
{
    if (get_flag(FLAGS::N))
    {
        registers.PC += static_cast<int16_t>(fetched_operand);
    }
}

void CPU::BVC()
{
    if (!get_flag(FLAGS::V))
    {
        registers.PC += static_cast<int16_t>(fetched_operand);
    }
}

void CPU::BVS()
{
    if (get_flag(FLAGS::V))
    {
        registers.PC += static_cast<int16_t>(fetched_operand);
    }
}

void CPU::BCC()
{
    if (!get_flag(FLAGS::C))
    {
        registers.PC += static_cast<int16_t>(fetched_operand);
    }
}

void CPU::BCS()
{
    if (get_flag(FLAGS::C))
    {
        registers.PC += static_cast<int16_t>(fetched_operand);
    }
}

void CPU::BNE()
{
    if (!get_flag(FLAGS::Z))
    {
        //std::cout << "BNE FROM " << std::hex << registers.PC << "to " << std::hex << registers.PC + static_cast<int16_t>(fetched_operand) << "+" << std::hex << static_cast<int16_t>(fetched_operand) << std::endl;
        registers.PC += static_cast<int16_t>(fetched_operand);
    }
}

void CPU::BEQ()
{
    if (get_flag(FLAGS::Z))
    {
        registers.PC += static_cast<int16_t>(fetched_operand);
    }
}

void CPU::BRK()
{
    ++registers.PC;
    push_stack(static_cast<uint8_t>(registers.PC >> 8));
    push_stack(static_cast<uint8_t>(registers.PC & 0xFF));

    push_stack(registers.P | static_cast<uint8_t>(FLAGS::B));
    // BRK does set the interrupt-disable I flag like an IRQ does, and if you have the CMOS 6502 (65C02), it will also clear the decimal D flag.
    //TODO D flag & BRK: contradictorial information
    set_flag(FLAGS::I, true);
    set_flag(FLAGS::D, false);
    registers.PC = (static_cast<uint16_t>(read(0xFFFF)) << 8) | read(0xFFFE);
    /*
    std::cout << "BRK STACKPOINTER "
              << std::hex << (unsigned)registers.SP
              << " (" << std::hex << (unsigned) STACK_BASE_ADDR + registers.SP << ") P: "
              << std::hex << (unsigned)(registers.P | static_cast<uint8_t>(FLAGS::B))
              << std::endl;
              */
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

void CPU::RTS()
{
    registers.PC = pop_stack() | (static_cast<uint16_t>(pop_stack()) << 8);
    ++registers.PC;
}

void CPU::JMP()
{
    registers.PC = fetched_operand;
    //std::cout << "JMP " << std::hex << registers.PC;
}

void CPU::BIT()
{
    uint8_t value = read(fetched_operand);
    //std::cout << "BIT: (" << std::hex << fetched_operand << ") " << std::hex << (unsigned)value << "|" << std::hex << (unsigned) (registers.A & value) << std::endl;
    set_flag(FLAGS::Z, !(registers.A & value));
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
    //std::cout << "CLI\n";
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

std::ostream &operator<<(std::ostream &os, const CPU &cpu)
{
    os << "A:" << static_cast<unsigned>(cpu.registers.A)
        << " X:" << static_cast<unsigned>(cpu.registers.X)
        << " Y:" << static_cast<unsigned>(cpu.registers.Y)
        << " P:" << static_cast<unsigned>(cpu.registers.P)
        << " PC:" << cpu.registers.PC
        << " SP:" << cpu.registers.SP
        << std::endl;

    os << "C:" << (cpu.get_flag(CPU::FLAGS::C) ? "x" : "o")
        << " Z:" << (cpu.get_flag(CPU::FLAGS::Z) ? "x" : "o")
        << " I:" << (cpu.get_flag(CPU::FLAGS::I) ? "x" : "o")
        << " D:" << (cpu.get_flag(CPU::FLAGS::D) ? "x" : "o")
        << " B:" << (cpu.get_flag(CPU::FLAGS::B) ? "x" : "o")
        << " U:" << (cpu.get_flag(CPU::FLAGS::U) ? "x" : "o")
        << " V:" << (cpu.get_flag(CPU::FLAGS::V) ? "x" : "o")
        << " N:" << (cpu.get_flag(CPU::FLAGS::N) ? "x" : "o")
        << std::endl;

    os << "Instruction : " << cpu.instruction_set[cpu.read(cpu.registers.PC)].name
        << " Cycles left: " << static_cast<unsigned>(cpu.cycles_left) << std::endl;
/*
    for (int i = 0; i < 10; i++){
        os << std::hex << static_cast<unsigned>(cpu.read(i)) <<  " ";
    }
    os << std::endl;
    */return os;
}
