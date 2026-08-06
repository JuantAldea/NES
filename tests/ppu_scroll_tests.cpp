// The loopy scroll registers: v, t, x and w.
//
// Every expectation here is a literal transcribed from NESdev's "PPU scrolling"
// page, with the derivation written out beside it. Nothing is computed from the
// implementation's own constants, because a test that recomputes what the code
// computes only proves the code is self-consistent.
//
//   v and t layout:  yyy NN YYYYY XXXXX
//                    ||| || ||||| +++++-- coarse X scroll
//                    ||| || +++++-------- coarse Y scroll
//                    ||| ++-------------- nametable select
//                    +++----------------- fine Y scroll
//
// v is PPU::registers.PPUADDR, t is PPU::temp_addr, x is PPU::fine_x, and w is
// the inverse of PPU::high_byte_input.
#include "../include/bus.h"
#include "gtest/gtest.h"

namespace tests
{
namespace scroll
{
namespace
{
// $2000/$2005/$2006 writes are ignored until the reset lockout expires.
void run_past_reset_lockout(PPU& ppu)
{
    while (ppu.in_reset_write_lockout()) {
        ppu.clock();
    }
}

// A PPU past the lockout, with rendering off and the dot counter parked
// somewhere harmless, so a test can set v/t by hand without the pipeline
// moving them underneath it.
PPU& ready(Bus& console)
{
    run_past_reset_lockout(console.ppu);
    console.ppu.scanline = PPU::post_render_scanline;
    console.ppu.cycle = 0;
    return console.ppu;
}
}  // namespace

// --- $2000: t: ...GH.. ........ <- d: ......GH -----------------------------

GTEST_TEST(testPPUScroll, ppuctrl_writes_the_nametable_select_into_t)
{
    Bus console;
    PPU& ppu = ready(console);

    // Bits 0-1 of $2000 are bits 10-11 of t. $03 -> both set -> $0C00.
    ppu.write(PPU::PPUCTRL, 0x03);
    EXPECT_EQ(0x0C00, ppu.temp_addr);

    ppu.write(PPU::PPUCTRL, 0x01);
    EXPECT_EQ(0x0400, ppu.temp_addr);

    ppu.write(PPU::PPUCTRL, 0x02);
    EXPECT_EQ(0x0800, ppu.temp_addr);

    // Nothing outside bits 10-11 moves: the other six bits of $2000 select
    // pattern tables, sprite size and NMI, none of which are part of t.
    ppu.write(PPU::PPUCTRL, 0xFC);
    EXPECT_EQ(0x0000, ppu.temp_addr);
}

GTEST_TEST(testPPUScroll, ppuctrl_leaves_the_rest_of_t_alone)
{
    Bus console;
    PPU& ppu = ready(console);

    // Put a full scroll into t first: $2005 $F8 then $2005 $F8.
    //   first  ($F8): coarse X <- $F8 >> 3 = $1F, x <- 0
    //   second ($F8): fine Y <- 0, coarse Y <- $1F -> bits 5-9
    //   t = ($1F << 5) | $1F = $03E0 | $001F = $03FF
    ppu.write(PPU::PPUSCROLL, 0xF8);
    ppu.write(PPU::PPUSCROLL, 0xF8);
    ASSERT_EQ(0x03FF, ppu.temp_addr);

    ppu.write(PPU::PPUCTRL, 0x02);
    EXPECT_EQ(0x0BFF, ppu.temp_addr) << "only bits 10-11 should have changed";
}

// --- $2005 -----------------------------------------------------------------

GTEST_TEST(testPPUScroll, ppuscroll_first_write_sets_coarse_x_and_fine_x)
{
    Bus console;
    PPU& ppu = ready(console);

    // t: ....... ...ABCDE <- d: ABCDE... ;  x: FGH <- d: .....FGH
    // $5D = 0101 1101 -> ABCDE = 01011 = $0B, FGH = 101 = 5
    ppu.write(PPU::PPUSCROLL, 0x5D);

    EXPECT_EQ(0x000B, ppu.temp_addr);
    EXPECT_EQ(0x05, ppu.fine_x);
    EXPECT_TRUE(ppu.write_toggle_w()) << "w must be 1 after the first write";
    EXPECT_EQ(0x0000, ppu.registers.PPUADDR) << "$2005 must not touch v";
}

GTEST_TEST(testPPUScroll, ppuscroll_second_write_sets_coarse_y_and_fine_y)
{
    Bus console;
    PPU& ppu = ready(console);

    ppu.write(PPU::PPUSCROLL, 0x00);  // consume the first slot

    // t: FGH..AB CDE..... <- d: ABCDEFGH
    // $A9 = 1010 1001 -> ABCDE = 10101 = $15 (coarse Y), FGH = 001 = 1 (fine Y)
    // t = (1 << 12) | ($15 << 5) = $1000 | $02A0 = $12A0
    ppu.write(PPU::PPUSCROLL, 0xA9);

    EXPECT_EQ(0x12A0, ppu.temp_addr);
    EXPECT_FALSE(ppu.write_toggle_w()) << "w must be 0 after the second write";
    EXPECT_EQ(0x0000, ppu.registers.PPUADDR) << "$2005 must not touch v";
    EXPECT_EQ(0x00, ppu.fine_x) << "the second write must not touch x";
}

GTEST_TEST(testPPUScroll, ppuscroll_second_write_preserves_coarse_x_and_nametable)
{
    Bus console;
    PPU& ppu = ready(console);

    ppu.write(PPU::PPUCTRL, 0x03);    // nametable select -> t bits 10-11 = $0C00
    ppu.write(PPU::PPUSCROLL, 0x1F);  // coarse X <- 3, x <- 7 ; t = $0C03
    ASSERT_EQ(0x0C03, ppu.temp_addr);

    // $FF: coarse Y <- $1F, fine Y <- 7 -> $7000 | $03E0
    ppu.write(PPU::PPUSCROLL, 0xFF);
    EXPECT_EQ(0x7FE3, ppu.temp_addr) << "$7000 | $0C00 | $03E0 | $0003";
}

// --- $2006 -----------------------------------------------------------------

GTEST_TEST(testPPUScroll, ppuaddr_first_write_clears_bit_14)
{
    Bus console;
    PPU& ppu = ready(console);

    // Set fine Y to 7 through $2005, which puts bit 14 of t up.
    ppu.write(PPU::PPUSCROLL, 0x00);
    ppu.write(PPU::PPUSCROLL, 0x07);
    ASSERT_EQ(0x7000, ppu.temp_addr) << "fine Y = 7";

    // t: .CDEFGH ........ <- d: ..CDEFGH, and bit 14 is cleared. $FF has both
    // top bits set; only bits 0-5 survive, into t bits 8-13.
    ppu.write(PPU::PPUADDR, 0xFF);
    EXPECT_EQ(0x3F00, ppu.temp_addr);
    EXPECT_TRUE(ppu.write_toggle_w());
}

GTEST_TEST(testPPUScroll, ppuaddr_second_write_copies_t_into_v)
{
    Bus console;
    PPU& ppu = ready(console);

    ppu.write(PPU::PPUADDR, 0x2C);
    EXPECT_EQ(0x0000, ppu.registers.PPUADDR) << "v must not move on the first write";

    ppu.write(PPU::PPUADDR, 0x59);
    EXPECT_EQ(0x2C59, ppu.temp_addr);
    EXPECT_EQ(0x2C59, ppu.registers.PPUADDR);
    EXPECT_FALSE(ppu.write_toggle_w());
}

GTEST_TEST(testPPUScroll, ppuaddr_and_ppuscroll_write_into_the_same_t)
{
    Bus console;
    PPU& ppu = ready(console);

    // $2006 first write: t = $2000. Then $2005's SECOND write lands (w is 1),
    // rewriting fine Y and coarse Y but leaving t bits 8-9 (the top of coarse
    // Y is in the same byte) - $2000 has coarse Y = 0 and nametable bit 11 set.
    ppu.write(PPU::PPUADDR, 0x20);
    ASSERT_EQ(0x2000, ppu.temp_addr);

    // $88 = 1000 1000 -> coarse Y <- $11, fine Y <- 0
    // t = ($2000 & ~$73E0) | ($11 << 5) = $0000 | $0220 = $0220
    ppu.write(PPU::PPUSCROLL, 0x88);
    EXPECT_EQ(0x0220, ppu.temp_addr);
    EXPECT_EQ(0x0000, ppu.registers.PPUADDR) << "no second $2006 write, so v never moved";
}

// --- $2002 read: w <- 0 ----------------------------------------------------

GTEST_TEST(testPPUScroll, ppustatus_read_clears_w_but_not_t_or_v)
{
    Bus console;
    PPU& ppu = ready(console);

    ppu.write(PPU::PPUADDR, 0x21);
    ppu.write(PPU::PPUADDR, 0x08);
    ASSERT_EQ(0x2108, ppu.registers.PPUADDR);
    ppu.write(PPU::PPUSCROLL, 0x1F);  // first write; w -> 1, coarse X <- 3, x <- 7
    ASSERT_TRUE(ppu.write_toggle_w());

    ppu.read(PPU::PPUSTATUS);

    EXPECT_FALSE(ppu.write_toggle_w());
    EXPECT_EQ(0x2103, ppu.temp_addr) << "t must survive the read";
    EXPECT_EQ(0x2108, ppu.registers.PPUADDR) << "v must survive the read";
    EXPECT_EQ(0x07, ppu.fine_x) << "x must survive the read";
}

// --- coarse X increment ----------------------------------------------------
//
//   if ((v & 0x001F) == 31) { v &= ~0x001F; v ^= 0x0400; }
//   else                    { v += 1; }

GTEST_TEST(testPPUScroll, coarse_x_increment_counts_within_the_nametable)
{
    Bus console;
    PPU& ppu = ready(console);

    ppu.registers.PPUADDR = 0x0000;
    ppu.increment_coarse_x();
    EXPECT_EQ(0x0001, ppu.registers.PPUADDR);

    ppu.registers.PPUADDR = 0x001E;
    ppu.increment_coarse_x();
    EXPECT_EQ(0x001F, ppu.registers.PPUADDR) << "30 -> 31 is an ordinary increment";
}

GTEST_TEST(testPPUScroll, coarse_x_increment_wraps_into_the_next_nametable)
{
    Bus console;
    PPU& ppu = ready(console);

    // Coarse X 31 with the horizontal nametable bit clear: it wraps to 0 and
    // the bit goes up. NOT a carry into coarse Y.
    ppu.registers.PPUADDR = 0x001F;
    ppu.increment_coarse_x();
    EXPECT_EQ(0x0400, ppu.registers.PPUADDR);

    // ...and back again, which is what makes it a toggle rather than an add.
    ppu.registers.PPUADDR = 0x041F;
    ppu.increment_coarse_x();
    EXPECT_EQ(0x0000, ppu.registers.PPUADDR);
}

GTEST_TEST(testPPUScroll, coarse_x_increment_leaves_y_and_the_vertical_nametable_alone)
{
    Bus console;
    PPU& ppu = ready(console);

    // fine Y = 5 ($5000), nametable = $0800, coarse Y = $0D ($01A0), coarse X = 31.
    ppu.registers.PPUADDR = 0x5000 | 0x0800 | 0x01A0 | 0x001F;
    ppu.increment_coarse_x();
    // Only bits 0-4 and bit 10 may move: coarse X -> 0, bit 10 -> 1.
    EXPECT_EQ(0x5000 | 0x0C00 | 0x01A0 | 0x0000, ppu.registers.PPUADDR);
}

// --- Y increment -----------------------------------------------------------
//
//   if ((v & 0x7000) != 0x7000) v += 0x1000;
//   else { v &= ~0x7000; y = (v & 0x03E0) >> 5;
//          if (y == 29) { y = 0; v ^= 0x0800; }
//          else if (y == 31) { y = 0; }
//          else { y += 1; }
//          v = (v & ~0x03E0) | (y << 5); }

GTEST_TEST(testPPUScroll, y_increment_counts_fine_y_first)
{
    Bus console;
    PPU& ppu = ready(console);

    ppu.registers.PPUADDR = 0x0000;
    for (int expected = 1; expected <= 7; ++expected) {
        ppu.increment_y();
        EXPECT_EQ(static_cast<uint16_t>(expected << 12), ppu.registers.PPUADDR)
            << "fine Y should be " << expected;
    }
}

GTEST_TEST(testPPUScroll, y_increment_carries_fine_y_into_coarse_y)
{
    Bus console;
    PPU& ppu = ready(console);

    // fine Y = 7, coarse Y = 3 ($0060). Fine Y resets and coarse Y becomes 4.
    ppu.registers.PPUADDR = 0x7000 | 0x0060;
    ppu.increment_y();
    EXPECT_EQ(0x0080, ppu.registers.PPUADDR) << "coarse Y 3 -> 4, fine Y -> 0";
}

GTEST_TEST(testPPUScroll, y_increment_wraps_coarse_y_at_29_and_flips_the_nametable)
{
    Bus console;
    PPU& ppu = ready(console);

    // A nametable is 30 tiles tall, so coarse Y 29 is the last row: it wraps to
    // 0 and toggles bit 11. 29 << 5 = $03A0.
    ppu.registers.PPUADDR = 0x7000 | 0x03A0;
    ppu.increment_y();
    EXPECT_EQ(0x0800, ppu.registers.PPUADDR);

    // And back, from the other nametable.
    ppu.registers.PPUADDR = 0x7000 | 0x0800 | 0x03A0;
    ppu.increment_y();
    EXPECT_EQ(0x0000, ppu.registers.PPUADDR);
}

GTEST_TEST(testPPUScroll, y_increment_wraps_coarse_y_at_31_without_flipping_the_nametable)
{
    Bus console;
    PPU& ppu = ready(console);

    // Coarse Y 30 and 31 address the attribute table rather than tiles; software
    // can only get there through $2006. 31 wraps to 0 and the nametable bit
    // stays put - this is the case that distinguishes the two wraps.
    // 31 << 5 = $03E0.
    ppu.registers.PPUADDR = 0x7000 | 0x03E0;
    ppu.increment_y();
    EXPECT_EQ(0x0000, ppu.registers.PPUADDR) << "no nametable flip at 31";

    ppu.registers.PPUADDR = 0x7000 | 0x0800 | 0x03E0;
    ppu.increment_y();
    EXPECT_EQ(0x0800, ppu.registers.PPUADDR) << "bit 11 must be unchanged";

    // 30 is an ordinary increment to 31. 30 << 5 = $03C0.
    ppu.registers.PPUADDR = 0x7000 | 0x03C0;
    ppu.increment_y();
    EXPECT_EQ(0x03E0, ppu.registers.PPUADDR);
}

GTEST_TEST(testPPUScroll, y_increment_leaves_coarse_x_and_the_horizontal_nametable_alone)
{
    Bus console;
    PPU& ppu = ready(console);

    // coarse X = $15, horizontal nametable bit set, coarse Y = 29, fine Y = 7.
    ppu.registers.PPUADDR = 0x7000 | 0x0400 | 0x03A0 | 0x0015;
    ppu.increment_y();
    EXPECT_EQ(0x0800 | 0x0400 | 0x0015, ppu.registers.PPUADDR);
}

// --- the t -> v copies -----------------------------------------------------

GTEST_TEST(testPPUScroll, horizontal_copy_takes_coarse_x_and_bit_10_only)
{
    Bus console;
    PPU& ppu = ready(console);

    // t: every bit set that a 15-bit t can hold.
    ppu.temp_addr = 0x7FFF;
    ppu.registers.PPUADDR = 0x0000;

    ppu.copy_horizontal_from_t();

    // v: ....A.. ...BCDEF -> bit 10 and bits 0-4 = $0400 | $001F = $041F.
    EXPECT_EQ(0x041F, ppu.registers.PPUADDR);
}

GTEST_TEST(testPPUScroll, horizontal_copy_clears_bits_that_are_clear_in_t)
{
    Bus console;
    PPU& ppu = ready(console);

    // The copy is a copy, not an OR: a v with those bits set and a t without
    // them must come back with them clear.
    ppu.temp_addr = 0x0000;
    ppu.registers.PPUADDR = 0x7FFF;

    ppu.copy_horizontal_from_t();

    EXPECT_EQ(0x7BE0, ppu.registers.PPUADDR) << "$7FFF with $041F removed";
}

GTEST_TEST(testPPUScroll, vertical_copy_takes_fine_y_coarse_y_and_bit_11_only)
{
    Bus console;
    PPU& ppu = ready(console);

    ppu.temp_addr = 0x7FFF;
    ppu.registers.PPUADDR = 0x0000;

    ppu.copy_vertical_from_t();

    // v: GHIA.BC DEF..... -> $7000 | $0800 | $03E0 = $7BE0.
    EXPECT_EQ(0x7BE0, ppu.registers.PPUADDR);
}

GTEST_TEST(testPPUScroll, vertical_copy_clears_bits_that_are_clear_in_t)
{
    Bus console;
    PPU& ppu = ready(console);

    ppu.temp_addr = 0x0000;
    ppu.registers.PPUADDR = 0x7FFF;

    ppu.copy_vertical_from_t();

    EXPECT_EQ(0x041F, ppu.registers.PPUADDR) << "$7FFF with $7BE0 removed";
}

// --- the addresses derived from v ------------------------------------------

GTEST_TEST(testPPUScroll, tile_address_is_2000_plus_the_low_twelve_bits_of_v)
{
    Bus console;
    PPU& ppu = ready(console);

    // tile address = 0x2000 | (v & 0x0FFF): fine Y is masked off, the nametable
    // select and both coarse counters are not.
    ppu.registers.PPUADDR = 0x0000;
    EXPECT_EQ(0x2000, ppu.tile_address());

    ppu.registers.PPUADDR = 0x7FFF;
    EXPECT_EQ(0x2FFF, ppu.tile_address());

    // coarse X = $0D, coarse Y = $12 ($0240), nametable 1, fine Y = 3.
    ppu.registers.PPUADDR = 0x3000 | 0x0400 | 0x0240 | 0x000D;
    EXPECT_EQ(0x264D, ppu.tile_address());
}

GTEST_TEST(testPPUScroll, attribute_address_indexes_32x32_pixel_blocks)
{
    Bus console;
    PPU& ppu = ready(console);

    // attribute address = 0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 0x07)
    ppu.registers.PPUADDR = 0x0000;
    EXPECT_EQ(0x23C0, ppu.attribute_address());

    // coarse X = $1F, coarse Y = $1F, nametable 3, fine Y = 7 -> v = $7FFF.
    // (v >> 4) & 0x38 = $38, (v >> 2) & 0x07 = $07.
    ppu.registers.PPUADDR = 0x7FFF;
    EXPECT_EQ(0x2FFF, ppu.attribute_address()) << "$23C0 | $0C00 | $38 | $07";

    // One attribute byte covers four tiles in each direction, so coarse X 8-11
    // and coarse Y 4-7 all land on the same byte. coarse X = 9, coarse Y = 5:
    //   (v >> 4) & 0x38 = (($00A9) >> 4) & $38 = $0A & $38 = $08
    //   (v >> 2) & 0x07 = ($00A9 >> 2) & $07 = $2A & $07 = $02
    ppu.registers.PPUADDR = (5 << 5) | 9;
    EXPECT_EQ(0x23CA, ppu.attribute_address());
    ppu.registers.PPUADDR = (7 << 5) | 11;
    EXPECT_EQ(0x23CA, ppu.attribute_address()) << "same 32x32 block, same byte";
    ppu.registers.PPUADDR = (8 << 5) | 11;
    EXPECT_EQ(0x23D2, ppu.attribute_address()) << "one block down: +8";
    ppu.registers.PPUADDR = (7 << 5) | 12;
    EXPECT_EQ(0x23CB, ppu.attribute_address()) << "one block right: +1";
}

// --- $2007 during rendering ------------------------------------------------

GTEST_TEST(testPPUScroll, ppudata_access_during_rendering_increments_the_scroll)
{
    Bus console;
    PPU& ppu = ready(console);

    ppu.write(PPU::PPUMASK, 0x08);  // show background -> rendering enabled
    ppu.scanline = 100;             // a visible scanline
    ppu.cycle = 50;

    // coarse X = 3, coarse Y = 4 ($0080), fine Y = 0.
    ppu.registers.PPUADDR = 0x0083;
    ppu.write(PPU::PPUDATA, 0x00);

    // A coarse X increment AND a Y increment, not +1: coarse X 3 -> 4 and fine
    // Y 0 -> 1.
    EXPECT_EQ(0x1084, ppu.registers.PPUADDR);
}

GTEST_TEST(testPPUScroll, ppudata_access_outside_rendering_uses_the_plain_step)
{
    Bus console;
    PPU& ppu = ready(console);

    ppu.write(PPU::PPUMASK, 0x08);
    ppu.scanline = PPU::vblank_start_scanline;  // rendering on, but no fetches here
    ppu.cycle = 50;

    ppu.registers.PPUADDR = 0x0083;
    ppu.write(PPU::PPUDATA, 0x00);
    EXPECT_EQ(0x0084, ppu.registers.PPUADDR);

    // ...and with rendering disabled on a visible scanline, likewise.
    ppu.write(PPU::PPUMASK, 0x00);
    ppu.scanline = 100;
    ppu.registers.PPUADDR = 0x0083;
    ppu.write(PPU::PPUDATA, 0x00);
    EXPECT_EQ(0x0084, ppu.registers.PPUADDR);
}

// v is fifteen bits wide, not fourteen. The PPU's address BUS is fourteen -
// which is why $3000-$3EFF mirrors the nametables - but v itself carries fine
// Y in bits 12-14, and the $2007 increment wraps modulo $8000.
//
// Masking with the bus width instead looks harmless, because the extra bit is
// never driven onto the bus. It is not: it clears the top bit of fine Y. The
// middle case below is the one that bites in practice - the pre-render line
// copies a fine Y of 4-7 out of t, and a single $2007 access with rendering
// off would drag it into 0-3 for the rest of the frame.
GTEST_TEST(testPPUScroll, ppudata_increment_wraps_at_15_bits_not_at_the_bus_width)
{
    Bus console;
    PPU& ppu = ready(console);

    ppu.write(PPU::PPUMASK, 0x00);  // rendering off: the plain-step path
    ppu.scanline = 100;

    // $3FFF + 1. A fourteen-bit wrap gives $0000; the register is wider than
    // that, so the carry out of bit 13 lands in bit 14 and fine Y becomes 4.
    ppu.registers.PPUADDR = 0x3FFF;
    ppu.write(PPU::PPUDATA, 0x00);
    EXPECT_EQ(0x4000, ppu.registers.PPUADDR) << "the carry out of bit 13 belongs in bit 14, not discarded";

    // A mid-range address with fine Y = 5 must keep it. Masking to fourteen
    // bits would return $1001 here and silently lose the 4.
    ppu.registers.PPUADDR = 0x5000;
    ppu.write(PPU::PPUDATA, 0x00);
    EXPECT_EQ(0x5001, ppu.registers.PPUADDR) << "fine Y bit 2 must survive a $2007 access";

    // And it does wrap at $8000: $7FFF + 1 == $0000.
    ppu.registers.PPUADDR = 0x7FFF;
    ppu.write(PPU::PPUDATA, 0x00);
    EXPECT_EQ(0x0000, ppu.registers.PPUADDR);
}

GTEST_TEST(testPPUScroll, ppudata_read_during_rendering_increments_the_scroll_too)
{
    Bus console;
    PPU& ppu = ready(console);

    ppu.write(PPU::PPUMASK, 0x08);
    ppu.scanline = PPU::pre_render_scanline;  // the pre-render line fetches as well
    ppu.cycle = 50;

    ppu.registers.PPUADDR = 0x0083;
    ppu.read(PPU::PPUDATA);
    EXPECT_EQ(0x1084, ppu.registers.PPUADDR);
}

}  // namespace scroll
}  // namespace tests
