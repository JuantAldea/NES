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
    //
    // A POWER OF TWO, for the reason check_cnrom and check_axrom already give
    // and this one used to omit: cpu_write latches `data & (prg_bank_count - 1)`,
    // which is only a bank-select mask when the count is a power of two.
    //
    // MEASURED before the clause was added. A 12-bank image loaded, and the mask
    // 11 - 0b1011 - made banks 4 through 7 alias onto 0 through 3: a third of the
    // cartridge unreachable, silently, while 8 through 11 worked by coincidence.
    // Nothing reported anything.
    //
    // Rejecting rather than switching the mask to a modulo, because the mask IS
    // the board: a real UNROM decodes as many bank lines as its ROM has, mask
    // ambiguity is the hardware's too, and no such cartridge was made - ROM parts
    // come in powers of two. A modulo would invent correct behaviour for a board
    // that cannot exist.
    //
    // TWO MUTANTS OF THIS LINE SURVIVE AND BOTH ARE EQUIVALENT, worked out rather
    // than assumed. `> 16` becoming `> 17` can only differ at exactly 17, which
    // the power-of-two clause rejects anyway. And `n & (n - 1)` becoming
    // `n & (n - 2)` agrees with it on EVERY value from 2 to 16 - checked one by
    // one - which is the whole domain that reaches it, because the two clauses
    // before have already excluded anything smaller or larger. The two differ
    // only at n = 1, and n = 1 never gets here.
    if (prg_16k_banks < 2 || prg_16k_banks > 16 || (prg_16k_banks & (prg_16k_banks - 1)) != 0) {
        return "UNROM requires 2, 4, 8 or 16 PRG-ROM banks, header advertises " + std::to_string(prg_16k_banks);
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

std::string check_txsrom(const size_t prg_16k_banks, const size_t chr_8k_banks)
{
    // The MMC3's PRG constraint, restated rather than delegated to check_mmc3 so
    // the message names the board the header actually claims.
    if (prg_16k_banks < 2) {
        return "TxSROM requires at least 2 PRG-ROM banks, header advertises " + std::to_string(prg_16k_banks);
    }
    // CHR-RAM is what makes no sense here, not merely what no board shipped: the
    // whole board IS a CHR address line rerouted, so a cartridge with no CHR-ROM
    // has nothing to reroute and its nametables would follow a bank register
    // indexing an array that does not exist.
    if (chr_8k_banks == 0) {
        return "TxSROM has no CHR-RAM variant - CHR A17 is what drives its nametables, header advertises 0 CHR banks";
    }
    // And 128KB is the ceiling for the same reason. The MMC3's 1KB bank
    // registers are eight bits wide, which would address 256KB, but bit 7 is A17
    // and this board spends it on CIRAM A10 - leaving seven bits of 1KB banks.
    if (chr_8k_banks > 16) {
        return "TxSROM addresses at most 128KB of CHR-ROM (A17 drives CIRAM A10 instead), header advertises " +
               std::to_string(chr_8k_banks) + " bank(s)";
    }
    return {};
}

std::string check_unrom_7408(const size_t prg_16k_banks, const size_t chr_8k_banks)
{
    // The same shape as UNROM's, because it is the same cartridge with two wires
    // swapped: CHR-RAM only, and a PRG size the 4-bit register can reach.
    if (chr_8k_banks != 0) {
        return "UNROM 7408 has no CHR-ROM, header advertises " + std::to_string(chr_8k_banks) + " bank(s)";
    }
    if (prg_16k_banks < 2 || prg_16k_banks > 16 || (prg_16k_banks & (prg_16k_banks - 1)) != 0) {
        return "UNROM 7408 requires 2, 4, 8 or 16 PRG-ROM banks, header advertises " + std::to_string(prg_16k_banks);
    }
    return {};
}

std::string check_gxrom(const size_t prg_16k_banks, const size_t chr_8k_banks)
{
    // Two bits of PRG and two of CHR, so four 32KB banks and four 8KB banks at
    // the very most. Both masks in the implementation are bank-count masks, so
    // both counts have to be non-zero powers of two for the decoding to be
    // unambiguous - the same requirement AxROM's check states for its PRG.
    const size_t banks32 = prg_16k_banks / 2;
    if (prg_16k_banks % 2 != 0 || banks32 == 0 || banks32 > 4 || (banks32 & (banks32 - 1)) != 0) {
        return "GxROM requires 2, 4 or 8 PRG-ROM banks (32KB to 128KB in 32KB steps), header advertises " +
               std::to_string(prg_16k_banks);
    }
    // Unlike UNROM and AxROM, this board DOES carry CHR-ROM - switching it is
    // half the register's job - so an image without any is not a GxROM.
    //
    // THE CLAUSES OVERLAP, which is why mutating `> 4` to `> 5` survives in both
    // this test and the PRG one above: the only count between them is 5, and the
    // power-of-two clause rejects it first. Deliberately not tightened - each
    // clause states one independent thing about the board, and collapsing them
    // into the minimum set that happens to be distinguishable would make the
    // rule harder to read for no gain in what it accepts.
    if (chr_8k_banks == 0 || chr_8k_banks > 4 || (chr_8k_banks & (chr_8k_banks - 1)) != 0) {
        return "GxROM requires 1, 2 or 4 CHR-ROM banks (8KB to 32KB), header advertises " +
               std::to_string(chr_8k_banks);
    }
    return {};
}

std::string check_axrom(const size_t prg_16k_banks, const size_t chr_8k_banks)
{
    // No CHR-ROM: CHR-RAM is the pattern memory, as on UNROM.
    if (chr_8k_banks != 0) {
        return "AxROM has no CHR-ROM, header advertises " + std::to_string(chr_8k_banks) + " bank(s)";
    }
    // It switches 32KB at a time, so the header's 16KB count has to be even -
    // and a power of two once halved, because prg_read masks with the bank
    // count rather than dividing. Real boards are 32KB to 256KB, which is 1 to
    // 8 banks of 32KB. A 96KB image would make that mask ambiguous and is not a
    // size any AxROM cartridge was made in.
    const size_t banks32 = prg_16k_banks / 2;
    if (prg_16k_banks % 2 != 0 || banks32 == 0 || banks32 > 8 || (banks32 & (banks32 - 1)) != 0) {
        return "AxROM requires 2, 4, 8 or 16 PRG-ROM banks (32KB to 256KB in 32KB steps), header advertises " +
               std::to_string(prg_16k_banks);
    }
    return {};
}

std::string check_latched_chr(const char* board, const size_t prg_16k_banks, const size_t chr_8k_banks)
{
    // Both boards page CHR in 4KB windows through 5-bit registers, so 32 banks
    // of 4KB - 128KB - is the ceiling, and CHR-ROM is not optional: the latch
    // has nothing to switch without it.
    if (chr_8k_banks == 0 || chr_8k_banks > 16) {
        return std::string(board) + " requires 1 to 16 CHR-ROM banks (8KB to 128KB), header advertises " +
               std::to_string(chr_8k_banks);
    }
    // Four PRG bits, and both boards wire part of the space down, so there has
    // to be more than one bank to switch between.
    if (prg_16k_banks < 2 || prg_16k_banks > 16) {
        return std::string(board) + " requires 2 to 16 PRG-ROM banks, header advertises " +
               std::to_string(prg_16k_banks);
    }
    return {};
}

std::string check_mmc2(const size_t prg_16k_banks, const size_t chr_8k_banks)
{
    return check_latched_chr("MMC2", prg_16k_banks, chr_8k_banks);
}

std::string check_mmc4(const size_t prg_16k_banks, const size_t chr_8k_banks)
{
    return check_latched_chr("MMC4", prg_16k_banks, chr_8k_banks);
}

std::string check_fme7(const size_t prg_16k_banks, const size_t chr_8k_banks)
{
    // 8KB PRG windows through 6-bit registers, so 64 banks - 512KB - is the
    // ceiling, and there must be enough to fill the four windows.
    if (prg_16k_banks < 2 || prg_16k_banks > 32) {
        return "FME-7 requires 2 to 32 PRG-ROM banks, header advertises " + std::to_string(prg_16k_banks);
    }
    // 1KB CHR windows through 8-bit registers: 256 banks, 256KB.
    if (chr_8k_banks == 0 || chr_8k_banks > 32) {
        return "FME-7 requires 1 to 32 CHR-ROM banks, header advertises " + std::to_string(chr_8k_banks);
    }
    return {};
}

struct Board {
    MapperId id;
    std::unique_ptr<Mapper> (*make)(ROM&);
    std::string (*check)(size_t prg_16k_banks, size_t chr_8k_banks);
};

const Board kBoards[] = {
    {MapperId::nrom, construct<NRom>, check_nrom},
    {MapperId::mmc1, construct<Mmc1>, check_mmc1},
    {MapperId::uxrom, construct<UnRom>, check_uxrom},
    {MapperId::cnrom, construct<CnRom>, check_cnrom},
    {MapperId::mmc3, construct<Mmc3>, check_mmc3},
    {MapperId::axrom, construct<AxRom>, check_axrom},
    {MapperId::mmc2, construct<Mmc2>, check_mmc2},
    {MapperId::mmc4, construct<Mmc4>, check_mmc4},
    {MapperId::sunsoft_fme7, construct<Fme7>, check_fme7},
    {MapperId::txsrom, construct<TxSRom>, check_txsrom},
    {MapperId::gxrom, construct<GxRom>, check_gxrom},
    {MapperId::unrom_7408, construct<UnRom7408>, check_unrom_7408},
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

// One 8KB chip behind the window, which is every board but SOROM and SXROM.
uint16_t Mapper::prg_ram_offset(const uint16_t cpu_addr) const { return static_cast<uint16_t>(cpu_addr - 0x6000); }

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

uint16_t Mmc1::prg_ram_offset(const uint16_t cpu_addr) const
{
    // SOROM and SXROM carry 16KB and 32KB of work RAM, more than the
    // $6000-$7FFF window shows, and select the 8KB bank with bits 2-3 of the
    // CHR register that drives the $0000 window - the same register whose bit 4
    // supplies PRG A18 on SUROM. One register, three jobs, because MMC1 ran out
    // of pins long before it ran out of things to switch.
    //
    // chr_reg[0] specifically, in both CHR modes. In 8KB mode it is the only
    // CHR register that does anything; in 4KB mode it is the one driving
    // $0000, which is how NESdev specifies the WRAM select and how the PRG A18
    // select above already reads it.
    //
    // No clamp here: a board with one 8KB chip leaves these lines unconnected,
    // and ROM::prg_ram_offset folding to the declared size is what reproduces
    // that. Masking here as well would state the same rule in two places and
    // make the SUROM case - 512KB PRG, 8KB WRAM, bits 2-3 live for neither -
    // silently depend on which clamp ran first.
    const uint32_t bank = (chr_reg[0] >> 2) & 0x03;
    return static_cast<uint16_t>(bank * 8 * 1024 + (cpu_addr & 0x1FFF));
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

// --- CNROM (3) --------------------------------------------------------------

// A write anywhere in $8000-$FFFF latches the low bits as the CHR bank number.
// Bus conflicts are not modelled, for the same reason as UNROM's.
// Identical to UnRom's - the boards differ only in which window is fixed.
//
// BOTH HALVES OF THE GUARD ARE UNREACHABLE, and are kept anyway. Mutation leaves
// them alive: `addr >= 0x8000` because Bus only routes cartridge-space writes
// here in the first place, and `prg_bank_count != 0` because check_unrom_7408
// has already refused any image with fewer than two banks. They are a defence
// against a future caller rather than against any present one, which is the only
// honest reason to keep a branch no test can enter. GxRom::cpu_write's
// chr_bank_count check is the same case.
void UnRom7408::cpu_write(const uint16_t addr, const uint8_t data)
{
    if (addr >= 0x8000 && rom.prg_bank_count != 0) {
        rom.prg_bank = static_cast<uint8_t>(data & (rom.prg_bank_count - 1));
    }
}

uint8_t UnRom7408::prg_read(const uint16_t addr) const
{
    // The mirror of UnRom: bank 0 is fixed low and the selected bank sits high,
    // where UNROM fixes the LAST bank high and selects low.
    const size_t bank = (addr < 0xC000) ? 0u : rom.prg_bank;
    return rom.prg_rom[bank * PRG_ROM_BANK_SIZE + (addr & 0x3FFF)];
}

void GxRom::cpu_write(const uint16_t addr, const uint8_t data)
{
    if (addr < 0x8000) {
        return;
    }
    bank_select = data;

    // CHR goes through rom.chr_bank and the base chr_offset, exactly as CNROM's
    // does. Masked by the bank count rather than by 0x03, because a real board
    // decodes only as many lines as it has banks - the two-bank image here would
    // otherwise be able to select banks that are not on the cartridge.
    if (rom.chr_bank_count != 0) {
        rom.chr_bank = static_cast<uint8_t>((data & 0x03u) & (rom.chr_bank_count - 1));
    }
}

uint8_t GxRom::prg_read(const uint16_t addr) const
{
    // 32KB at a time, so the header's 16KB count is halved - the same conversion
    // AxROM does, and the load-time check guarantees the result is a non-zero
    // power of two so the mask is the whole of the decoding.
    const uint32_t banks = rom.prg_bank_count / 2u;
    const uint32_t bank = ((bank_select >> 4) & 0x03u) & (banks - 1u);
    return rom.prg_rom[bank * 32u * 1024u + (addr & 0x7FFFu)];
}

void CnRom::cpu_write(const uint16_t addr, const uint8_t data)
{
    if (addr >= 0x8000 && rom.chr_bank_count != 0) {
        rom.chr_bank = static_cast<uint8_t>(data & (rom.chr_bank_count - 1));
    }
}

// PRG behaves exactly as NROM's does.
uint8_t CnRom::prg_read(const uint16_t addr) const { return rom.prg_rom[(addr - 0x8000) % rom.prg_rom.size()]; }

// --- MMC2 (9) and MMC4 (10), the shared half -------------------------------

// Registers are decoded from the top nibble only, so $A000 and $AFFF are the
// same register - the board compares four address lines and no more.
void LatchedChr::cpu_write(const uint16_t addr, const uint8_t data)
{
    switch (addr & 0xF000) {
    case 0xA000:
        prg_bank = data & 0x0F;
        break;
    case 0xB000:
        chr_bank[0][0] = data & 0x1F;
        break;
    case 0xC000:
        chr_bank[0][1] = data & 0x1F;
        break;
    case 0xD000:
        chr_bank[1][0] = data & 0x1F;
        break;
    case 0xE000:
        chr_bank[1][1] = data & 0x1F;
        break;
    case 0xF000:
        // Mirroring, live - PPU::nametable_offset reads it on every access.
        //
        // POLARITY DECIDED BY THE ORACLE, not by reading a table, because the
        // MMC3 register was inverted here for as long as it was taken from a
        // wiki row written in ARRANGEMENT terms rather than mirroring ones.
        //
        // MEASURED both ways. Bit 0 clear meaning VERTICAL is what identifies
        // the boards - 009 PNROM and 010 F*ROM. Flipped, neither image renders
        // a report at all inside 400 frames: mapper detection runs from RAM and
        // hangs when a probe answers wrongly, so it never reaches the screen.
        // A louder failure than MMC3's, which at least printed a wrong board.
        rom.mirroring = (data & 0x01) ? ROM::Mirroring::horizontal : ROM::Mirroring::vertical;
        break;
    default:
        // $8000-$9FFF decodes nothing on either board.
        break;
    }
}

uint32_t LatchedChr::chr_offset(const uint16_t ppu_addr) const
{
    const uint32_t window = (ppu_addr & 0x1000) ? 1u : 0u;
    const uint32_t banks = static_cast<uint32_t>(rom.chr_bank_count) * 2u;
    if (banks == 0) {
        return ppu_addr & 0x1FFF;
    }

    const uint32_t bank = chr_bank[window][latch[window]] % banks;
    return bank * CHR_ROM_4K_BANK_SIZE + (ppu_addr & 0x0FFF);
}

// Tiles $FD and $FE, in either window, swap that window's bank for the NEXT
// fetch. PPU::ppu_bus_read calls this after serving the byte, which is what
// makes "next" true - see the comment at that call site.
//
// $0FD8 is tile $FD's last byte plane: the fetch pipeline reads the low plane
// at $xFD0-$xFD7 and the high plane at $xFD8-$xFDF, so the trigger sits at the
// END of the tile and the swap lands between tiles rather than inside one.
void LatchedChr::observe_pattern_fetch(const uint16_t ppu_addr)
{
    uint8_t which = 0;

    if ((ppu_addr & 0x1000) == 0) {
        if (lower_window_latches(ppu_addr, which)) {
            latch[0] = which;
        }
        return;
    }

    // The upper window decodes eight-byte runs on both boards.
    if (ppu_addr >= 0x1FD8 && ppu_addr <= 0x1FDF) {
        latch[1] = 0;
    } else if (ppu_addr >= 0x1FE8 && ppu_addr <= 0x1FEF) {
        latch[1] = 1;
    }
}

// MMC2: an 8KB switchable window and three fixed banks. The fixed three are the
// LAST three of the cartridge, so the vectors cannot be banked away and neither
// can the code that does the banking.
uint8_t Mmc2::prg_read(const uint16_t addr) const
{
    const uint32_t banks = rom.prg_8k_bank_count;
    const uint32_t bank = addr < 0xA000 ? (prg_bank % banks) : (banks - (4u - ((addr >> 13) & 3u)));

    return rom.prg_rom[bank * 8u * 1024u + (addr & 0x1FFFu)];
}

// MMC2 compares the whole address in the lower window: exactly $0FD8 and
// exactly $0FE8, where MMC4 takes a run. Punch-Out!! is the only game on the
// board and it does not depend on the difference, but the boards are not the
// same part and this is where they differ.
bool Mmc2::lower_window_latches(const uint16_t ppu_addr, uint8_t& which) const
{
    if (ppu_addr == 0x0FD8) {
        which = 0;
        return true;
    }
    if (ppu_addr == 0x0FE8) {
        which = 1;
        return true;
    }
    return false;
}

// MMC4: UNROM's PRG layout - 16KB switchable at $8000, last bank fixed at
// $C000.
uint8_t Mmc4::prg_read(const uint16_t addr) const
{
    const uint32_t banks = rom.prg_bank_count;
    const uint32_t bank = addr < 0xC000 ? (prg_bank % banks) : (banks - 1u);

    return rom.prg_rom[bank * PRG_ROM_BANK_SIZE + (addr & 0x3FFFu)];
}

bool Mmc4::lower_window_latches(const uint16_t ppu_addr, uint8_t& which) const
{
    if (ppu_addr >= 0x0FD8 && ppu_addr <= 0x0FDF) {
        which = 0;
        return true;
    }
    if (ppu_addr >= 0x0FE8 && ppu_addr <= 0x0FEF) {
        which = 1;
        return true;
    }
    return false;
}

// --- FME-7 (69) -------------------------------------------------------------

// Two ports and nothing else: $8000-$9FFF latches the command, $A000-$BFFF
// supplies the parameter. $C000-$FFFF is the Sunsoft 5B's audio pair on boards
// that carry the sound chip and decodes nothing here.
void Fme7::cpu_write(const uint16_t addr, const uint8_t data)
{
    switch (addr & 0xE000) {
    case 0x8000:
        command = data & 0x0F;
        break;
    case 0xA000:
        write_register(data);
        break;
    default:
        break;
    }
}

void Fme7::write_register(const uint8_t value)
{
    if (command <= 0x07) {
        chr[command] = value;
        return;
    }

    switch (command) {
    case 0x08:
        prg_ram_control = value;
        // BIT 6 SELECTS, BIT 7 ENABLES - in that order, which is the reverse of
        // the reading that "RAM/ROM select bit, RAM enable bit" invites. Three
        // states out of the two, and they are not a hierarchy:
        //
        //   bit 6 clear             $6000 shows PRG-ROM bank (value & $3F)
        //   bit 6 set, bit 7 set    $6000 shows work RAM
        //   bit 6 set, bit 7 clear  $6000 shows nothing - open bus
        //
        // The ordering is not a guess: Holy Mapperel's global.inc names all
        // three values, and only this assignment of the bits explains them -
        // FME7_PRGBANK_ROM $00, FME7_PRGBANK_OFF $40, FME7_PRGBANK_RAM $C0. Read
        // the bits the other way round and $40 is a second ROM encoding with no
        // "off" state at all, which the constant's own name rules out.
        //
        // Getting it backwards is not a silent error, it is a WRONG-KIND error:
        // the $6000 window answers with the last PRG bank where it should read
        // open bus, so the oracle's WRAM digit reports MAPTEST_WRAMEN rather
        // than nothing. That is how this was found.
        //
        // Both flags are written on every pass because Bus::decode reads them
        // independently: leaving prg_ram_enabled true while selecting ROM would
        // leave work RAM in a window the board just gave away.
        rom.prg_rom_at_6000 = (value & 0x40) == 0;
        rom.prg_ram_enabled = (value & 0xC0) == 0xC0;
        break;

    case 0x09:
    case 0x0A:
    case 0x0B:
        prg[command - 0x09] = value & 0x3F;
        break;

    case 0x0C:
        // The only board here that can select all four mirroring modes from one
        // register, in the order the hardware numbers them.
        switch (value & 0x03) {
        case 0:
            rom.mirroring = ROM::Mirroring::vertical;
            break;
        case 1:
            rom.mirroring = ROM::Mirroring::horizontal;
            break;
        case 2:
            rom.mirroring = ROM::Mirroring::single_screen_lower;
            break;
        default:
            rom.mirroring = ROM::Mirroring::single_screen_upper;
            break;
        }
        break;

    case 0x0D:
        irq_counter_enabled = (value & 0x80) != 0;
        irq_enabled = (value & 0x01) != 0;
        // Writing this register acknowledges a pending assertion. There is no
        // separate acknowledge port, so a handler that only reloads the counter
        // and returns would take the same interrupt forever.
        irq_asserted = false;
        rom.drive_irq_line(false);
        break;

    case 0x0E:
        irq_counter = static_cast<uint16_t>((irq_counter & 0xFF00) | value);
        break;

    default:  // 0x0F
        irq_counter = static_cast<uint16_t>((irq_counter & 0x00FF) | (static_cast<uint16_t>(value) << 8));
        break;
    }
}

// One M2 cycle. Unlike the MMC3's counter this is not filtered, not tied to the
// PPU, and does not stop at zero: it wraps and keeps going.
void Fme7::clock_cpu_cycle()
{
    if (!irq_counter_enabled) {
        return;
    }

    // Post-decrement, so `before == 0` is exactly the $0000 -> $FFFF wrap that
    // fires the interrupt. Testing the NEW value against $FFFF would fire one
    // cycle late and also fire on a counter deliberately loaded with $FFFF.
    const uint16_t before = irq_counter--;
    if (before == 0 && irq_enabled && !irq_asserted) {
        irq_asserted = true;
        rom.drive_irq_line(true);
    }
}

uint8_t Fme7::prg_read(const uint16_t addr) const
{
    const uint32_t banks = rom.prg_8k_bank_count;

    // Four 8KB windows, not three: Bus::decode sends $6000-$7FFF here when
    // command $8 has selected ROM, and that window's bank is the low six bits
    // of the same register that selected it.
    //
    // $E000-$FFFF is wired to the last bank; $8000/$A000/$C000 are the $9/$A/$B
    // registers in order.
    uint32_t bank;
    if (addr < 0x8000) {
        bank = (prg_ram_control & 0x3Fu) % banks;
    } else if (addr >= 0xE000) {
        bank = banks - 1u;
    } else {
        bank = prg[(addr - 0x8000u) / 0x2000u] % banks;
    }

    return rom.prg_rom[bank * 8u * 1024u + (addr & 0x1FFFu)];
}

uint32_t Fme7::chr_offset(const uint16_t ppu_addr) const
{
    const uint32_t banks = rom.chr_1k_bank_count;
    if (banks == 0) {
        return ppu_addr & 0x1FFF;
    }

    // Eight registers, one per 1KB of pattern space - the finest CHR granularity
    // of any board here, matching the MMC3's but without its mode bits.
    const uint32_t bank = chr[(ppu_addr >> 10) & 0x07] % banks;
    return bank * 1024u + (ppu_addr & 0x03FFu);
}

// --- AxROM (7) --------------------------------------------------------------

AxRom::AxRom(ROM& cartridge) : Mapper{cartridge}
{
    // The board is physically one-screen: CIRAM A10 comes from a register bit,
    // not from a PPU address line, so the iNES header's horizontal/vertical bit
    // describes nothing that exists here. Overriding it at construction rather
    // than waiting for the first register write is what stops the PPU spending
    // the first frames mirroring a way this cartridge cannot be wired.
    //
    // Lower rather than upper because bank_select powers up at 0 and bit 4 is
    // the screen select; hardware does not specify the power-on value, but the
    // register and the field have to agree and 0 is what the register holds.
    rom.mirroring = ROM::Mirroring::single_screen_lower;
}

// One latch, from a write anywhere in cartridge space - the board decodes
// nothing finer.
//
// Bus conflicts are not modelled, the same call UNROM and CNROM make and for a
// stronger reason here: AOROM uses a 74HC161 with a '32 to gate the ROM's
// output, so it genuinely has none. ANROM and AMROM do, and cartridges written
// for them avoid the hazard by storing the bank number at the address they
// write to, which makes the AND a no-op for correct software either way.
void AxRom::cpu_write(const uint16_t addr, const uint8_t data)
{
    if (addr < 0x8000) {
        return;
    }

    bank_select = data;

    // Bit 4, live: PPU::nametable_offset reads ROM::mirroring on every access,
    // so switching screens needs nothing else told.
    rom.mirroring = (data & 0x10) ? ROM::Mirroring::single_screen_upper : ROM::Mirroring::single_screen_lower;
}

uint8_t AxRom::prg_read(const uint16_t addr) const
{
    // prg_bank_count is in 16KB units because that is what the iNES header
    // counts; this board switches 32KB at a time, so halve it. The load-time
    // check guarantees the result is a non-zero power of two, which is what
    // makes the mask below the whole of the decoding - a real board decodes
    // only as many bank lines as it has banks.
    const uint32_t banks = rom.prg_bank_count / 2u;
    const uint32_t bank = (bank_select & 0x07u) & (banks - 1u);

    return rom.prg_rom[bank * 32u * 1024u + (addr & 0x7FFFu)];
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

// --- TxSROM (118) -----------------------------------------------------------

// Bit 7 of whichever CHR bank register covers the slot, which on this board is
// CHR A17 and is soldered to CIRAM A10.
//
// The `screen >> 1` and `2 + screen` are the whole board. Everything else -
// banking, the IRQ counter, the register file - is inherited from Mmc3
// untouched, because on hardware it is the same chip.
uint8_t TxSRom::ciram_page(const uint16_t screen) const
{
    // Only the registers currently driving $0000-$0FFF have their A17 on the
    // CIRAM line, so the $8000 inversion bit chooses which set answers. With the
    // 2KB banks low it is R0 and R1, each covering two slots because each covers
    // two 1KB CHR banks; with the 1KB banks low it is R2-R5, one slot each.
    const uint8_t reg = chr_a12_inverted() ? bank[2 + screen] : bank[screen >> 1];

    return static_cast<uint8_t>((reg >> 7) & 1);
}
