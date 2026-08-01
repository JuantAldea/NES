#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "bus.h"
#include "gtest/gtest.h"
#include "ram.h"
#include "rom.h"

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
    console.write(PPU::PPUADDR, 0x00);
    console.write(PPU::PPUADDR, 0x10);
    console.write(PPU::PPUDATA, 0x42);

    // Rewind PPUADDR the same way and read back through the $3FFF mirror.
    console.write(PPU::PPUADDR, 0x00);
    console.write(PPU::PPUADDR, 0x10);
    // A dummy PPUDATA read is required on real hardware (buffered read), but
    // this PPU implementation returns VRAM contents directly, so read once.
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

GTEST_TEST(testMemory, reject_bad_magic)
{
    ROM rom(nullptr);
    EXPECT_FALSE(rom.load(std::string(NES_TEST_FILES_DIR) + "/6502_functional_test.bin"));
    EXPECT_FALSE(rom.loaded());
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
