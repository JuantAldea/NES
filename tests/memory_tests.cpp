#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#include "../include/bus.h"
#include "../include/ram.h"
#include "../include/rom.h"
#include "gtest/gtest.h"

namespace tests
{

static_assert(SystemRAM::SIZE == 2048, "internal RAM must be 2048 bytes (2KB), not 64KB");

// --- RAM mirroring, symmetric in both directions ---------------------------

GTEST_TEST(testMemory, ram_mirror_read_direction)
{
    Bus console;
    console.write(0x0000, 0xAA);
    EXPECT_EQ(0xAA, console.read(0x0800));
    EXPECT_EQ(0xAA, console.read(0x1000));
    EXPECT_EQ(0xAA, console.read(0x1800));
}

GTEST_TEST(testMemory, ram_mirror_write_direction)
{
    // Previously write() bypassed mirroring entirely, so this direction was
    // broken: write(0x1800, ...) would not alias read(0x0000).
    Bus console;
    console.write(0x1800, 0xBB);
    EXPECT_EQ(0xBB, console.read(0x0000));
    EXPECT_EQ(0xBB, console.read(0x0800));
    EXPECT_EQ(0xBB, console.read(0x1000));
}

// --- PPU register mirroring, including the $3FFF edge ----------------------

GTEST_TEST(testMemory, ppu_register_mirroring_reaches_2007_mirror_at_3fff)
{
    // $2000-$3FFF mirrors every 8 bytes; $3FFF should land on the same
    // register as $2007 (PPUDATA), not fall through to RAM. The original
    // decode used `addr < 0x3FFF`, which excluded $3FFF from the PPU range.
    Bus console;
    // $2006 writes are ignored during the post-reset lockout. This test is
    // about address decoding, not lockout timing, so get past it first.
    while (console.ppu.in_reset_write_lockout()) {
        console.ppu.clock();
    }
    console.write(PPU::PPUADDR, 0x00);
    console.write(PPU::PPUADDR, 0x10);
    console.write(PPU::PPUDATA, 0x42);

    // Rewind PPUADDR and prime the read buffer: $2007 reads below $3F00 are
    // delayed by one, so the first read returns the stale latch and refills it.
    // Rewind again for the read that matters, which goes through the $3FFF
    // mirror rather than $2007 directly - that mirror is what this test is
    // about.
    console.write(PPU::PPUADDR, 0x00);
    console.write(PPU::PPUADDR, 0x10);
    console.read(0x3FFF);  // priming read, also through the mirror

    console.write(PPU::PPUADDR, 0x00);
    console.write(PPU::PPUADDR, 0x10);
    EXPECT_EQ(0x42, console.read(0x3FFF));
}

// --- $6000-$7FFF PRG-RAM round trip -----------------------------------------

GTEST_TEST(testMemory, prg_ram_round_trip)
{
    Bus console;
    console.write(0x6000, 0x11);
    console.write(0x7FFF, 0x22);
    EXPECT_EQ(0x11, console.read(0x6000));
    EXPECT_EQ(0x22, console.read(0x7FFF));

    // Not mirrored: distinct addresses in this range are independent bytes.
    console.write(0x6001, 0x33);
    EXPECT_EQ(0x11, console.read(0x6000));
    EXPECT_EQ(0x33, console.read(0x6001));

    // Pins the offset as (addr - 0x6000) specifically. A plausible wrong
    // implementation masking with 0x0FFF would alias these two, 4KB apart.
    console.write(0x6000, 0xAA);
    console.write(0x7000, 0xBB);
    EXPECT_EQ(0xAA, console.read(0x6000));
    EXPECT_EQ(0xBB, console.read(0x7000));
}

// write_ram is a bulk convenience, not a separate address path: it must agree
// with write(). It previously went straight at the RAM array with the raw
// address, so a bulk write starting in a mirror silently wrote nothing.
GTEST_TEST(testMemory, write_ram_agrees_with_write_through_mirrors)
{
    const uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};

    Bus base;
    Bus mirrored;

    base.write_ram(0x0400, sizeof(payload), payload);
    mirrored.write_ram(0x0C00, sizeof(payload), payload);  // $0C00 mirrors $0400

    for (uint16_t i = 0; i < sizeof(payload); ++i) {
        EXPECT_EQ(payload[i], base.read(0x0400 + i)) << "base region, byte " << i;
        EXPECT_EQ(payload[i], mirrored.read(0x0400 + i)) << "written via mirror, byte " << i;
        EXPECT_EQ(payload[i], mirrored.read(0x0C00 + i)) << "readback via mirror, byte " << i;
    }
}

// A freshly constructed PPU must not report a spurious vblank or sprite-0 hit
// out of indeterminate register memory.
//
// Constructing a plain `Bus` is NOT a valid test of this: fresh allocations are
// usually already zero, so the test passes whether or not PPU::registers has an
// initializer. Placement-new over deliberately poisoned storage makes the check
// deterministic - it fails reliably if that initializer is removed.
GTEST_TEST(testMemory, ppu_status_is_deterministic_over_poisoned_storage)
{
    alignas(PPU) unsigned char storage[sizeof(PPU)];
    std::memset(storage, 0xCD, sizeof(storage));

    PPU* ppu = new (storage) PPU(nullptr);
    const uint8_t status = ppu->read(PPU::PPUSTATUS);
    ppu->~PPU();

    EXPECT_EQ(0x00, status & 0xE0) << "vblank/sprite0/overflow read back from uninitialized register memory";
}

// --- iNES / NROM cartridge loading ------------------------------------------

std::string nestest_path() { return std::string(NES_TEST_FILES_DIR) + "/nestest.nes"; }

GTEST_TEST(testMemory, load_nestest_header_fields)
{
    ROM rom(nullptr);
    ASSERT_TRUE(rom.load(nestest_path()));
    EXPECT_EQ(16384u, rom.prg_rom.size());
    EXPECT_EQ(8192u, rom.chr_rom.size());
    EXPECT_EQ(0, rom.mapper_id);
}

GTEST_TEST(testMemory, load_nestest_through_bus)
{
    Bus console;
    ASSERT_TRUE(console.load_cartridge(nestest_path()));

    EXPECT_EQ(0x4C, console.read(0xC000));
    EXPECT_EQ(0xF5, console.read(0xC001));
    EXPECT_EQ(0xC5, console.read(0xC002));

    EXPECT_EQ(0x04, console.read(0xFFFC));
    EXPECT_EQ(0xC0, console.read(0xFFFD));

    // nestest.nes is a 16KB PRG-ROM image: $8000-$BFFF mirrors $C000-$FFFF.
    EXPECT_EQ(console.read(0xC000), console.read(0x8000));
}

// Builds an iNES image on disk. Caller supplies the header bytes and the bank
// payloads, so a test can pin exactly one header field at a time.
struct SyntheticRom {
    explicit SyntheticRom(const std::string& name) : path(std::string(NES_TEST_FILES_DIR) + "/" + name) {}
    ~SyntheticRom() { std::remove(path.c_str()); }

    SyntheticRom(const SyntheticRom&) = delete;
    SyntheticRom& operator=(const SyntheticRom&) = delete;

    // prg_fill/chr_fill let a test prove which region of the file a byte came
    // from, which is what makes the trainer-offset check meaningful.
    void write(uint8_t prg_banks,
               uint8_t chr_banks,
               uint8_t flags6,
               uint8_t flags7,
               uint8_t prg_fill = 0xAA,
               uint8_t chr_fill = 0xBB,
               bool include_banks = true)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const uint8_t header[16] = {'N', 'E', 'S', 0x1A, prg_banks, chr_banks, flags6, flags7, 0, 0, 0, 0, 0, 0, 0, 0};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));

        if (!include_banks) {
            return;
        }

        if (flags6 & 0x04) {
            // 512-byte trainer, filled with a value distinct from the PRG data
            // so that misapplying the offset is detectable.
            const std::vector<uint8_t> trainer(512, 0x5A);
            out.write(reinterpret_cast<const char*>(trainer.data()), trainer.size());
        }

        const std::vector<uint8_t> prg(prg_banks * 16u * 1024u, prg_fill);
        out.write(reinterpret_cast<const char*>(prg.data()), prg.size());
        const std::vector<uint8_t> chr(chr_banks * 8u * 1024u, chr_fill);
        out.write(reinterpret_cast<const char*>(chr.data()), chr.size());
    }

    std::string path;
};

GTEST_TEST(testMemory, reject_bad_magic)
{
    // Self-contained rather than reusing an unrelated fixture: pointing this at
    // another test's file would make it pass on "could not open" if that file
    // ever moved, i.e. pass for the wrong reason.
    const std::string path = std::string(NES_TEST_FILES_DIR) + "/bad_magic_test.nes";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const uint8_t header[16] = {'N', 'O', 'P', 0xFF, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        const std::vector<uint8_t> body(24 * 1024, 0);
        out.write(reinterpret_cast<const char*>(body.data()), body.size());
    }

    ROM rom(nullptr);
    EXPECT_FALSE(rom.load(path));
    EXPECT_FALSE(rom.loaded());

    std::remove(path.c_str());
}

// The trainer is 512 bytes between the header and the PRG data. Getting the
// offset wrong shifts every PRG byte, which is invisible unless the trainer and
// the PRG carry different values.
GTEST_TEST(testMemory, trainer_offset_is_skipped)
{
    SyntheticRom rom_file("trainer_test.nes");
    rom_file.write(/*prg_banks=*/1, /*chr_banks=*/1, /*flags6=*/0x04, /*flags7=*/0, /*prg_fill=*/0xAA);

    ROM rom(nullptr);
    ASSERT_TRUE(rom.load(rom_file.path));
    ASSERT_EQ(16384u, rom.prg_rom.size());
    EXPECT_EQ(0xAA, rom.prg_rom.front()) << "PRG data starts at the trainer's bytes: offset not applied";
    EXPECT_EQ(0xAA, rom.prg_rom.back());
}

// The truncation check must account for the CHR banks too, not just PRG.
GTEST_TEST(testMemory, reject_file_truncated_within_chr)
{
    const std::string path = std::string(NES_TEST_FILES_DIR) + "/short_chr_test.nes";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        // Header claims 1 PRG + 1 CHR bank, but only the PRG bank follows.
        const uint8_t header[16] = {'N', 'E', 'S', 0x1A, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        const std::vector<uint8_t> prg(16 * 1024, 0);
        out.write(reinterpret_cast<const char*>(prg.data()), prg.size());
    }

    ROM rom(nullptr);
    EXPECT_FALSE(rom.load(path)) << "CHR shortfall not detected: truncation check ignores chr_size";
    EXPECT_FALSE(rom.loaded());

    std::remove(path.c_str());
}

// Mapper id is (flags7 & 0xF0) | (flags6 >> 4). Only exercising the flags6
// nibble leaves the high half of that expression untested.
GTEST_TEST(testMemory, reject_mapper_encoded_in_flags7_high_nibble)
{
    SyntheticRom rom_file("mapper16_test.nes");
    // flags6 nibble 0, flags7 high nibble 1 -> mapper 16.
    rom_file.write(/*prg_banks=*/1, /*chr_banks=*/1, /*flags6=*/0x00, /*flags7=*/0x10);

    ROM rom(nullptr);
    EXPECT_FALSE(rom.load(rom_file.path)) << "mapper id ignores the flags7 high nibble";
    EXPECT_FALSE(rom.loaded());
}

GTEST_TEST(testMemory, mirroring_flag_is_decoded)
{
    SyntheticRom horizontal("mirror_h_test.nes");
    horizontal.write(1, 1, /*flags6=*/0x00, 0);
    ROM rom_h(nullptr);
    ASSERT_TRUE(rom_h.load(horizontal.path));
    EXPECT_TRUE(rom_h.horizontal_mirroring) << "flags6 bit 0 clear means horizontal mirroring";

    SyntheticRom vertical("mirror_v_test.nes");
    vertical.write(1, 1, /*flags6=*/0x01, 0);
    ROM rom_v(nullptr);
    ASSERT_TRUE(rom_v.load(vertical.path));
    EXPECT_FALSE(rom_v.horizontal_mirroring) << "flags6 bit 0 set means vertical mirroring";
}

// NROM addresses at most 32KB of PRG. A larger image would otherwise load with
// most of it permanently unreachable and no error reported.
GTEST_TEST(testMemory, reject_prg_bank_count_outside_nrom)
{
    SyntheticRom too_big("prg3_test.nes");
    too_big.write(/*prg_banks=*/3, /*chr_banks=*/1, 0, 0);

    ROM rom(nullptr);
    EXPECT_FALSE(rom.load(too_big.path));
    EXPECT_FALSE(rom.loaded());
}

// A failed load must not leave the previous cartridge resident while still
// reporting loaded(). Every other ROM test uses a fresh object, so nothing
// covered this documented guarantee.
GTEST_TEST(testMemory, failed_load_after_success_fully_unloads)
{
    ROM rom(nullptr);
    ASSERT_TRUE(rom.load(nestest_path()));
    ASSERT_TRUE(rom.loaded());
    ASSERT_EQ(16384u, rom.prg_rom.size());

    EXPECT_FALSE(rom.load(std::string(NES_TEST_FILES_DIR) + "/does_not_exist.nes"));
    EXPECT_FALSE(rom.loaded()) << "failed load left the previous cartridge resident";
    EXPECT_TRUE(rom.prg_rom.empty());
    EXPECT_TRUE(rom.chr_rom.empty());
}

// $4018-$401F and $4020-$5FFF have no device. Nothing asserted that they read
// as open bus rather than aliasing into RAM or PRG-RAM.
//
// $4016 and $4017 USED to be in this list, and their removal is not a test
// being relaxed to fit the code. They were unmapped only because the
// controllers were unimplemented, so the assertion recorded a gap in this
// emulator rather than a property of the NES - on hardware $4016 is the
// controller strobe and both are readable controller ports. The gap is now
// closed and controller_tests.cpp asserts what they actually do.
//
// $4018-$401F stay: those are CPU test registers, disabled on a retail NES.
GTEST_TEST(testMemory, unmapped_ranges_are_open_bus)
{
    Bus console;

    // Seed a known byte in RAM to read through, as the marker below.
    console.write(0x0010, 0x5A);

    // The technique had to change when open bus became real. This used to write
    // $FF to the unmapped address and expect the read back to be 0 - but 0 was
    // the SIMPLIFICATION, not the hardware. A write puts its value on the data
    // bus, so on a real NES reading straight back returns $FF whether or not
    // anything is there, and the old test could no longer tell the two apart.
    //
    // So: write $FF, then read a known address to put something else on the
    // bus, THEN read the unmapped one. A device that latched the write returns
    // $FF; genuine open bus returns the marker.
    for (const uint16_t addr : {0x4018, 0x401A, 0x401F, 0x4020, 0x5000, 0x5FFF}) {
        console.write(addr, 0xFF);
        ASSERT_EQ(0x5A, console.read(0x0010)) << "marker read failed; the rest of this test is meaningless";

        EXPECT_EQ(0x5A, console.read(addr))
            << "$" << std::hex << addr << std::dec
            << " returned the value written to it, so something is backing it; expected open bus";
    }

    // And the discarded writes must not have landed anywhere observable.
    console.write(0x0000, 0x11);
    console.write(0x6000, 0x22);
    EXPECT_EQ(0x11, console.read(0x0000));
    EXPECT_EQ(0x22, console.read(0x6000));
}

GTEST_TEST(testMemory, reject_missing_file)
{
    ROM rom(nullptr);
    EXPECT_FALSE(rom.load(std::string(NES_TEST_FILES_DIR) + "/does_not_exist.nes"));
    EXPECT_FALSE(rom.loaded());
}

GTEST_TEST(testMemory, reject_truncated_file)
{
    // A well-formed 16-byte iNES header claiming 1 PRG bank (16KB) and 1 CHR
    // bank (8KB), but with no bank data following it at all.
    const std::string path = std::string(NES_TEST_FILES_DIR) + "/truncated_test.nes";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const uint8_t header[16] = {'N', 'E', 'S', 0x1A, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
    }

    ROM rom(nullptr);
    EXPECT_FALSE(rom.load(path));
    EXPECT_FALSE(rom.loaded());

    std::remove(path.c_str());
}

GTEST_TEST(testMemory, reject_nonzero_mapper)
{
    const std::string path = std::string(NES_TEST_FILES_DIR) + "/mapper1_test.nes";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        // flags6 high nibble = 0x1 -> mapper id low nibble 1 (mapper 1, MMC1).
        const uint8_t header[16] = {'N', 'E', 'S', 0x1A, 1, 1, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        std::vector<uint8_t> prg(16 * 1024, 0);
        std::vector<uint8_t> chr(8 * 1024, 0);
        out.write(reinterpret_cast<const char*>(prg.data()), prg.size());
        out.write(reinterpret_cast<const char*>(chr.data()), chr.size());
    }

    ROM rom(nullptr);
    EXPECT_FALSE(rom.load(path));
    EXPECT_FALSE(rom.loaded());

    std::remove(path.c_str());
}

GTEST_TEST(testMemory, reject_zero_prg_banks)
{
    const std::string path = std::string(NES_TEST_FILES_DIR) + "/zero_prg_test.nes";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const uint8_t header[16] = {'N', 'E', 'S', 0x1A, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        std::vector<uint8_t> chr(8 * 1024, 0);
        out.write(reinterpret_cast<const char*>(chr.data()), chr.size());
    }

    ROM rom(nullptr);
    EXPECT_FALSE(rom.load(path));
    EXPECT_FALSE(rom.loaded());

    std::remove(path.c_str());
}

// --- PPU register access must never abort (Task 4 regression test) --------

GTEST_TEST(testMemory, ppu_and_apu_register_sweep_does_not_abort)
{
    Bus console;
    for (uint32_t addr = 0x2000; addr <= 0x3FFF; ++addr) {
        console.read(static_cast<uint16_t>(addr));
        console.write(static_cast<uint16_t>(addr), 0x00);
    }
    for (uint32_t addr = 0x4000; addr <= 0x401F; ++addr) {
        console.read(static_cast<uint16_t>(addr));
        console.write(static_cast<uint16_t>(addr), 0x00);
    }
    SUCCEED();
}

}  // namespace tests
