/*
http://www.nesdev.com/NESDoc.pdf
http://nesdev.com/6502.txt
http://nesdev.com/6502_cpu.txt
http://nesdev.com/6502bugs.txt
http://www.oxyron.de/html/opcodes02.html
https://www.masswerk.at/6502/6502_instruction_set.html
http://www.obelisk.me.uk/6502/reference.html
http://6502.org/tutorials/interrupts.html
*/
#pragma once

#include <functional>
#include <ostream>
#include <valarray>

#include "addressing_types.h"
#include "device.h"

struct Instruction;

class CPU
{
public:
    CPU() = delete;
    CPU(std::function<uint8_t(uint16_t)> read_callback, std::function<void(uint16_t, uint8_t)> write_callback);

    void register_update_signal_callback(std::function<void(void)> callback);

    std::function<uint8_t(uint16_t)> read{nullptr};
    std::function<void(uint16_t, uint8_t)> write{nullptr};
    std::function<void(void)> signal_update{[] {}};

    enum class FLAGS {
        C = (1 << 0),  // carry bit
        Z = (1 << 1),  // zero
        I = (1 << 2),  // disable interrupts
        D = (1 << 3),  // decimal mode
        B = (1 << 4),  // "B-flag "
        U = (1 << 5),  // "Unused"
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

    // int8_t interrupt_delay = 0;

    void raise_NMI();
    void raise_IRQ();

    void set_flag(const FLAGS flag, const bool value);
    bool get_flag(const FLAGS flag) const;
    const Instruction* current_instruction;

    // The effective address the current instruction operates on, once the
    // schedule has resolved it. For an immediate operand it is the address of
    // the operand byte itself.
    uint16_t fetched_operand = 0;
    uint8_t current_op_code = 0;

    // Non-zero while an instruction is part-way through its cycle schedule,
    // zero at an instruction boundary. Assigning zero from outside forces the
    // next clock() to begin a fresh fetch, which is how the vector runners
    // reset between cases.
    //
    // It is a boundary marker, not a countdown. An instruction's length is
    // emergent from the accesses its schedule makes (see clock()), and is not
    // always known when the instruction starts - a branch does not learn
    // whether it costs 2, 3 or 4 cycles until its third - so there is nothing
    // to count down from.
    uint8_t cycles_left = 0;

    // Set while resolving an indexed address when adding the index to the low
    // byte of the base carried, i.e. the effective address lies on a different
    // page than the un-carried address the CPU speculatively accessed first.
    // Only meaningful for the instruction currently executing.
    bool page_crossed = false;

    uint16_t previous_pc = 0;
    uint64_t total_cycles = 0;

    friend std::ostream& operator<<(std::ostream& os, const CPU& cpu);

public:
    // Advances the CPU by exactly one cycle, performing exactly one bus access.
    // Returns true on the cycle that completes the current instruction.
    bool clock(bool trace);
    bool branch_is_taken() const;
    void reset();
    uint8_t& register_A() { return registers.A; };
    uint8_t& register_X() { return registers.X; };
    uint8_t& register_Y() { return registers.Y; };
    uint8_t& register_P() { return registers.P; };
    uint16_t& register_PC() { return registers.PC; };
    uint8_t& register_SP() { return registers.SP; };

protected:
    // clang-format off
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

    void NMI(); void IRQ();

    //purely unoficial
    void STP(); void SLO(); void ANC(); void RLA();
    void LAX(); void AXS(); void DCP();
    void SAX(); void RRA(); void SRE(); void ALR();
    void ARR(); void ISC(); void AHX(); void SHX();
    void SHY(); void TAS(); void XAA(); void LAS(); void LXA();
private:
    void ADC_SBC_internal(const uint8_t value);
    void unstable_store(const uint8_t source);

    // clang-format on

    // The operand an operation works on, and where its result goes.
    //
    // Operations do not touch the bus. The schedule has already read the
    // operand into `latched_value` by the time an operation runs, and for the
    // instructions that write, the schedule performs the write afterwards from
    // the same field. Making the result land back in `latched_value` is what
    // keeps the composed illegal opcodes correct: SLO is ASL followed by ORA,
    // and the ORA must see the shifted value, exactly as it would if it re-read
    // the memory the ASL had just written.
    uint8_t operand_value() const { return latched_value; }
    void store_result(const uint8_t value) { latched_value = value; }

    // Pulling P from the stack, shared by PLP and RTI so the two cannot
    // disagree about B and U.
    void set_P_from_pulled(const uint8_t pulled);

protected:
    void push_stack(const uint8_t byte);
    uint8_t pop_stack();

    bool nmi_requested = false;
    bool irq_requested = false;

private:
    // The per-cycle access schedules.
    //
    // A schedule is a property of the (addressing mode, access class) pair
    // rather than of the opcode, which is why 256 opcodes need only these few
    // entries plus the control-flow specials. The three access classes differ
    // in ways that are visible on the bus:
    //
    //   read   - the operand read is the last access; an indexed read only
    //            pays for the address fix-up when the index actually carried,
    //            and the un-carried access IS that speculative read.
    //   write  - the address fix-up read always happens, so the cost is fixed.
    //   rmw    - as write, plus the operand is read, written back UNCHANGED,
    //            and only then written modified. That dummy write is real and
    //            observable: aimed at $2007 it corrupts VRAM.
    enum class Schedule : uint8_t {
        implied,        // 2:    dummy read at PC
        immediate,      // 2:    operand read at PC++
        zero_page_read,      // 3
        zero_page_write,     // 3
        zero_page_rmw,       // 5
        zero_page_indexed_read,   // 4:  dummy read at the un-indexed zero-page address
        zero_page_indexed_write,  // 4
        zero_page_indexed_rmw,    // 6
        absolute_read,       // 4
        absolute_write,      // 4
        absolute_rmw,        // 6
        absolute_indexed_read,   // 4, or 5 when the index carried
        absolute_indexed_write,  // 5
        absolute_indexed_rmw,    // 7
        indexed_indirect_read,   // 6   (zp,X)
        indexed_indirect_write,  // 6
        indexed_indirect_rmw,    // 8
        indirect_indexed_read,   // 5, or 6 when the index carried   (zp),Y
        indirect_indexed_write,  // 6
        indirect_indexed_rmw,    // 8
        branch,         // 2 not taken, 3 taken, 4 taken onto another page
        jump_absolute,  // 3
        jump_indirect,  // 5
        jump_subroutine,  // 6
        return_subroutine,   // 6
        return_interrupt,    // 6
        software_interrupt,  // 7   BRK
        push,           // 3
        pull,           // 4
        interrupt,      // 7   NMI/IRQ

        // NOT a halt. On real NMOS hardware KIL/JAM stops the sequencer until
        // RESET; this runs a fixed-length access pattern and then lets the CPU
        // carry on fetching. The length matches what the test vectors record,
        // which is itself an artefact of their generator giving up rather than a
        // hardware figure. Nothing in this emulator currently needs a true halt,
        // but do not read this as modelling one.
        jam,
    };

    // schedule_for() selects the read/write/read-modify-write member of an
    // addressing mode's family by adding 0, 1 or 2 to the read form, so the
    // three must stay adjacent and in that order. Inserting an enumerator in
    // the middle would otherwise silently mis-schedule a whole family.
    static_assert(static_cast<int>(Schedule::zero_page_write) == static_cast<int>(Schedule::zero_page_read) + 1 &&
                      static_cast<int>(Schedule::zero_page_rmw) == static_cast<int>(Schedule::zero_page_read) + 2 &&
                      static_cast<int>(Schedule::zero_page_indexed_write) ==
                          static_cast<int>(Schedule::zero_page_indexed_read) + 1 &&
                      static_cast<int>(Schedule::zero_page_indexed_rmw) ==
                          static_cast<int>(Schedule::zero_page_indexed_read) + 2 &&
                      static_cast<int>(Schedule::absolute_write) == static_cast<int>(Schedule::absolute_read) + 1 &&
                      static_cast<int>(Schedule::absolute_rmw) == static_cast<int>(Schedule::absolute_read) + 2 &&
                      static_cast<int>(Schedule::absolute_indexed_write) ==
                          static_cast<int>(Schedule::absolute_indexed_read) + 1 &&
                      static_cast<int>(Schedule::absolute_indexed_rmw) ==
                          static_cast<int>(Schedule::absolute_indexed_read) + 2 &&
                      static_cast<int>(Schedule::indexed_indirect_write) ==
                          static_cast<int>(Schedule::indexed_indirect_read) + 1 &&
                      static_cast<int>(Schedule::indexed_indirect_rmw) ==
                          static_cast<int>(Schedule::indexed_indirect_read) + 2 &&
                      static_cast<int>(Schedule::indirect_indexed_write) ==
                          static_cast<int>(Schedule::indirect_indexed_read) + 1 &&
                      static_cast<int>(Schedule::indirect_indexed_rmw) ==
                          static_cast<int>(Schedule::indirect_indexed_read) + 2,
                  "Schedule read/write/rmw forms must stay adjacent in that order");

    static Schedule schedule_for(const uint8_t opcode);

    // Performs this cycle's single bus access and advances the schedule.
    // Returns true when the access just made was the instruction's last.
    bool step();

    // The index register the current addressing mode adds, or 0 for the modes
    // that do not index.
    uint8_t index_register() const;

    // Address arithmetic, split across cycles the way the hardware splits it.
    void resolve_indexed_address();
    void fix_up_indexed_address();
    bool read_indexed_indirect_pointer();

    // Runs the current instruction's effect. The schedule calls this on the one
    // cycle at which the effect is observable, which is not always the last:
    // a taken branch moves PC on its third cycle and may still have a fourth.
    void run_operation();

    // Scratch shared by the schedules. None of it survives an instruction.
    Schedule schedule = Schedule::implied;
    uint8_t cycle = 0;            // 1-based index of the cycle about to run
    uint8_t base_low = 0;         // low byte of the base address, as fetched
    uint8_t base_high = 0;        // high byte of the base address, as fetched
    uint8_t zero_page_pointer = 0;
    uint8_t latched_value = 0;    // operand read by the schedule / result to write
    uint16_t indirect_pointer = 0;
    uint16_t branch_target = 0;
    bool servicing_nmi = false;

public:
    // Observability for tests: whether an NMI has been requested but not yet
    // taken. The PPU is expected to raise this only when PPUCTRL bit 7 asks
    // for it, and that gating is easy to break silently.
    bool nmi_pending() const { return nmi_requested; }

    friend class InstructionSet;
};
