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

// The NMI input is edge-sensitive: what the CPU latches is the /NMI line going
// low, not the fact that it is low. raise_NMI is that falling edge.
void CPU::raise_NMI() { nmi_requested = true; }

// And this is the line going back high.
//
// On hardware the edge detector samples /NMI once per cycle, so a pulse that
// begins and ends between two samples is never seen at all. That is the whole
// mechanism behind NMI suppression: the PPU asserts /NMI when the vblank flag
// is set with NMI enabled, and a $2002 read a dot or two later takes it back
// high before the CPU ever looks.
//
// Rather than defer edge detection to the sample point - which would make
// nmi_pending() lag the line by up to a cycle, and is not observable from the
// PPU's side anyway - the edge is latched here and stays revocable until a
// sample has actually seen it. The two are equivalent: an assertion survives
// iff a sample falls inside it.
void CPU::lower_NMI()
{
    if (!nmi_committed) {
        nmi_requested = false;
    }
}

void CPU::raise_IRQ() { irq_requested = true; }

// A device asserting or releasing its /IRQ line. Level-sensitive: the CPU keeps
// taking the interrupt while this is true and I is clear, so the device must
// release it when the handler acknowledges (for the APU frame counter, that is
// a read of $4015).
void CPU::set_IRQ_line(const IRQSource source, const bool asserted)
{
    const uint8_t bit = static_cast<uint8_t>(source);
    irq_sources = asserted ? (irq_sources | bit) : static_cast<uint8_t>(irq_sources & ~bit);
}

// Once per CPU cycle, after that cycle's bus access.
//
// Two distinct things happen here, and conflating them is what made the NMI
// timing wrong:
//
//   1. The pending NMI edge, if any, becomes irrevocable. See lower_NMI.
//
//   2. The interrupt lines are POLLED, and the result is what the next
//      instruction boundary acts on. The poll is skipped on an instruction's
//      final cycle, so the latch that survives into the boundary is the one
//      taken on the PENULTIMATE cycle. That is not an off-by-one to be tidied
//      away: it is why an interrupt asserted during an instruction's last
//      cycle is not taken until after the *following* instruction
//      (04-nmi_control's "Immediate occurence should be after NEXT
//      instruction"), and why CLI/SEI/PLP - which write I on their last cycle
//      - are polled against I's *old* value.
// The edge-detector half of sampling, separated so it can also run on CPU
// cycles the CPU does not execute (those stolen by OAM DMA). Once an edge has
// been seen here it can no longer be revoked by the line going back high.
void CPU::latch_nmi_edge()
{
    if (nmi_requested) {
        nmi_committed = true;
    }
}

void CPU::sample_interrupts()
{
    latch_nmi_edge();

    if (completed_instruction_this_cycle) {
        return;
    }

    // An interrupt sequence does not poll for a *new* interrupt. This is what
    // guarantees that at least one instruction of the handler executes before
    // another interrupt is serviced - without it, a second edge arriving
    // mid-sequence is taken at the sequence's own boundary and the handler
    // never runs an instruction at all. BRK is an interrupt sequence for this
    // purpose too.
    //
    // It does still look at /NMI once, on cycle 4, to choose the vector it is
    // about to fetch. That is a different question from "is there another
    // interrupt to take after this one", and it is answered here rather than at
    // the top of cycle 5 so that it uses the state of the line at the SAMPLE
    // point - the PPU has two more dots to run between this and cycle 5's bus
    // access, and an NMI raised on either of them is one cycle too late.
    if (schedule == Schedule::interrupt || schedule == Schedule::software_interrupt) {
        if (cycle == 4) {
            poll_interrupt_hijack();
        }
        return;
    }

    // The one instruction that does not poll on its penultimate cycle.
    //
    // Hardware polls before a branch's operand fetch and then, on a TAKEN
    // branch, not again before the third cycle. What that leaves is:
    //
    //   not taken       2 cycles   polls after cycle 1        (= penultimate)
    //   taken           3 cycles   polls after cycle 1        (one cycle EARLIER
    //                                                          than penultimate)
    //   taken, crossing 4 cycles   polls after cycles 1 and 3 (= penultimate)
    //
    // so only the 3-cycle form differs, and it differs by keeping the result of
    // cycle 1's poll rather than taking a fresh one. An IRQ asserted during a
    // taken non-page-crossing branch's second cycle therefore waits out the
    // whole instruction AFTER the branch as well - which is what
    // 5-branch_delays_irq measures, and it is a real quirk rather than an
    // accounting convenience: no other instruction does this.
    //
    // Reaching here on cycle 2 of a branch already implies the branch was
    // taken; a branch that is not taken finishes on cycle 2 and the
    // completed_instruction_this_cycle check above has returned.
    if (schedule == Schedule::branch && cycle == 2) {
        return;
    }

    nmi_poll = nmi_requested;

    // /IRQ is level-sensitive, unlike /NMI: there is no edge to latch. As long
    // as a device holds the line low and I is clear, the interrupt is taken
    // again after every handler. `irq_requested` is the older one-shot pulse,
    // kept for harnesses that inject an interrupt directly; a device that owns
    // a line uses set_IRQ_line with its own IRQSource bit, and is responsible
    // for releasing it.
    irq_poll = (irq_requested || irq_sources != 0) && !get_flag(FLAGS::I);
}

// The vector an interrupt sequence ends at is not decided when the sequence
// starts.
//
// Hardware runs the pushes first and only then puts the vector's high address
// bits on the bus, so /NMI gets one more look in - between the push of PCL on
// cycle 4 and the push of P on cycle 5. An NMI latched by that point STEALS the
// sequence already in flight: a BRK or an IRQ fetches $FFFA/$FFFB instead of
// $FFFE/$FFFF. It does not run to completion and then take a second sequence,
// which would cost seven more cycles and leave a second frame on the stack.
//
// Only the vector changes. Everything the sequence had already committed to
// stands: BRK has advanced PC past its padding byte and pushes P with B SET,
// and it is that stack frame the NMI handler sees. 2-nmi_and_brk checks exactly
// this - an NMI handler entered with B set on the stack, which no NMI of its
// own could ever produce - and 3-nmi_and_irq the IRQ equivalent, entered with B
// clear and the IRQ handler never run at all.
//
// The window the two ROMs pin is an NMI asserted anywhere from the last cycle
// of the instruction before the sequence through the sequence's cycle 4 - five
// cycle positions, and both ROMs count them out one row at a time. It is the
// state of the line at the cycle-4 SAMPLE, which is why this runs from
// sample_interrupts and not from step()'s cycle 5: between the two the PPU
// still has two dots to draw, and an NMI raised on either of them belongs to
// cycle 5, not cycle 4. Taking the decision at the top of cycle 5 instead
// widened the window by one row, which is visible in 2-nmi_and_brk as a sixth
// hijacked line where hardware prints five.
//
// An NMI sequence does not hijack itself. Without that guard a second edge
// arriving during an NMI's own cycles 1-4 would be swallowed here instead of
// being taken after the handler's first instruction.
//
// ---------------------------------------------------------------------------
// KNOWN ORACLE CONFLICT: this costs Klaus2m5's 6502_interrupt_test, and that is
// not a bug here.
//
// That suite's last case ("test overlapping NMI, IRQ & BRK", $06d7-$06f5 of the
// image) asserts both NMI and IRQ into its feedback port on the final cycle of
// a `sta I_port` and then executes BRK. The edge lands one cycle later than the
// poll that decides the BRK's own boundary, so the BRK runs - and is then
// hijacked from inside, which is precisely Blargg row 5 of the five.
// Its NMI handler traps on B being set in the pushed status ($075c, `and #$10 /
// bne *`), and traps again afterwards on the BRK never having been serviced
// ($06f3, "lost an interrupt"). Klaus flags both in his own source:
//
//     trap_ne   ;unexpected B-flag! - this may fail on a real 6502
//               ;due to a hardware bug on concurrent BRK & NMI
//     ;may fail due to a bug on a real NMOS 6502 - NMI could mask BRK
//
// Both "may"s are unconditional on NMOS. Visual6502 sets B and takes $FFFA;
// Blargg's 2-nmi_and_brk prints $36 / $00 - B set, IRQ handler never entered -
// for five consecutive rows and nothing else matches; the NESdev CPU-interrupts
// page says the same. Klaus2m5 issue #23 is the upstream report, still open.
//
// So Klaus's interrupt test encodes an idealised 6502 for this one case, and
// the two oracles are mutually exclusive: deleting `|| schedule ==
// Schedule::software_interrupt` from the caller flips 6502_interrupt_test to
// its success trap at $06f5 and 2-nmi_and_brk to a failure, one for one. The
// only other test that moves is CpuInterrupts.an_interrupt_sequence_does_not_
// poll, which pins the same five-cycle window directly; the rest of the 640 -
// nestest, all 512 SingleStepTests, the ten ppu_vbl_nmi ROMs, the other four
// cpu_interrupts_v2 ROMs and Klaus's functional test - are indifferent.
// Hardware wins; the Klaus case is left failing deliberately rather than traded
// for a ROM.
// ---------------------------------------------------------------------------
void CPU::poll_interrupt_hijack()
{
    if (servicing_nmi || !nmi_requested) {
        return;
    }

    servicing_nmi = true;

    // The sequence is now an NMI, so point current_instruction at NMI too.
    // This is currently invisible - NMI(), IRQ() and BRK() are the identical
    // one-liner `PC = fetched_operand` - but leaving it stale means the first
    // person to give NMI() distinct behaviour gets a hijacked sequence running
    // the wrong operation, with nothing to indicate why.
    current_instruction = &InstructionSet::NMI;

    nmi_requested = false;
    nmi_committed = false;
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

// Which of the ~30 access schedules an opcode follows.
//
// Derived from the instruction table rather than transcribed as a 256-row list
// of its own: the schedule is a function of the addressing mode and of what the
// operation does to memory, both of which the table already states. Writing it
// out again by hand would be 256 more chances to disagree with it.
//
// The access class comes from the mnemonic. Only two small closed sets are not
// plain reads: the stores, and the read-modify-writes. Everything else - loads,
// compares, ALU operations, and the undocumented NOPs that still fetch an
// operand they discard - reads.
CPU::Schedule CPU::schedule_for(const uint8_t opcode)
{
    const Instruction& instruction = InstructionSet::Table[opcode];
    const std::string& name = instruction.name;
    const Addressing mode = instruction.addr_type;

    // Control flow: each of these has a sequence no addressing mode describes.
    if (name == "BRK") return Schedule::software_interrupt;
    if (name == "JSR") return Schedule::jump_subroutine;
    if (name == "RTS") return Schedule::return_subroutine;
    if (name == "RTI") return Schedule::return_interrupt;
    if (name == "STP") return Schedule::jam;
    if (name == "PHA" || name == "PHP") return Schedule::push;
    if (name == "PLA" || name == "PLP") return Schedule::pull;
    if (name == "JMP") {
        return mode == Addressing::absolute ? Schedule::jump_absolute : Schedule::jump_indirect;
    }
    if (mode == Addressing::relative) return Schedule::branch;

    // The accumulator forms of the shifts are encoded as `implicit` and touch
    // no memory, so they must be classified before the access class is
    // consulted - ASL A is not a read-modify-write of anything.
    if (mode == Addressing::implicit) return Schedule::implied;
    if (mode == Addressing::immediate) return Schedule::immediate;

    const bool writes = name == "STA" || name == "STX" || name == "STY" || name == "SAX" || name == "AHX" ||
                        name == "SHX" || name == "SHY" || name == "TAS";
    const bool modifies = name == "ASL" || name == "LSR" || name == "ROL" || name == "ROR" || name == "INC" ||
                          name == "DEC" || name == "SLO" || name == "SRE" || name == "RLA" || name == "RRA" ||
                          name == "ISC" || name == "DCP";

    // Picks the read/write/read-modify-write member of one addressing mode's
    // family of three, which are laid out adjacently in the enum.
    const auto by_access_class = [writes, modifies](const Schedule read_form) {
        const int offset = writes ? 1 : (modifies ? 2 : 0);
        return static_cast<Schedule>(static_cast<uint8_t>(read_form) + offset);
    };

    switch (mode) {
        case Addressing::zero_page: return by_access_class(Schedule::zero_page_read);
        case Addressing::zero_page_X:
        case Addressing::zero_page_Y: return by_access_class(Schedule::zero_page_indexed_read);
        case Addressing::absolute: return by_access_class(Schedule::absolute_read);
        case Addressing::absolute_X:
        case Addressing::absolute_Y: return by_access_class(Schedule::absolute_indexed_read);
        case Addressing::indexed_indirect: return by_access_class(Schedule::indexed_indirect_read);
        case Addressing::indirect_indexed: return by_access_class(Schedule::indirect_indexed_read);
        default: return Schedule::implied;
    }
}

void CPU::run_operation() { current_instruction->operation(*this); }

uint8_t CPU::index_register() const
{
    switch (current_instruction->addr_type) {
        case Addressing::absolute_X:
        case Addressing::zero_page_X:
        case Addressing::indexed_indirect: return registers.X;
        case Addressing::absolute_Y:
        case Addressing::zero_page_Y:
        case Addressing::indirect_indexed: return registers.Y;
        default: return 0;
    }
}

// One cycle. Exactly one bus access, every time.
//
// The 6502 has no idle cycles: it drives the address bus on every one of them,
// so an instruction's cycle count is not something to track separately - it
// falls out of the number of accesses its schedule makes. That is why there is
// no counter here to get out of step with the accesses, and why `cycles_left`
// is only ever a boundary marker.
bool CPU::clock(bool trace)
{
    ++total_cycles;

    if (cycles_left == 0) {
        cycles_left = 1;
        cycle = 0;
        page_crossed = false;
    }

    ++cycle;

    const bool completed = step();
    completed_instruction_this_cycle = completed;

    // When a Bus is driving this CPU it samples the interrupt lines itself,
    // one PPU dot after this access - see Bus::clock, which is where that dot
    // of separation is justified. Standalone harnesses (nestest, the
    // SingleStepTests runners, Klaus2m5) have no PPU and no dots, so the CPU
    // samples at the end of its own cycle.
    if (!external_interrupt_sampling) {
        sample_interrupts();
    }

    if (!completed) {
        return false;
    }

    // The instruction is over. U is not a real flag, but it reads as set, so
    // pin it once per instruction rather than in each of the 79 operations.
    cycles_left = 0;
    set_flag(FLAGS::U, true);

    if (trace) {
        signal_update();
    }

    return true;
}

bool CPU::step()
{
    // Cycle 1 is the same for every instruction: fetch the opcode. An interrupt
    // that is about to be taken hijacks it - the CPU still drives the bus and
    // still reads at PC, it just discards what comes back and does not advance
    // PC.
    if (cycle == 1) {
        // NMI outranks IRQ, and IRQ is masked by I. Both then run the same
        // seven-cycle sequence, differing only in which vector it ends at.
        //
        // What is tested here is the POLL latched on the previous
        // instruction's penultimate cycle (see sample_interrupts), not the
        // live state of the lines. An NMI that arrived after that poll is
        // still pending and will be taken one instruction later.
        if (nmi_poll || irq_poll) {
            servicing_nmi = nmi_poll;

            if (servicing_nmi) {
                nmi_requested = false;
                nmi_committed = false;
            } else {
                irq_requested = false;
            }

            // Consumed. A fresh poll on this sequence's own penultimate cycle
            // decides whether another interrupt follows it.
            nmi_poll = false;
            irq_poll = false;

            current_instruction = servicing_nmi ? &InstructionSet::NMI : &InstructionSet::IRQ;
            schedule = Schedule::interrupt;
            read(registers.PC);
            return false;
        }

        current_op_code = read(registers.PC++);
        current_instruction = &InstructionSet::Table[current_op_code];
        schedule = schedule_for(current_op_code);

        // A BRK starts out heading for $FFFE, and can be redirected on its
        // cycle 5 like any other interrupt sequence.
        servicing_nmi = false;
        return false;
    }

    switch (schedule) {
        // -------------------------------------------------------------------
        // No operand, or an operand that is the next program byte.
        // -------------------------------------------------------------------

        // The second cycle still reads - at PC, which is left where it is. This
        // is the access an emulator that "does nothing" on implied cycles
        // silently omits.
        case Schedule::implied:
            read(registers.PC);
            run_operation();
            return true;

        case Schedule::immediate:
            fetched_operand = registers.PC;
            latched_value = read(registers.PC++);
            run_operation();
            return true;

        // -------------------------------------------------------------------
        // Zero page.
        // -------------------------------------------------------------------

        case Schedule::zero_page_read:
            if (cycle == 2) {
                fetched_operand = read(registers.PC++);
                return false;
            }
            latched_value = read(fetched_operand);
            run_operation();
            return true;

        case Schedule::zero_page_write:
            if (cycle == 2) {
                fetched_operand = read(registers.PC++);
                return false;
            }
            run_operation();
            write(fetched_operand, latched_value);
            return true;

        case Schedule::zero_page_rmw:
            switch (cycle) {
                case 2: fetched_operand = read(registers.PC++); return false;
                case 3: latched_value = read(fetched_operand); return false;
                // The dummy write. The value is the one just read, unmodified:
                // the ALU is still working on it during this cycle.
                case 4: write(fetched_operand, latched_value); return false;
                default: run_operation(); write(fetched_operand, latched_value); return true;
            }

        // -------------------------------------------------------------------
        // Zero page, indexed. The index is added within the page, and the CPU
        // reads at the un-indexed address first while it does the addition.
        // -------------------------------------------------------------------

        case Schedule::zero_page_indexed_read:
            switch (cycle) {
                case 2: zero_page_pointer = read(registers.PC++); return false;
                case 3:
                    read(zero_page_pointer);
                    fetched_operand = static_cast<uint8_t>(zero_page_pointer + index_register());
                    return false;
                default: latched_value = read(fetched_operand); run_operation(); return true;
            }

        case Schedule::zero_page_indexed_write:
            switch (cycle) {
                case 2: zero_page_pointer = read(registers.PC++); return false;
                case 3:
                    read(zero_page_pointer);
                    fetched_operand = static_cast<uint8_t>(zero_page_pointer + index_register());
                    return false;
                default: run_operation(); write(fetched_operand, latched_value); return true;
            }

        case Schedule::zero_page_indexed_rmw:
            switch (cycle) {
                case 2: zero_page_pointer = read(registers.PC++); return false;
                case 3:
                    read(zero_page_pointer);
                    fetched_operand = static_cast<uint8_t>(zero_page_pointer + index_register());
                    return false;
                case 4: latched_value = read(fetched_operand); return false;
                case 5: write(fetched_operand, latched_value); return false;
                default: run_operation(); write(fetched_operand, latched_value); return true;
            }

        // -------------------------------------------------------------------
        // Absolute.
        // -------------------------------------------------------------------

        case Schedule::absolute_read:
            switch (cycle) {
                case 2: base_low = read(registers.PC++); return false;
                case 3:
                    base_high = read(registers.PC++);
                    fetched_operand = static_cast<uint16_t>(base_high << 8) | base_low;
                    return false;
                default: latched_value = read(fetched_operand); run_operation(); return true;
            }

        case Schedule::absolute_write:
            switch (cycle) {
                case 2: base_low = read(registers.PC++); return false;
                case 3:
                    base_high = read(registers.PC++);
                    fetched_operand = static_cast<uint16_t>(base_high << 8) | base_low;
                    return false;
                default: run_operation(); write(fetched_operand, latched_value); return true;
            }

        case Schedule::absolute_rmw:
            switch (cycle) {
                case 2: base_low = read(registers.PC++); return false;
                case 3:
                    base_high = read(registers.PC++);
                    fetched_operand = static_cast<uint16_t>(base_high << 8) | base_low;
                    return false;
                case 4: latched_value = read(fetched_operand); return false;
                case 5: write(fetched_operand, latched_value); return false;
                default: run_operation(); write(fetched_operand, latched_value); return true;
            }

        // -------------------------------------------------------------------
        // Absolute, indexed.
        //
        // The index is added to the low byte only, and the CPU accesses that
        // un-carried address on the next cycle while the carry propagates. For
        // a read that speculative access is the operand read itself, and the
        // instruction is over unless the carry made it wrong - which is exactly
        // what the "oops" cycle is. Stores and read-modify-writes cannot
        // speculate (they would write to the wrong address), so for them the
        // access is discarded and the fix-up cycle is unconditional. That, not
        // an accounting convention, is why their cost does not vary.
        // -------------------------------------------------------------------

        case Schedule::absolute_indexed_read:
            switch (cycle) {
                case 2: base_low = read(registers.PC++); return false;
                case 3: base_high = read(registers.PC++); resolve_indexed_address(); return false;
                case 4:
                    latched_value = read(fetched_operand);
                    if (!page_crossed) {
                        run_operation();
                        return true;
                    }
                    fetched_operand += 0x0100;
                    return false;
                default: latched_value = read(fetched_operand); run_operation(); return true;
            }

        case Schedule::absolute_indexed_write:
            switch (cycle) {
                case 2: base_low = read(registers.PC++); return false;
                case 3: base_high = read(registers.PC++); resolve_indexed_address(); return false;
                case 4: read(fetched_operand); fix_up_indexed_address(); return false;
                default: run_operation(); write(fetched_operand, latched_value); return true;
            }

        case Schedule::absolute_indexed_rmw:
            switch (cycle) {
                case 2: base_low = read(registers.PC++); return false;
                case 3: base_high = read(registers.PC++); resolve_indexed_address(); return false;
                case 4: read(fetched_operand); fix_up_indexed_address(); return false;
                case 5: latched_value = read(fetched_operand); return false;
                case 6: write(fetched_operand, latched_value); return false;
                default: run_operation(); write(fetched_operand, latched_value); return true;
            }

        // -------------------------------------------------------------------
        // (zero page,X): the index is added to the pointer, not to the address
        // it holds, so there is no page to cross and the cost is fixed.
        // -------------------------------------------------------------------

        case Schedule::indexed_indirect_read:
            if (read_indexed_indirect_pointer()) {
                latched_value = read(fetched_operand);
                run_operation();
                return true;
            }
            return false;

        case Schedule::indexed_indirect_write:
            if (read_indexed_indirect_pointer()) {
                run_operation();
                write(fetched_operand, latched_value);
                return true;
            }
            return false;

        case Schedule::indexed_indirect_rmw:
            if (!read_indexed_indirect_pointer()) {
                return false;
            }
            switch (cycle) {
                case 6: latched_value = read(fetched_operand); return false;
                case 7: write(fetched_operand, latched_value); return false;
                default: run_operation(); write(fetched_operand, latched_value); return true;
            }

        // -------------------------------------------------------------------
        // (zero page),Y: the index is added to the address the pointer holds,
        // so this indexes like the absolute forms and behaves the same way
        // about the carry.
        // -------------------------------------------------------------------

        case Schedule::indirect_indexed_read:
            switch (cycle) {
                case 2: zero_page_pointer = read(registers.PC++); return false;
                case 3: base_low = read(zero_page_pointer); return false;
                case 4:
                    base_high = read(static_cast<uint8_t>(zero_page_pointer + 1));
                    resolve_indexed_address();
                    return false;
                case 5:
                    latched_value = read(fetched_operand);
                    if (!page_crossed) {
                        run_operation();
                        return true;
                    }
                    fetched_operand += 0x0100;
                    return false;
                default: latched_value = read(fetched_operand); run_operation(); return true;
            }

        case Schedule::indirect_indexed_write:
            switch (cycle) {
                case 2: zero_page_pointer = read(registers.PC++); return false;
                case 3: base_low = read(zero_page_pointer); return false;
                case 4:
                    base_high = read(static_cast<uint8_t>(zero_page_pointer + 1));
                    resolve_indexed_address();
                    return false;
                case 5: read(fetched_operand); fix_up_indexed_address(); return false;
                default: run_operation(); write(fetched_operand, latched_value); return true;
            }

        case Schedule::indirect_indexed_rmw:
            switch (cycle) {
                case 2: zero_page_pointer = read(registers.PC++); return false;
                case 3: base_low = read(zero_page_pointer); return false;
                case 4:
                    base_high = read(static_cast<uint8_t>(zero_page_pointer + 1));
                    resolve_indexed_address();
                    return false;
                case 5: read(fetched_operand); fix_up_indexed_address(); return false;
                case 6: latched_value = read(fetched_operand); return false;
                case 7: write(fetched_operand, latched_value); return false;
                default: run_operation(); write(fetched_operand, latched_value); return true;
            }

        // -------------------------------------------------------------------
        // Control flow.
        // -------------------------------------------------------------------

        // A branch adds the offset to PC in two halves, one cycle apart, and
        // reads at the intermediate address in between - so a taken branch onto
        // another page reads once from an address that is not its target and
        // not where it came from.
        case Schedule::branch:
            if (cycle == 2) {
                const uint8_t offset = read(registers.PC++);
                fetched_operand = (offset & 0x80) ? (0xFF00 | offset) : offset;

                if (!branch_is_taken()) {
                    return true;
                }
                return false;
            }

            if (cycle == 3) {
                read(registers.PC);

                const uint16_t from = registers.PC;
                run_operation();  // the branch itself, which moves PC to the target
                branch_target = registers.PC;

                if ((branch_target & 0xFF00) == (from & 0xFF00)) {
                    return true;
                }

                // Only the low byte has landed yet.
                registers.PC = (from & 0xFF00) | (branch_target & 0x00FF);
                return false;
            }

            read(registers.PC);
            registers.PC = branch_target;
            return true;

        case Schedule::jump_absolute:
            if (cycle == 2) {
                base_low = read(registers.PC++);
                return false;
            }
            base_high = read(registers.PC++);
            fetched_operand = static_cast<uint16_t>(base_high << 8) | base_low;
            run_operation();
            return true;

        case Schedule::jump_indirect:
            switch (cycle) {
                case 2: base_low = read(registers.PC++); return false;
                case 3:
                    base_high = read(registers.PC++);
                    indirect_pointer = static_cast<uint16_t>(base_high << 8) | base_low;
                    return false;
                case 4: base_low = read(indirect_pointer); return false;
                // The famous bug: the pointer increment does not carry out of
                // the low byte, so a pointer of $xxFF takes its high byte from
                // $xx00.
                default:
                    base_high = read((indirect_pointer & 0xFF00) | static_cast<uint8_t>(indirect_pointer + 1));
                    fetched_operand = static_cast<uint16_t>(base_high << 8) | base_low;
                    run_operation();
                    return true;
            }

        // JSR interleaves its two operand fetches around the pushes: the low
        // byte of the target is read on cycle 2, but the high byte not until
        // cycle 6, after the return address is already on the stack. The
        // address pushed is that of the last byte of the JSR, one short of the
        // instruction that follows it.
        case Schedule::jump_subroutine:
            switch (cycle) {
                case 2: base_low = read(registers.PC++); return false;
                case 3: read(STACK_BASE_ADDR + registers.SP); return false;
                case 4: push_stack(high_byte(registers.PC)); return false;
                case 5: push_stack(low_byte(registers.PC)); return false;
                default:
                    base_high = read(registers.PC);
                    fetched_operand = static_cast<uint16_t>(base_high << 8) | base_low;
                    run_operation();
                    return true;
            }

        case Schedule::return_subroutine:
            switch (cycle) {
                case 2: read(registers.PC); return false;
                case 3: read(STACK_BASE_ADDR + registers.SP); return false;
                case 4: base_low = pop_stack(); return false;
                case 5: base_high = pop_stack(); return false;
                // The last cycle reads at the popped address before stepping
                // past it: JSR pushed the address of its own last byte, so the
                // return address is one further on.
                default:
                    fetched_operand = static_cast<uint16_t>(base_high << 8) | base_low;
                    read(fetched_operand);
                    run_operation();
                    return true;
            }

        case Schedule::return_interrupt:
            switch (cycle) {
                case 2: read(registers.PC); return false;
                case 3: read(STACK_BASE_ADDR + registers.SP); return false;
                case 4: set_P_from_pulled(pop_stack()); return false;
                case 5: base_low = pop_stack(); return false;
                // Unlike RTS there is no adjustment: an interrupt pushed the
                // address it was going to run next, not the one before it.
                default:
                    base_high = pop_stack();
                    fetched_operand = static_cast<uint16_t>(base_high << 8) | base_low;
                    run_operation();
                    return true;
            }

        // BRK reads and discards the byte after the opcode - which is why it is
        // a two-byte instruction despite having no operand - and pushes a
        // return address past it.
        //
        // The vector is not hard-wired to $FFFE: see poll_interrupt_hijack.
        case Schedule::software_interrupt:
            switch (cycle) {
                case 2: read(registers.PC++); return false;
                case 3: push_stack(high_byte(registers.PC)); return false;
                case 4: push_stack(low_byte(registers.PC)); return false;
                case 5:
                    // B exists only in the copy of P that reaches the stack,
                    // and it is set here whether or not an NMI has just taken
                    // the sequence over: what B records is which instruction
                    // STARTED the sequence, not where it ends up.
                    push_stack(registers.P | static_cast<uint8_t>(FLAGS::B));
                    return false;
                case 6:
                    // I is set here, with the vector fetch, not with the push on
                    // cycle 5. The pushed copy therefore carries the OLD I, which
                    // is what lets RTI restore it. Unobservable while sequences
                    // return early from the poll, but it stops being so the
                    // moment anything makes them poll.
                    base_low = read(servicing_nmi ? 0xFFFA : 0xFFFE);
                    set_flag(FLAGS::I, true);
                    return false;
                default:
                    base_high = read(servicing_nmi ? 0xFFFB : 0xFFFF);
                    fetched_operand = static_cast<uint16_t>(base_high << 8) | base_low;
                    run_operation();
                    return true;
            }

        case Schedule::push:
            if (cycle == 2) {
                read(registers.PC);
                return false;
            }
            run_operation();  // performs the single write
            return true;

        case Schedule::pull:
            switch (cycle) {
                case 2: read(registers.PC); return false;
                // SP is pre-incremented by the pull, so this reads the slot
                // above the one the value will come from.
                case 3: read(STACK_BASE_ADDR + registers.SP); return false;
                default: run_operation(); return true;  // performs the single read
            }

        // Hardware takes an interrupt by running a BRK whose opcode fetch was
        // suppressed: same seven cycles, same pushes, but PC is not advanced
        // and the pushed copy of P has B clear.
        case Schedule::interrupt:
            switch (cycle) {
                case 2: read(registers.PC); return false;
                case 3: push_stack(high_byte(registers.PC)); return false;
                case 4: push_stack(low_byte(registers.PC)); return false;
                case 5:
                    push_stack(registers.P & ~static_cast<uint8_t>(FLAGS::B));
                    return false;
                case 6:
                    // See the BRK sequence: I is set with the vector fetch.
                    base_low = read(servicing_nmi ? 0xFFFA : 0xFFFE);
                    set_flag(FLAGS::I, true);
                    return false;
                default:
                    base_high = read(servicing_nmi ? 0xFFFB : 0xFFFF);
                    fetched_operand = static_cast<uint16_t>(base_high << 8) | base_low;
                    run_operation();
                    return true;
            }

        // The undocumented "jam" opcodes stop the sequencer. The address bus is
        // left driven, and what it settles into is this fixed pattern - the
        // last few cycles of a vector fetch, repeating. Modelled rather than
        // hung so the emulator stays steppable; SingleStepTests captures the
        // same eleven accesses for all twelve jam opcodes.
        case Schedule::jam:
            switch (cycle) {
                case 2: read(registers.PC); return false;
                case 3: read(0xFFFF); return false;
                case 4:
                case 5: read(0xFFFE); return false;
                default: read(0xFFFF); return cycle == 11;
            }
    }

    return true;
}

// Splits the indexed address computation the way the hardware does: the index
// goes into the low byte now, and the carry into the high byte a cycle later.
// Leaves fetched_operand holding the un-carried address, which is the one the
// next cycle accesses.
void CPU::resolve_indexed_address()
{
    const uint16_t indexed_low = static_cast<uint16_t>(base_low) + index_register();
    page_crossed = indexed_low > 0xFF;
    fetched_operand = static_cast<uint16_t>(base_high << 8) | static_cast<uint8_t>(indexed_low);
}

void CPU::fix_up_indexed_address()
{
    if (page_crossed) {
        fetched_operand += 0x0100;
    }
}

// Cycles 2 to 5 of the (zero page,X) modes, which are identical whatever the
// instruction then does with the address. Returns true once the address is
// resolved, i.e. on the cycle the caller should start its own access.
bool CPU::read_indexed_indirect_pointer()
{
    switch (cycle) {
        case 2: zero_page_pointer = read(registers.PC++); return false;
        case 3:
            // The un-indexed pointer is read and discarded while X is added.
            read(zero_page_pointer);
            zero_page_pointer = static_cast<uint8_t>(zero_page_pointer + registers.X);
            return false;
        case 4: base_low = read(zero_page_pointer); return false;
        case 5:
            // The pointer's high byte wraps within zero page: a pointer at $FF
            // takes it from $00, not $0100.
            base_high = read(static_cast<uint8_t>(zero_page_pointer + 1));
            fetched_operand = static_cast<uint16_t>(base_high << 8) | base_low;
            return false;
        default: return true;
    }
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

    cycles_left = 0;

    // Pending interrupts do not survive a reset. Without this, a raise_NMI()
    // that arrived before reset() would still be latched, and the first
    // instruction after reset would be an NMI.
    nmi_requested = false;
    irq_requested = false;
    servicing_nmi = false;

    // irq_sources is deliberately NOT cleared. Those are wires held low by
    // devices, not CPU state: RESET does not reach into the APU and cancel its
    // pending frame interrupt. Clearing them would drop a real pending interrupt
    // while $4015 still reported it, leaving the two out of step until the
    // next sequence re-drove the line.
    nmi_committed = false;
    nmi_poll = false;
    irq_poll = false;
    completed_instruction_this_cycle = false;

    signal_update();
}

void CPU::push_stack(const uint8_t byte) { write(STACK_BASE_ADDR + (registers.SP--), byte); }

uint8_t CPU::pop_stack() { return read(STACK_BASE_ADDR + (++registers.SP)); }

// B does not exist as a real bit in P; it only ever appears in the copy pushed
// onto the stack. Pulling one back has to discard it and leave U set. PLP and
// RTI both go through here so they cannot disagree.
void CPU::set_P_from_pulled(const uint8_t pulled)
{
    registers.P = (pulled & ~static_cast<uint8_t>(FLAGS::B)) | static_cast<uint8_t>(FLAGS::U);
}

void CPU::set_flag(const FLAGS flag, const bool value)
{
    if (value) {
        registers.P |= static_cast<uint8_t>(flag);
    } else {
        registers.P &= ~static_cast<uint8_t>(flag);
    }
}

bool CPU::get_flag(const FLAGS flag) const { return registers.P & static_cast<uint8_t>(flag); }

/* Operations */

void CPU::ADC() { ADC_SBC_internal(operand_value()); }

void CPU::SBC() { ADC_SBC_internal(operand_value() ^ 0xFF); }

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
    const uint8_t value = operand_value();
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
    const uint8_t value = operand_value() ^ 0xFF;
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
    registers.A &= operand_value();
    set_flag(FLAGS::Z, !registers.A);
    set_flag(FLAGS::N, registers.A & 0x80);
}

void CPU::ORA()
{
    registers.A |= operand_value();
    set_flag(FLAGS::Z, !registers.A);
    set_flag(FLAGS::N, registers.A & 0x80);
}

void CPU::EOR()
{
    registers.A ^= operand_value();
    set_flag(FLAGS::Z, !registers.A);
    set_flag(FLAGS::N, registers.A & 0x80);
}

void CPU::CMP()
{
    uint8_t result = registers.A - operand_value();
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::C, registers.A >= operand_value());
}

void CPU::CPX()
{
    uint8_t result = registers.X - operand_value();
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::C, registers.X >= operand_value());
}

void CPU::CPY()
{
    uint8_t data = operand_value();
    uint8_t result = registers.Y - data;
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    set_flag(FLAGS::C, registers.Y >= data);
}

void CPU::DEC()
{
    uint8_t result = operand_value() - 1;
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    store_result(result);
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
    uint8_t result = operand_value() + 1;
    set_flag(FLAGS::Z, !result);
    set_flag(FLAGS::N, result & 0x80);
    store_result(result);
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

    uint8_t value = addressing_is_implicit ? registers.A : operand_value();
    set_flag(FLAGS::C, value & 0x80);
    value <<= 1;
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::Z, value == 0);

    if (addressing_is_implicit) {
        registers.A = value;
    } else {
        store_result(value);
    }
}

void CPU::ROL()
{
    bool addressing_is_implicit = InstructionSet::Table[current_op_code].addr_type == Addressing::implicit;

    uint8_t value = addressing_is_implicit ? registers.A : operand_value();

    auto carry = get_flag(FLAGS::C);
    set_flag(FLAGS::C, value & 0x80);
    value <<= 1;
    value |= carry;
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::Z, value == 0);

    if (addressing_is_implicit) {
        registers.A = value;
    } else {
        store_result(value);
    }
}

void CPU::LSR()
{
    bool addressing_is_implicit = InstructionSet::Table[current_op_code].addr_type == Addressing::implicit;

    uint8_t value = addressing_is_implicit ? registers.A : operand_value();

    set_flag(FLAGS::C, value & 0x1);
    value >>= 1;
    set_flag(FLAGS::N, false);
    set_flag(FLAGS::Z, value == 0);

    if (addressing_is_implicit) {
        registers.A = value;
    } else {
        store_result(value);
    }
}

void CPU::ROR()
{
    bool addressing_is_implicit = InstructionSet::Table[current_op_code].addr_type == Addressing::implicit;

    uint8_t value = addressing_is_implicit ? registers.A : operand_value();

    auto carry = get_flag(FLAGS::C);
    set_flag(FLAGS::C, value & 0x1);
    value >>= 1;
    value |= (carry << 7);
    set_flag(FLAGS::N, value & 0x80);
    set_flag(FLAGS::Z, value == 0);

    if (addressing_is_implicit) {
        registers.A = value;
    } else {
        store_result(value);
    }
}

void CPU::LDA()
{
    registers.A = operand_value();
    set_flag(FLAGS::N, registers.A & 0x80);
    set_flag(FLAGS::Z, registers.A == 0);
}

void CPU::STA() { store_result(registers.A); }

void CPU::LDX()
{
    registers.X = operand_value();
    set_flag(FLAGS::N, registers.X & 0x80);
    set_flag(FLAGS::Z, registers.X == 0);
}

void CPU::STX() { store_result(registers.X); }

void CPU::LDY()
{
    registers.Y = operand_value();
    set_flag(FLAGS::N, registers.Y & 0x80);
    set_flag(FLAGS::Z, registers.Y == 0);
}

void CPU::STY() { store_result(registers.Y); }

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

void CPU::PLP() { set_P_from_pulled(pop_stack()); }

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

/* Control flow.
 *
 * These six are the one place where an "operation" is not separable from its
 * timing: the pushes, the pulls and the vector fetch ARE the instruction, and
 * they happen on specific cycles. The schedule in step() performs them, one
 * access per cycle. What is left here is the state change that lands on the
 * final cycle, once the schedule has resolved the destination into
 * fetched_operand.
 */

// The pushes and both vector reads happen on cycles 3 to 7; I is set after P
// reaches the stack, so the copy pushed has the old value.
void CPU::BRK() { registers.PC = fetched_operand; }

void CPU::NMI() { registers.PC = fetched_operand; }

void CPU::IRQ() { registers.PC = fetched_operand; }

// P is pulled on cycle 4, through the same path PLP uses.
void CPU::RTI() { registers.PC = fetched_operand; }

void CPU::JSR() { registers.PC = fetched_operand; }

// JSR pushes the address of its own last byte rather than the one after it, so
// the pulled address is one short of where execution resumes.
void CPU::RTS() { registers.PC = fetched_operand + 1; }

void CPU::JMP() { registers.PC = fetched_operand; }

void CPU::BIT()
{
    uint8_t value = operand_value();
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
void CPU::SAX() { store_result(registers.A & registers.X); }

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

/* The unstable stores.
 *
 * These four AND the value being stored with the high byte of the *base*
 * address plus one - a value that is on an internal bus at the moment the store
 * happens, not anything the programmer asked for. Worse, when indexing carried,
 * the high byte of the destination is replaced by that same value, so the store
 * lands on a page nobody named:
 *
 *     SHA ($18),Y   pointer -> $708B, Y = $FD, so the address is $7188
 *                   value = A & X & ($70 + 1)  =  $40
 *                   the carry never reaches the address, which becomes $4088
 *
 * They are famous for being chip- and temperature-dependent, but the part that
 * varies is a further AND with the data left floating on the bus, which only
 * shows up when the value would otherwise have bits the floating bus does not.
 * The behaviour encoded here is what SingleStepTests captures, and it matches
 * all 10,000 vectors for each of the four - so it is an oracle-backed
 * behaviour, not a guess. The address is rewritten in place because the
 * schedule performs the write from fetched_operand after this runs.
 */
void CPU::unstable_store(const uint8_t source)
{
    const uint8_t value = source & static_cast<uint8_t>(base_high + 1);

    if (page_crossed) {
        fetched_operand = static_cast<uint16_t>(value << 8) | low_byte(fetched_operand);
    }

    store_result(value);
}

void CPU::AHX() { unstable_store(registers.A & registers.X); }

void CPU::SHX() { unstable_store(registers.X); }

void CPU::SHY() { unstable_store(registers.Y); }

// Sets SP to A & X as well, then stores like AHX.
void CPU::TAS()
{
    registers.SP = registers.A & registers.X;
    unstable_store(registers.SP);
}

/* The remaining undocumented opcodes.
 *
 * These were previously left as no-ops on the grounds that there was "no oracle
 * to test against". That was wrong: the SingleStepTests vectors in this repo
 * cover all 256 opcodes, and each of the behaviours below was derived from them
 * and then checked against 10,000 cases apiece rather than guessed.
 */

// Halts the CPU until RESET ("jam"/"KIL"). The bus pattern of a halted CPU is
// produced by Schedule::jam; there is no register state to change here.
void CPU::STP() { ; }

// LXA/ATX: A = X = (A | magic) & immediate.
//
// The magic constant is a genuine analogue effect - on real hardware it depends
// on the chip, its temperature and what is floating on the bus, and is commonly
// observed as $EE or $FF. $EE is what the test vectors encode and what most
// emulators use; it is a defensible choice, not a hardware invariant. Code that
// relies on this opcode is relying on undefined behaviour.
void CPU::LXA()
{
    const uint8_t result = (registers.A | 0xEE) & operand_value();
    registers.A = result;
    registers.X = result;
    set_flag(FLAGS::Z, result == 0);
    set_flag(FLAGS::N, result & 0x80);
}

// XAA/ANE: A = (A | magic) & X & immediate. Same magic-constant caveat as LXA.
void CPU::XAA()
{
    const uint8_t result = (registers.A | 0xEE) & registers.X & operand_value();
    registers.A = result;
    set_flag(FLAGS::Z, result == 0);
    set_flag(FLAGS::N, result & 0x80);
}

// LAS/LAR: A = X = SP = memory & SP.
void CPU::LAS()
{
    const uint8_t result = operand_value() & registers.SP;
    registers.A = result;
    registers.X = result;
    registers.SP = result;
    set_flag(FLAGS::Z, result == 0);
    set_flag(FLAGS::N, result & 0x80);
}

// ARR: AND then rotate right through carry, with its own idiosyncratic flags -
// C is bit 6 of the result and V is bit 6 XOR bit 5, which is why this cannot
// just be written as AND followed by ROR.
void CPU::ARR()
{
    const uint8_t result =
        static_cast<uint8_t>(((registers.A & operand_value()) >> 1) | (get_flag(FLAGS::C) ? 0x80 : 0x00));
    registers.A = result;
    set_flag(FLAGS::C, result & 0x40);
    set_flag(FLAGS::V, ((result >> 6) ^ (result >> 5)) & 0x01);
    set_flag(FLAGS::Z, result == 0);
    set_flag(FLAGS::N, result & 0x80);
}

// AXS/SBX: X = (A & X) - immediate, setting flags like a compare (no borrow in,
// and V untouched).
void CPU::AXS()
{
    const uint8_t lhs = registers.A & registers.X;
    const uint8_t rhs = operand_value();
    const uint8_t result = static_cast<uint8_t>(lhs - rhs);
    registers.X = result;
    set_flag(FLAGS::C, lhs >= rhs);
    set_flag(FLAGS::Z, result == 0);
    set_flag(FLAGS::N, result & 0x80);
}

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
       << " Mid-instruction: " << (cpu.cycles_left != 0 ? "yes" : "no") << std::endl;

    return os;
}
