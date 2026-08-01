#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "device.h"

// iNES (.nes) cartridge loader and NROM (mapper 0) memory behavior.
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

    uint8_t mapper_id = 0;
    bool horizontal_mirroring = true;

    std::vector<uint8_t> prg_rom;
    std::vector<uint8_t> chr_rom;
};
