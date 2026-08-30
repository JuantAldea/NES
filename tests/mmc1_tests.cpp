// MMC1 (mapper 1), against synthetic images.
//
// The board is covered by nine Holy Mapperel images, which is the strongest
// evidence in this repository about any mapper - they identify SGROM, SFROM,
// SJROM, SLROM, SKROM, SUROM and SXROM by behaviour alone. What no oracle here
// covers is what happens to an image whose SIZE the board cannot address.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "../include/bus.h"
#include "gtest/gtest.h"

namespace tests
{
namespace mmc1
{
namespace
{

constexpr size_t kPrgBankSize = 16 * 1024;
constexpr size_t kChrBankSize = 8 * 1024;

// An MMC1 image whose PRG bank N is filled with the byte N, and whose CHR bank N
// likewise. `chr8` of zero means CHR-RAM, which is what SGROM and SUROM carry.
struct Mmc1Image {
    Mmc1Image(const std::string& name, uint8_t prg16, uint8_t chr8) : path(std::string(NES_TEST_FILES_DIR) + "/" + name)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        // Mapper 1: low nibble into flags6's high nibble, high nibble is zero.
        const uint8_t header[16] = {'N', 'E', 'S', 0x1A, prg16, chr8, 0x10, 0x00, 0, 0, 0, 0, 0, 0, 0, 0};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));

        for (uint8_t b = 0; b < prg16; ++b) {
            const std::vector<uint8_t> bank(kPrgBankSize, b);
            out.write(reinterpret_cast<const char*>(bank.data()), bank.size());
        }
        for (uint8_t b = 0; b < chr8; ++b) {
            const std::vector<uint8_t> bank(kChrBankSize, b);
            out.write(reinterpret_cast<const char*>(bank.data()), bank.size());
        }
    }
    ~Mmc1Image() { std::remove(path.c_str()); }

    Mmc1Image(const Mmc1Image&) = delete;
    Mmc1Image& operator=(const Mmc1Image&) = delete;

    std::string path;
};

// MMC1 takes five writes, least-significant bit first, and the register is
// chosen by the address of the FIFTH one. Consecutive-cycle writes are filtered,
// but only when exactly one cycle apart - these are all in the same cycle, so
// they land.
void serial_write(Bus& console, const uint16_t addr, const uint8_t value)
{
    for (int bit = 0; bit < 5; ++bit) {
        console.write(addr, static_cast<uint8_t>((value >> bit) & 1));
    }
}

}  // namespace

// A PRG SIZE THE MASK CANNOT DECODE IS REJECTED, and this test exists because a
// 24-bank image used to load and lose half its banks.
//
// prg_bank_offset masks with `banks - 1` and its comment calls that mask "the
// whole of the address decoding a real board performs" - true only for a power of
// two, which the check did not require. MEASURED on 24 banks, mask 23 = 0b10111:
//
//     select 0..7    -> banks 0..7    correct
//     select 8..15   -> banks 0..7    ALIASED, bit 3 is clear in the mask
//
// Half the reachable range, silently. The same defect as UNROM's, in the mapper
// with the largest commercial catalogue on the system.
GTEST_TEST(mmc1, a_prg_bank_count_that_is_not_a_power_of_two_is_rejected)
{
    Mmc1Image rom("mmc1_twentyfour.nes", 24, 0);
    Bus console;
    EXPECT_FALSE(console.load_cartridge(rom.path)) << "384K cannot be masked to twenty-four banks unambiguously";

    // Both ends of the range, so this cannot pass by rejecting everything. 2 is
    // the smallest board and 32 is SUROM, whose fifth bank bit comes from a CHR
    // register rather than the PRG one.
    Mmc1Image smallest("mmc1_two.nes", 2, 0);
    Bus a;
    EXPECT_TRUE(a.load_cartridge(smallest.path)) << "32K is two banks, the smallest SxROM";

    Mmc1Image largest("mmc1_thirtytwo.nes", 32, 0);
    Bus b;
    EXPECT_TRUE(b.load_cartridge(largest.path)) << "512K is SUROM, the largest";
}

// THE CHR SIDE WAS NOT CHECKED AT ALL. check_mmc1 opened with a (void) cast on
// its chr_8k_banks argument and never looked at it, while chr_offset masks CHR
// exactly as prg_bank_offset masks PRG - `bank & (banks - 1)`, over a count of
// 4KB banks that is chr_bank_count doubled.
//
// So a 24KB image gave six 4KB banks, mask 5 = 0b101, and banks 2 and 3 aliased
// onto 0 and 1.
GTEST_TEST(mmc1, a_chr_bank_count_that_is_not_a_power_of_two_is_rejected)
{
    Mmc1Image rom("mmc1_three_chr.nes", 8, 3);
    Bus console;
    EXPECT_FALSE(console.load_cartridge(rom.path)) << "24K of CHR cannot be masked to six 4KB banks";

    Mmc1Image too_much("mmc1_much_chr.nes", 8, 32);
    Bus big;
    EXPECT_FALSE(big.load_cartridge(too_much.path)) << "256K exceeds the five CHR bank bits";

    // CHR-RAM stays legal - it is what SGROM and SUROM carry, and chr_offset
    // banks it from chr_ram_size instead of from the header.
    Mmc1Image chr_ram("mmc1_chr_ram.nes", 8, 0);
    Bus a;
    EXPECT_TRUE(a.load_cartridge(chr_ram.path)) << "no CHR-ROM means CHR-RAM, which is a real SxROM";

    Mmc1Image chr_rom("mmc1_chr_rom.nes", 8, 16);
    Bus b;
    EXPECT_TRUE(b.load_cartridge(chr_rom.path)) << "128K is the ceiling and must still load";
}

// The positive case, so the rejections above are not the only thing asserted:
// every bank of a legal image is reachable and distinct.
GTEST_TEST(mmc1, every_bank_of_a_legal_image_is_reachable)
{
    Mmc1Image rom("mmc1_eight.nes", 8, 0);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    // Reset leaves PRG in mode 3: $8000 switchable, $C000 fixed to the last bank.
    for (uint8_t bank = 0; bank < 8; ++bank) {
        serial_write(console, 0xE000, bank);
        EXPECT_EQ(bank, console.read(0x8000)) << "selecting bank " << int(bank);
        EXPECT_EQ(7, console.read(0xC000)) << "the fixed window is the last bank, whatever is selected";
    }
}

}  // namespace mmc1
}  // namespace tests
