#include "../include/mapper.h"

#include "../include/rom.h"

// For bus->cpu.set_IRQ_line via ROM: the MMC3's counter drives a real wire into
// the CPU, so this needs the Bus definition rather than a forward declaration.
#include "../include/bus.h"

namespace
{
constexpr size_t PRG_ROM_BANK_SIZE = 16 * 1024;
constexpr size_t CHR_ROM_BANK_SIZE = 8 * 1024;

// MMC1 switches CHR in halves of the pattern table rather than whole ones.
constexpr size_t CHR_ROM_4K_BANK_SIZE = 4 * 1024;

struct KnownMapper {
    MapperId id;
    const char* name;
};

// Names only. A row here does NOT mean the board is implemented; see kBoards.
const KnownMapper kKnownMappers[] = {
    {MapperId::nrom, "NROM"},
    {MapperId::mmc1, "MMC1 / SxROM"},
    {MapperId::uxrom, "UxROM"},
    {MapperId::cnrom, "CNROM"},
    {MapperId::mmc3, "MMC3 / TxROM"},
    {MapperId::mmc5, "MMC5 / ExROM"},
    {MapperId::axrom, "AxROM"},
    {MapperId::mmc2, "MMC2 / PxROM"},
    {MapperId::mmc4, "MMC4 / FxROM"},
    {MapperId::color_dreams, "Color Dreams"},
    {MapperId::cprom, "CPROM"},
    {MapperId::bandai_fcg, "Bandai FCG"},
    {MapperId::jaleco_ss88006, "Jaleco SS 88006"},
    {MapperId::namco_163, "Namco 163"},
    {MapperId::vrc4a, "Konami VRC4a/VRC4c"},
    {MapperId::vrc2a, "Konami VRC2a"},
    {MapperId::vrc4b, "Konami VRC2b/VRC4e"},
    {MapperId::vrc6a, "Konami VRC6a"},
    {MapperId::vrc4c, "Konami VRC2c/VRC4b/VRC4d"},
    {MapperId::vrc6b, "Konami VRC6b"},
    {MapperId::action53, "Action 53 / INL-ROM"},
    {MapperId::unrom512, "UNROM 512"},
    {MapperId::irem_g101, "Irem G-101"},
    {MapperId::taito_tc0190, "Taito TC0190"},
    {MapperId::bnrom, "BNROM / NINA-001"},
    {MapperId::rambo1, "RAMBO-1"},
    {MapperId::gxrom, "GxROM"},
    {MapperId::sunsoft4, "Sunsoft-4"},
    {MapperId::sunsoft_fme7, "Sunsoft FME-7"},
    {MapperId::codemasters, "Codemasters"},
    {MapperId::vrc3, "Konami VRC3"},
    {MapperId::vrc1, "Konami VRC1"},
    {MapperId::holy_diver, "Irem IF-12 / Holy Diver"},
    {MapperId::nina003, "NINA-003-006"},
    {MapperId::vrc7, "Konami VRC7"},
    {MapperId::txsrom, "TxSROM"},
    {MapperId::tqrom, "TQROM"},
    {MapperId::unrom_7408, "UNROM (7408)"},
    {MapperId::dxrom, "DxROM"},
    {MapperId::namco_175, "Namco 175/340"},
};

// --- the board table --------------------------------------------------------
//
// The single place a mapper number becomes behaviour. It used to be two places
// - a chain of `!=` comparisons in the header check and a switch further down
// in load() - and keeping those in step was a standing invitation to add a
// board that got rejected before it could run, or accept a cartridge with no
// board behind it. Both failure modes are silent from the header check's side.
//
// Each row's `check` validates the bank counts the iNES header advertises
// against what the board can physically be. Boards whose only constraint is
// "any legal size" use nullptr rather than an empty function.

template <typename T>
std::unique_ptr<Mapper> construct(ROM& rom)
{
    return std::make_unique<T>(rom);
}

std::string check_nrom(const size_t prg_16k_banks, const size_t chr_8k_banks)
{
    // NROM carries one 16KB PRG bank (mirrored across $8000-$FFFF) or two
    // (filling it). Anything else is not NROM however byte 4 reads: the CPU can
    // only address 32KB of cartridge space, so a larger image would load with
    // most of it permanently unreachable and no error.
    if (prg_16k_banks == 0 || prg_16k_banks > 2) {
        return "NROM requires 1 or 2 PRG-ROM banks, header advertises " + std::to_string(prg_16k_banks);
    }
    // One fixed CHR bank. CNROM is the mapper that exists in order to have
    // several; more than one here would load with the excess unreachable, the
    // same failure the PRG check above prevents.
    if (chr_8k_banks > 1) {
        return "NROM supports at most 1 CHR-ROM bank, header advertises " + std::to_string(chr_8k_banks);
    }
    return {};
}

std::string check_mmc1(const size_t prg_16k_banks, const size_t chr_8k_banks)
{
    (void)chr_8k_banks;  // CHR-ROM is optional; SNROM and friends use CHR-RAM.
    // Every SxROM board has at least two PRG banks - the fixed half of the
    // address space has to come from somewhere.
    if (prg_16k_banks < 2 || prg_16k_banks > 32) {
        return "MMC1 requires 2 to 32 PRG-ROM banks, header advertises " + std::to_string(prg_16k_banks);
    }
    return {};
}

std::string check_uxrom(const size_t prg_16k_banks, const size_t chr_8k_banks)
{
    // UNROM switches PRG rather than CHR, so it carries no CHR-ROM at all - the
    // console's CHR-RAM is the pattern memory. A header claiming CHR banks is
    // not a UNROM image whatever byte 7 says.
    if (chr_8k_banks != 0) {
        return "UNROM has no CHR-ROM, header advertises " + std::to_string(chr_8k_banks) + " bank(s)";
    }
    // UNROM exists to carry more PRG than the CPU can address, so NROM's
    // two-bank ceiling does not apply. 8 or 16 banks are the real board sizes.
    if (prg_16k_banks < 2 || prg_16k_banks > 16) {
        return "UNROM requires 2 to 16 PRG-ROM banks, header advertises " + std::to_string(prg_16k_banks);
    }
    return {};
}

std::string check_cnrom(const size_t prg_16k_banks, const size_t chr_8k_banks)
{
    if (prg_16k_banks == 0 || prg_16k_banks > 2) {
        return "CNROM requires 1 or 2 PRG-ROM banks, header advertises " + std::to_string(prg_16k_banks);
    }
    // The bank register is masked with the bank count, so a count that is not a
    // power of two would make the masking ambiguous. Real boards carry 1, 2 or
    // 4 banks (8KB, 16KB, 32KB of CHR).
    if (chr_8k_banks == 0 || chr_8k_banks > 4 || (chr_8k_banks & (chr_8k_banks - 1)) != 0) {
        return "CNROM requires 1, 2 or 4 CHR-ROM banks, header advertises " + std::to_string(chr_8k_banks);
    }
    return {};
}

std::string check_mmc3(const size_t prg_16k_banks, const size_t chr_8k_banks)
{
    (void)chr_8k_banks;
    // MMC3 banks PRG in 8KB and CHR in 1KB units, so the only header constraint
    // is that there is enough of each to index. A cartridge with no CHR-ROM is
    // legal and uses CHR-RAM; several MMC3 boards do.
    if (prg_16k_banks < 2) {
        return "MMC3 requires at least 2 PRG-ROM banks, header advertises " + std::to_string(prg_16k_banks);
    }
    return {};
}

struct Board {
    MapperId id;
    std::unique_ptr<Mapper> (*make)(ROM&);
    std::string (*check)(size_t prg_16k_banks, size_t chr_8k_banks);
};

const Board kBoards[] = {
    {MapperId::nrom, construct<NRom>, check_nrom},    {MapperId::mmc1, construct<Mmc1>, check_mmc1},
    {MapperId::uxrom, construct<UnRom>, check_uxrom}, {MapperId::cnrom, construct<CnRom>, check_cnrom},
    {MapperId::mmc3, construct<Mmc3>, check_mmc3},
};

const Board* find_board(const MapperId id)
{
    for (const Board& board : kBoards) {
        if (board.id == id) {
            return &board;
        }
    }
    return nullptr;
}
}  // namespace

const char* mapper_name(const MapperId id)
{
    for (const KnownMapper& known : kKnownMappers) {
        if (known.id == id) {
            return known.name;
        }
    }
    return nullptr;
}

bool mapper_supported(const MapperId id) { return find_board(id) != nullptr; }

std::string mapper_header_error(const MapperId id, const size_t prg_16k_banks, const size_t chr_8k_banks)
{
    const Board* board = find_board(id);
    if (board == nullptr || board->check == nullptr) {
        return {};
    }
    return board->check(prg_16k_banks, chr_8k_banks);
}

std::unique_ptr<Mapper> make_mapper(const MapperId id, ROM& rom)
{
    const Board* board = find_board(id);
    return board == nullptr ? nullptr : board->make(rom);
}

// The single 8KB window, at whatever bank a one-latch board last selected.
// NROM, UNROM and CNROM are all exactly this - chr_bank never leaves 0 on the
// first two - so none of them overrides it.
uint32_t Mapper::chr_offset(const uint16_t ppu_addr) const
{
    return static_cast<uint32_t>(rom.chr_bank) * CHR_ROM_BANK_SIZE + (ppu_addr & 0x1FFF);
}

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

// --- MMC1 (1) ---------------------------------------------------------------

Mmc1::Mmc1(ROM& cartridge) : Mapper{cartridge}
{
    // $0C in the PRG-mode bits is guaranteed by hardware and load-bearing (see
    // mapper.h). The MIRRORING bits are NOT guaranteed, and leaving them zero
    // would power the board on in one-screen-lower - so they are seeded from
    // the iNES header, which keeps ROM::mirroring the single source of truth
    // instead of having the field and the register disagree until a game
    // happens to write $8000.
    control = static_cast<uint8_t>(0x0C | (rom.mirroring == ROM::Mirroring::vertical ? 0x02 : 0x03));
}

void Mmc1::cpu_write(const uint16_t addr, const uint8_t data)
{
    if (addr < 0x8000) {
        return;
    }

    // Consecutive-cycle writes are ignored - see last_write_cycle in mapper.h
    // for why that is required rather than an optimisation. A cartridge with no
    // Bus (the loader tests build one) has no clock to compare against, so the
    // filter is simply off there; those tests drive ROM::write directly and
    // never from a read-modify-write instruction.
    if (rom.has_bus()) {
        const uint64_t now = rom.cpu_cycle();
        if (wrote_before && now == last_write_cycle + 1) {
            return;
        }
        last_write_cycle = now;
        wrote_before = true;
    }

    // Bit 7 empties the shift register and forces PRG mode 3. Note what it does
    // NOT do: the other three registers keep their values, and the OR with $0C
    // leaves the mirroring bits alone. So a game can resynchronise the serial
    // port without losing its bank selection, which is why the reset is usable
    // as the first instruction of an interrupt handler.
    if (data & 0x80) {
        shift = 0;
        shift_count = 0;
        control |= 0x0C;
        return;
    }

    // Bits arrive least-significant first and shift right into bit 4, so after
    // five writes the first bit written has reached bit 0.
    shift = static_cast<uint8_t>((shift >> 1) | ((data & 0x01) << 4));
    if (++shift_count < 5) {
        return;
    }

    // The register is chosen by the address of the FIFTH write, not the first:
    // a game may move around inside $8000-$FFFF while clocking bits in, and
    // only the last address counts.
    write_register(addr, shift);
    shift = 0;
    shift_count = 0;
}

void Mmc1::write_register(const uint16_t addr, const uint8_t value)
{
    switch ((addr >> 13) & 0x03) {
    case 0:
        control = value;
        apply_mirroring();
        break;
    case 1:
        chr_reg[0] = value;
        break;
    case 2:
        chr_reg[1] = value;
        break;
    default:
        prg_reg = value;
        // Bit 4 disables the work RAM, and Bus::decode is what enforces it -
        // the RAM behind $6000-$7FFF is a separate device with no idea a mapper
        // exists. Same route the MMC3's $A001 takes.
        //
        // SNROM ALSO disables it from $A000 bit 4, and this does not implement
        // that: the board cannot be told apart from an iNES header with
        // certainty, and guessing wrong breaks the boards that use those bits
        // for CHR. Holy Mapperel reports the disagreement as a detail digit
        // rather than a failure, so it is a measured divergence - see
        // tests/holy_mapperel_tests.cpp.
        rom.prg_ram_enabled = !wram_disabled();
        break;
    }
}

void Mmc1::apply_mirroring()
{
    switch (control & 0x03) {
    case 0:
        rom.mirroring = ROM::Mirroring::single_screen_lower;
        break;
    case 1:
        rom.mirroring = ROM::Mirroring::single_screen_upper;
        break;
    case 2:
        rom.mirroring = ROM::Mirroring::vertical;
        break;
    default:
        rom.mirroring = ROM::Mirroring::horizontal;
        break;
    }
}

uint32_t Mmc1::prg_bank_offset(const uint16_t cpu_addr) const
{
    const uint32_t banks = rom.prg_bank_count;

    // SUROM (512KB): MMC1 has four PRG bank lines and 32 banks need five, so
    // CHR register 0 bit 4 supplies A18 and picks which 256KB half EVERYTHING
    // is read from - the fixed window included. That is what makes switching
    // halves survivable: if the fixed bank stayed behind, the code performing
    // the switch would vanish out from under itself mid-instruction.
    const uint32_t half = (banks > 16 && (chr_reg[0] & 0x10) != 0) ? 16u : 0u;
    const uint32_t select = prg_reg & 0x0F;

    uint32_t bank = 0;
    switch (prg_mode()) {
    case 0:
    case 1:
        // 32KB at a time: the register's low bit is ignored, and $C000 is
        // simply the bank after $8000.
        bank = (select & 0x0E) | ((cpu_addr >= 0xC000) ? 1u : 0u);
        break;
    case 2:
        // $8000 fixed to the first bank of the half, $C000 switchable.
        bank = (cpu_addr < 0xC000) ? 0u : select;
        break;
    default:
        // $8000 switchable, $C000 fixed to the last bank of the half. Written
        // as 15 rather than banks-1 because 15 is what a full-size board's four
        // address lines carry; the mask below is what lands a smaller cartridge
        // on its own last bank, which is exactly what its unconnected upper
        // lines do on hardware.
        bank = (cpu_addr < 0xC000) ? select : 15u;
        break;
    }

    // Every MMC1 PRG size is a power of two (32KB to 512KB), so this mask is
    // the whole of the address decoding a real board performs.
    return ((half + bank) & (banks - 1)) * PRG_ROM_BANK_SIZE + (cpu_addr & 0x3FFF);
}

uint32_t Mmc1::chr_offset(const uint16_t ppu_addr) const
{
    // Whichever pattern memory the cartridge brought, counted in 4KB banks.
    // CHR-RAM is banked here exactly as CHR-ROM is - it sits behind the same
    // mapper CHR address lines - and an 8KB chip in 4KB mode is two banks, not
    // an unbanked flat 8KB. That distinction is the whole of Holy Mapperel's
    // CHR window test on SGROM and SUROM.
    const uint32_t banks =
        rom.chr_rom.empty() ? rom.chr_ram_size / CHR_ROM_4K_BANK_SIZE : static_cast<uint32_t>(rom.chr_bank_count) * 2;
    if (banks == 0) {
        return ppu_addr & 0x1FFF;
    }

    const bool upper_half = (ppu_addr & 0x1000) != 0;
    const uint32_t bank = chr_4k_mode() ? (chr_reg[upper_half ? 1 : 0] & 0x1F)
                                        // 8KB mode reads register 0 only, with its low
                                        // bit ignored: the pair is one bank, not two.
                                        : ((chr_reg[0] & 0x1E) | (upper_half ? 1u : 0u));

    return ((bank & (banks - 1)) * CHR_ROM_4K_BANK_SIZE) + (ppu_addr & 0x0FFF);
}

uint8_t Mmc1::prg_read(const uint16_t addr) const { return rom.prg_rom[prg_bank_offset(addr)]; }

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
            //
            // BIT 0 CLEAR IS VERTICAL, which is the opposite of the iNES header
            // bit and was inverted here until the M4 oracle landed. The wiki
            // states it in ARRANGEMENT terms - "0: horizontal (A10); 1: vertical
            // (A11)" - and arrangement is the inverse of mirroring, so reading
            // that row as if it named a mirroring mode yields exactly the bug
            // that was here. The A10/A11 annotation is the unambiguous part:
            // bit 0 clear routes CIRAM A10 from PPU A10, which makes $2000 and
            // $2800 the same screen, and that is vertical mirroring by the
            // naming PPU::nametable_offset uses. FCEUX writes the same fact as
            // `setmirror((V & 1) ^ 1)` with MI_H = 0, and Mesen2 as
            // `(RegA000 & 0x01) ? Horizontal : Vertical`.
            //
            // Holy Mapperel is what caught it: it identifies the board from how
            // the mapper answers, and with the polarity reversed M4_P128K
            // reported "002 UNROM" - the right answer to the wrong question.
            rom.mirroring = (data & 0x01) ? ROM::Mirroring::horizontal : ROM::Mirroring::vertical;
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

uint32_t Mmc3::chr_offset(const uint16_t ppu_addr) const
{
    // The M4 images DID settle it, which is what this comment used to say they
    // would. CHR-RAM is banked here exactly as CHR-ROM is, in 1KB units, for the
    // same reason MMC1 banks it: the RAM sits on the cartridge behind the same
    // mapper CHR address lines, so "no CHR-ROM" changes which chip answers and
    // not how it is addressed.
    //
    // MEASURED. Holy Mapperel M4_P128K is the TGROM build - 128K PRG, 8K
    // CHR-RAM - and with the identity mapping that stood here it reported detail
    // 0003: both 4K halves of its CHR window failed their bank-tag readback.
    // That is bit-for-bit the signature the three MMC1 CHR-RAM boards showed
    // before 58f1ce6 banked theirs, on a board with no MMC1 code in its path.
    // Banking it takes M4_P128K to 0000. M4_P256K_C256K is CHR-ROM, was already
    // 0000, and stays there - so the change is confined to the RAM path.
    const uint32_t banks = rom.chr_rom.empty() ? rom.chr_ram_size / 1024 : rom.chr_1k_bank_count;
    if (banks == 0) {
        return ppu_addr & 0x1FFF;
    }
    const uint32_t bank = chr_bank_for(ppu_addr) % banks;
    return bank * 1024 + (ppu_addr & 0x03FF);
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
