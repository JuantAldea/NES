#include <fstream>
#include <iostream>

#include "../include/rom.h"

namespace
{
constexpr size_t INES_HEADER_SIZE = 16;
constexpr size_t TRAINER_SIZE = 512;
constexpr size_t PRG_ROM_BANK_SIZE = 16 * 1024;
constexpr size_t CHR_ROM_BANK_SIZE = 8 * 1024;
constexpr uint8_t INES_MAGIC[4] = {'N', 'E', 'S', 0x1A};
}  // namespace

bool ROM::load(const std::string& path)
{
    // Reset any previously loaded cartridge up front so a failed load never
    // leaves a half-loaded (and therefore misleadingly "loaded()") cartridge.
    prg_rom.clear();
    chr_rom.clear();
    mapper_id = 0;
    horizontal_mirroring = true;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "ROM: could not open '" << path << "'\n";
        return false;
    }

    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size < static_cast<std::streamsize>(INES_HEADER_SIZE)) {
        std::cerr << "ROM: file too small to contain an iNES header\n";
        return false;
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        std::cerr << "ROM: short read while loading '" << path << "'\n";
        return false;
    }

    if (data[0] != INES_MAGIC[0] || data[1] != INES_MAGIC[1] || data[2] != INES_MAGIC[2] || data[3] != INES_MAGIC[3]) {
        std::cerr << "ROM: missing iNES magic number\n";
        return false;
    }

    const size_t prg_rom_banks = data[4];
    const size_t chr_rom_banks = data[5];
    const uint8_t flags6 = data[6];
    const uint8_t flags7 = data[7];

    const bool has_trainer = flags6 & 0x04;
    // Mapper id: low nibble comes from the high nibble of flags6, high nibble
    // from the high nibble of flags7.
    const uint8_t parsed_mapper_id = (flags7 & 0xF0) | (flags6 >> 4);

    if (parsed_mapper_id != 0) {
        std::cerr << "ROM: mapper " << static_cast<int>(parsed_mapper_id) << " is not supported (only NROM/mapper 0)\n";
        return false;
    }

    // NROM carries either one 16KB PRG bank (mirrored across $8000-$FFFF) or
    // two (filling it). Anything else is not NROM, however byte 4 reads: the
    // CPU can only address 32KB of cartridge space, so a larger image would
    // load with most of it permanently unreachable and no error.
    if (prg_rom_banks == 0 || prg_rom_banks > 2) {
        std::cerr << "ROM: NROM requires 1 or 2 PRG-ROM banks, header advertises " << prg_rom_banks << "\n";
        return false;
    }

    size_t offset = INES_HEADER_SIZE;
    if (has_trainer) {
        offset += TRAINER_SIZE;
    }

    const size_t prg_size = prg_rom_banks * PRG_ROM_BANK_SIZE;
    const size_t chr_size = chr_rom_banks * CHR_ROM_BANK_SIZE;

    if (data.size() < offset + prg_size + chr_size) {
        std::cerr << "ROM: file is smaller than advertised by its header (truncated)\n";
        return false;
    }

    mapper_id = parsed_mapper_id;
    horizontal_mirroring = !(flags6 & 0x01);

    prg_rom.assign(data.begin() + offset, data.begin() + offset + prg_size);
    offset += prg_size;

    // chr_rom_banks == 0 means the cartridge uses CHR-RAM instead; leave chr_rom empty in that case.
    chr_rom.assign(data.begin() + offset, data.begin() + offset + chr_size);

    return true;
}

void ROM::write(const uint16_t addr, const uint8_t data)
{
    // NROM PRG-ROM is not writable.
}

uint8_t ROM::read(const uint16_t addr)
{
    if (prg_rom.empty()) {
        return 0;
    }

    // NROM: 16KB PRG-ROM images are mirrored across $8000-$BFFF and $C000-$FFFF;
    // 32KB images fill $8000-$FFFF directly. The modulo handles both cases since
    // prg_rom.size() is either 16384 or 32768 (a multiple of the bank size).
    return prg_rom[(addr - 0x8000) % prg_rom.size()];
}
