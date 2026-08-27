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
    mapper_wants_cpu_clock = false;
    mapper_drives_ciram_a10 = false;
    mapper_id = MapperId::nrom;
    mirroring = Mirroring::horizontal;
    nes2 = false;
    has_battery = false;
    // Back to the no-cartridge default, not to zero: a load that fails part way
    // through must leave the console as it would be with nothing plugged in.
    prg_ram_size = 8 * 1024;
    prg_nvram_size = 0;
    chr_ram_size = 0;
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

    // NES 2.0 identifies itself in byte 7 bits 2-3 reading exactly 10b. The
    // test has to be that precise: 00b is iNES 1.0 and 01b is an "archaic"
    // dump, and treating either as 2.0 would read bytes 9-11 as sizes when they
    // are whatever the dumper left there - some fill them with a name string.
    const bool parsed_nes2 = (flags7 & 0x0C) == 0x08;

    // Byte 9's nibbles extend the PRG and CHR sizes by 256 banks each, and the
    // value $F switches that field to an exponent-multiplier encoding entirely.
    // Nothing here carries either: the largest board this emulator implements
    // is 512KB, which is 32 banks. Rejecting rather than ignoring, because
    // ignoring loads a multi-megabyte image at a thirty-second of its size with
    // no complaint.
    if (parsed_nes2 && data[9] != 0) {
        std::cerr << "ROM: NES 2.0 byte 9 is " << static_cast<int>(data[9])
                  << ", so the image is larger than 4MB or uses the exponent size form; neither is supported\n";
        return false;
    }

    const bool has_trainer = flags6 & 0x04;
    // Mapper id: low nibble comes from the high nibble of flags6, high nibble
    // from the high nibble of flags7.
    const MapperId parsed_mapper_id = static_cast<MapperId>((flags7 & 0xF0) | (flags6 >> 4));

    // Two questions, one table. Whether a board exists, and whether the header
    // is consistent with it, both come from kBoards in mapper.cpp - so adding a
    // mapper cannot leave it accepted with no board behind it, or implemented
    // and rejected before it runs. Those were the two ways the previous chain
    // of `!=` comparisons could fail, and both were silent.
    if (!mapper_supported(parsed_mapper_id)) {
        const char* name = mapper_name(parsed_mapper_id);
        std::cerr << "ROM: mapper " << static_cast<int>(parsed_mapper_id);
        if (name != nullptr) {
            std::cerr << " (" << name << ")";
        }
        std::cerr << " is not supported\n";
        return false;
    }

    const std::string header_error = mapper_header_error(parsed_mapper_id, prg_rom_banks, chr_rom_banks);
    if (!header_error.empty()) {
        std::cerr << "ROM: " << header_error << "\n";
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
    nes2 = parsed_nes2;
    mirroring = (flags6 & 0x01) ? Mirroring::vertical : Mirroring::horizontal;
    has_battery = (flags6 & 0x02) != 0;

    // Byte 10 stores each RAM size as a shift count in one nibble: 0 means the
    // chip is absent, and n means 64 << n bytes. Byte 11 says the same for CHR.
    // A shift of 15 would be 2MB and is not a size any board has, but it is
    // also not worth rejecting - it costs nothing to compute and the bank
    // arithmetic never indexes by it.
    if (nes2) {
        const auto ram_size = [](const uint8_t shift) -> uint32_t {
            return shift == 0 ? 0u : static_cast<uint32_t>(64) << shift;
        };
        prg_ram_size = ram_size(data[10] & 0x0F);
        prg_nvram_size = ram_size(data[10] >> 4);
        chr_ram_size = ram_size(data[11] & 0x0F) + ram_size(data[11] >> 4);
    } else {
        // iNES 1.0 cannot express "no work RAM" - byte 8 is a count of 8KB
        // units that "0 means 1" for compatibility, and most dumps leave it
        // zero regardless. Assuming 8KB is the format's own advice, and it is
        // what keeps every cartridge that loaded before this parsing existed
        // seeing the same $6000-$7FFF window it saw then.
        if (has_battery) {
            prg_nvram_size = 8 * 1024;
        } else {
            prg_ram_size = 8 * 1024;
        }
        chr_ram_size = chr_rom_banks == 0 ? 8 * 1024 : 0;
    }

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
    // mapper_supported() above asked the same table, so this cannot come back
    // null; the check is kept because "cannot" is a claim about two functions
    // agreeing, and a loaded cartridge with no board is a null dereference on
    // the first instruction fetch.
    mapper = make_mapper(mapper_id, *this);
    if (!mapper) {
        std::cerr << "ROM: no board for mapper " << static_cast<int>(mapper_id)
                  << ", which the support check above should have rejected\n";
        return false;
    }

    mapper_wants_cpu_clock = mapper->wants_cpu_clock();
    mapper_drives_ciram_a10 = mapper->drives_ciram_a10();

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
    if (chr_rom.empty() || !mapper) {
        return 0;
    }
    const uint32_t offset = mapper->chr_offset(addr);
    // Cannot go out of range given the load-time bank-count checks, but a
    // cartridge whose file was shorter than its header claimed would have been
    // rejected there - so this is belt and braces rather than a live path.
    return offset < chr_rom.size() ? chr_rom[offset] : 0;
}

// Where in the console's CHR-RAM array a PPU address lands.
//
// The array is the PPU's because that is where it was put, but the ADDRESSING
// is the cartridge's: CHR-RAM sits behind the mapper's CHR lines exactly as
// CHR-ROM does. Indexing it flat - which is what happened until Holy Mapperel's
// CHR-RAM boards failed their window test - silently unbanks it, and no
// CHR-ROM oracle can see that.
uint32_t ROM::chr_ram_offset(const uint16_t addr) const { return mapper ? mapper->chr_offset(addr) : (addr & 0x1FFF); }

// The board's work-RAM banking, folded into the RAM the cartridge declares.
//
// The fold is the interesting half. An 8KB board's upper bank lines go nowhere,
// so a mapper selecting bank 3 on one still reads its single chip - and doing
// that here, once, rather than in each override, is what stops a board indexing
// RAM it does not have. NROM through MMC3 take the flat default and are
// unaffected either way.
//
// Volatile and battery-backed sizes are summed. No SxROM board carries both,
// but NES 2.0 can describe one, and summing is the reading that gives such a
// cartridge all of its RAM rather than silently half.
uint16_t ROM::prg_ram_offset(const uint16_t addr) const
{
    const uint32_t raw = mapper ? mapper->prg_ram_offset(addr) : static_cast<uint32_t>(addr - 0x6000);
    const uint32_t declared = prg_ram_size + prg_nvram_size;

    // Bus::decode consults has_prg_ram() before ever reaching here, so this
    // cannot be zero on a live path. Guarded anyway, because the alternative to
    // a wrong byte is a modulo by zero.
    const uint32_t folded = declared == 0 ? 0 : raw % declared;

    // And never past the array, whatever a header claims: NES 2.0 can encode
    // 2MB of work RAM, which no board carries and this device does not have.
    return static_cast<uint16_t>(folded % PrgRAM::SIZE);
}

// A change on PPU address line A12. Only the MMC3 has the wire; Mapper's
// default override is what makes that true of three boards without them saying
// so.
void ROM::observe_pattern_fetch(const uint16_t ppu_addr)
{
    if (mapper) {
        mapper->observe_pattern_fetch(ppu_addr);
    }
}

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

uint64_t ROM::cpu_cycle() const { return bus == nullptr ? 0 : bus->cpu_cycles; }
