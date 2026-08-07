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
};
