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
//
// `mapper` is a parameter so mapper 180 can reuse this: it is the same cartridge
// with its two PRG windows exchanged, so the same synthetic image tests both and
// the assertions can be read side by side.
struct BankedRom {
    explicit BankedRom(const std::string& name, uint8_t banks, uint8_t mapper = 2)
        : path(std::string(NES_TEST_FILES_DIR) + "/" + name)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        // The mapper number straddles two bytes: flags6 carries its low nibble
        // in the HIGH nibble, flags7 carries its high nibble in place.
        const uint8_t flags6 = static_cast<uint8_t>((mapper & 0x0F) << 4);
        const uint8_t flags7 = static_cast<uint8_t>(mapper & 0xF0);
        const uint8_t header[16] = {'N', 'E', 'S', 0x1A, banks, 0, flags6, flags7, 0, 0, 0, 0, 0, 0, 0, 0};
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

// --- UNROM 7408 (180), the same board with its windows exchanged -------------
//
// Every assertion below is the mirror of one above, and that is the point: the
// two mappers differ in nothing else, so anything that reads the same in both
// places is not testing the difference.
//
// Holy Mapperel's M180_P128K_H already identifies the board correctly, which is
// a stronger statement than any of these. What it does NOT do is probe the
// window boundary: mechanical mutation moved the split from $C000 to $C001 and
// changed `<` to `<=`, and that ROM passed both times. A one-byte error at the
// seam is invisible to a board-detection oracle and fatal to a game.

GTEST_TEST(unrom7408, loads_and_reports_its_bank_count)
{
    BankedRom rom("unrom7408_load.nes", 8, 180);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    EXPECT_EQ(MapperId::unrom_7408, console.rom.mapper_id);
    EXPECT_EQ(8, console.rom.prg_bank_count);
    EXPECT_EQ(0, console.rom.prg_bank) << "power-on bank is 0";
    EXPECT_TRUE(console.rom.chr_rom.empty()) << "like UNROM, the console supplies CHR-RAM";
}

GTEST_TEST(unrom7408, the_high_window_follows_the_latched_bank)
{
    BankedRom rom("unrom7408_switch.nes", 8, 180);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    for (uint8_t bank = 0; bank < 8; ++bank) {
        console.write(0x8000, bank);
        EXPECT_EQ(bank, console.read(0xC000)) << "start of the switchable window";
        EXPECT_EQ(bank, console.read(0xFFFF)) << "end of it - and the vectors live here, which is the "
                                                 "whole reason mapper 2 fixes this half instead";
    }
}

GTEST_TEST(unrom7408, the_low_window_is_always_bank_zero)
{
    BankedRom rom("unrom7408_fixed.nes", 8, 180);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    // The FIRST bank, not the last. That is the difference from UNROM, and it is
    // why the board exists: the bank-select routine lives in this half, so what
    // the CPU reads at the address it writes is a constant and cannot conflict
    // with the bank being selected.
    for (uint8_t bank = 0; bank < 8; ++bank) {
        console.write(0x8000, bank);
        EXPECT_EQ(0, console.read(0x8000)) << "start of the fixed window, after selecting " << int(bank);
        EXPECT_EQ(0, console.read(0xBFFF)) << "end of the fixed window, after selecting " << int(bank);
    }
}

GTEST_TEST(unrom7408, the_seam_between_the_windows_is_exactly_at_c000)
{
    BankedRom rom("unrom7408_seam.nes", 8, 180);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    console.write(0x8000, 5);

    // Two adjacent bytes that must come from different banks. Nothing else in
    // this file or in Holy Mapperel pins the boundary to the byte, and both
    // one-byte errors - moving the split to $C001, or letting $C000 fall on the
    // fixed side - survived the oracle.
    EXPECT_EQ(0, console.read(0xBFFF)) << "the last byte of the fixed half";
    EXPECT_EQ(5, console.read(0xC000)) << "the first byte of the switchable half";
}

GTEST_TEST(unrom7408, an_image_with_chr_rom_is_rejected)
{
    // The check exists so a mislabelled header fails loudly rather than being
    // half-read. This board carries CHR-RAM, like the UNROM it is wired from.
    BankedRom rom("unrom7408_chr.nes", 8, 180);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path)) << "the CHR-less image must still load";

    BankedRom one("unrom7408_one_bank.nes", 1, 180);
    Bus small;
    EXPECT_FALSE(small.load_cartridge(one.path)) << "one PRG bank is NROM, not this board";

    // The upper limit too, which is the only clause a valid-looking image can
    // reach: 32 banks is 512K, even and a power of two, and still more than the
    // four register bits address.
    BankedRom huge("unrom7408_too_big.nes", 32, 180);
    Bus large;
    EXPECT_FALSE(large.load_cartridge(huge.path)) << "512K needs five bank bits; this board latches four";

    // And the same non-power-of-two rejection UNROM needs, inherited with the
    // rest of the board - see the mapper-2 test above for what it costs.
    BankedRom twelve("unrom7408_twelve.nes", 12, 180);
    Bus odd;
    EXPECT_FALSE(odd.load_cartridge(twelve.path)) << "the bank-select mask would alias four of the twelve";
}

// BOTH ENDS OF THE ACCEPTED RANGE, which is what stops the check above from
// being satisfied by a rule that rejects everything. Two banks and sixteen are
// the smallest and largest real boards, and each is one step from a rejection -
// so these are the assertions that pin where the boundary IS rather than that
// there is one somewhere.
GTEST_TEST(unrom7408, the_smallest_and_largest_legal_images_both_load)
{
    BankedRom smallest("unrom7408_two_banks.nes", 2, 180);
    Bus a;
    ASSERT_TRUE(a.load_cartridge(smallest.path)) << "two banks is the minimum that has a window to switch";
    a.write(0x8000, 1);
    EXPECT_EQ(0, a.read(0x8000)) << "fixed half is still bank 0";
    EXPECT_EQ(1, a.read(0xC000)) << "and the other bank is switched in above it";

    BankedRom largest("unrom7408_sixteen_banks.nes", 16, 180);
    Bus b;
    ASSERT_TRUE(b.load_cartridge(largest.path)) << "sixteen banks is 256K, the largest the four bits reach";
    b.write(0x8000, 15);
    EXPECT_EQ(15, b.read(0xC000)) << "the top bank is reachable";
}

// A BANK COUNT THAT IS NOT A POWER OF TWO IS REJECTED, and this test exists
// because it used to be accepted and mis-banked in silence.
//
// MEASURED before the check was tightened, on a 12-bank image that loaded
// cleanly. cpu_write latches `data & (prg_bank_count - 1)`, and 11 is 0b1011:
//
//     select 0..3   -> banks 0..3    correct
//     select 4..7   -> banks 0..3    ALIASED, a third of the cartridge lost
//     select 8..11  -> banks 8..11   correct, by coincidence
//
// Nothing reported anything. The banks that worked are what made it quiet.
//
// check_cnrom and check_axrom already required a power of two, and both say why
// in as many words; this check was the one that omitted it, and mapper 180
// inherited the omission by being wired from this board.
GTEST_TEST(unrom, a_bank_count_that_is_not_a_power_of_two_is_rejected)
{
    BankedRom rom("unrom_twelve.nes", 12);
    Bus console;
    EXPECT_FALSE(console.load_cartridge(rom.path)) << "192K cannot be masked to twelve banks unambiguously";

    // The accepted sizes on either side of it, so this cannot pass by rejecting
    // everything - and 8 and 16 are the two real UNROM boards.
    BankedRom eight("unrom_eight.nes", 8);
    Bus a;
    EXPECT_TRUE(a.load_cartridge(eight.path)) << "128K UNROM";

    BankedRom sixteen("unrom_sixteen.nes", 16);
    Bus b;
    EXPECT_TRUE(b.load_cartridge(sixteen.path)) << "256K UOROM";

    // Both ends of the range itself. Two banks is the smallest image with a
    // window to switch, and thirty-two is the first power of two past the
    // ceiling - the only size that reaches the upper bound on its own, since
    // anything between is rejected for not being a power of two first.
    BankedRom smallest("unrom_two.nes", 2);
    Bus c;
    EXPECT_TRUE(c.load_cartridge(smallest.path)) << "32K is two banks, the minimum this board can be";

    BankedRom huge("unrom_thirtytwo.nes", 32);
    Bus d;
    EXPECT_FALSE(d.load_cartridge(huge.path)) << "512K needs five bank bits; UNROM latches four";
}

}  // namespace unrom
}  // namespace tests
