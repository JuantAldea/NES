#include "../include/bus.h"

Bus::Bus()
    : cpu{std::bind(&Bus::read, this, std::placeholders::_1),
          std::bind(&Bus::write, this, std::placeholders::_1, std::placeholders::_2)},
      apu{this},
      ppu{this},
      ram{this},
      prg_ram{this},
      rom{this},
      controllers{this}
{
    // Bus::clock places the interrupt sample one PPU dot after the CPU's bus
    // access, which the CPU cannot do for itself.
    cpu.external_interrupt_sampling = true;

    // Before cpu.reset(), and that order is the point: the APU's power-on
    // sequence is defined as running BEFORE execution begins at the reset
    // vector, and it leaves the frame counter mid-sequence rather than at zero.
    apu.power_on();
    cpu.reset();
}

// The APU goes first for the same reason it does at power-on: its restart
// re-writes $4017 and burns the 9-12 cycle delay, and that has to be settled
// before the CPU fetches from the reset vector.
//
// The PPU is deliberately NOT reset here. Hardware does clear PPUCTRL, PPUMASK
// and the write latch, but nothing in this repo measures it: the apu_reset ROMs
// re-initialise the PPU themselves, and len_ctrs_enabled passes across a reset
// without it. Adding it unmeasured would be a guess sitting underneath every
// rendering test. It belongs with the first oracle that actually demands it.
void Bus::reset()
{
    apu.reset();
    cpu.reset();
}

bool Bus::load_cartridge(const std::string& path) { return rom.load(path); }

// Single address decode used by both read() and write(). This is the only
// place CPU addresses get mapped to a device + effective (mirrored) address,
// so the two paths cannot drift apart on any address where hardware agrees.
//
// It used to say they "can never disagree", full stop. That was true only
// while the controllers were unimplemented. $4017 really is two devices - the
// APU frame counter on write, controller port 2 on read - so the direction is
// now a parameter. Keeping one function with an Access argument, rather than
// letting read() and write() each grow a special case, is what preserves the
// original guarantee everywhere it still holds.
//
// $6000-$7FFF is the second place the direction matters, for a different
// reason: the mapper can make the PRG-RAM readable but not writable. See the
// $A001 block below.
//
// NES CPU memory map (as relevant here):
//   $0000-$1FFF  2KB internal RAM, mirrored every $0800
//   $2000-$3FFF  8 PPU registers, mirrored every 8 bytes
//   $4000-$4013, $4015  APU registers
//   $4014        PPU OAMDMA
//   $4016        controller strobe (write), controller 1 data (read)
//   $4017        APU frame counter (write), controller 2 data (READ - a
//                different device from the write; see the Access parameter)
//   $4018-$401F  APU/IO test registers (stubbed: no device, open bus)
//   $4020-$5FFF  cartridge expansion area (stubbed: no device, open bus)
//   $6000-$7FFF  8KB cartridge PRG-RAM
//   $8000-$FFFF  cartridge PRG-ROM
Bus::DecodedAddress Bus::decode(const uint16_t addr, const Access access)
{
    if (addr < 0x2000) {
        return {&ram, static_cast<uint16_t>(addr % 0x0800)};
    } else if (addr < 0x4000) {
        return {&ppu, static_cast<uint16_t>(0x2000 + (addr % 8))};
    } else if (addr == 0x4014) {
        return {&ppu, addr};
    } else if (addr == 0x4016) {
        // Symmetric: the write is the strobe for BOTH ports, the read is
        // port 1. One device owns both because one strobe line does.
        return {&controllers, addr};
    } else if (addr == 0x4017) {
        // The one address where the two directions are genuinely different
        // devices on hardware.
        return access == Access::Write ? DecodedAddress{&apu, addr} : DecodedAddress{&controllers, addr};
    } else if (addr <= 0x4013 || addr == 0x4015) {
        return {&apu, addr};
    } else if (addr <= 0x401F) {
        // $4018-$401F are CPU test registers, disabled on a retail NES.
        return {nullptr, addr};
    } else if (addr < 0x6000) {
        // Cartridge expansion area; no device backs it.
        return {nullptr, addr};
    } else if (addr < 0x8000) {
        // MMC3's $A001 gates this window, and the mapper is the only thing that
        // knows: PrgRAM is a plain memory device with no idea a mapper exists.
        // Boards without those bits leave both flags at their power-on values
        // (enabled, writable) for the lifetime of the cartridge, so this costs
        // them two predictable loads and no behaviour.
        //
        //   bit 7 clear - the chip is not selected. Nothing drives the data
        //                 bus, so a read is open bus and a write goes nowhere,
        //                 which is precisely what a null device gives.
        //   bit 6 set   - /WE is held inactive. The chip still answers reads;
        //                 only the write is swallowed. This is the direction-
        //                 dependent case, and the reason Access has to be
        //                 consulted here at all.
        if (!rom.prg_ram_enabled) {
            return {nullptr, addr};
        }
        if (access == Access::Write && rom.prg_ram_write_protected) {
            return {nullptr, addr};
        }
        return {&prg_ram, static_cast<uint16_t>(addr - 0x6000)};
    }

    return {&rom, addr};
}

void Bus::write(const uint16_t addr, const uint8_t data)
{
    // See Bus::clock. True only while the CPU is executing a cycle, so the
    // cartridge loader, tests and the debugger do not register as write cycles.
    if (watching_cpu_access) {
        cpu_wrote_this_cycle = true;
    }

    // The value sits on the data bus whether or not anything latches it, so a
    // write refreshes open bus even at an address no device backs.
    cpu_open_bus = data;

    const DecodedAddress d = decode(addr, Access::Write);
    if (d.device) {
        d.device->write(d.effective_addr, data);
    }
    // Open-bus ranges (no device): writes are discarded.
}

void Bus::write_ram(const uint16_t start_addr, const size_t n_bytes, const uint8_t* bytes)
{
    // Routed through write() one byte at a time rather than straight at the RAM
    // array: passing the raw address to RAM::write would bypass decode() and
    // make this a third address path that disagrees with the other two. A bulk
    // write starting in a mirror ($0800-$1FFF) silently wrote nothing, because
    // RAM::write clamps out-of-range offsets.
    for (size_t i = 0; i < n_bytes; ++i) {
        write(static_cast<uint16_t>(start_addr + i), bytes[i]);
    }
}

uint8_t Bus::read(const uint16_t addr)
{
    const DecodedAddress d = decode(addr, Access::Read);
    if (d.device) {
        cpu_open_bus = d.device->read(d.effective_addr);
        return cpu_open_bus;
    }

    // Nothing drives the bus here, so it keeps its previous value. A device
    // whose register is write-only reports the same thing for itself, through
    // Device::open_bus().
    return cpu_open_bus;
}

// One master-clock tick. The CPU divides it by 12 and the PPU by 4, so a CPU
// cycle spans exactly three PPU dots.
//
// The CPU does two things per cycle that land at DIFFERENT points in that
// three-dot window, and collapsing them into one is what made the NMI timing
// wrong:
//
//   * its single bus access, placed immediately BEFORE the dot that shares its
//     master tick;
//   * its interrupt sample, immediately AFTER that same dot.
//
// One dot of separation, no more. It is not a free parameter - the ROMs pin it
// exactly. Take the dot at which the vblank flag is set, S, and let A be the
// dot a $2002 read is placed just ahead of:
//
//   A = S    the read and the set collide; the flag never sets   (02 row 04)
//   A = S+1  flag reads back set, but no NMI                     (06 row 05)
//   A = S+2  likewise                                            (06 row 06)
//   A = S+3  NMI occurs normally                                 (06 row 07)
//
// Rows 05 and 06 require the sample to be no more than one dot past the access
// (any later and the previous cycle's sample would already have caught the
// assertion); row 07 requires it to be at least one dot past (any earlier and
// the previous cycle's sample, taken at A = S, would miss a flag set on that
// very dot). Only +1 satisfies both.
//
// The access phase itself - CPU on master ticks divisible by 12, ahead of the
// PPU - is the one the ROMs agree with, and is left alone.
bool Bus::clock()
{
    ++total_cycles;

    const bool cpu_tick = total_cycles % 12 == 0;

    // A CPU cycle is stolen outright while OAM DMA holds the bus. The DMA unit
    // runs at the CPU's rate, not the PPU's - it is stealing the CPU's cycles,
    // one transfer every other one. Driving it from PPU::clock instead made the
    // whole transfer take ~171 CPU cycles rather than 513.
    // DMC DMA outranks OAM DMA: "when accesses collide, DMC DMA is allowed to
    // run and OAM DMA is paused". Checking it first, and skipping
    // perform_OAM_DMA_cycle below while it holds the bus, is that rule.
    const bool dma_holds_the_bus = ppu.dma_in_progress() || dmc_dma_holds_the_bus();
    const bool cpu_cycle = cpu_tick && !dma_holds_the_bus;

    // The frame counter divides the CPU clock and keeps running while DMA holds
    // the bus - it is not gated on the CPU executing, any more than the /NMI
    // edge detector is. Bus::cpu_cycles is that same divider, and is the phase
    // reference OAM DMA aligns to.
    //
    // Both advance BEFORE the CPU's bus access, not after. The frame interrupt
    // flag belongs to the start of the cycle it is set on, so a $4015 read
    // placed on that very cycle reads it back set - which is what hardware
    // does, and what blargg's sync_apu relies on to decide whether to burn an
    // extra clock:
    //
    //     lda #$40
    //     bit SNDCHN      ; the read lands exactly on cycle 29828
    //     bne :+          ; +1 clock iff the flag is already set
    //
    // With the APU clocked after the access instead, that read returned zero,
    // the branch fell through, and every cpu_interrupts_v2 ROM that calls
    // sync_apu ran one CPU cycle out of step with the frame counter for the
    // rest of the test - which is exactly the one-cycle skew 4-irq_and_dma
    // reported.
    if (cpu_tick) {
        ++cpu_cycles;
        apu.clock();
    }

    // Cleared on every CPU tick, not just the ones the CPU runs. While OAM DMA
    // holds the bus the CPU executes nothing, and a value left over from before
    // the DMA started would answer "is the CPU writing?" with ancient news - a
    // stale true made a DMC halt spin for the length of the whole OAM DMA.
    if (cpu_tick) {
        cpu_wrote_this_cycle = false;
    }

    bool completed_instruction = false;
    if (cpu_cycle) {
        // Watched, not asked. DMC DMA needs to know whether the cycle the CPU
        // just ran was a read or a write - "the CPU only allows this on read
        // cycles. If the CPU is writing, it ignores the halt...repeating until
        // successful" - and the CPU has no notion of that itself.
        //
        // Rather than thread a flag through all 79 operations and every
        // addressing mode, this observes the one place every access already
        // passes through. The CPU is cycle-stepped at exactly one bus access
        // per cycle, so whether Bus::write was reached during clock_CPU() is
        // the whole answer.
        //
        // Gated on `watching_cpu_access` because Bus::write is public: tests,
        // the cartridge loader and the debugger all use it, and none of those
        // are a CPU write cycle.
        watching_cpu_access = true;
        completed_instruction = clock_CPU();
        watching_cpu_access = false;
    } else if (cpu_tick && dmc_dma == DmcDma::Get) {
        // The one cycle of the sequence that touches memory. Samples live in
        // PRG-ROM, where a read has no side effect.
        apu.dmc_deliver_sample_byte(read(apu.dmc_sample_address()));
    } else if (cpu_tick && ppu.dma_in_progress() && !dmc_dma_holds_the_bus()) {
        ppu.perform_OAM_DMA_cycle();
    }

    if (cpu_tick) {
        advance_dmc_dma();
    }

    clock_PPU();

    if (cpu_cycle) {
        cpu.sample_interrupts();
    } else if (cpu_tick) {
        // The CPU is halted for DMA, but the /NMI edge detector is not part of
        // its execution unit - stealing bus cycles does not stop it latching an
        // edge. Only the poll (deciding to act on the latch) belongs to a cycle
        // the CPU actually runs. Without this, an /NMI pulse that both fell and
        // rose inside a DMA would be lost entirely.
        cpu.latch_nmi_edge();
    }

    return completed_instruction;
}

void Bus::clock_PPU()
{
    if (total_cycles % 4 == 0) {
        ppu.clock();
    }
}

// Gating lives entirely in clock(). This used to re-test `% 12` and
// dma_in_progress() itself, which meant the CPU tick and its interrupt sample
// were guarded by two copies of the same condition - editing one would have
// desynchronised them silently.
// Runs after the cycle it describes, because two of its transitions depend on
// what that cycle turned out to be: whether the CPU wrote (Halting), and which
// half of the APU clock the next cycle falls on (Dummy).
void Bus::advance_dmc_dma()
{
    switch (dmc_dma) {
    case DmcDma::Idle:
        // The wait for a read cycle happens HERE, before anything is stolen. A
        // halt refused on a write cycle costs the CPU nothing; only once it is
        // accepted does the four-cycle sequence begin.
        if (apu.dmc_wants_sample_byte()) {
            // The halt lands on a specific PHASE, and the 3-or-4 cycle length
            // falls out of it: a load halts on a get, so its dummy lands on a
            // put and the following get needs no alignment; a reload halts on a
            // put, so its dummy lands on a get and one alignment cycle is spent
            // before the read.
            //
            // advance_dmc_dma runs at the END of a cycle and sets the state for
            // the next one, so "the next cycle is a get" is cpu_cycles being
            // odd here.
            const bool next_is_get = (cpu_cycles % 2) != 0;
            if (next_is_get != apu.dmc_transfer_is_load()) {
                break;  // wrong phase - wait for the right one
            }
            // A halt is refused on a write cycle and retried. NESdev's DMA
            // page: "the CPU only allows this on read cycles. If the CPU is
            // writing, it ignores the halt...repeating until successful". That
            // bounds the delay at 3 cycles - a read-modify-write has two
            // consecutive writes, an interrupt has three.
            //
            // A longer comment here used to assert the opposite: that a write
            // makes the DMA cheaper rather than later, and that the rule
            // inverts between loads and reloads. It contradicted both the line
            // below it and the paragraph in bus.h, and it was wrong. What this
            // state machine produces - 3 or 4 cycles standalone, 2 during an
            // OAM DMA - is measured correct, and so is the placement: the sync
            // loops in sprdma_and_dmc_dma run at blargg's designed 433 and 3423
            // cycles, which they only can if the DMA lands where this puts it.
            if (cpu_wrote_this_cycle) {
                break;
            }

            // Half price during an OAM DMA. The halt and dummy cycles exist to
            // stop the CPU and give it time to let go of the bus - and the CPU
            // is already stopped, so there is nothing to halt. Only the access
            // itself collides: "DMA units don't interfere with each other
            // unless they're both trying to access on the same cycle, in which
            // case DMC DMA wins", which costs the OAM DMA 2 cycles, not 4.
            dmc_dma = ppu.dma_in_progress() ? DmcDma::Align : DmcDma::Halt;
        }
        break;

    case DmcDma::Halt:
        dmc_dma = DmcDma::Dummy;
        break;

    case DmcDma::Dummy:
        // "Get and put cycles are aligned to the first and second halves of the
        // APU clock." cpu_cycles is that divider - the same one OAM DMA and the
        // frame counter align to - so its low bit is the phase, and a cycle is
        // spent on alignment only when the next one is not a get.
        dmc_dma = (cpu_cycles % 2 != 0) ? DmcDma::Get : DmcDma::Align;
        break;

    case DmcDma::Align:
        dmc_dma = DmcDma::Get;
        break;

    case DmcDma::Get:
        dmc_dma = DmcDma::Idle;
        break;
    }
}

bool Bus::clock_CPU() { return cpu.clock(trace_cpu); }

// The cap is not defensive padding. Two things genuinely never finish: a jammed
// opcode (KIL/JAM), whose whole purpose is to stop fetching, and a CPU already
// halted for the 513 cycles of an OAM DMA. A single-step that spun on either
// would freeze the UI with no diagnosis at all, which is worse than reporting
// that the CPU is stuck.
//
// 100000 master ticks is ~8300 CPU cycles: far more than the longest real
// instruction plus a DMA that preempted it, and far less than a frame.
bool Bus::step_instruction()
{
    constexpr int give_up_after = 100000;
    for (int i = 0; i < give_up_after; ++i) {
        if (clock()) {
            return true;
        }
    }
    return false;
}

void Bus::run_frame()
{
    // Cannot spin: PPU::advance_dot runs on every tick whatever is enabled, so
    // the frame counter always reaches the next value.
    const uint64_t started_on = ppu.frame;
    do {
        clock();
    } while (ppu.frame == started_on);
}
