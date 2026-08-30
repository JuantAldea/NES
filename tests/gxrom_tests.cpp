// Guards GxROM (mapper 66): one register, a 32KB PRG window and an 8KB CHR
// window, both moved by the same write.
//
//     xxPP xxCC     PP = 32KB PRG bank at $8000, CC = 8KB CHR bank at $0000
//
// THE FIELD SPLIT IS THE WHOLE BOARD, and it is what these tests are for. Holy
// Mapperel's M66_P64K_C16K_V identifies the cartridge and reports 0000, which is
// a stronger statement about the banking than anything below - but that image
// carries only two PRG banks and two CHR banks, so it exercises ONE bit of each
// field. A register that read PRG from bits 5-4 and CHR from bits 5-4 as well
// would satisfy it.
//
// So the images here are the full size the register can address - four banks of
// each - and the decisive test writes a byte whose two fields differ.
//
// Each bank is filled with its own index, as in unrom_tests.cpp, so a failure
// names the bank it found rather than a magic byte.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "../include/bus.h"
#include "gtest/gtest.h"

namespace tests
{
namespace gxrom
{
namespace
{

constexpr size_t kPrgBankSize = 32 * 1024;
constexpr size_t kChrBankSize = 8 * 1024;

// `prg32` banks of 32KB and `chr8` banks of 8KB, bank N filled with the byte N.
struct GxRomImage {
    GxRomImage(const std::string& name, uint8_t prg32, uint8_t chr8)
        : path(std::string(NES_TEST_FILES_DIR) + "/" + name)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        // Mapper 66 is $42: low nibble 2 into flags6's high nibble, high nibble
        // 4 into flags7's. The header counts PRG in 16KB units, so double.
        const uint8_t header[16] = {'N', 'E', 'S', 0x1A, static_cast<uint8_t>(prg32 * 2), chr8, 0x20, 0x40, 0, 0, 0, 0,
                                    0,   0,   0,   0};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));

        for (uint8_t b = 0; b < prg32; ++b) {
            const std::vector<uint8_t> bank(kPrgBankSize, b);
            out.write(reinterpret_cast<const char*>(bank.data()), bank.size());
        }
        for (uint8_t b = 0; b < chr8; ++b) {
            const std::vector<uint8_t> bank(kChrBankSize, b);
            out.write(reinterpret_cast<const char*>(bank.data()), bank.size());
        }
    }
    ~GxRomImage() { std::remove(path.c_str()); }

    GxRomImage(const GxRomImage&) = delete;
    GxRomImage& operator=(const GxRomImage&) = delete;

    std::string path;
};

}  // namespace

GTEST_TEST(gxrom, loads_and_reports_both_bank_counts)
{
    GxRomImage rom("gxrom_load.nes", 4, 4);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    EXPECT_EQ(MapperId::gxrom, console.rom.mapper_id);
    EXPECT_EQ(8, console.rom.prg_bank_count) << "counted in 16KB units, so four 32KB banks read as eight";
    EXPECT_EQ(4, console.rom.chr_bank_count);
    EXPECT_FALSE(console.rom.chr_rom.empty()) << "unlike UNROM, this board carries CHR-ROM";
}

GTEST_TEST(gxrom, bits_five_and_four_select_a_thirty_two_kilobyte_prg_window)
{
    GxRomImage rom("gxrom_prg.nes", 4, 4);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    for (uint8_t bank = 0; bank < 4; ++bank) {
        console.write(0x8000, static_cast<uint8_t>(bank << 4));
        EXPECT_EQ(bank, console.read(0x8000)) << "start of the window";
        EXPECT_EQ(bank, console.read(0xFFFF)) << "end of it - one 32KB window, so the vectors move with "
                                                 "everything else and there is no fixed half";
    }
}

GTEST_TEST(gxrom, bits_one_and_zero_select_an_eight_kilobyte_chr_window)
{
    GxRomImage rom("gxrom_chr.nes", 4, 4);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    for (uint8_t bank = 0; bank < 4; ++bank) {
        console.write(0x8000, bank);
        EXPECT_EQ(bank, console.ppu.ppu_bus_read(0x0000)) << "start of the pattern window";
        EXPECT_EQ(bank, console.ppu.ppu_bus_read(0x1FFF)) << "end of it";
    }
}

// THE TEST THE ORACLE CANNOT BE: one write, two fields, different values.
//
// Every assertion above moves one field with the other at zero, so a register
// that decoded both from the same bits would pass all of them. This is the one
// that separates them, and it needs four banks of each - which is why these
// images are larger than the Holy Mapperel one that already passes.
GTEST_TEST(gxrom, the_two_fields_are_independent)
{
    GxRomImage rom("gxrom_both.nes", 4, 4);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    // PRG 3, CHR 1 - deliberately different, and neither of them zero.
    console.write(0x8000, static_cast<uint8_t>((3 << 4) | 1));
    EXPECT_EQ(3, console.read(0x8000)) << "PRG follows bits 5-4";
    EXPECT_EQ(1, console.ppu.ppu_bus_read(0x0000)) << "CHR follows bits 1-0, independently";

    // And the other way round, so neither field can be reading the other's bits.
    console.write(0x8000, static_cast<uint8_t>((1 << 4) | 3));
    EXPECT_EQ(1, console.read(0x8000));
    EXPECT_EQ(3, console.ppu.ppu_bus_read(0x0000));
}

GTEST_TEST(gxrom, the_bits_above_the_two_fields_are_ignored)
{
    GxRomImage rom("gxrom_mask.nes", 4, 4);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    // $CC is 1100 1100: the two field positions hold 0 and 0, everything else is
    // set. A board decoding more bits than it has would move somewhere.
    console.write(0x8000, 0xCC);
    EXPECT_EQ(0, console.read(0x8000)) << "bits 7-6 are not part of the PRG field";
    EXPECT_EQ(0, console.ppu.ppu_bus_read(0x0000)) << "bits 3-2 are not part of the CHR field";
}

GTEST_TEST(gxrom, an_image_without_chr_rom_is_rejected)
{
    // Switching CHR is half of what the register does, so a CHR-less image is
    // not this board whatever its header says - the mirror of the check UNROM
    // makes in the opposite direction.
    GxRomImage rom("gxrom_no_chr.nes", 4, 0);
    Bus console;
    EXPECT_FALSE(console.load_cartridge(rom.path));
}

GTEST_TEST(gxrom, a_chr_count_the_register_cannot_address_is_rejected)
{
    // Two bits reach four banks. Eight would make the bank-count mask in
    // cpu_write silently drop the top bit rather than fail.
    GxRomImage rom("gxrom_too_much_chr.nes", 4, 8);
    Bus console;
    EXPECT_FALSE(console.load_cartridge(rom.path));
}

}  // namespace gxrom
}  // namespace tests
