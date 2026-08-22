#pragma once
#include <cstdint>

class ROM;

// One cartridge board's decoding logic, as an object rather than as a run of
// `if (mapper_id == N)` branches inside ROM.
//
// WHY THIS EXISTS. Every mapper added a branch to each of ROM::read,
// ROM::write and ROM::chr_read, and a clause to the load-time header checks -
// so a board's behaviour was spread across four functions and interleaved with
// three other boards'. Four mappers was already twelve sites; MMC1 would have
// been the fifth, and it is the first with state that is not a latch (a 5-bit
// shift register clocked one bit per write, reset by bit 7 of the data). That
// does not read well as another branch beside CNROM's single assignment.
//
// WHAT IS AND IS NOT MOVED. The mapper holds a reference to its ROM and reads
// the register fields that still live there. That is deliberate for this step:
// the dispatch moves, the state does not, so nothing outside rom.cpp changes
// and the existing MMC3/UNROM suites are a real check that behaviour is
// identical rather than merely recompiled. Moving the state into the subclasses
// is a separate step with a separate blast radius - mmc3_tests.cpp alone pokes
// those fields twenty-five times.
//
// The PRG-RAM enable/protect bits stay on ROM for a further reason: Bus::decode
// reads them on every access to $6000-$7FFF, and it holds a ROM, not a Mapper.
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
