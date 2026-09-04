#include "../include/bus.h"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>

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

// A cartridge's save file: Zelda.nes -> Zelda.sav, beside the ROM.
//
// Replaces a .nes extension rather than appending to it, which is what every
// other emulator does and therefore what an existing save is already called.
// Anything else gets .sav appended, because a path with no extension - or with
// someone else's - has no part this is entitled to throw away.
std::string Bus::battery_save_path(const std::string& rom_path)
{
    const size_t dot = rom_path.find_last_of('.');
    const size_t slash = rom_path.find_last_of("/\\");

    // A dot before the last separator belongs to a DIRECTORY name, not to the
    // file - "/home/a.b/rom" must not become "/home/a.sav".
    const bool has_extension = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    if (has_extension && rom_path.compare(dot, std::string::npos, ".nes") == 0) {
        return rom_path.substr(0, dot) + ".sav";
    }
    return rom_path + ".sav";
}

bool Bus::load_cartridge(const std::string& path)
{
    // The outgoing cartridge first - see the header. Its result is deliberately
    // not consulted: failing to save game A is not a reason to refuse to load
    // game B, and save_battery_ram has already complained on stderr.
    save_battery_ram();

    // Work RAM does NOT survive a cartridge swap, and used to. Nothing cleared
    // it, so game B booted seeing game A's save data at $6000 - harmless while
    // nothing wrote it to disk, and silent corruption of B's .sav the moment
    // something did. A cartridge swap is a power cycle for everything on the
    // cartridge, which the work RAM is.
    prg_ram.memory.fill(0);
    prg_ram_dirty = false;
    cartridge_path.clear();

    // Not cleared here: clearing the ring from this thread would race a
    // running audio callback. See Bus::audio_reset_pending.
    audio_reset_pending = true;

    if (!rom.load(path)) {
        return false;
    }
    cartridge_path = path;

    if (rom.prg_nvram_size == 0) {
        return true;
    }

    // Restore. A missing file is the normal case for a game's first run, so it
    // is not reported; a short one is restored as far as it goes, because a
    // truncated save is still worth more to a player than none.
    std::ifstream save(battery_save_path(path), std::ios::binary);
    if (!save) {
        return true;
    }
    const size_t want = std::min<size_t>(rom.prg_nvram_size, PrgRAM::SIZE);
    save.read(reinterpret_cast<char*>(prg_ram.memory.data()), static_cast<std::streamsize>(want));

    return true;
}

bool Bus::save_battery_ram()
{
    if (cartridge_path.empty() || rom.prg_nvram_size == 0 || !prg_ram_dirty) {
        return true;
    }

    // WHICH BYTES ARE BATTERY-BACKED is only a question when a header declares
    // both kinds, which NES 2.0 byte 10 can do and no board implemented here
    // does. ROM::prg_ram_offset folds both into one flat array and nothing says
    // where the join is, so this takes the low end and says so rather than
    // inventing a split the format does not describe. Revisit with a board that
    // actually carries both.
    const size_t bytes = std::min<size_t>(rom.prg_nvram_size, PrgRAM::SIZE);

    const std::string path = battery_save_path(cartridge_path);
    std::ofstream save(path, std::ios::binary | std::ios::trunc);
    if (!save) {
        std::cerr << "could not open '" << path << "' to save battery RAM\n";
        return false;
    }
    save.write(reinterpret_cast<const char*>(prg_ram.memory.data()), static_cast<std::streamsize>(bytes));
    if (!save) {
        std::cerr << "could not write battery RAM to '" << path << "'\n";
        return false;
    }

    prg_ram_dirty = false;
    return true;
}

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
        // Before any of the work-RAM question: the window may not be work RAM
        // at all. The FME-7 can map an 8KB PRG-ROM bank here, which no other
        // board in this emulator can, and until it could the enable/protect
        // pair below was the whole story for $6000-$7FFF.
        //
        // Routing to the cartridge means a WRITE here now reaches
        // Mapper::cpu_write too, which was previously $8000-$FFFF only. That is
        // correct - writing to ROM does nothing - but it is a widened contract,
        // so cpu_write implementations must not assume the top bit is set. Only
        // the FME-7 can reach this line today and it decodes on addr & $E000,
        // where $6000 falls through to its default.
        if (rom.prg_rom_at_6000) {
            return {&rom, addr};
        }
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
        // Nothing soldered to the board at all is a different thing from a chip
        // the mapper has switched off, and both decode to no device. Only an
        // NES 2.0 header can say "no work RAM"; an iNES 1.0 image cannot
        // express it and load() gives those 8KB, so every cartridge that
        // worked before this check existed still gets its window.
        if (!rom.has_prg_ram() || !rom.prg_ram_enabled) {
            return {nullptr, addr};
        }
        if (access == Access::Write && rom.prg_ram_write_protected) {
            return {nullptr, addr};
        }
        // Not addr - 0x6000: SOROM and SXROM carry more work RAM than the
        // window shows and page it with MMC1 CHR register bits 2-3, so where
        // the byte lives is the cartridge's answer to give, not the Bus's.
        return {&prg_ram, rom.prg_ram_offset(addr)};
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
        // Every route to the work RAM passes through here, so this is the one
        // place that can answer "has this cartridge been written to". Compared
        // against the pointer rather than the address range because decode() has
        // already resolved the mapper's enable and write-protect bits: a write
        // the board swallowed did not reach the chip and must not count.
        if (d.device == &prg_ram) {
            prg_ram_dirty = true;
        }
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
    // Same gate as cpu_wrote_this_cycle, and for the same reason: Bus::read is
    // public, so the cartridge loader, the tests and the debugger all reach it
    // and none of them is a CPU read cycle. A DMA's no-op cycles repeat this
    // address, so letting a debugger peek set it would make the phantom read
    // depend on whether anyone was looking.
    if (watching_cpu_access) {
        last_cpu_read_addr = addr;
    }

    const DecodedAddress d = decode(addr, Access::Read);

    // See controller_read_is_continuation: /OE clocks once per contiguous set.
    const bool is_controller = d.device == &controllers;
    controller_read_is_continuation = is_controller && prev_bus_read_was_controller;
    prev_bus_read_was_controller = is_controller;

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

        // AFTER apu.clock(), not before: the sample belongs to the state the
        // cycle produced, and a transition recorded a cycle early would put
        // every edge in the stream one cycle ahead of the machine that made it.
        if (audio_enabled) {
            audio.push(apu.mixer_output());
        }
        // Sunsoft's FME-7 counts M2 cycles rather than PPU A12 edges, so it is
        // the only board here that needs the CPU clock at all. Gated on a flag
        // rather than always dispatching, because this is the hottest line in
        // the emulator and the flag is false for whatever else is plugged in.
        rom.clock_cpu_cycle();
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
        // Rather than thread a flag through every operation and addressing
        // mode, this observes the one place every access already passes
        // through. The CPU is cycle-stepped at exactly one bus access per
        // cycle, so whether Bus::write was reached during clock_CPU() is the
        // whole answer.
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
        dmc_fetch_cycle = cpu_cycles;
    } else if (cpu_tick && ppu.dma_in_progress() && !dmc_dma_holds_the_bus()) {
        // The OAM DMA's leading halt, and its alignment cycle when it has one,
        // are no-operation cycles like the DMC's: perform_OAM_DMA_cycle returns
        // without touching the bus on them. It decrements first and then tests
        // >= 512, so 513 and above are the ones that steal a cycle without using
        // it, and the CPU repeats its access through them exactly as it does for
        // a DMC halt.
        //
        // NO ROM HERE REACHES IT, which is a property of the ROMs. An OAM DMA
        // starts from `sta $4014`, so the CPU is at an instruction boundary and
        // the access it repeats is the next opcode fetch: instrumented across
        // the suite, 2000 repeats and NONE in $2000-$401F, where one would be
        // visible. The oracle is a unit test that puts the fetch on $2007 and
        // counts VRAM address steps - see
        // oamDmaTiming.the_no_operation_cycles_repeat_the_cpus_opcode_fetch.
        const bool no_operation_cycle = ppu.remaining_dma_cycles >= 513;
        ppu.perform_OAM_DMA_cycle();
        if (no_operation_cycle) {
            repeat_halted_cpu_access();
        }
    } else if (cpu_tick && dmc_dma_holds_the_bus()) {
        // PHANTOM READS. The halt, the dummy and the optional alignment steal
        // the bus without using it, and the 6502 does not idle through them.
        // NESdev's DMA page: "When RDY is deasserted, the 6502 core repeats the
        // last read cycle indefinitely, making no forward progress nor handling
        // interrupts. On 2A03 CPUs, these repeated reads are externally visible
        // on any no-operation DMA cycle, causing data loss if reading a register
        // with side effects."
        //
        // It is the access the CPU is ATTEMPTING that repeats, not the last one
        // it completed. Measured in Mesen with tools/mesen_2007_trace.cpp: a
        // stall landing after a $2007 read leaves the read buffer and the VRAM
        // address untouched for its whole length, because by then the CPU is
        // waiting on an opcode fetch and the repeats land in ROM. A stall
        // landing on the read gives three reads of $2007, and the CPU keeps the
        // LAST.
        //
        // Reading last_cpu_read_addr gets both of those wrong at once - it
        // re-reads $2007 in the rows hardware does not touch it, and it keeps
        // the first value in the row where it does.
        //
        // So the CPU is asked rather than predicted: run it for one cycle and
        // un-run it. The access goes out on the bus with its side effects, the
        // CPU makes no forward progress, and it attempts the same access again
        // next cycle - which is what "repeats the last read cycle indefinitely"
        // describes. Predicting the address instead would mean a second model of
        // every addressing mode's arithmetic, since it comes from PC and
        // fetched_operand rather than from (Schedule, cycle) the way
        // cycle_writes does.
        //
        // This is deliberately a real read() with its side effects: that IS the
        // behaviour under test. blargg's dmc_dma_during_read4 measures exactly
        // the damage - $2007 swapping the buffer and stepping the VRAM address,
        // $2002 clearing vblank, $4015 acknowledging.
        //
        // dmc_dma_holds_the_bus() here is REDUNDANT AND KEPT, an equivalent
        // mutant rather than a hole. cpu_cycle is `cpu_tick && !(OAM || DMC)`,
        // so reaching this last branch with cpu_tick set already implies a DMA
        // holds the bus, and the two branches above have taken the DMC's get
        // cycle and every OAM cycle the DMC is not overriding. What is left is
        // exactly the DMC's no-op cycles. It stays because it names the
        // condition the block is about.
        repeat_halted_cpu_access();
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
// NESdev's DMA page: "When RDY is deasserted, the 6502 core repeats the last read
// cycle indefinitely, making no forward progress nor handling interrupts. On 2A03
// CPUs, these repeated reads are externally visible on any no-operation DMA
// cycle, causing data loss if reading a register with side effects."
//
// It is the access the CPU is ATTEMPTING that repeats, not the last one it
// completed - measured in Mesen with tools/mesen_2007_trace.cpp, where a stall
// landing after a $2007 read leaves the read buffer untouched for its whole
// length because by then the CPU is waiting on an opcode fetch. So the CPU is run
// and un-run rather than predicted: the access goes out with its side effects,
// the CPU makes no forward progress, and it attempts the same access again next
// cycle.
void Bus::repeat_halted_cpu_access()
{
    // Never a write: a halt is only accepted when the cycle it lands on is a
    // read, and an OAM DMA begins after `sta $4014`'s write has completed, so
    // the CPU is at an instruction boundary facing an opcode fetch. Asserted
    // rather than assumed, because repeating a WRITE would corrupt memory
    // silently.
    assert(!cpu.next_cycle_is_write());

    const CPU before_repeat = cpu;
    clock_CPU();
    cpu = before_repeat;
}

void Bus::advance_dmc_dma()
{
    switch (dmc_dma) {
    case DmcDma::Idle:
        // The wait for a read cycle happens HERE, before anything is stolen. A
        // halt refused on a write cycle costs the CPU nothing; only once it is
        // accepted does the four-cycle sequence begin.
        if (!apu.dmc_wants_sample_byte()) {
            // No request, so nothing is deferred. Without this the refusal latch
            // outlives the request that set it: a $4015 DISABLE landing inside
            // the 1-3 cycle refusal window clears transfer_requested from the
            // APU side (the disable_delay path in clock_dmc) without the halt
            // ever being accepted here, and the next request - possibly a load,
            // possibly seconds later - would then skip the phase wait and take
            // the wrong length.
            //
            // LATENT, NOT OBSERVED: instrumented across the whole test binary,
            // that sequence happens 0 times, so no oracle here covers it. It is
            // reachable all the same - nothing stops a game disabling the DMC on
            // the cycle after a refused halt - and the guard costs one branch.
            // tests/bus_write_cycle_tests.cpp pins the invariant instead.
            halt_refused_for_write = false;
            break;
        }
        {
            // The halt lands on a specific PHASE, and the 3-or-4 cycle length
            // falls out of it: a load halts on a get, so its dummy lands on a
            // put and the following get needs no alignment; a reload halts on a
            // put, so its dummy lands on a get and one alignment cycle is spent
            // before the read.
            //
            // advance_dmc_dma runs at the END of a cycle and sets the state for
            // the next one, so "the next cycle is a get" is cpu_cycles being
            // odd here.
            //
            // The placement is measured, not just the length: the sync loops in
            // sprdma_and_dmc_dma run at blargg's designed 433 and 3423 cycles,
            // which they only can if the DMA lands where this puts it.
            //
            // Skipped once a write has already deferred this halt: the two
            // waits are the same one-cycle deferral expressed twice, and
            // serving both delays the halt twice for one cause. Serving both
            // makes rows 0A and 0B of sprdma_and_dmc_dma_512 read 527 and 528
            // against hardware's 526 and 527, and leaves the other fourteen
            // rows correct.
            //
            // THE GATE CANNOT SIMPLY BE DELETED INSTEAD. It is load-bearing
            // three times over: it picks the DMA's
            // length, it decides which cycles the write refusal below is even
            // reached on, and it is what puts acceptance on the ODD
            // remaining_dma_cycles values that the collision costs further down
            // are calibrated against. Removing it moves every row of both ROMs
            // by +1 - all 32, uniformly - where a pure timing shift would be
            // phase-dependent. A uniform shift is the signature of the
            // calibration moving, not of a delay.
            const bool next_is_get = (cpu_cycles % 2) != 0;
            if (!halt_refused_for_write && next_is_get != apu.dmc_transfer_is_load()) {
                break;  // wrong phase - wait for the right one
            }
            // A halt is refused on a write cycle and retried. NESdev's DMA
            // page: "the CPU only allows this on read cycles. If the CPU is
            // writing, it ignores the halt...repeating until successful". That
            // bounds the delay at 3 cycles - a read-modify-write has two
            // consecutive writes, an interrupt has three.
            //
            // Mesen2 enforces this structurally rather than with a test:
            // ProcessPendingDma is called only from NesCpu::MemoryRead, so a
            // halt cannot fire on a write cycle at all.
            //
            // "A WRITE MAKES THE DMA CHEAPER" IS A TRUE OBSERVATION WITH THE
            // WRONG CAUSE, and reading it as an alternative to this deferral is
            // what sent several attempts at the DMC stall wrong. Get and put
            // alternate, so deferring the halt flips which phase the dummy cycle
            // lands on, which is what decides whether the alignment cycle is
            // spent. Same page: "load DMAs take 3 cycles and reload DMAs take 4
            // unless the halt is delayed by an odd number of cycles". The 3-vs-4
            // outcome falls out of the deferral; it does not replace it.
            //
            // THE CYCLE THE HALT WOULD LAND ON, not the one that just ended.
            // This tested cpu_wrote_this_cycle, which is the wrong cycle: the
            // halt runs on the NEXT one, and asking about the previous is only
            // ever right when consecutive cycles happen to match. Measured on
            // sprdma_and_dmc_dma_512 row 0A, where the decision is taken at the
            // end of a read and the halt then lands on `sta $100`'s write - a
            // cycle hardware refuses, and refusing it is what makes such a DMA
            // cost 3 rather than 4, per blargg's own dma_timing.inc.
            //
            // Guarded on the OAM DMA because the CPU is frozen through one, so
            // its schedule and cycle describe an instruction that is not
            // running and the next cycle is not the CPU's to write.
            //
            // THE GUARD IS UNREACHABLE TODAY, AND THAT IS WHY IT IS ASSERTED
            // RATHER THAN DELETED. An OAM DMA begins on the cycle after `sta
            // $4014`'s write COMPLETES, so the CPU freezes at an instruction
            // boundary and its schedule points at the next opcode fetch, which
            // is a read. Instrumented across the whole test binary: the two
            // conditions are never true together, 0 hits. Dropping the clause
            // therefore survives mutation, which is an equivalent mutant and
            // not a hole. The assert is what makes an OAM DMA that ever starts
            // mid-instruction surface here instead of silently changing every
            // DMC collision cost.
            assert(!(ppu.dma_in_progress() && cpu.next_cycle_is_write()));
            if (!ppu.dma_in_progress() && cpu.next_cycle_is_write()) {
                halt_refused_for_write = true;
                break;
            }
            halt_refused_for_write = false;

            // Half price during an OAM DMA. The halt and dummy cycles exist to
            // stop the CPU and give it time to let go of the bus - and the CPU
            // is already stopped, so there is nothing to halt. Only the access
            // itself collides: "DMA units don't interfere with each other
            // unless they're both trying to access on the same cycle, in which
            // case DMC DMA wins", which costs the OAM DMA 2 cycles, not 4.
            //
            // AND 2 ONLY IN THE MIDDLE. NESdev and AprNes both give 1 at the
            // second-to-last put and 3 at the last, where the DMC's read extends
            // past the end of the transfer. remaining_dma_cycles counts down, so
            // 1 IS the transfer's last cycle and 3 is the write of the
            // second-to-last read/write pair - the two positions the rule names.
            //
            // Measured, on sprdma_and_dmc_dma_512, which is the only ROM here
            // that sweeps the DMC through the tail: acceptance lands on rem 7, 5,
            // 3 and 1 at rows 00-07 and on 0 for the other 325 fetches in the
            // run, and Mesen appends exactly 1 cycle after the transfer's last
            // OAM write at rows 04-05 and exactly 3 at rows 06-07 while appending
            // none at the other twelve rows. A flat 2 is wrong at both ends.
            if (!ppu.dma_in_progress()) {
                dmc_dma = DmcDma::Halt;
            } else if (ppu.remaining_dma_cycles == 3) {
                dmc_dma = DmcDma::Get;  // the second-to-last put: 1 cycle
            } else if (ppu.remaining_dma_cycles == 1) {
                dmc_dma = DmcDma::Extend;  // the last: 3 cycles
            } else {
                dmc_dma = DmcDma::Align;  // the middle: 2 cycles
            }
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

    case DmcDma::Extend:
        dmc_dma = DmcDma::Align;
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
