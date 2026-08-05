// The PPU's address space: pattern tables, nametable mirroring, palette
// mirroring, and the $2007 read buffer.
//
// These are unit tests rather than ROM checks on purpose. The ROMs that cover
// this ground - Blargg's palette_ram, vram_access and sprite_ram - predate the
// $6000 reporting protocol and draw their results instead of writing them, so
// they cannot be read headlessly until there is a framebuffer. See
// tests/test_files/fetch_ppu_address_space.sh.
//
// Expected values are literals with their derivation stated, not expressions
// over the implementation's own constants.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "../include/bus.h"

namespace tests
{
namespace ppu_memory
{

// A minimal NROM image, written so a test can choose the mirroring bit and
// whether the cartridge brings CHR-ROM.
struct TinyRom {
    explicit TinyRom(const std::string& name) : path(std::string(NES_TEST_FILES_DIR) + "/" + name) {}
    ~TinyRom() { std::remove(path.c_str()); }

    TinyRom(const TinyRom&) = delete;
    TinyRom& operator=(const TinyRom&) = delete;

    void write(const bool vertical_mirroring, const uint8_t chr_banks, const uint8_t chr_fill = 0x00)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const uint8_t flags6 = vertical_mirroring ? 0x01 : 0x00;
        const uint8_t header[16] = {'N', 'E', 'S', 0x1A, 1, chr_banks, flags6, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));

        const std::vector<uint8_t> prg(16 * 1024, 0xEA);
        out.write(reinterpret_cast<const char*>(prg.data()), prg.size());
        const std::vector<uint8_t> chr(chr_banks * 8u * 1024u, chr_fill);
        out.write(reinterpret_cast<const char*>(chr.data()), chr.size());
    }

    std::string path;
};

// --- nametable mirroring -----------------------------------------------------
//
// Two 1KB screens of internal RAM behind four 1KB address windows. The
// cartridge decides the arrangement:
//
//   horizontal   $2000 $2400 -> screen 0     vertical   $2000 $2800 -> screen 0
//                $2800 $2C00 -> screen 1                $2400 $2C00 -> screen 1

GTEST_TEST(testPPUMemory, horizontal_mirroring_pairs_2000_with_2400)
{
    TinyRom rom("mirror_h_ppu.nes");
    rom.write(/*vertical_mirroring=*/false, /*chr_banks=*/1);

    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));
    ASSERT_TRUE(console.rom.horizontal_mirroring);

    console.ppu.ppu_bus_write(0x2000, 0x11);
    EXPECT_EQ(0x11, console.ppu.ppu_bus_read(0x2400)) << "$2400 must alias $2000 under horizontal mirroring";

    console.ppu.ppu_bus_write(0x2800, 0x22);
    EXPECT_EQ(0x22, console.ppu.ppu_bus_read(0x2C00)) << "$2C00 must alias $2800";

    // ...and the two pairs are genuinely different screens.
    EXPECT_EQ(0x11, console.ppu.ppu_bus_read(0x2000));
    EXPECT_EQ(0x22, console.ppu.ppu_bus_read(0x2800));
}

GTEST_TEST(testPPUMemory, vertical_mirroring_pairs_2000_with_2800)
{
    TinyRom rom("mirror_v_ppu.nes");
    rom.write(/*vertical_mirroring=*/true, /*chr_banks=*/1);

    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));
    ASSERT_FALSE(console.rom.horizontal_mirroring);

    console.ppu.ppu_bus_write(0x2000, 0x33);
    EXPECT_EQ(0x33, console.ppu.ppu_bus_read(0x2800)) << "$2800 must alias $2000 under vertical mirroring";

    console.ppu.ppu_bus_write(0x2400, 0x44);
    EXPECT_EQ(0x44, console.ppu.ppu_bus_read(0x2C00)) << "$2C00 must alias $2400";

    EXPECT_EQ(0x33, console.ppu.ppu_bus_read(0x2000));
    EXPECT_EQ(0x44, console.ppu.ppu_bus_read(0x2400));
}

// The two arrangements must actually differ - a decode that ignored the flag
// would satisfy one of the tests above by accident.
GTEST_TEST(testPPUMemory, the_mirroring_flag_changes_the_arrangement)
{
    TinyRom horizontal("mirror_cmp_h.nes");
    horizontal.write(false, 1);
    Bus h;
    ASSERT_TRUE(h.load_cartridge(horizontal.path));
    h.ppu.ppu_bus_write(0x2000, 0xAA);
    h.ppu.ppu_bus_write(0x2400, 0xBB);

    TinyRom vertical("mirror_cmp_v.nes");
    vertical.write(true, 1);
    Bus v;
    ASSERT_TRUE(v.load_cartridge(vertical.path));
    v.ppu.ppu_bus_write(0x2000, 0xAA);
    v.ppu.ppu_bus_write(0x2400, 0xBB);

    // Horizontal: $2400 aliases $2000, so the second write overwrote the first.
    EXPECT_EQ(0xBB, h.ppu.ppu_bus_read(0x2000));
    // Vertical: they are separate screens, so both survive.
    EXPECT_EQ(0xAA, v.ppu.ppu_bus_read(0x2000));
    EXPECT_EQ(0xBB, v.ppu.ppu_bus_read(0x2400));
}

GTEST_TEST(testPPUMemory, the_3000_region_mirrors_the_nametables)
{
    Bus console;
    console.ppu.ppu_bus_write(0x2123, 0x5A);
    EXPECT_EQ(0x5A, console.ppu.ppu_bus_read(0x3123)) << "$3000-$3EFF mirrors $2000-$2EFF";

    console.ppu.ppu_bus_write(0x3456, 0xA5);
    EXPECT_EQ(0xA5, console.ppu.ppu_bus_read(0x2456));
}

// --- palette -----------------------------------------------------------------

GTEST_TEST(testPPUMemory, palette_repeats_every_32_bytes_to_3fff)
{
    Bus console;
    console.ppu.ppu_bus_write(0x3F01, 0x2A);

    // $3F21, $3F41 ... $3FE1 are all the same cell.
    EXPECT_EQ(0x2A, console.ppu.ppu_bus_read(0x3F21));
    EXPECT_EQ(0x2A, console.ppu.ppu_bus_read(0x3F41));
    EXPECT_EQ(0x2A, console.ppu.ppu_bus_read(0x3FE1));

    // And writing through a high mirror lands in the same place.
    console.ppu.ppu_bus_write(0x3FE1, 0x15);
    EXPECT_EQ(0x15, console.ppu.ppu_bus_read(0x3F01));
}

// $3F10/$14/$18/$1C are not entries of their own: hardware wires them to
// $3F00/$04/$08/$0C. Those four are the backdrop colour and the unused entry of
// each sprite palette.
GTEST_TEST(testPPUMemory, the_sprite_palette_backdrop_entries_alias_the_background_ones)
{
    Bus console;

    const uint16_t aliases[4][2] = {{0x3F10, 0x3F00}, {0x3F14, 0x3F04}, {0x3F18, 0x3F08}, {0x3F1C, 0x3F0C}};

    for (const auto& pair : aliases) {
        console.ppu.ppu_bus_write(pair[0], 0x27);
        EXPECT_EQ(0x27, console.ppu.ppu_bus_read(pair[1]))
            << "$" << std::hex << pair[0] << " must alias $" << pair[1];

        // ...and in the other direction.
        console.ppu.ppu_bus_write(pair[1], 0x0C);
        EXPECT_EQ(0x0C, console.ppu.ppu_bus_read(pair[0]));
    }
}

// Only those four alias. $3F11 and $3F01 are independent cells, and a decode
// that masked bit 4 unconditionally would merge them.
GTEST_TEST(testPPUMemory, other_sprite_palette_entries_do_not_alias)
{
    Bus console;

    for (const uint16_t low : {0x3F01, 0x3F02, 0x3F03, 0x3F05, 0x3F06, 0x3F07}) {
        const uint16_t high = static_cast<uint16_t>(low + 0x10);
        console.ppu.ppu_bus_write(low, 0x11);
        console.ppu.ppu_bus_write(high, 0x22);
        EXPECT_EQ(0x11, console.ppu.ppu_bus_read(low))
            << "$" << std::hex << high << " must NOT alias $" << low;
    }
}

// --- pattern tables ----------------------------------------------------------

GTEST_TEST(testPPUMemory, chr_rom_is_readable_and_not_writable)
{
    TinyRom rom("chr_rom_ppu.nes");
    rom.write(/*vertical_mirroring=*/false, /*chr_banks=*/1, /*chr_fill=*/0x3C);

    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));
    ASSERT_FALSE(console.rom.chr_rom.empty());

    EXPECT_EQ(0x3C, console.ppu.ppu_bus_read(0x0000));
    EXPECT_EQ(0x3C, console.ppu.ppu_bus_read(0x1FFF));

    console.ppu.ppu_bus_write(0x0000, 0xFF);

    // Not just "the read is unchanged" - the write must not have landed in the
    // console's CHR-RAM either. Checking only the read misses a write that goes
    // to the wrong array, because the read comes from CHR-ROM regardless.
    EXPECT_EQ(0x00, console.ppu.chr_ram[0x0000])
        << "a write to CHR-ROM leaked into CHR-RAM; the cartridge's memory is what is addressed here";
    EXPECT_EQ(0x3C, console.ppu.ppu_bus_read(0x0000)) << "CHR-ROM must ignore writes";
}

// A cartridge declaring zero CHR banks uses CHR-RAM, which the console
// supplies and which IS writable. Several of the PPU test ROMs are built this
// way, so this is not a corner case.
GTEST_TEST(testPPUMemory, chr_ram_is_writable_when_the_cartridge_brings_no_chr_rom)
{
    TinyRom rom("chr_ram_ppu.nes");
    rom.write(/*vertical_mirroring=*/false, /*chr_banks=*/0);

    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));
    ASSERT_TRUE(console.rom.chr_rom.empty()) << "this cartridge declares no CHR banks";

    console.ppu.ppu_bus_write(0x0000, 0x81);
    console.ppu.ppu_bus_write(0x1FFF, 0x7E);
    EXPECT_EQ(0x81, console.ppu.ppu_bus_read(0x0000));
    EXPECT_EQ(0x7E, console.ppu.ppu_bus_read(0x1FFF));
}

// --- the $2007 read buffer ---------------------------------------------------

// $2006 writes are among those ignored during the post-reset lockout, so a test
// that drives PPUADDR has to clock past it first. Skipping this leaves PPUADDR
// at 0 and every read comes back from $0000 - which looks exactly like a broken
// read buffer.
void run_past_reset_lockout(PPU& ppu)
{
    while (ppu.in_reset_write_lockout()) {
        ppu.clock();
    }
}

void set_ppu_addr(PPU& ppu, uint16_t addr)
{
    ppu.read(PPU::PPUSTATUS);  // reset the address latch
    ppu.write(PPU::PPUADDR, static_cast<uint8_t>(addr >> 8));
    ppu.write(PPU::PPUADDR, static_cast<uint8_t>(addr & 0xFF));
}

GTEST_TEST(testPPUMemory, reads_below_3f00_are_delayed_by_one)
{
    Bus console;
    run_past_reset_lockout(console.ppu);
    console.ppu.ppu_bus_write(0x2000, 0xC1);
    console.ppu.ppu_bus_write(0x2001, 0xC2);

    set_ppu_addr(console.ppu, 0x2000);

    // The first read returns whatever the latch held - not $2000's contents -
    // and refills the latch from $2000. This is why software reads $2007 twice.
    const uint8_t first = console.ppu.read(PPU::PPUDATA);
    EXPECT_NE(0xC1, first) << "the first read after setting the address must be the stale latch";

    EXPECT_EQ(0xC1, console.ppu.read(PPU::PPUDATA)) << "the second read returns what the first fetched";
    EXPECT_EQ(0xC2, console.ppu.read(PPU::PPUDATA)) << "and reads then stream one byte behind";
}

// Palette reads are NOT buffered - they come back immediately - but the latch
// is still refilled, from the nametable underneath the palette rather than from
// the palette itself.
GTEST_TEST(testPPUMemory, palette_reads_are_immediate_and_refill_the_latch_from_the_nametable)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    console.ppu.ppu_bus_write(0x3F00, 0x21);
    // $2F00 is what lies under $3F00 once the nametable mirroring is applied.
    console.ppu.ppu_bus_write(0x2F00, 0x9D);

    set_ppu_addr(console.ppu, 0x3F00);
    EXPECT_EQ(0x21, console.ppu.read(PPU::PPUDATA)) << "a palette read is immediate, not delayed";

    // The latch now holds the byte from underneath, so a read from ordinary
    // VRAM immediately afterwards hands that back rather than the palette byte.
    set_ppu_addr(console.ppu, 0x2400);
    EXPECT_EQ(0x9D, console.ppu.read(PPU::PPUDATA))
        << "a palette read refills the latch from the nametable beneath it";
}

}  // namespace ppu_memory
}  // namespace tests
