#include "../include/rom.h"

#include <fstream>
#include <iostream>

// For bus->cpu.set_IRQ_line: the MMC3's counter drives a real wire into the
// CPU, so the mapper needs the Bus definition rather than just a forward
// declaration. bus.h includes rom.h, not the other way round, so this is safe
// here and would not be in the header.
#include "../include/bus.h"

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
    mapper.reset();
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

    if (parsed_mapper_id != 0 && parsed_mapper_id != 2 && parsed_mapper_id != 3 && parsed_mapper_id != 4) {
        std::cerr << "ROM: mapper " << static_cast<int>(parsed_mapper_id)
                  << " is not supported (only NROM/0, UNROM/2, CNROM/3 and MMC3/4)\n";
        return false;
    }

    // MMC3 banks PRG in 8KB and CHR in 1KB units, so the only header constraint
    // is that there is enough of each to index. A cartridge with no CHR-ROM is
    // legal and uses CHR-RAM; several MMC3 boards do.
    if (parsed_mapper_id == 4 && prg_rom_banks < 2) {
        std::cerr << "ROM: MMC3 requires at least 2 PRG-ROM banks, header advertises " << prg_rom_banks << "\n";
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
    if (parsed_mapper_id == 3 &&
        (chr_rom_banks == 0 || chr_rom_banks > 4 || (chr_rom_banks & (chr_rom_banks - 1)) != 0)) {
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
    } else if (parsed_mapper_id == 4) {
        // Checked above; MMC3 images run to 512KB.
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

    // MMC3 counts in different units from the other mappers: 8KB PRG banks and
    // 1KB CHR banks, where UNROM uses 16KB and CNROM 8KB.
    prg_8k_bank_count = static_cast<uint16_t>(prg_rom_banks * 2);
    chr_1k_bank_count = static_cast<uint16_t>(chr_rom_banks * 8);

    // Power-on bank is 0. The value is not specified by the hardware, but a
    // CNROM game sets it before drawing anything.
    chr_bank = 0;

    // The board is chosen once, here, rather than re-decided on every access.
    // The header check above is what guarantees this switch is exhaustive - add
    // a mapper to one without the other and the cartridge either loads with no
    // board or is rejected while implemented.
    switch (mapper_id) {
    case 0:
        mapper = std::make_unique<NRom>(*this);
        break;
    case 2:
        mapper = std::make_unique<UnRom>(*this);
        break;
    case 3:
        mapper = std::make_unique<CnRom>(*this);
        break;
    case 4:
        mapper = std::make_unique<Mmc3>(*this);
        break;
    default:
        std::cerr << "ROM: no board for mapper " << static_cast<int>(mapper_id)
                  << ", which the header check above should have rejected\n";
        return false;
    }

    // A fresh board comes up with its registers at their power-on values, so the
    // long reset block that used to live here is now the Mmc3 members' own
    // initialisers. The /IRQ line is the exception and must still be released
    // explicitly: it is the CONSOLE's wire, not the cartridge's, so a new
    // cartridge inherits whatever the previous one left latched in the CPU and
    // would take an interrupt it never asked for before executing an
    // instruction.
    drive_irq_line(false);

    prg_rom.assign(data.begin() + offset, data.begin() + offset + prg_size);
    offset += prg_size;

    // chr_rom_banks == 0 means the cartridge uses CHR-RAM instead; leave chr_rom empty in that case.
    chr_rom.assign(data.begin() + offset, data.begin() + offset + chr_size);

    return true;
}

// Every cartridge access below is one line, because the board decides what it
// means and the board is an object. What is left here is the part that is the
// same on every board: whether there is anything plugged in at all.

void ROM::write(const uint16_t addr, const uint8_t data)
{
    if (mapper) {
        mapper->cpu_write(addr, data);
    }
}

uint8_t ROM::read(const uint16_t addr)
{
    if (prg_rom.empty() || !mapper) {
        return 0;
    }
    return mapper->prg_read(addr);
}

uint8_t ROM::chr_read(const uint16_t addr) const
{
    // An empty chr_rom means the cartridge uses CHR-RAM, which is the PPU's, not
    // the mapper's. Checked here rather than in four boards.
    if (chr_rom.empty() || !mapper) {
        return 0;
    }
    return mapper->chr_read(addr);
}

// A change on PPU address line A12. Only the MMC3 has the wire; Mapper's
// default override is what makes that true of three boards without them saying
// so.
void ROM::mmc3_observe_a12(const uint16_t ppu_addr, const uint64_t ppu_cycle)
{
    if (mapper) {
        mapper->observe_a12(ppu_addr, ppu_cycle);
    }
}

// /IRQ is open-drain and shared, so the mapper owns one bit of it and nothing
// else. Going through CPU::set_IRQ_line rather than raise_IRQ is what keeps the
// APU's frame counter and the DMC from having their assertions dropped when the
// cartridge releases its own.
void ROM::drive_irq_line(const bool asserted)
{
    // The loader tests in memory_tests.cpp construct a ROM with no Bus at all,
    // deliberately: parsing an iNES header needs nothing else, and giving those
    // tests a whole console to exercise the header checks would be noise. A
    // bus-less cartridge has no CPU to interrupt, so there is nothing to drive.
    if (bus == nullptr) {
        return;
    }

    bus->cpu.set_IRQ_line(CPU::IRQSource::cartridge, asserted);
}
