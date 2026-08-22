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
