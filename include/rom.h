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

    std::vector<uint8_t> prg_rom;
    std::vector<uint8_t> chr_rom;
};
