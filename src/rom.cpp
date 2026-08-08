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

    mmc3_bank_select = 0;
    for (uint8_t& r : mmc3_bank) {
        r = 0;
    }
    prg_ram_enabled = true;
    prg_ram_write_protected = false;

    // The IRQ counter powers up disabled with everything clear. Releasing the
    // /IRQ line matters as much as clearing the flag: loading a second
    // cartridge into a running Bus would otherwise leave the previous game's
    // assertion latched in the CPU, and the new one would take an interrupt it
    // never asked for before executing a single instruction.
    mmc3_irq_latch = 0;
    mmc3_irq_counter = 0;
    mmc3_irq_reload_pending = false;
    mmc3_irq_enabled = false;
    mmc3_irq_asserted = false;
    mmc3_a12_high = false;
    mmc3_a12_low_since = 0;
    mmc3_update_irq_line();

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

    // MMC3 decodes the register pair from bit 13 and the write's parity: even
    // addresses are the first of each pair, odd the second. The board decodes
    // nothing finer, so $8000 and $9FFE are the same register.
    if (mapper_id == 4 && addr >= 0x8000) {
        const bool odd = (addr & 0x0001) != 0;
        switch (addr & 0xE000) {
        case 0x8000:
            if (odd) {
                mmc3_bank[mmc3_bank_select & 0x07] = data;
            } else {
                mmc3_bank_select = data;
            }
            break;

        case 0xA000:
            if (odd) {
                // $A001 PRG-RAM protect. Bit 7 enables the chip, bit 6 denies
                // writes. Held here; Bus is what has to honour them, since
                // PrgRAM is a separate device.
                prg_ram_enabled = (data & 0x80) != 0;
                prg_ram_write_protected = (data & 0x40) != 0;
            } else {
                // $A000 mirroring, live: PPU::nametable_offset reads this flag
                // on every access, so nothing else has to be told.
                horizontal_mirroring = (data & 0x01) == 0;
            }
            break;

        case 0xC000:
            if (odd) {
                // $C001 does NOT reload the counter itself. It clears it and
                // raises a flag that the NEXT rising edge of A12 acts on, which
                // is why the reload value can be changed at $C000 after the
                // fact and still take effect. Writing the counter directly here
                // would make a $C001 immediately before an edge behave the same
                // as one long before it, and 1-clocking measures the
                // difference.
                mmc3_irq_counter = 0;
                mmc3_irq_reload_pending = true;
            } else {
                mmc3_irq_latch = data;
            }
            break;

        case 0xE000:
            if (odd) {
                // $E001 enables. It does not re-examine the counter: a counter
                // sitting at zero does not raise an IRQ until the next clock.
                mmc3_irq_enabled = true;
            } else {
                // $E000 does two things at once, and both matter. It disables
                // further IRQs AND acknowledges the current one - that is, it
                // releases /IRQ. A handler that only cleared the enable flag
                // would return to a line that is still held low and be
                // re-entered forever.
                mmc3_irq_enabled = false;
                mmc3_irq_asserted = false;
                mmc3_update_irq_line();
            }
            break;

        default:
            // $8000-$FFFF is fully decoded by the three cases above; nothing
            // else reaches here.
            break;
        }
    }
}

// A change on PPU address line A12, as the mapper sees it.
//
// The mapper is wired to the PPU's address bus and cannot tell a rendering
// fetch from a $2006 write - both drive the same eleven-and-a-bit lines. So
// this takes a raw address and looks at one bit of it, rather than being told
// "a scanline happened".
void ROM::mmc3_observe_a12(const uint16_t ppu_addr, const uint64_t ppu_cycle)
{
    if (mapper_id != 4) {
        return;
    }

    const bool high = (ppu_addr & 0x1000) != 0;
    const bool was_high = mmc3_a12_high;
    mmc3_a12_high = high;

    if (!high) {
        // Falling edge: start the clock on how long the line stays down. A
        // level that was already low must NOT restart it, or a run of $2xxx
        // nametable fetches would keep resetting the timer and no edge would
        // ever pass the filter.
        if (was_high) {
            mmc3_a12_low_since = ppu_cycle;
        }
        return;
    }

    if (was_high) {
        return;  // still high - a level, not an edge.
    }

    // A rising edge, but only a long enough low period makes it a real one. See
    // mmc3_a12_filter_dots for why the short ones have to be thrown away.
    if (ppu_cycle - mmc3_a12_low_since < mmc3_a12_filter_dots) {
        return;
    }

    mmc3_clock_irq_counter();
}

// One filtered rising edge. NESdev, and the ORDER here is the whole of it:
//
//   "if zero or the reload flag is true, it's reloaded with the IRQ latched
//    value at $C000; otherwise, it decrements. If the IRQ counter is zero and
//    IRQs are enabled ($E001), an IRQ is triggered."
//
// The zero test happens AFTER the reload-or-decrement, not before. Testing
// first - "if the counter is zero, fire, then reload or decrement" - is the
// classic MMC3 bug: it fires one edge late, so every raster split lands a
// scanline low, and a latch of 0 never fires at all instead of firing on every
// clock.
//
// This is the Sharp part (MMC3 revision B/C). Reloading to zero passes the test
// below and raises an IRQ immediately, so a latch of $00 interrupts on every
// clock - which is what 5-MMC3 asserts. The NEC revision A part suppresses that
// case, and 6-MMC3_alt asserts the opposite; the two cannot both hold. Sharp is
// implemented because Super Mario Bros. 3 and Mega Man 3 are Sharp boards, so
// it is the behaviour real games were written against. See mmc3_rom_tests.cpp,
// which pins 6-MMC3_alt's exact failure rather than hiding it.
void ROM::mmc3_clock_irq_counter()
{
    if (mmc3_irq_counter == 0 || mmc3_irq_reload_pending) {
        mmc3_irq_counter = mmc3_irq_latch;
        mmc3_irq_reload_pending = false;
    } else {
        --mmc3_irq_counter;
    }

    if (mmc3_irq_counter == 0 && mmc3_irq_enabled) {
        mmc3_irq_asserted = true;
        mmc3_update_irq_line();
    }
}

// /IRQ is open-drain and shared, so the mapper owns one bit of it and nothing
// else. Going through CPU::set_IRQ_line rather than raise_IRQ is what keeps the
// APU's frame counter and the DMC from having their assertions dropped when the
// cartridge releases its own.
void ROM::mmc3_update_irq_line()
{
    // The loader tests in memory_tests.cpp construct a ROM with no Bus at all,
    // deliberately: parsing an iNES header needs nothing else, and giving those
    // tests a whole console to exercise the header checks would be noise. A
    // bus-less cartridge has no CPU to interrupt, so there is nothing to drive.
    if (bus == nullptr) {
        return;
    }

    bus->cpu.set_IRQ_line(CPU::IRQSource::cartridge, mmc3_irq_asserted);
}

// Which 1KB CHR bank a PPU address reads through.
//
// The six CHR registers cover 8KB as two 2KB banks and four 1KB banks. $8000
// bit 7 swaps which half of pattern space each group occupies, which is why the
// address is folded through that bit rather than switched on directly.
//
// NESdev: "R0 and R1 ignore the bottom bit" - they address 2KB, so the low bit
// of the register is not connected.
uint16_t ROM::mmc3_chr_bank_for(const uint16_t ppu_addr) const
{
    // Position within the 8KB pattern space, in 1KB units: 0-7.
    uint16_t slot = static_cast<uint16_t>((ppu_addr >> 10) & 0x07);

    if (mmc3_chr_a12_inverted()) {
        slot ^= 0x04;
    }

    switch (slot) {
    case 0:
        return static_cast<uint16_t>(mmc3_bank[0] & 0xFE);
    case 1:
        return static_cast<uint16_t>((mmc3_bank[0] & 0xFE) + 1);
    case 2:
        return static_cast<uint16_t>(mmc3_bank[1] & 0xFE);
    case 3:
        return static_cast<uint16_t>((mmc3_bank[1] & 0xFE) + 1);
    default:
        // Slots 4-7 are the four 1KB registers R2-R5.
        return mmc3_bank[2 + (slot - 4)];
    }
}

// Which 8KB PRG bank a CPU address reads through.
//
// Two windows switch and two are wired down. $E000-$FFFF is ALWAYS the last
// bank, which is what keeps the reset and NMI vectors reachable no matter what
// the game selects - the same reason UNROM fixes its upper half. $8000 bit 6
// decides whether R6 drives the bottom window or the third one; the other of
// those two is fixed to the second-last bank.
uint16_t ROM::mmc3_prg_bank_for(const uint16_t cpu_addr) const
{
    const uint16_t last = static_cast<uint16_t>(prg_8k_bank_count - 1);
    const uint16_t second_last = static_cast<uint16_t>(prg_8k_bank_count - 2);

    // R6 and R7 "ignore the top two bits, as the MMC3 has only 6 PRG ROM
    // address lines".
    const uint16_t r6 = static_cast<uint16_t>(mmc3_bank[6] & 0x3F);
    const uint16_t r7 = static_cast<uint16_t>(mmc3_bank[7] & 0x3F);

    switch (cpu_addr & 0xE000) {
    case 0x8000:
        return mmc3_prg_mode_swapped() ? second_last : r6;
    case 0xA000:
        return r7;
    case 0xC000:
        return mmc3_prg_mode_swapped() ? r6 : second_last;
    default:
        return last;
    }
}

uint8_t ROM::chr_read(const uint16_t addr) const
{
    if (chr_rom.empty()) {
        return 0;
    }

    if (mapper_id == 4) {
        if (chr_1k_bank_count == 0) {
            return 0;
        }
        const uint16_t bank = mmc3_chr_bank_for(addr) % chr_1k_bank_count;
        const size_t mmc3_offset = static_cast<size_t>(bank) * 1024 + (addr & 0x03FF);
        return mmc3_offset < chr_rom.size() ? chr_rom[mmc3_offset] : 0;
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

    // MMC3: four 8KB windows, two switchable and two wired down.
    if (mapper_id == 4) {
        if (prg_8k_bank_count == 0) {
            return 0;
        }
        const uint16_t bank = mmc3_prg_bank_for(addr) % prg_8k_bank_count;
        const size_t offset = static_cast<size_t>(bank) * 8192 + (addr & 0x1FFF);
        return offset < prg_rom.size() ? prg_rom[offset] : 0;
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
