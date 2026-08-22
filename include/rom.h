#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "device.h"
#include "mapper.h"

// iNES (.nes) cartridge loader for NROM (0), UNROM (2), CNROM (3) and MMC3 (4).
//
// The first three are one latch each. CNROM is NROM plus a switchable CHR
// window: a write anywhere in $8000-$FFFF latches which 8KB CHR-ROM bank the
// PPU sees, and PRG behaves exactly as NROM's does. UNROM is the mirror image -
// a switchable 16KB PRG window with $C000-$FFFF wired down so the vectors
// cannot be banked away.
//
// MMC3 is the first with real state: a register file, mode bits, mirroring
// changed at runtime, and a counter that watches the PPU's address bus. See the
// MMC3 section below.
//
// iNES header layout (16 bytes):
//   0-3  magic "NES\x1A"
//   4    PRG-ROM size in 16KB units
//   5    CHR-ROM size in 8KB units (0 means CHR-RAM, not handled here)
//   6    flags6: bit0 mirroring, bit2 trainer present, bits4-7 mapper low nibble
//   7    flags7: bits4-7 mapper high nibble
//   8-15 (unused by this loader)
// followed by a 512-byte trainer if flags6 bit2 is set, then PRG-ROM, then CHR-ROM.
class ROM : public Device
{
public:
    ROM(Bus* b) : Device{b} {};

    // Parses an iNES (.nes) file and loads it as an NROM (mapper 0) cartridge.
    // Returns false (and leaves the cartridge unloaded) on any parse error or
    // unsupported mapper. A failed load never leaves prg_rom/chr_rom partially
    // populated: on any error path they are left (or reset to) empty.
    bool load(const std::string& path);

    bool loaded() const { return !prg_rom.empty(); }

    void write(const uint16_t addr, const uint8_t data);
    uint8_t read(const uint16_t addr);

    bool has_chr_rom() const { return !chr_rom.empty(); }

    // Reads pattern-table space ($0000-$1FFF) through the currently selected
    // CHR bank. The PPU must go through this rather than indexing chr_rom
    // directly, or a CNROM cartridge would be stuck on bank 0 forever.
    uint8_t chr_read(const uint16_t addr) const;

    uint8_t mapper_id = 0;
    bool horizontal_mirroring = true;

    // Which 8KB CHR-ROM bank the PPU currently sees, and how many exist.
    // NROM always has at most one, so both stay 0/1 and chr_read() reduces to
    // a plain index.
    uint8_t chr_bank = 0;
    uint8_t chr_bank_count = 0;

    // UNROM's switchable 16KB PRG window at $8000-$BFFF, and how many banks the
    // cartridge carries. $C000-$FFFF is always the last bank, so the vectors
    // cannot be switched away.
    uint8_t prg_bank = 0;
    uint8_t prg_bank_count = 0;

    // $A001: bit 7 enables the PRG-RAM chip, bit 6 write-protects it.
    //
    // Read by Bus::decode, which is the only place they can be honoured: the
    // mapper owns the bits, but the RAM behind $6000-$7FFF is a separate Bus
    // device (PrgRAM) that knows nothing about mappers. Disabled decodes to no
    // device at all, so reads are open bus and writes are dropped; write-
    // protected decodes to no device on writes only.
    //
    // These were stored and consulted by nothing for as long as MMC3 existed
    // here, and the reason is worth keeping: no test ROM in the suite covers
    // them, so the omission was invisible - green everywhere, enforced nowhere.
    // What covers them now is mmc3_tests.cpp, written from the register
    // description rather than from an oracle, which is the weaker of the two
    // kinds of evidence this project runs on. Treat a game that misbehaves
    // around save RAM as evidence about THESE lines first.
    bool prg_ram_enabled = true;
    bool prg_ram_write_protected = false;

    // Called by the PPU on every access that drives its EXTERNAL address bus,
    // and on the $2006 write that commits a new address to v. `ppu_cycle` is
    // PPU::total_cycles, which is the only clock both sides share.
    void mmc3_observe_a12(const uint16_t ppu_addr, const uint64_t ppu_cycle);

    // The board this cartridge is on, chosen by load() from the header's mapper
    // id. Null until a cartridge is loaded, and every read/write path checks
    // prg_rom/chr_rom emptiness before dereferencing it - loaded() is defined as
    // "prg_rom is not empty", so the two cannot disagree.
    std::unique_ptr<Mapper> mapper;

    // The board as an Mmc3, or null if this cartridge is on any other board.
    //
    // Exists for tests that reach into the register file and the IRQ counter -
    // white-box checks written from the register description, which is how the
    // PRG-RAM gating and the A12 filter are covered at all. Putting those
    // members on Mapper instead would give NROM an IRQ counter, so the cast is
    // the honest option: only one board has them.
    Mmc3* as_mmc3() { return mapper_id == 4 ? static_cast<Mmc3*>(mapper.get()) : nullptr; }

    // How many 8KB PRG banks and 1KB CHR banks the cartridge carries. MMC3
    // indexes in those units, unlike UNROM's 16KB and CNROM's 8KB.
    uint16_t prg_8k_bank_count = 0;
    uint16_t chr_1k_bank_count = 0;

    std::vector<uint8_t> prg_rom;
    std::vector<uint8_t> chr_rom;

    // Drives the cartridge's bit of the CPU's /IRQ line. Every path that can
    // change the assertion goes through here, so there is one place where the
    // wire is driven rather than four.
    //
    // Takes the state rather than reading it because the counter that decides it
    // now lives on Mmc3, and the wire does not: /IRQ is open-drain and shared
    // with the APU's frame counter and the DMC, so it is the console's, reached
    // through Device's Bus pointer. Hence CPU::set_IRQ_line rather than
    // raise_IRQ - the cartridge releasing its own bit must not drop theirs.
    void drive_irq_line(const bool asserted);
};
