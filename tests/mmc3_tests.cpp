// MMC3 (mapper 4) banking. The IRQ counter is a separate step and is not
// covered here.
//
// Every bank in the synthetic images below is filled with its own index, so an
// assertion names the bank it expects rather than a magic byte - the same trick
// unrom_tests.cpp uses, and for the same reason.
//
// The windows that DO NOT move are tested hardest. $E000-$FFFF is wired to the
// last PRG bank on every MMC3 board, and that is what keeps the reset and NMI
// vectors reachable however the game banks; a mapper that let them move would
// look fine until the first interrupt.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "../include/bus.h"
#include "gtest/gtest.h"

namespace tests
{
namespace mmc3
{
namespace
{

// An MMC3 image whose every 8KB PRG bank is filled with its own index, and
// every 1KB CHR bank likewise. `prg16` is in 16KB header units, so bank count
// in MMC3's own 8KB units is twice that.
struct BankedRom {
    BankedRom(const std::string& name, uint8_t prg16, uint8_t chr8) : path(std::string(NES_TEST_FILES_DIR) + "/" + name)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        // mapper 4 -> flags6 high nibble 4
        const uint8_t header[16] = {'N', 'E', 'S', 0x1A, prg16, chr8, 0x40, 0x00, 0, 0, 0, 0, 0, 0, 0, 0};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));

        for (uint16_t b = 0; b < prg16 * 2u; ++b) {
            const std::vector<uint8_t> bank(8192, static_cast<uint8_t>(b));
            out.write(reinterpret_cast<const char*>(bank.data()), bank.size());
        }
        for (uint16_t b = 0; b < chr8 * 8u; ++b) {
            const std::vector<uint8_t> bank(1024, static_cast<uint8_t>(b));
            out.write(reinterpret_cast<const char*>(bank.data()), bank.size());
        }
    }
    ~BankedRom() { std::remove(path.c_str()); }
    BankedRom(const BankedRom&) = delete;
    BankedRom& operator=(const BankedRom&) = delete;

    std::string path;
};

// $8000 selects a register, $8001 writes it.
void select(Bus& console, const uint8_t reg_and_modes, const uint8_t value)
{
    console.write(0x8000, reg_and_modes);
    console.write(0x8001, value);
}

}  // namespace

GTEST_TEST(mmc3, loads_and_reports_its_bank_counts)
{
    BankedRom rom("mmc3_load.nes", 8, 2);  // 128KB PRG, 16KB CHR
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    EXPECT_EQ(4, console.rom.mapper_id);
    EXPECT_EQ(16, console.rom.prg_8k_bank_count) << "8 x 16KB = 16 banks of 8KB";
    EXPECT_EQ(16, console.rom.chr_1k_bank_count) << "2 x 8KB = 16 banks of 1KB";
}

// The property that makes the mapper usable at all.
GTEST_TEST(mmc3, the_last_prg_bank_is_wired_to_e000_whatever_is_selected)
{
    BankedRom rom("mmc3_fixed.nes", 8, 1);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));
    const uint8_t last = 15;

    for (uint8_t r6 = 0; r6 < 8; ++r6) {
        select(console, 6, r6);
        for (uint8_t mode : {uint8_t{0}, uint8_t{0x40}}) {
            console.write(0x8000, mode | 6);
            EXPECT_EQ(last, console.read(0xE000)) << "R6=" << int(r6) << " mode=" << int(mode);
            EXPECT_EQ(last, console.read(0xFFFA)) << "the NMI vector must never be switched away";
            EXPECT_EQ(last, console.read(0xFFFF));
        }
    }
}

// $8000 bit 6 swaps which of the two windows R6 drives; the other becomes the
// second-last bank. Both directions are checked, because a mapper that ignored
// the mode bit would pass a test that only ever looked at one of them.
GTEST_TEST(mmc3, prg_mode_bit_swaps_the_switchable_window)
{
    BankedRom rom("mmc3_prgmode.nes", 8, 1);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));
    const uint8_t second_last = 14;

    select(console, 6, 3);  // R6 = bank 3
    select(console, 7, 5);  // R7 = bank 5

    console.write(0x8000, 6);  // mode 0: $8000 switchable
    EXPECT_EQ(3, console.read(0x8000)) << "mode 0: $8000-$9FFF follows R6";
    EXPECT_EQ(5, console.read(0xA000)) << "$A000-$BFFF always follows R7";
    EXPECT_EQ(second_last, console.read(0xC000)) << "mode 0: $C000-$DFFF is fixed";

    console.write(0x8000, 0x40 | 6);  // mode 1: the two swap
    EXPECT_EQ(second_last, console.read(0x8000)) << "mode 1: $8000-$9FFF is fixed";
    EXPECT_EQ(5, console.read(0xA000)) << "$A000-$BFFF is unaffected by the mode bit";
    EXPECT_EQ(3, console.read(0xC000)) << "mode 1: $C000-$DFFF follows R6";
}

// NESdev: "R6 and R7 will ignore the top two bits, as the MMC3 has only 6 PRG
// ROM address lines."
GTEST_TEST(mmc3, prg_registers_ignore_their_top_two_bits)
{
    BankedRom rom("mmc3_prgmask.nes", 8, 1);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    console.write(0x8000, 6);
    select(console, 6, 0x03);
    const uint8_t plain = console.read(0x8000);

    select(console, 6, 0xC3);  // same low 6 bits, both top bits set
    EXPECT_EQ(plain, console.read(0x8000)) << "the top two bits of R6 are not connected";
}

// R0 and R1 address 2KB, so their low bit is not connected and each covers a
// consecutive PAIR of 1KB banks.
GTEST_TEST(mmc3, the_two_kilobyte_chr_registers_ignore_their_bottom_bit)
{
    BankedRom rom("mmc3_chr2k.nes", 2, 2);  // 16 x 1KB CHR
    Bus console;
    PPU& ppu = console.ppu;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    console.write(0x8000, 0);     // R0, no inversion
    console.write(0x8001, 0x05);  // odd value: the bottom bit must be dropped

    EXPECT_EQ(4, ppu.ppu_bus_read(0x0000)) << "R0=5 addresses the 2KB pair starting at bank 4";
    EXPECT_EQ(5, ppu.ppu_bus_read(0x0400)) << "and its second half is bank 5";
}

// $8000 bit 7 swaps the 2KB and 1KB groups between $0000 and $1000.
GTEST_TEST(mmc3, chr_a12_inversion_swaps_the_two_halves_of_pattern_space)
{
    BankedRom rom("mmc3_chrinv.nes", 2, 2);
    Bus console;
    PPU& ppu = console.ppu;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    select(console, 0, 0x00);  // R0 -> 2KB at bank 0
    select(console, 2, 0x09);  // R2 -> 1KB at bank 9

    console.write(0x8000, 0x00);  // not inverted
    EXPECT_EQ(0, ppu.ppu_bus_read(0x0000)) << "2KB group at $0000";
    EXPECT_EQ(9, ppu.ppu_bus_read(0x1000)) << "1KB group at $1000";

    console.write(0x8000, 0x80);  // inverted
    EXPECT_EQ(9, ppu.ppu_bus_read(0x0000)) << "1KB group moves to $0000";
    EXPECT_EQ(0, ppu.ppu_bus_read(0x1000)) << "2KB group moves to $1000";
}

// $A000 controls mirroring at RUNTIME, unlike every other mapper here where it
// is fixed at load. PPU::nametable_offset reads the flag live, so this works
// with no PPU change - which is worth pinning, because it would otherwise look
// like an accident.
GTEST_TEST(mmc3, a000_changes_mirroring_at_runtime)
{
    BankedRom rom("mmc3_mirror.nes", 2, 1);
    Bus console;
    PPU& ppu = console.ppu;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    // Write a byte through one nametable and see where its mirror lands.
    console.write(0xA000, 0x00);  // horizontal
    ASSERT_TRUE(console.rom.horizontal_mirroring);
    ppu.ppu_bus_write(0x2000, 0x11);
    EXPECT_EQ(0x11, ppu.ppu_bus_read(0x2400)) << "horizontal: $2000 and $2400 are the same screen";

    console.write(0xA000, 0x01);  // vertical
    ASSERT_FALSE(console.rom.horizontal_mirroring);
    ppu.ppu_bus_write(0x2000, 0x22);
    EXPECT_EQ(0x22, ppu.ppu_bus_read(0x2800)) << "vertical: $2000 and $2800 are the same screen";
}

GTEST_TEST(mmc3, a001_records_the_prg_ram_protect_bits)
{
    BankedRom rom("mmc3_prgram.nes", 2, 1);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    console.write(0xA001, 0x80);  // chip enabled, writes allowed
    EXPECT_TRUE(console.rom.prg_ram_enabled);
    EXPECT_FALSE(console.rom.prg_ram_write_protected);

    console.write(0xA001, 0xC0);  // chip enabled, write-protected
    EXPECT_TRUE(console.rom.prg_ram_enabled);
    EXPECT_TRUE(console.rom.prg_ram_write_protected);

    console.write(0xA001, 0x00);  // chip disabled
    EXPECT_FALSE(console.rom.prg_ram_enabled);
}

// Recording the bits is not obeying them, and for a long time this mapper did
// only the first. The two failures below are what a game would actually see:
// a save that survives a crash because the RAM was protected, and a $6000-$7FFF
// window that reads back open bus because the chip was deselected.
GTEST_TEST(mmc3, a001_write_protect_stops_writes_but_not_reads)
{
    BankedRom rom("mmc3_prgram_wp.nes", 2, 1);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    console.write(0xA001, 0x80);  // enabled, writable
    console.write(0x6000, 0x5A);
    console.write(0x7FFF, 0x5B);
    ASSERT_EQ(0x5A, console.read(0x6000)) << "the writable case has to work, or the test below proves nothing";
    ASSERT_EQ(0x5B, console.read(0x7FFF));

    console.write(0xA001, 0xC0);  // enabled, write-protected
    console.write(0x6000, 0xA5);
    console.write(0x7FFF, 0xA6);
    EXPECT_EQ(0x5A, console.read(0x6000)) << "/WE inactive: the write is swallowed and the old byte survives";
    EXPECT_EQ(0x5B, console.read(0x7FFF)) << "protection covers the whole window, not just its first address";

    console.write(0xA001, 0x80);  // writable again
    console.write(0x6000, 0xA5);
    EXPECT_EQ(0xA5, console.read(0x6000)) << "clearing bit 6 has to let writes through again";
}

GTEST_TEST(mmc3, a001_disable_leaves_the_window_open_bus)
{
    BankedRom rom("mmc3_prgram_off.nes", 2, 1);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    console.write(0xA001, 0x80);
    console.write(0x6000, 0x42);
    ASSERT_EQ(0x42, console.read(0x6000));

    // With the chip deselected nothing drives the data bus, so the read returns
    // whatever was last on it. The write immediately before is what put $37
    // there, which makes the expected value a real prediction rather than a
    // restatement of whatever the code happens to do.
    console.write(0xA001, 0x00);
    console.write(0x6001, 0x37);
    EXPECT_EQ(0x37, console.read(0x6000)) << "disabled: open bus, holding the last value driven onto it";

    // And the write above must not have reached the chip either.
    console.write(0xA001, 0x80);
    EXPECT_EQ(0x42, console.read(0x6000)) << "re-enabling reveals the original byte: the write went nowhere";
    EXPECT_EQ(0x00, console.read(0x6001)) << "$6001 was never written; the disabled write was dropped, not queued";
}

// The board decodes only bit 13 and the write's parity, so $8000 and $9FFE are
// the same register. A decode that compared the whole address would pass every
// test above and fail here.
GTEST_TEST(mmc3, the_register_pair_is_decoded_from_bit_13_and_parity_only)
{
    BankedRom rom("mmc3_decode.nes", 8, 1);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    console.write(0x8000, 6);
    console.write(0x8001, 2);
    ASSERT_EQ(2, console.read(0x8000));

    // Same registers reached through different addresses in the same range.
    console.write(0x9FFE, 6);
    console.write(0x9FFF, 4);
    EXPECT_EQ(4, console.read(0x8000)) << "$9FFE/$9FFF are $8000/$8001";
}

// --- the A12 filter -------------------------------------------------------
//
// These exist because a mutation survived. Lowering mmc3_a12_filter_dots from 9
// to 1 failed NOTHING in the whole suite, which meant the filter - the thing
// that makes the counter mean "scanline" rather than "tile fetch" - had no test
// at all.
//
// No ROM could catch it AT THE TIME, because the only one that drives A12 from
// real rendering - 4-scanline_timing - was itself failing and pinned. It now
// passes, and re-running the same mutation confirms it now catches it too: with
// $2000=$10 the background fetches drop A12 for four dots between tiles, so a
// threshold of 1 clocks the counter 32 times a line instead of once.
//
// These stay anyway, and are the better failure. The ROM reports "Failed #2"
// after 309 frames; these say which side of the threshold moved, in 0 ms.
// NESdev: the counter is "triggered on a rising edge after the line has
// remained low for three falling edges of M2" - three CPU cycles, nine PPU
// dots.

namespace
{

// Drives A12 low at `low_at`, then high at `high_at`, and reports whether the
// counter clocked. The counter is armed with a latch of 1 so that a single
// clock takes it to a value distinguishable from its start.
bool edge_clocks_counter(Bus& console, const uint64_t low_at, const uint64_t high_at)
{
    ROM& rom = console.rom;

    console.write(0xC000, 0x05);  // latch
    console.write(0xC001, 0x00);  // request reload on the next edge

    // A long, unambiguous edge to consume the reload and leave a known count.
    rom.mmc3_observe_a12(0x0000, 0);
    rom.mmc3_observe_a12(0x1000, 100);
    const uint8_t before = rom.mmc3_irq_counter;

    rom.mmc3_observe_a12(0x0000, low_at);
    rom.mmc3_observe_a12(0x1000, high_at);

    return rom.mmc3_irq_counter != before;
}

}  // namespace

GTEST_TEST(mmc3A12Filter, a_rise_after_a_short_low_is_rejected)
{
    BankedRom rom("mmc3_a12_short.nes", 2, 1);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    // Four dots is what A12 actually drops for inside a single background tile
    // fetch - the nametable and attribute reads at $2xxx between two pattern
    // reads at $1xxx. Without the filter every tile would clock the counter and
    // an IRQ meant for one scanline would arrive 32 times a line.
    EXPECT_FALSE(edge_clocks_counter(console, 1000, 1004)) << "a 4-dot low is tile-fetch noise, not a scanline";
    EXPECT_FALSE(edge_clocks_counter(console, 2000, 2008)) << "8 dots is still below the 9-dot threshold";
}

GTEST_TEST(mmc3A12Filter, a_rise_after_a_long_low_clocks_the_counter)
{
    BankedRom rom("mmc3_a12_long.nes", 2, 1);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));

    // The control, and the half that makes the test above mean something: the
    // same call sequence with a longer low period MUST clock. Without this a
    // filter that rejected everything would pass.
    EXPECT_TRUE(edge_clocks_counter(console, 1000, 1009)) << "9 dots is the threshold and must count";
    EXPECT_TRUE(edge_clocks_counter(console, 2000, 2016)) << "a real scanline gap is 16+ dots";
}

// A level is not an edge. A run of $2xxx nametable fetches keeps A12 low, and
// re-arming the timer on each of them would mean no low period ever grew long
// enough to pass the filter.
GTEST_TEST(mmc3A12Filter, staying_low_does_not_restart_the_timer)
{
    BankedRom rom("mmc3_a12_level.nes", 2, 1);
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom.path));
    ROM& r = console.rom;

    console.write(0xC000, 0x05);
    console.write(0xC001, 0x00);
    r.mmc3_observe_a12(0x0000, 0);
    r.mmc3_observe_a12(0x1000, 100);
    const uint8_t before = r.mmc3_irq_counter;

    // Falls once, then several more low accesses, then rises well past the
    // threshold measured from the FIRST fall.
    r.mmc3_observe_a12(0x0000, 1000);
    r.mmc3_observe_a12(0x2000, 1002);
    r.mmc3_observe_a12(0x2400, 1004);
    r.mmc3_observe_a12(0x0000, 1006);
    r.mmc3_observe_a12(0x1000, 1012);

    EXPECT_NE(before, r.mmc3_irq_counter)
        << "the low period is measured from the first fall; later low accesses must not restart it";
}

// --- the DOT the counter is clocked on ------------------------------------
//
// 4-scanline_timing already pins this, but it costs 309 frames to say so and it
// says it as "Failed #3". These two numbers are the whole content of that ROM,
// and getting them wrong is a one-dot slip that nothing else in the suite
// notices: every other MMC3 test cares only about how MANY edges arrive.

namespace
{

// The dot of the FIRST counter clock after rendering is switched on during
// vblank - which is exactly the event 4-scanline_timing times. Returns the dot
// within the pre-render line; the scanline is asserted by the caller.
struct FirstClock {
    int scanline = -1;
    int dot = -1;
};

FirstClock first_clock_after_enabling_rendering(Bus& console, const uint8_t ppuctrl)
{
    PPU& ppu = console.ppu;

    while (ppu.in_reset_write_lockout()) {
        ppu.clock();
    }

    // Settle in vblank before enabling anything, so the low period leading into
    // the first fetch is unambiguously long and the filter cannot be what
    // decides the answer.
    for (uint64_t guard = 0; guard < 2ull * 341 * 262; ++guard) {
        if (ppu.scanline == 245 && ppu.cycle == 0) {
            break;
        }
        ppu.clock();
    }

    ppu.write(0x2000, ppuctrl);
    ppu.write(0x2001, 0x18);  // show background and sprites

    // A latch of $FF rather than 0: the counter starts at 0, so any clock at
    // all changes it, and no IRQ is enabled to complicate the run.
    console.write(0xC000, 0xFF);
    console.write(0xC001, 0x00);  // reload on the next edge

    for (uint64_t guard = 0; guard < 2ull * 341 * 262; ++guard) {
        const int scanline = ppu.scanline;
        const int dot = ppu.cycle;
        ppu.clock();
        if (console.rom.mmc3_irq_counter != 0) {
            return {scanline, dot};
        }
    }
    ADD_FAILURE() << "the counter was never clocked with PPUCTRL=$" << std::hex << static_cast<int>(ppuctrl);
    return {};
}

}  // namespace

// $2000=$08 puts sprites at $1000 and the background at $0000, so the sprite
// fetch is the only thing on the line that raises A12; $2000=$10 is the reverse
// and the first background pattern fetch raises it. blargg's 4-scanline_timing
// hard-codes the gap between those two moments as 256 dots
// (scanline_0_10 = scanline_0_08 - 256), which is what makes each of these
// absolute numbers checkable rather than merely self-consistent.
GTEST_TEST(mmc3A12Filter, rendering_clocks_the_counter_on_the_first_dot_of_the_pattern_fetch)
{
    {
        BankedRom rom("mmc3_dot_sprite.nes", 2, 1);
        Bus console;
        ASSERT_TRUE(console.load_cartridge(rom.path));

        const FirstClock c = first_clock_after_enabling_rendering(console, 0x08);
        EXPECT_EQ(PPU::pre_render_scanline, c.scanline);
        EXPECT_EQ(261, c.dot) << "sprite fetch: eight dots per sprite from 257, and the pattern-low address goes on\n"
                                 "  the bus at the FIRST of its two dots - 257 + 4, not 257 + 5.";
    }
    {
        BankedRom rom("mmc3_dot_bg.nes", 2, 1);
        Bus console;
        ASSERT_TRUE(console.load_cartridge(rom.path));

        const FirstClock c = first_clock_after_enabling_rendering(console, 0x10);
        EXPECT_EQ(PPU::pre_render_scanline, c.scanline);
        EXPECT_EQ(5, c.dot) << "background fetch: tile 0 occupies dots 1-8 and its pattern-low address goes on the\n"
                               "  bus at dot 5. 261 - 5 = 256, the constant 4-scanline_timing is built on.";
    }
}

// --- 8x16 sprites across both pattern tables ---------------------------------
//
// The case no ROM in this suite reaches, and the reason the garbage nametable
// fetches at dots 257-260 have to be on the bus. NESdev's MMC3 page:
//
//   "Using 8x16-pixel sprites from both pattern tables confuses the timer even
//    more, as this increases the time between edges on PPU A12 past the time
//    that the MMC3 is able to filter out, causing the timer to count more than
//    once per scanline."
//
// Written from that wording rather than from this emulator's own model of
// itself, because the two would otherwise agree by construction. blargg's six
// ROMs never set this up, so nothing else here would notice if it broke.

namespace
{

// Counts how many times the IRQ counter is clocked across one whole visible
// scanline. The latch is $FF and the counter is left free-running, so every
// clock is a visible change and none of them wrap inside a single line.
int counter_clocks_on_one_line(Bus& console, const uint8_t* tiles)
{
    PPU& ppu = console.ppu;

    while (ppu.in_reset_write_lockout()) {
        ppu.clock();
    }
    for (uint64_t guard = 0; guard < 2ull * 341 * 262; ++guard) {
        if (ppu.scanline == 245 && ppu.cycle == 0) {
            break;
        }
        ppu.clock();
    }

    // Eight sprites on the same line, spread across it so all eight fetch
    // groups hold a real sprite rather than the $FF dummy.
    ppu.write(0x2003, 0x00);
    for (int i = 0; i < 8; ++i) {
        ppu.write(0x2004, 110);                           // Y: visible on line 111
        ppu.write(0x2004, tiles[i]);                      // tile: bit 0 picks the table
        ppu.write(0x2004, 0x00);                          // attributes
        ppu.write(0x2004, static_cast<uint8_t>(i * 24));  // X
    }

    // $20 = 8x16 sprites, background at $0000. In 8x16 mode the sprite table
    // comes from bit 0 of the tile number, NOT from PPUCTRL - which is what
    // makes the alternating case expressible at all.
    ppu.write(0x2000, 0x20);
    ppu.write(0x2001, 0x18);

    console.write(0xC000, 0xFF);
    console.write(0xC001, 0x00);

    // Settle onto the line before counting, so the count covers one line
    // exactly rather than part of two.
    for (uint64_t guard = 0; guard < 2ull * 341 * 262; ++guard) {
        if (ppu.scanline == 110 && ppu.cycle == 0) {
            break;
        }
        ppu.clock();
    }

    int clocks = 0;
    uint8_t previous = console.rom.mmc3_irq_counter;
    while (ppu.scanline == 110) {
        ppu.clock();
        if (console.rom.mmc3_irq_counter != previous) {
            ++clocks;
            previous = console.rom.mmc3_irq_counter;
        }
    }
    return clocks;
}

}  // namespace

GTEST_TEST(mmc3A12Filter, alternating_pattern_tables_clock_the_counter_more_than_once)
{
    // All eight sprites out of $1000: A12 goes high for the whole fetch phase,
    // dropping only for the four-dot garbage pair between groups, which is
    // under the filter. One clock for the line, the ordinary case.
    int same_table = 0;
    {
        BankedRom rom("mmc3_a12_same_table.nes", 2, 2);
        Bus console;
        ASSERT_TRUE(console.load_cartridge(rom.path));
        const uint8_t tiles[8] = {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
        same_table = counter_clocks_on_one_line(console, tiles);
    }

    // Alternating: every other group drops to $0xxx, so A12 is low for that
    // group's four garbage dots PLUS its four pattern dots. Eight consecutive
    // low dots, plus the neighbouring garbage pairs, carries the low period
    // past the 9-dot filter and a second edge is counted.
    int alternating = 0;
    {
        BankedRom rom("mmc3_a12_alt_table.nes", 2, 2);
        Bus console;
        ASSERT_TRUE(console.load_cartridge(rom.path));
        const uint8_t tiles[8] = {0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00};
        alternating = counter_clocks_on_one_line(console, tiles);
    }

    EXPECT_EQ(1, same_table) << "sprites from one table give the ordinary one clock per scanline";
    EXPECT_GT(alternating, same_table)
        << "8x16 sprites from BOTH pattern tables must clock the counter more than once per\n"
           "  scanline - NESdev MMC3: the gap between A12 edges grows 'past the time that the\n"
           "  MMC3 is able to filter out'. Getting "
        << alternating
        << " means the garbage\n"
           "  nametable fetches at dots 257-260 are not holding A12 low.";
}

}  // namespace mmc3
}  // namespace tests
