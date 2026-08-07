#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "device.h"

// iNES (.nes) cartridge loader for NROM (mapper 0), UNROM (mapper 2) and
// CNROM (mapper 3).
//
// CNROM is NROM plus a switchable CHR window: a write anywhere in
// $8000-$FFFF latches which 8KB CHR-ROM bank the PPU sees. PRG behaves
// exactly as NROM's does, so the two share everything but chr_read().
//
// iNES header layout (16 bytes):
//   0-3  magic "NES\x1A"
//   4    PRG-ROM size in 16KB units
//   5    CHR-ROM size in 8KB units (0 means CHR-RAM, not handled here)
//   6    flags6: bit0 mirroring, bit2 trainer present, bits4-7 mapper low nibble
//   7    flags7: bits4-7 mapper high nibble
//   8-15 (unused by this loader)
// followed by a 512-byte trainer if flags6 bit2 is set, then PRG-ROM, then CHR-ROM.
class ROM : public Device
{
public:
    ROM(Bus* b) : Device{b} {};

    // Parses an iNES (.nes) file and loads it as an NROM (mapper 0) cartridge.
    // Returns false (and leaves the cartridge unloaded) on any parse error or
    // unsupported mapper. A failed load never leaves prg_rom/chr_rom partially
    // populated: on any error path they are left (or reset to) empty.
    bool load(const std::string& path);

    bool loaded() const { return !prg_rom.empty(); }

    void write(const uint16_t addr, const uint8_t data);
    uint8_t read(const uint16_t addr);

    bool has_chr_rom() const { return !chr_rom.empty(); }

    // Reads pattern-table space ($0000-$1FFF) through the currently selected
    // CHR bank. The PPU must go through this rather than indexing chr_rom
    // directly, or a CNROM cartridge would be stuck on bank 0 forever.
    uint8_t chr_read(const uint16_t addr) const;

    uint8_t mapper_id = 0;
    bool horizontal_mirroring = true;

    // Which 8KB CHR-ROM bank the PPU currently sees, and how many exist.
    // NROM always has at most one, so both stay 0/1 and chr_read() reduces to
    // a plain index.
    uint8_t chr_bank = 0;
    uint8_t chr_bank_count = 0;

    // UNROM's switchable 16KB PRG window at $8000-$BFFF, and how many banks the
    // cartridge carries. $C000-$FFFF is always the last bank, so the vectors
    // cannot be switched away.
    uint8_t prg_bank = 0;
    uint8_t prg_bank_count = 0;

    // --- MMC3 (mapper 4) ----------------------------------------------------
    //
    // Eight bank registers behind a select/data pair, rather than a single
    // latch. $8000 chooses which of R0-R7 the next $8001 write lands in, and
    // also carries the two mode bits that decide WHICH window each register
    // drives:
    //
    //   R0, R1   2KB CHR banks   (bit 0 ignored - "R0 and R1 ignore the bottom bit")
    //   R2-R5    1KB CHR banks
    //   R6, R7   8KB PRG banks   (top two bits ignored - only 6 PRG address lines)
    //
    // The two mode bits invert which half of the address space the switchable
    // windows occupy, which is why they are kept rather than folded in at write
    // time: a later mode change has to re-point the existing register values.
    uint8_t mmc3_bank_select = 0;   // the last value written to $8000
    uint8_t mmc3_bank[8] = {0};     // R0-R7

    // $8000 bit 6: 0 -> $8000-$9FFF switchable, $C000-$DFFF fixed to second-last
    //              1 -> $C000-$DFFF switchable, $8000-$9FFF fixed to second-last
    bool mmc3_prg_mode_swapped() const { return (mmc3_bank_select & 0x40) != 0; }

    // $8000 bit 7: swaps the 2KB and 1KB CHR windows between $0000 and $1000.
    bool mmc3_chr_a12_inverted() const { return (mmc3_bank_select & 0x80) != 0; }

    // $A001: bit 7 enables the PRG-RAM chip, bit 6 write-protects it. Held here
    // because the mapper owns them, but PrgRAM is a separate Bus device, so the
    // Bus is what has to consult these.
    bool prg_ram_enabled = true;
    bool prg_ram_write_protected = false;

    // --- MMC3 scanline IRQ counter ------------------------------------------
    //
    // The MMC3 has no idea what a scanline is. It counts RISING EDGES OF PPU
    // ADDRESS LINE A12, and a scanline happens to produce exactly one of them
    // in the usual configuration: the background fetches sit in one pattern
    // table and the sprite fetches in the other, so A12 goes up once per line
    // when the fetch phase crosses over. Games get "interrupt at scanline N" by
    // counting those edges - which is why a game that puts both tables on the
    // same side gets no IRQs at all, and why the counter is clocked just as
    // happily by a program poking $2006 with rendering switched off.
    //
    // blargg's readme for the IRQ ROMs is explicit about that second case:
    // "The ROMs mainly test behavior by manually clocking the MMC3's IRQ
    // counter by writing to $2006 to change the current VRAM address." A design
    // that hooked only the rendering fetches would pass none of them.
    uint8_t mmc3_irq_latch = 0;    // $C000, the value reloaded into the counter
    uint8_t mmc3_irq_counter = 0;  // the live count
    bool mmc3_irq_reload_pending = false;  // set by $C001, consumed by the next edge
    bool mmc3_irq_enabled = false;         // $E001 enables, $E000 disables
    bool mmc3_irq_asserted = false;        // are we currently pulling /IRQ low?

    // The A12 line as the mapper last saw it, and the PPU dot at which it went
    // low. Only the duration matters, so one timestamp is enough.
    bool mmc3_a12_high = false;
    uint64_t mmc3_a12_low_since = 0;

    // A rising edge only counts if A12 has been low for a while first. NESdev:
    // the counter is "triggered on a rising edge after the line has remained
    // low for three falling edges of M2" - M2 is the CPU clock, so three of its
    // cycles, and the PPU runs three times faster, giving nine PPU dots.
    //
    // The filter is not an optimisation, it is what makes the count mean
    // "scanline". Within one background tile fetch A12 drops for exactly four
    // dots (the nametable and attribute reads at $2xxx, between two pattern
    // reads at $1xxx); without the filter every tile would clock the counter
    // and an IRQ meant for one scanline would arrive 32 times a line.
    //
    // Nine rather than four-plus-one because the margin has to survive both
    // sides: real clocks follow low periods of 16 dots and up, so there is a
    // wide gap between "noise" and "signal" and the threshold only has to land
    // inside it. 3-A12_clocking is what pins this - see mmc3_rom_tests.cpp.
    static constexpr uint64_t mmc3_a12_filter_dots = 9;

    // Called by the PPU on every access that drives its EXTERNAL address bus,
    // and on the $2006 write that commits a new address to v. `ppu_cycle` is
    // PPU::total_cycles, which is the only clock both sides share.
    void mmc3_observe_a12(const uint16_t ppu_addr, const uint64_t ppu_cycle);

    // How many 8KB PRG banks and 1KB CHR banks the cartridge carries. MMC3
    // indexes in those units, unlike UNROM's 16KB and CNROM's 8KB.
    uint16_t prg_8k_bank_count = 0;
    uint16_t chr_1k_bank_count = 0;

    std::vector<uint8_t> prg_rom;
    std::vector<uint8_t> chr_rom;

private:
    // Which 1KB CHR bank the given PPU address currently reads through, and
    // which 8KB PRG bank the given CPU address does. Split out because both are
    // pure functions of the register file and the mode bits, and are far easier
    // to test and to reason about than the same arithmetic inlined into
    // chr_read()/read().
    uint16_t mmc3_chr_bank_for(const uint16_t ppu_addr) const;
    uint16_t mmc3_prg_bank_for(const uint16_t cpu_addr) const;

    // One filtered rising edge of A12: reload or decrement, then decide whether
    // to pull /IRQ low. Split out from mmc3_observe_a12 so the edge DETECTION
    // and the counter SEMANTICS can be read - and mutated - independently.
    void mmc3_clock_irq_counter();

    // Pushes mmc3_irq_asserted onto the CPU's cartridge /IRQ bit. Every path
    // that can change the assertion goes through here, so there is one place
    // where the wire is driven rather than four.
    void mmc3_update_irq_line();
};
