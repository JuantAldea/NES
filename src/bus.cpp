#include "../include/bus.h"

Bus::Bus()
    : cpu{std::bind(&Bus::read, this, std::placeholders::_1),
          std::bind(&Bus::write, this, std::placeholders::_1, std::placeholders::_2)},
      apu{this},
      ppu{this},
      ram{this},
      prg_ram{this},
      rom{this}
{
    // Bus::clock places the interrupt sample one PPU dot after the CPU's bus
    // access, which the CPU cannot do for itself.
    cpu.external_interrupt_sampling = true;
    cpu.reset();
}

bool Bus::load_cartridge(const std::string& path) { return rom.load(path); }

// Single address decode used by both read() and write(). This is the only
// place CPU addresses get mapped to a device + effective (mirrored) address,
// so a read and a write to the same CPU address can never disagree about
// where they land.
//
// NES CPU memory map (as relevant here):
//   $0000-$1FFF  2KB internal RAM, mirrored every $0800
//   $2000-$3FFF  8 PPU registers, mirrored every 8 bytes
//   $4000-$4013, $4015  APU registers
//   $4014        PPU OAMDMA
//   $4016-$4017  controllers (stubbed: no device, open bus)
//   $4018-$401F  APU/IO test registers (stubbed: no device, open bus)
//   $4020-$5FFF  cartridge expansion area (stubbed: no device, open bus)
//   $6000-$7FFF  8KB cartridge PRG-RAM
//   $8000-$FFFF  cartridge PRG-ROM
Bus::DecodedAddress Bus::decode(const uint16_t addr)
{
    if (addr < 0x2000) {
        return {&ram, static_cast<uint16_t>(addr % 0x0800)};
    } else if (addr < 0x4000) {
        return {&ppu, static_cast<uint16_t>(0x2000 + (addr % 8))};
    } else if (addr <= 0x4013 || addr == 0x4015) {
        return {&apu, addr};
    } else if (addr == 0x4014) {
        return {&ppu, addr};
    } else if (addr <= 0x401F) {
        // $4016/$4017 controllers and the remaining APU/IO test range are not
        // implemented yet; treat as open bus rather than routing to RAM/APU.
        return {nullptr, addr};
    } else if (addr < 0x6000) {
        // Cartridge expansion area; no device backs it.
        return {nullptr, addr};
    } else if (addr < 0x8000) {
        return {&prg_ram, static_cast<uint16_t>(addr - 0x6000)};
    }

    return {&rom, addr};
}

void Bus::write(const uint16_t addr, const uint8_t data)
{
    const DecodedAddress d = decode(addr);
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
    const DecodedAddress d = decode(addr);
    if (d.device) {
        return d.device->read(d.effective_addr);
    }
    // Open-bus ranges (no device): reads return 0.
    return 0;
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
void Bus::clock()
{
    ++total_cycles;

    const bool cpu_tick = total_cycles % 12 == 0;

    // A CPU cycle is stolen outright while OAM DMA holds the bus.
    const bool cpu_cycle = cpu_tick && !ppu.dma_in_progress();

    if (cpu_cycle) {
        clock_CPU();
    }

    clock_PPU();

    if (cpu_cycle) {
        cpu.sample_interrupts();
    } else if (cpu_tick) {
        // The CPU is halted for DMA, but the /NMI edge detector is not part of
        // its execution unit - stealing bus cycles does not stop it latching an
        // edge. Only the poll (deciding to act on the latch) belongs to a cycle
        // the CPU actually runs. Without this, an /NMI pulse that both fell and
        // rose inside a DMA would be lost entirely, and that window grows
        // threefold once the DMA length is corrected: it currently runs ~171
        // CPU cycles instead of 513.
        cpu.latch_nmi_edge();
    }
}

void Bus::clock_PPU()
{
    if (total_cycles % 4 == 0) {
        ppu.clock();
    }
}

void Bus::clock_CPU()
{
    if (total_cycles % 12 == 0) {
        if (!ppu.dma_in_progress()) {
            cpu.clock(false);
        }
    }
}
