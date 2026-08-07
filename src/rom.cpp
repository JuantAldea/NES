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
    chr_bank = 0;
    chr_bank_count = 0;

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

    if (parsed_mapper_id != 0 && parsed_mapper_id != 2 && parsed_mapper_id != 3) {
        std::cerr << "ROM: mapper " << static_cast<int>(parsed_mapper_id)
                  << " is not supported (only NROM/0, UNROM/2 and CNROM/3)\n";
        return false;
    }

    // UNROM switches PRG rather than CHR, so it carries no CHR-ROM at all - the
    // console's CHR-RAM is the pattern memory. A header claiming CHR banks is
    // not a UNROM image whatever byte 7 says.
    if (parsed_mapper_id == 2 && chr_rom_banks != 0) {
        std::cerr << "ROM: UNROM has no CHR-ROM, header advertises " << chr_rom_banks << " bank(s)\n";
        return false;
    }

    // NROM carries either one 16KB PRG bank (mirrored across $8000-$FFFF) or
    // two (filling it). Anything else is not NROM, however byte 4 reads: the
    // CPU can only address 32KB of cartridge space, so a larger image would
    // load with most of it permanently unreachable and no error.
    // NROM carries at most one 8KB CHR bank. A header claiming more would load
    // happily with the excess permanently unreachable, which is the same
    // failure the PRG check below exists to prevent.
    // NROM has a single fixed CHR bank; CNROM is the mapper that exists in
    // order to have several. A CNROM image with one bank is legal and just
    // never switches.
    if (parsed_mapper_id == 0 && chr_rom_banks > 1) {
        std::cerr << "ROM: NROM supports at most 1 CHR-ROM bank, header advertises " << chr_rom_banks << "\n";
        return false;
    }

    // The bank register is masked with the bank count, so a count that is not
    // a power of two would make the masking ambiguous. Real CNROM boards carry
    // 1, 2 or 4 banks (8KB, 16KB, 32KB of CHR).
    if (parsed_mapper_id == 3 && (chr_rom_banks == 0 || chr_rom_banks > 4 || (chr_rom_banks & (chr_rom_banks - 1)) != 0)) {
        std::cerr << "ROM: CNROM requires 1, 2 or 4 CHR-ROM banks, header advertises " << chr_rom_banks << "\n";
        return false;
    }

    // UNROM is the mapper that exists to carry more PRG than the CPU can
    // address, so the NROM ceiling of two banks does not apply. Eight or
    // sixteen 16KB banks (128KB or 256KB) are the real board sizes.
    if (parsed_mapper_id == 2) {
        if (prg_rom_banks < 2 || prg_rom_banks > 16) {
            std::cerr << "ROM: UNROM requires 2 to 16 PRG-ROM banks, header advertises " << prg_rom_banks << "\n";
            return false;
        }
    } else if (prg_rom_banks == 0 || prg_rom_banks > 2) {
        std::cerr << "ROM: NROM/CNROM require 1 or 2 PRG-ROM banks, header advertises " << prg_rom_banks << "\n";
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
    chr_bank_count = static_cast<uint8_t>(chr_rom_banks);
    prg_bank_count = static_cast<uint8_t>(prg_rom_banks);
    prg_bank = 0;
    // Power-on bank is 0. The value is not specified by the hardware, but a
    // CNROM game sets it before drawing anything.
    chr_bank = 0;

    prg_rom.assign(data.begin() + offset, data.begin() + offset + prg_size);
    offset += prg_size;

    // chr_rom_banks == 0 means the cartridge uses CHR-RAM instead; leave chr_rom empty in that case.
    chr_rom.assign(data.begin() + offset, data.begin() + offset + chr_size);

    return true;
}

void ROM::write(const uint16_t addr, const uint8_t data)
{
    // NROM PRG-ROM is not writable, and neither is CNROM's - but on CNROM the
    // write is not discarded either: the cartridge latches the low bits as the
    // CHR bank number. Any address in $8000-$FFFF selects, since the board
    // decodes nothing finer than "the CPU wrote to cartridge space".
    //
    // Bus conflicts are NOT modelled. Real CNROM drives the ROM's own byte
    // onto the bus at the same time, so hardware sees (data & prg_byte).
    // Cartridges are written to avoid that - they store the bank number at the
    // address they write to - so the AND is a no-op for correct software, and
    // leaving it out cannot mask a bug in anything being tested here.
    if (mapper_id == 3 && addr >= 0x8000 && chr_bank_count != 0) {
        chr_bank = static_cast<uint8_t>(data & (chr_bank_count - 1));
    }

    // UNROM latches a PRG bank the same way, from any address in cartridge
    // space. Bus conflicts are not modelled here either, for the same reason.
    //
    // The mask is by bank COUNT rather than a fixed width: real boards decode
    // only as many bits as they have banks, so a write of $0F to a 128KB
    // cartridge selects bank 7, not a bank that does not exist.
    if (mapper_id == 2 && addr >= 0x8000 && prg_bank_count != 0) {
        prg_bank = static_cast<uint8_t>(data & (prg_bank_count - 1));
    }
}

uint8_t ROM::chr_read(const uint16_t addr) const
{
    if (chr_rom.empty()) {
        return 0;
    }

    const size_t offset = static_cast<size_t>(chr_bank) * CHR_ROM_BANK_SIZE + (addr & 0x1FFF);
    // Cannot go out of range given the load-time bank-count check, but a
    // cartridge whose file was shorter than its header claimed would have been
    // rejected there, so this is belt and braces rather than a live path.
    return offset < chr_rom.size() ? chr_rom[offset] : 0;
}

uint8_t ROM::read(const uint16_t addr)
{
    if (prg_rom.empty()) {
        return 0;
    }

    // UNROM: $8000-$BFFF is a switchable 16KB window, $C000-$FFFF is HARD-WIRED
    // to the last bank. The fixed half is not a convenience - it is what makes
    // the mapper usable at all, because the reset and NMI vectors live at
    // $FFFA-$FFFF and a game that switched them out could never come back.
    if (mapper_id == 2) {
        const size_t bank = (addr < 0xC000) ? prg_bank : (prg_bank_count - 1);
        return prg_rom[bank * PRG_ROM_BANK_SIZE + (addr & 0x3FFF)];
    }

    // NROM: 16KB PRG-ROM images are mirrored across $8000-$BFFF and $C000-$FFFF;
    // 32KB images fill $8000-$FFFF directly. The modulo handles both cases since
    // prg_rom.size() is either 16384 or 32768 (a multiple of the bank size).
    return prg_rom[(addr - 0x8000) % prg_rom.size()];
}
