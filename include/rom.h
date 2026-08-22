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
//   6    flags6: bit0 mirroring, bit1 battery, bit2 trainer present,
//                bits4-7 mapper low nibble
//   7    flags7: bits2-3 == 10b means NES 2.0, bits4-7 mapper high nibble
//   8    NES 2.0: submapper and mapper bits 8-11. iNES 1.0: PRG-RAM in 8KB units
//   9    NES 2.0: the high nibbles of the PRG and CHR sizes
//   10   NES 2.0: PRG-RAM and PRG-NVRAM sizes, as shift counts
//   11   NES 2.0: CHR-RAM and CHR-NVRAM sizes, as shift counts
//   12-15 (unused by this loader: timing, system type, expansion device)
// followed by a 512-byte trainer if flags6 bit2 is set, then PRG-ROM, then CHR-ROM.
//
// Bytes 10 and 11 were "unused by this loader" until the Holy Mapperel mapper-1
// images arrived: all nine carry NES 2.0 headers, and the RAM sizes those ROMs
// exist to measure appear NOWHERE ELSE in the file. A loader that skips them
// gives every cartridge 8KB of work RAM, which is wrong for six of the nine.
class ROM : public Device
{
public:
    ROM(Bus* b) : Device{b} {};

    // How the four 1KB nametable slots at $2000-$2FFF map onto the console's
    // two screens of internal RAM. The first two are all the iNES header can
    // say, and all a fixed-wiring board can do; the single-screen modes are
    // MMC1 writing its control register, which is why this is an enum now and
    // was a bool for as long as no mapper could change it.
    enum class Mirroring : uint8_t {
        horizontal,
        vertical,
        single_screen_lower,
        single_screen_upper,
    };

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

    // Where a PPU address lands in the console's CHR-RAM array. Same mapping as
    // chr_read uses for CHR-ROM, because it is the same set of wires - see the
    // definition.
    uint32_t chr_ram_offset(const uint16_t addr) const;

    // Where a CPU address in $6000-$7FFF lands in the work RAM, after the
    // board's banking and after folding into the size the cartridge actually
    // declares. Bus::decode calls it; PrgRAM is a plain memory device and knows
    // nothing about either step.
    uint16_t prg_ram_offset(const uint16_t addr) const;

    MapperId mapper_id = MapperId::nrom;
    Mirroring mirroring = Mirroring::horizontal;

    // True when byte 7 bits 2-3 read 10b, which is the NES 2.0 marker. Only
    // then do bytes 9-11 mean what the table at the top of this file says; in
    // an iNES 1.0 image they are padding that some dumpers filled with junk, so
    // reading them unconditionally would size the RAM from a signature string.
    bool nes2 = false;

    // flags6 bit 1: the cartridge has a battery behind its PRG-RAM. Present in
    // both header versions.
    bool has_battery = false;

    // Work RAM at $6000-$7FFF, split by whether it survives power-off. NES 2.0
    // stores each as a shift count in one nibble of byte 10, where 0 means none
    // and n means 64 << n bytes.
    //
    // For an iNES 1.0 image both are unknowable, and the format's own advice is
    // to assume 8KB - so that is what load() writes, and every cartridge this
    // emulator handled before NES 2.0 parsing existed keeps exactly the window
    // it had. The behaviour only changes for images that say otherwise.
    // The default is 8KB rather than 0, and that is the same decision as the
    // iNES 1.0 branch in load(): a Bus with NO cartridge in it must decode
    // $6000-$7FFF exactly as it did before this field existed, or several
    // suites that never load a ROM lose their scratch memory. Only an NES 2.0
    // header can take the window away.
    uint32_t prg_ram_size = 8 * 1024;
    uint32_t prg_nvram_size = 0;

    // Pattern memory the cartridge supplies as RAM rather than ROM, byte 11,
    // same encoding. Read for completeness and for the loader's own checks; the
    // PPU's CHR-RAM is a fixed 8KB, which is every board here.
    uint32_t chr_ram_size = 0;

    // Is there anything at all behind $6000-$7FFF? Bus::decode asks, because a
    // board with no work RAM must read open bus there rather than a phantom
    // 8KB that only this emulator has. Six of the nine Holy Mapperel mapper-1
    // images are exactly that case, and detecting it is one of their tests.
    bool has_prg_ram() const { return prg_ram_size + prg_nvram_size > 0; }

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

    // A completed PPU pattern-table read, reported after the byte was served.
    // Only MMC2 and MMC4 care; see Mapper::observe_pattern_fetch for why the
    // ordering is load-bearing and why it is not folded into the call above.
    void observe_pattern_fetch(const uint16_t ppu_addr);

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
    Mmc3* as_mmc3() { return mapper_id == MapperId::mmc3 ? static_cast<Mmc3*>(mapper.get()) : nullptr; }

    // The board as an Mmc1, on the same terms and for the same reason.
    Mmc1* as_mmc1() { return mapper_id == MapperId::mmc1 ? static_cast<Mmc1*>(mapper.get()) : nullptr; }

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

    // Is this cartridge plugged into a console? The loader tests in
    // memory_tests.cpp deliberately build one that is not - parsing an iNES
    // header needs nothing else - so anything reaching for the Bus has to ask.
    bool has_bus() const { return bus != nullptr; }

    // The console's CPU cycle count, or 0 when has_bus() is false. Device::bus
    // is protected, and MMC1 is not a Device: it needs the clock to spot the
    // back-to-back writes a read-modify-write instruction produces, and this is
    // the same route drive_irq_line takes to reach the CPU.
    uint64_t cpu_cycle() const;
};
