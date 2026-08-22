#pragma once
#include <cstdint>
#include <memory>
#include <string>

class ROM;
class Mapper;

// Every mapper number this emulator has a NAME for. Deliberately far longer
// than the list it has a BOARD for, and the two must not be conflated: what is
// implemented is decided by the board table in mapper.cpp and nowhere else.
//
// The point of carrying the unimplemented ones is the error message. "mapper 69
// (Sunsoft FME-7) is not supported" tells you what to go and read; "mapper 69"
// sends you to a wiki to find out what you even have. Adding a number here
// costs a table row and claims nothing.
//
// Numbers are the iNES assignments, so the gaps are real - they are the
// mappers nobody has bothered to name here yet, not an ordering.
enum class MapperId : uint16_t {
    nrom = 0,
    mmc1 = 1,
    uxrom = 2,
    cnrom = 3,
    mmc3 = 4,
    mmc5 = 5,
    axrom = 7,
    mmc2 = 9,
    mmc4 = 10,
    color_dreams = 11,
    cprom = 13,
    bandai_fcg = 16,
    jaleco_ss88006 = 18,
    namco_163 = 19,
    vrc4a = 21,
    vrc2a = 22,
    vrc4b = 23,
    vrc6a = 24,
    vrc4c = 25,
    vrc6b = 26,
    action53 = 28,
    unrom512 = 30,
    irem_g101 = 32,
    taito_tc0190 = 33,
    bnrom = 34,
    rambo1 = 64,
    gxrom = 66,
    sunsoft4 = 68,
    sunsoft_fme7 = 69,
    codemasters = 71,
    vrc3 = 73,
    vrc1 = 75,
    holy_diver = 78,
    nina003 = 79,
    vrc7 = 85,
    txsrom = 118,
    tqrom = 119,
    unrom_7408 = 180,
    dxrom = 206,
    namco_175 = 210,
};

// The board name for a mapper number, or nullptr when the table above has never
// heard of it. Naming is independent of support - most of these have no board.
const char* mapper_name(MapperId id);

// Whether a board exists for this mapper number. The ONE place a number is
// turned into an implementation is make_mapper below, and this asks the same
// table, so the two cannot drift apart the way a header check and a
// construction switch did.
bool mapper_supported(MapperId id);

// Empty when the header's bank counts are consistent with what the board can
// physically be wired as; otherwise the complaint, ready for load() to print.
// Returns empty for an unsupported mapper - mapper_supported answers that, and
// answering it twice would produce two different messages for one cause.
std::string mapper_header_error(MapperId id, size_t prg_16k_banks, size_t chr_8k_banks);

// Constructs the board, or returns null when there is none. Called once per
// cartridge by ROM::load.
std::unique_ptr<Mapper> make_mapper(MapperId id, ROM& rom);

// One cartridge board's decoding logic, as an object rather than as a run of
// `if (mapper_id == N)` branches inside ROM.
//
// WHY THIS EXISTS. Every mapper used to add a branch to each of ROM::read,
// ROM::write and ROM::chr_read, and a clause to the load-time header checks -
// so a board's behaviour was spread across four functions and interleaved with
// three other boards'. Four mappers was already twelve sites, and MMC1 was the
// one that made it untenable: its registers are not latches but a 5-bit shift
// register clocked one bit per write, which does not read as another branch
// beside CNROM's single assignment.
//
// WHAT STAYS ON ROM, and why it is not laziness. The header-derived sizes, the
// mirroring the PPU reads live, and the PRG-RAM enable/protect bits: Bus::decode
// consults the last of those on every access to $6000-$7FFF and it holds a ROM,
// not a Mapper. Everything that is one board's business - the register file,
// the mode bits, the MMC3's IRQ counter - lives on the subclass.
class Mapper
{
public:
    explicit Mapper(ROM& rom) : rom{rom} {}
    virtual ~Mapper() = default;

    Mapper(const Mapper&) = delete;
    Mapper& operator=(const Mapper&) = delete;

    // A CPU write into cartridge space. Only $8000-$FFFF reaches a mapper -
    // $6000-$7FFF is PRG-RAM, which is a separate Bus device.
    virtual void cpu_write(const uint16_t addr, const uint8_t data) = 0;

    // A CPU read from $8000-$FFFF, already known to have PRG-ROM behind it.
    virtual uint8_t prg_read(const uint16_t addr) const = 0;

    // A PPU read from $0000-$1FFF, already known to have CHR-ROM behind it.
    virtual uint8_t chr_read(const uint16_t addr) const = 0;

    // PPU address line A12, watched for the MMC3's scanline counter. A default
    // rather than a pure virtual because three of the four boards genuinely do
    // not have the wire - making them each write an empty override would state
    // the same thing four times and invite one of them to be filled in.
    virtual void observe_a12(const uint16_t ppu_addr, const uint64_t ppu_cycle)
    {
        (void)ppu_addr;
        (void)ppu_cycle;
    }

protected:
    ROM& rom;
};

// NROM (0): no banking at all. A 16KB image is mirrored across $8000-$FFFF and
// a 32KB one fills it.
class NRom final : public Mapper
{
public:
    using Mapper::Mapper;
    void cpu_write(const uint16_t addr, const uint8_t data) override;
    uint8_t prg_read(const uint16_t addr) const override;
    uint8_t chr_read(const uint16_t addr) const override;
};

// MMC1 (1): the SxROM family. The first board here whose registers are not
// latches - the CPU cannot present five bits at once, so writes are serialised
// through a shift register one bit at a time.
class Mmc1 final : public Mapper
{
public:
    explicit Mmc1(ROM& rom);

    void cpu_write(const uint16_t addr, const uint8_t data) override;
    uint8_t prg_read(const uint16_t addr) const override;
    uint8_t chr_read(const uint16_t addr) const override;

    // The serial port. Five writes carrying one bit each in bit 0 fill this,
    // and the fifth commits it to whichever register the LAST write's address
    // selects. Bit 7 of any write empties it instead.
    uint8_t shift = 0;
    uint8_t shift_count = 0;

    // $8000-$9FFF. bits 0-1 mirroring, bits 2-3 PRG mode, bit 4 CHR mode.
    //
    // Power-on is $0C and that value is load-bearing, not arbitrary: PRG mode 3
    // fixes the LAST bank at $C000-$FFFF, which is where the reset vector is
    // read from. Come up in any other mode and the console jumps into whatever
    // bank happens to be there. Hardware guarantees it, and the reset that bit
    // 7 of a write performs ORs $0C in for the same reason.
    uint8_t control = 0x0C;

    // $A000-$BFFF and $C000-$DFFF: 5-bit CHR bank numbers, 4KB each. On boards
    // larger than 256KB of PRG or 8KB of work RAM these registers carry more
    // than CHR - see prg_bank_offset and wram_disabled.
    uint8_t chr_reg[2] = {0, 0};

    // $E000-$FFFF: bits 0-3 the 16KB PRG bank, bit 4 the work-RAM disable.
    uint8_t prg_reg = 0;

    // $E000 bit 4, and on SNROM also $A000 bit 4: hold the work RAM disabled so
    // a dying power rail cannot corrupt a save. Inverted from the register bit,
    // which is a disable rather than an enable.
    bool wram_disabled() const { return (prg_reg & 0x10) != 0; }

    // bits 2-3 of control.
    uint8_t prg_mode() const { return static_cast<uint8_t>((control >> 2) & 0x03); }

    // bit 4 of control: 0 switches 8KB of CHR at once, 1 switches two 4KB
    // halves independently.
    bool chr_4k_mode() const { return (control & 0x10) != 0; }

    // Which 16KB PRG bank and 4KB CHR bank the given address currently reads
    // through. Pure functions of the registers, so they can be reasoned about
    // and tested without driving a read.
    uint32_t prg_bank_offset(const uint16_t cpu_addr) const;
    uint32_t chr_bank_offset(const uint16_t ppu_addr) const;

private:
    // Commits a completed 5-bit value to the register the address selects, and
    // applies whatever side effects it has outside this class - mirroring lives
    // on ROM because the PPU reads it live.
    void write_register(const uint16_t addr, const uint8_t value);

    void apply_mirroring();

    // MMC1 ignores a write that lands on the CPU cycle immediately after
    // another one. That is not an optimisation: a read-modify-write instruction
    // targeting $8000-$FFFF puts two writes back to back, and games rely on
    // only the FIRST being seen. Without this, an INC $E000 clocks two bits in
    // and desynchronises the shift register for every write that follows.
    //
    // Held as an absolute cycle rather than a flag so that "the previous cycle"
    // stays true across whatever else the bus did in between.
    uint64_t last_write_cycle = 0;
    bool wrote_before = false;
};

// UNROM (2): a switchable 16KB PRG window at $8000-$BFFF, with $C000-$FFFF
// wired to the last bank so the vectors cannot be banked away. No CHR-ROM - the
// console's CHR-RAM is the pattern memory.
class UnRom final : public Mapper
{
public:
    using Mapper::Mapper;
    void cpu_write(const uint16_t addr, const uint8_t data) override;
    uint8_t prg_read(const uint16_t addr) const override;
    uint8_t chr_read(const uint16_t addr) const override;
};

// CNROM (3): NROM's PRG with a switchable 8KB CHR window.
class CnRom final : public Mapper
{
public:
    using Mapper::Mapper;
    void cpu_write(const uint16_t addr, const uint8_t data) override;
    uint8_t prg_read(const uint16_t addr) const override;
    uint8_t chr_read(const uint16_t addr) const override;
};

// MMC3 (4): eight bank registers behind a select/data pair, runtime mirroring,
// PRG-RAM gating, and an IRQ counter clocked by PPU A12. The register semantics
// are documented on the fields in rom.h, which is still where they live.
class Mmc3 final : public Mapper
{
public:
    using Mapper::Mapper;
    void cpu_write(const uint16_t addr, const uint8_t data) override;
    uint8_t prg_read(const uint16_t addr) const override;
    uint8_t chr_read(const uint16_t addr) const override;
    void observe_a12(const uint16_t ppu_addr, const uint64_t ppu_cycle) override;

    // Eight bank registers behind a select/data pair, rather than a single
    // latch. $8000 chooses which of R0-R7 the next $8001 write lands in, and
    // also carries the two mode bits that decide WHICH window each register
    // drives:
    //
    //   R0, R1   2KB CHR banks   (bit 0 ignored - "R0 and R1 ignore the bottom bit")
    //   R2-R5    1KB CHR banks
    //   R6, R7   8KB PRG banks   (top two bits ignored - only 6 PRG address lines)
    //
    // The two mode bits invert which half of the address space the switchable
    // windows occupy, which is why they are kept rather than folded in at write
    // time: a later mode change has to re-point the existing register values.
    uint8_t bank_select = 0;  // the last value written to $8000
    uint8_t bank[8] = {0};    // R0-R7

    // $8000 bit 6: 0 -> $8000-$9FFF switchable, $C000-$DFFF fixed to second-last
    //              1 -> $C000-$DFFF switchable, $8000-$9FFF fixed to second-last
    bool prg_mode_swapped() const { return (bank_select & 0x40) != 0; }

    // $8000 bit 7: swaps the 2KB and 1KB CHR windows between $0000 and $1000.
    bool chr_a12_inverted() const { return (bank_select & 0x80) != 0; }

    // --- scanline IRQ counter -----------------------------------------------
    //
    // The MMC3 has no idea what a scanline is. It counts RISING EDGES OF PPU
    // ADDRESS LINE A12, and a scanline happens to produce exactly one of them
    // in the usual configuration: the background fetches sit in one pattern
    // table and the sprite fetches in the other, so A12 goes up once per line
    // when the fetch phase crosses over. Games get "interrupt at scanline N" by
    // counting those edges - which is why a game that puts both tables on the
    // same side gets no IRQs at all, and why the counter is clocked just as
    // happily by a program poking $2006 with rendering switched off.
    //
    // blargg's readme for the IRQ ROMs is explicit about that second case:
    // "The ROMs mainly test behavior by manually clocking the MMC3's IRQ
    // counter by writing to $2006 to change the current VRAM address." A design
    // that hooked only the rendering fetches would pass none of them.
    uint8_t irq_latch = 0;            // $C000, the value reloaded into the counter
    uint8_t irq_counter = 0;          // the live count
    bool irq_reload_pending = false;  // set by $C001, consumed by the next edge
    bool irq_enabled = false;         // $E001 enables, $E000 disables
    bool irq_asserted = false;        // are we currently pulling /IRQ low?

    // The A12 line as the mapper last saw it, and the PPU dot at which it went
    // low. Only the duration matters, so one timestamp is enough.
    bool a12_high = false;
    uint64_t a12_low_since = 0;

    // A rising edge only counts if A12 has been low for a while first. NESdev:
    // the counter is "triggered on a rising edge after the line has remained
    // low for three falling edges of M2" - M2 is the CPU clock, so three of its
    // cycles, and the PPU runs three times faster, giving nine PPU dots.
    //
    // The filter is not an optimisation, it is what makes the count mean
    // "scanline". Within one background tile fetch A12 drops for exactly four
    // dots (the nametable and attribute reads at $2xxx, between two pattern
    // reads at $1xxx); without the filter every tile would clock the counter
    // and an IRQ meant for one scanline would arrive 32 times a line.
    //
    // Nine rather than four-plus-one because the margin has to survive both
    // sides: real clocks follow low periods of 16 dots and up, so there is a
    // wide gap between "noise" and "signal" and the threshold only has to land
    // inside it. 3-A12_clocking is what pins this - see mmc3_rom_tests.cpp.
    static constexpr uint64_t a12_filter_dots = 9;

private:
    // Which 1KB CHR bank the given PPU address currently reads through, and
    // which 8KB PRG bank the given CPU address does. Split out because both are
    // pure functions of the register file and the mode bits, and are far easier
    // to test and to reason about than the same arithmetic inlined into the
    // read paths.
    uint16_t chr_bank_for(const uint16_t ppu_addr) const;
    uint16_t prg_bank_for(const uint16_t cpu_addr) const;

    // One filtered rising edge of A12: reload or decrement, then decide whether
    // to pull /IRQ low. Split out from observe_a12 so the edge DETECTION and the
    // counter SEMANTICS can be read - and mutated - independently.
    void clock_irq_counter();
};
