#include "rom.h"

#include <fstream>
#include <iostream>

namespace
{
constexpr size_t INES_HEADER_SIZE = 16;
constexpr size_t TRAINER_SIZE = 512;
constexpr size_t PRG_ROM_BANK_SIZE = 16 * 1024;
constexpr size_t CHR_ROM_BANK_SIZE = 8 * 1024;
}  // namespace

bool ROM::load(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "ROM: could not open '" << path << "'" << std::endl;
        return false;
    }

    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size < static_cast<std::streamsize>(INES_HEADER_SIZE)) {
        std::cerr << "ROM: file too small to contain an iNES header" << std::endl;
        return false;
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);

    if (!(data[0] == 'N' && data[1] == 'E' && data[2] == 'S' && data[3] == 0x1A)) {
        std::cerr << "ROM: missing iNES magic number" << std::endl;
        return false;
    }

    const size_t prg_rom_banks = data[4];
    const size_t chr_rom_banks = data[5];
    const uint8_t flags6 = data[6];
    const uint8_t flags7 = data[7];

    const bool has_trainer = flags6 & 0x04;
    const uint8_t parsed_mapper_id = (flags7 & 0xF0) | (flags6 >> 4);

    if (parsed_mapper_id != 0) {
        std::cerr << "ROM: mapper " << static_cast<int>(parsed_mapper_id) << " is not supported (only NROM/mapper 0)"
                   << std::endl;
        return false;
    }

    size_t offset = INES_HEADER_SIZE;
    if (has_trainer) {
        offset += TRAINER_SIZE;
    }

    const size_t prg_size = prg_rom_banks * PRG_ROM_BANK_SIZE;
    const size_t chr_size = chr_rom_banks * CHR_ROM_BANK_SIZE;

    if (data.size() < offset + prg_size + chr_size) {
        std::cerr << "ROM: file is smaller than advertised by its header" << std::endl;
        return false;
    }

    if (prg_size == 0) {
        std::cerr << "ROM: header advertises zero PRG-ROM banks" << std::endl;
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

    // NROM: 16 KiB PRG-ROM images are mirrored across $8000-$BFFF and $C000-$FFFF;
    // 32 KiB images fill $8000-$FFFF directly. The modulo handles both cases.
    return prg_rom[(addr - 0x8000) % prg_rom.size()];
}
