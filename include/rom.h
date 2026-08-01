#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "device.h"

class ROM : public Device
{
public:
    ROM(Bus* b) : Device{b} {};

    // Parses an iNES (.nes) file and loads it as an NROM (mapper 0) cartridge.
    // Returns false (and leaves the cartridge unloaded) on any parse error or
    // unsupported mapper.
    bool load(const std::string& path);

    bool loaded() const { return !prg_rom.empty(); }

    void write(const uint16_t addr, const uint8_t data);
    uint8_t read(const uint16_t addr);

    uint8_t mapper_id = 0;
    bool horizontal_mirroring = true;

    std::vector<uint8_t> prg_rom;
    std::vector<uint8_t> chr_rom;
};
