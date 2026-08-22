// Guards UNROM (mapper 2): a switchable 16KB PRG window at $8000-$BFFF and a
// fixed last bank at $C000-$FFFF.
//
// Each bank in the synthetic images below is filled with its own index, so an
// assertion names the bank it expects rather than a magic byte. That is the
// whole trick that makes these tests readable: "reading $8000 gives 3" means
// "bank 3 is mapped", with no indirection to follow.
//
// The fixed half is the part worth testing hardest. It is not a convenience:
// the reset and NMI vectors live at $FFFA-$FFFF, so a cartridge that could
// switch them out could never return from an interrupt. A mapper that got the
// window boundary wrong would look fine until the first NMI.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "../include/bus.h"
#include "gtest/gtest.h"

namespace tests
{
namespace unrom
{
namespace
{

constexpr size_t kBankSize = 16 * 1024;

// A UNROM image whose bank N is filled entirely with the byte N.
struct BankedRom {
    explicit BankedRom(const std::string& name, uint8_t banks) : path(std::string(NES_TEST_FILES_DIR) + "/" + name)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        // flags6 bit 4 low nibble of mapper, flags7 high: mapper 2 -> flags6 $20.
        const uint8_t header[16] = {'N', 'E', 'S', 0x1A, banks, 0, 0x20, 0x00, 0, 0, 0, 0, 0, 0, 0, 0};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));

        for (uint8_t b = 0; b < banks; ++b) {
            const std::vector<uint8_t> bank(kBankSize, b);
            out.write(reinterpret_cast<const char*>(bank.data()), bank.size());
        }
    }
    ~BankedRom() { std::remove(path.c_str()); }

    BankedRom(const BankedRom&) = delete;
    BankedRom& operator=(const BankedRom&) = delete;

    std::string path;
};

}  // namespace

GTEST_TEST(unrom, loads_and_reports_its_bank_count)
{
    BankedRom rom("unrom_load.nes", 8);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    EXPECT_EQ(MapperId::uxrom, console.rom.mapper_id);
    EXPECT_EQ(8, console.rom.prg_bank_count);
    EXPECT_EQ(0, console.rom.prg_bank) << "power-on bank is 0";
    EXPECT_TRUE(console.rom.chr_rom.empty()) << "UNROM has no CHR-ROM; the console supplies CHR-RAM";
}

GTEST_TEST(unrom, the_low_window_follows_the_latched_bank)
{
    BankedRom rom("unrom_switch.nes", 8);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    EXPECT_EQ(0, console.read(0x8000)) << "bank 0 at power-on";

    for (uint8_t bank = 0; bank < 8; ++bank) {
        console.write(0x8000, bank);
        EXPECT_EQ(bank, console.read(0x8000)) << "start of the switchable window";
        EXPECT_EQ(bank, console.read(0xBFFF)) << "end of the switchable window";
    }
}

GTEST_TEST(unrom, the_high_window_is_always_the_last_bank)
{
    BankedRom rom("unrom_fixed.nes", 8);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    // Switching must not move $C000-$FFFF. The vectors live at the very top of
    // it, so this is what lets a game come back from an NMI at all.
    for (uint8_t bank = 0; bank < 8; ++bank) {
        console.write(0x8000, bank);
        EXPECT_EQ(7, console.read(0xC000)) << "start of the fixed window, bank " << int(bank) << " selected";
        EXPECT_EQ(7, console.read(0xFFFA)) << "the NMI vector must never be switched away";
        EXPECT_EQ(7, console.read(0xFFFF)) << "top of the address space";
    }
}

GTEST_TEST(unrom, the_window_boundary_is_exactly_at_c000)
{
    BankedRom rom("unrom_boundary.nes", 4);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    console.write(0x8000, 1);

    // An off-by-one in the boundary is invisible unless it is probed at the
    // seam itself: both halves read the same byte for bank 3 selected.
    EXPECT_EQ(1, console.read(0xBFFF)) << "last byte of the switchable window";
    EXPECT_EQ(3, console.read(0xC000)) << "first byte of the fixed window";
}

GTEST_TEST(unrom, any_address_in_cartridge_space_latches_the_bank)
{
    BankedRom rom("unrom_decode.nes", 8);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    // The board decodes nothing finer than "the CPU wrote to $8000-$FFFF".
    for (const uint16_t addr : {0x8000, 0x9ABC, 0xBFFF, 0xC000, 0xFFFF}) {
        console.write(0x8000, 0);  // back to a known bank
        console.write(addr, 5);
        EXPECT_EQ(5, console.read(0x8000)) << "a write to $" << std::hex << addr << " should have latched";
    }
}

GTEST_TEST(unrom, writes_below_cartridge_space_do_not_latch)
{
    BankedRom rom("unrom_noleak.nes", 8);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    console.write(0x8000, 3);
    ASSERT_EQ(3, console.read(0x8000));

    // RAM, a PPU register, and PRG-RAM. None of these reaches the mapper.
    console.write(0x0000, 6);
    console.write(0x2000, 6);
    console.write(0x6000, 6);

    EXPECT_EQ(3, console.read(0x8000)) << "only $8000-$FFFF selects a bank";
}

// Real boards decode only as many bits as they have banks, so a write wider
// than the cartridge wraps rather than selecting a bank that does not exist.
GTEST_TEST(unrom, the_bank_number_is_masked_by_the_bank_count)
{
    BankedRom rom("unrom_mask.nes", 4);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    console.write(0x8000, 0x0F);
    EXPECT_EQ(3, console.read(0x8000)) << "$0F on a 4-bank cartridge selects bank 3, not an absent bank 15";
    EXPECT_EQ(3, console.rom.prg_bank);
}

GTEST_TEST(unrom, a_header_claiming_chr_rom_is_rejected)
{
    // UNROM switches PRG and uses CHR-RAM; a CHR-ROM bank means this is not a
    // UNROM image, whatever the mapper nibble says.
    const std::string path = std::string(NES_TEST_FILES_DIR) + "/unrom_with_chr.nes";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const uint8_t header[16] = {'N', 'E', 'S', 0x1A, 4, 1, 0x20, 0x00, 0, 0, 0, 0, 0, 0, 0, 0};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        const std::vector<uint8_t> body(4 * kBankSize + 8192, 0);
        out.write(reinterpret_cast<const char*>(body.data()), body.size());
    }

    Bus console;
    EXPECT_FALSE(console.load_cartridge(path));
    EXPECT_FALSE(console.rom.loaded());
    std::remove(path.c_str());
}

GTEST_TEST(unrom, a_single_bank_image_is_rejected)
{
    // One bank means the switchable and fixed windows would be the same bank,
    // which is NROM. Accepting it would hide a mislabelled header.
    BankedRom rom("unrom_one_bank.nes", 1);
    Bus console;
    EXPECT_FALSE(console.load_cartridge(rom.path));
}

}  // namespace unrom
}  // namespace tests
