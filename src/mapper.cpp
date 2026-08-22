#include "../include/mapper.h"

#include "../include/rom.h"

// For bus->cpu.set_IRQ_line via ROM: the MMC3's counter drives a real wire into
// the CPU, so this needs the Bus definition rather than a forward declaration.
#include "../include/bus.h"

namespace
{
constexpr size_t PRG_ROM_BANK_SIZE = 16 * 1024;
constexpr size_t CHR_ROM_BANK_SIZE = 8 * 1024;
}  // namespace

// --- NROM (0) ---------------------------------------------------------------

// Nothing to latch. NROM PRG-ROM is not writable and the board decodes no
// registers, so a write into cartridge space is simply lost.
void NRom::cpu_write(const uint16_t addr, const uint8_t data)
{
    (void)addr;
    (void)data;
}

// 16KB PRG-ROM images are mirrored across $8000-$BFFF and $C000-$FFFF; 32KB
// images fill $8000-$FFFF directly. The modulo handles both cases since
// prg_rom.size() is either 16384 or 32768 (a multiple of the bank size).
uint8_t NRom::prg_read(const uint16_t addr) const { return rom.prg_rom[(addr - 0x8000) % rom.prg_rom.size()]; }

uint8_t NRom::chr_read(const uint16_t addr) const
{
    // One fixed bank, so chr_bank is always 0 and this reduces to a plain index.
    const size_t offset = static_cast<size_t>(rom.chr_bank) * CHR_ROM_BANK_SIZE + (addr & 0x1FFF);
    // Cannot go out of range given the load-time bank-count check, but a
    // cartridge whose file was shorter than its header claimed would have been
    // rejected there, so this is belt and braces rather than a live path.
    return offset < rom.chr_rom.size() ? rom.chr_rom[offset] : 0;
}

// --- UNROM (2) --------------------------------------------------------------

// UNROM latches a PRG bank from any address in cartridge space; the board
// decodes nothing finer than "the CPU wrote to cartridge space".
//
// Bus conflicts are NOT modelled. Real UNROM drives the ROM's own byte onto the
// bus at the same time, so hardware sees (data & prg_byte). Cartridges are
// written to avoid that - they store the bank number at the address they write
// to - so the AND is a no-op for correct software, and leaving it out cannot
// mask a bug in anything being tested here.
//
// The mask is by bank COUNT rather than a fixed width: real boards decode only
// as many bits as they have banks, so a write of $0F to a 128KB cartridge
// selects bank 7, not a bank that does not exist.
void UnRom::cpu_write(const uint16_t addr, const uint8_t data)
{
    if (addr >= 0x8000 && rom.prg_bank_count != 0) {
        rom.prg_bank = static_cast<uint8_t>(data & (rom.prg_bank_count - 1));
    }
}

// $8000-$BFFF is a switchable 16KB window, $C000-$FFFF is HARD-WIRED to the
// last bank. The fixed half is not a convenience - it is what makes the mapper
// usable at all, because the reset and NMI vectors live at $FFFA-$FFFF and a
// game that switched them out could never come back.
uint8_t UnRom::prg_read(const uint16_t addr) const
{
    const size_t bank = (addr < 0xC000) ? rom.prg_bank : (rom.prg_bank_count - 1);
    return rom.prg_rom[bank * PRG_ROM_BANK_SIZE + (addr & 0x3FFF)];
}

uint8_t UnRom::chr_read(const uint16_t addr) const
{
    // UNROM carries no CHR-ROM at all - the console's CHR-RAM is the pattern
    // memory, and ROM::chr_read has already returned for an empty chr_rom before
    // reaching here. Kept as the same plain index the other latch boards use so
    // that a UNROM image which somehow carried CHR-ROM reads it rather than
    // silently returning zero.
    const size_t offset = static_cast<size_t>(rom.chr_bank) * CHR_ROM_BANK_SIZE + (addr & 0x1FFF);
    return offset < rom.chr_rom.size() ? rom.chr_rom[offset] : 0;
}

// --- CNROM (3) --------------------------------------------------------------

// A write anywhere in $8000-$FFFF latches the low bits as the CHR bank number.
// Bus conflicts are not modelled, for the same reason as UNROM's.
void CnRom::cpu_write(const uint16_t addr, const uint8_t data)
{
    if (addr >= 0x8000 && rom.chr_bank_count != 0) {
        rom.chr_bank = static_cast<uint8_t>(data & (rom.chr_bank_count - 1));
    }
}

// PRG behaves exactly as NROM's does.
uint8_t CnRom::prg_read(const uint16_t addr) const { return rom.prg_rom[(addr - 0x8000) % rom.prg_rom.size()]; }

uint8_t CnRom::chr_read(const uint16_t addr) const
{
    const size_t offset = static_cast<size_t>(rom.chr_bank) * CHR_ROM_BANK_SIZE + (addr & 0x1FFF);
    return offset < rom.chr_rom.size() ? rom.chr_rom[offset] : 0;
}

// --- MMC3 (4) ---------------------------------------------------------------

// MMC3 decodes the register pair from bit 13 and the write's parity: even
// addresses are the first of each pair, odd the second. The board decodes
// nothing finer, so $8000 and $9FFE are the same register.
void Mmc3::cpu_write(const uint16_t addr, const uint8_t data)
{
    if (addr < 0x8000) {
        return;
    }

    const bool odd = (addr & 0x0001) != 0;
    switch (addr & 0xE000) {
    case 0x8000:
        if (odd) {
            bank[bank_select & 0x07] = data;
        } else {
            bank_select = data;
        }
        break;

    case 0xA000:
        if (odd) {
            // $A001 PRG-RAM protect. Bit 7 enables the chip, bit 6 denies
            // writes. Held here; Bus is what has to honour them, since PrgRAM
            // is a separate device.
            rom.prg_ram_enabled = (data & 0x80) != 0;
            rom.prg_ram_write_protected = (data & 0x40) != 0;
        } else {
            // $A000 mirroring, live: PPU::nametable_offset reads this flag on
            // every access, so nothing else has to be told.
            rom.horizontal_mirroring = (data & 0x01) == 0;
        }
        break;

    case 0xC000:
        if (odd) {
            // $C001 does NOT reload the counter itself. It clears it and raises
            // a flag that the NEXT rising edge of A12 acts on, which is why the
            // reload value can be changed at $C000 after the fact and still
            // take effect. Writing the counter directly here would make a $C001
            // immediately before an edge behave the same as one long before it,
            // and 1-clocking measures the difference.
            irq_counter = 0;
            irq_reload_pending = true;
        } else {
            irq_latch = data;
        }
        break;

    case 0xE000:
        if (odd) {
            // $E001 enables. It does not re-examine the counter: a counter
            // sitting at zero does not raise an IRQ until the next clock.
            irq_enabled = true;
        } else {
            // $E000 does two things at once, and both matter. It disables
            // further IRQs AND acknowledges the current one - that is, it
            // releases /IRQ. A handler that only cleared the enable flag would
            // return to a line that is still held low and be re-entered
            // forever.
            irq_enabled = false;
            irq_asserted = false;
            rom.drive_irq_line(irq_asserted);
        }
        break;

    default:
        // $8000-$FFFF is fully decoded by the three cases above; nothing else
        // reaches here.
        break;
    }
}

// Which 1KB CHR bank a PPU address reads through.
//
// The six CHR registers cover 8KB as two 2KB banks and four 1KB banks. $8000
// bit 7 swaps which half of pattern space each group occupies, which is why the
// address is folded through that bit rather than switched on directly.
//
// NESdev: "R0 and R1 ignore the bottom bit" - they address 2KB, so the low bit
// of the register is not connected.
uint16_t Mmc3::chr_bank_for(const uint16_t ppu_addr) const
{
    // Position within the 8KB pattern space, in 1KB units: 0-7.
    uint16_t slot = static_cast<uint16_t>((ppu_addr >> 10) & 0x07);

    if (chr_a12_inverted()) {
        slot ^= 0x04;
    }

    switch (slot) {
    case 0:
        return static_cast<uint16_t>(bank[0] & 0xFE);
    case 1:
        return static_cast<uint16_t>((bank[0] & 0xFE) + 1);
    case 2:
        return static_cast<uint16_t>(bank[1] & 0xFE);
    case 3:
        return static_cast<uint16_t>((bank[1] & 0xFE) + 1);
    default:
        // Slots 4-7 are the four 1KB registers R2-R5.
        return bank[2 + (slot - 4)];
    }
}

// Which 8KB PRG bank a CPU address reads through.
//
// Two windows switch and two are wired down. $E000-$FFFF is ALWAYS the last
// bank, which is what keeps the reset and NMI vectors reachable no matter what
// the game selects - the same reason UNROM fixes its upper half. $8000 bit 6
// decides whether R6 drives the bottom window or the third one; the other of
// those two is fixed to the second-last bank.
uint16_t Mmc3::prg_bank_for(const uint16_t cpu_addr) const
{
    const uint16_t last = static_cast<uint16_t>(rom.prg_8k_bank_count - 1);
    const uint16_t second_last = static_cast<uint16_t>(rom.prg_8k_bank_count - 2);

    // R6 and R7 "ignore the top two bits, as the MMC3 has only 6 PRG ROM address
    // lines".
    const uint16_t r6 = static_cast<uint16_t>(bank[6] & 0x3F);
    const uint16_t r7 = static_cast<uint16_t>(bank[7] & 0x3F);

    switch (cpu_addr & 0xE000) {
    case 0x8000:
        return prg_mode_swapped() ? second_last : r6;
    case 0xA000:
        return r7;
    case 0xC000:
        return prg_mode_swapped() ? r6 : second_last;
    default:
        return last;
    }
}

// Four 8KB windows, two switchable and two wired down.
uint8_t Mmc3::prg_read(const uint16_t addr) const
{
    if (rom.prg_8k_bank_count == 0) {
        return 0;
    }
    const uint16_t bank = prg_bank_for(addr) % rom.prg_8k_bank_count;
    const size_t offset = static_cast<size_t>(bank) * 8192 + (addr & 0x1FFF);
    return offset < rom.prg_rom.size() ? rom.prg_rom[offset] : 0;
}

uint8_t Mmc3::chr_read(const uint16_t addr) const
{
    if (rom.chr_1k_bank_count == 0) {
        return 0;
    }
    const uint16_t bank = chr_bank_for(addr) % rom.chr_1k_bank_count;
    const size_t offset = static_cast<size_t>(bank) * 1024 + (addr & 0x03FF);
    return offset < rom.chr_rom.size() ? rom.chr_rom[offset] : 0;
}

// A change on PPU address line A12, as the mapper sees it.
//
// The mapper is wired to the PPU's address bus and cannot tell a rendering
// fetch from a $2006 write - both drive the same eleven-and-a-bit lines. So
// this takes a raw address and looks at one bit of it, rather than being told
// "a scanline happened".
void Mmc3::observe_a12(const uint16_t ppu_addr, const uint64_t ppu_cycle)
{
    const bool high = (ppu_addr & 0x1000) != 0;
    const bool was_high = a12_high;
    a12_high = high;

    if (!high) {
        // Falling edge: start the clock on how long the line stays down. A level
        // that was already low must NOT restart it, or a run of $2xxx nametable
        // fetches would keep resetting the timer and no edge would ever pass the
        // filter.
        if (was_high) {
            a12_low_since = ppu_cycle;
        }
        return;
    }

    if (was_high) {
        return;  // still high - a level, not an edge.
    }

    // A rising edge, but only a long enough low period makes it a real one. See
    // a12_filter_dots for why the short ones have to be thrown away.
    if (ppu_cycle - a12_low_since < a12_filter_dots) {
        return;
    }

    clock_irq_counter();
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
// implemented because Super Mario Bros. 3 and Mega Man 3 are Sharp boards, so it
// is the behaviour real games were written against. See mmc3_rom_tests.cpp,
// which pins 6-MMC3_alt's exact failure rather than hiding it.
void Mmc3::clock_irq_counter()
{
    if (irq_counter == 0 || irq_reload_pending) {
        irq_counter = irq_latch;
        irq_reload_pending = false;
    } else {
        --irq_counter;
    }

    if (irq_counter == 0 && irq_enabled) {
        irq_asserted = true;
        rom.drive_irq_line(irq_asserted);
    }
}
