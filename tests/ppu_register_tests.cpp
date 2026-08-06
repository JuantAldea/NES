#include "../include/bus.h"
#include "gtest/gtest.h"

// Regression tests for the PPU register file: the $2005/$2006 two-write
// sequence, the $2002 read side effects, the VRAM address increment and the
// post-reset write lockout.

namespace tests
{
namespace
{
// Writes to PPUCTRL/PPUMASK/PPUSCROLL/PPUADDR are ignored until the reset
// lockout expires, so most tests have to run the PPU past it first.
void run_past_reset_lockout(PPU& ppu)
{
    while (ppu.in_reset_write_lockout()) {
        ppu.clock();
    }
}

void write_addr(PPU& ppu, uint8_t high, uint8_t low)
{
    ppu.write(PPU::PPUADDR, high);
    ppu.write(PPU::PPUADDR, low);
}
}  // namespace

// --- $2006 two-write sequence -------------------------------------------

GTEST_TEST(testPPURegisters, ppuaddr_assembles_both_writes)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    write_addr(console.ppu, 0x21, 0x08);

    EXPECT_EQ(0x2108, console.ppu.registers.PPUADDR);
}

GTEST_TEST(testPPURegisters, ppuaddr_holds_the_full_14_bit_range)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    // The nametables and the palette live well past the first 256 bytes of
    // VRAM, so an 8-bit address register cannot reach them.
    // A DISTINCT value per address. Writing the same byte everywhere would let
    // this pass even if the decode collapsed every address onto one cell.
    struct Probe {
        uint16_t addr;
        uint8_t value;
    };
    const Probe probes[] = {{0x2000, 0x11}, {0x23FF, 0x12}, {0x2400, 0x13}, {0x2800, 0x14},
                            {0x2C00, 0x15}, {0x3F00, 0x16}, {0x3F1F, 0x17}, {0x3FFF, 0x18}};

    for (const Probe& probe : probes) {
        write_addr(console.ppu, probe.addr >> 8, probe.addr & 0xFF);
        EXPECT_EQ(probe.addr, console.ppu.registers.PPUADDR) << "address " << std::hex << probe.addr;

        console.ppu.write(PPU::PPUDATA, probe.value);
        EXPECT_EQ(probe.value, console.ppu.ppu_bus_read(probe.addr)) << "address " << std::hex << probe.addr;
    }

    // ...and the writes did not all land in one cell, which is what a collapsed
    // decode would produce.
    //
    // Mind the mirroring: with no cartridge loaded the default is horizontal,
    // so $2400 aliases $2000 and $2C00 aliases $2800 - the later write of each
    // pair overwrote the earlier one, and expecting otherwise here would be
    // asserting against the very mirroring this decode implements.
    EXPECT_EQ(0x13, console.ppu.ppu_bus_read(0x2000)) << "$2400's write should have landed here too";
    EXPECT_EQ(0x15, console.ppu.ppu_bus_read(0x2800)) << "$2C00's write should have landed here too";
    EXPECT_EQ(0x12, console.ppu.ppu_bus_read(0x23FF)) << "nothing else wrote here";

    // The two screens are genuinely distinct storage.
    EXPECT_NE(console.ppu.ppu_bus_read(0x2000), console.ppu.ppu_bus_read(0x2800));
}

GTEST_TEST(testPPURegisters, ppuaddr_first_write_drops_the_top_two_bits)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    // The VRAM address is 14 bits wide; $C1 -> $01.
    write_addr(console.ppu, 0xC1, 0x23);

    EXPECT_EQ(0x0123, console.ppu.registers.PPUADDR);
}

GTEST_TEST(testPPURegisters, ppuaddr_writes_stay_within_vram)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    // An access at the very top of the address space must land in VRAM rather
    // than running past the end of the array.
    // $3FFF is palette space, and palette RAM is six bits wide, so the value
    // is chosen to survive that truncation - this test is about addressing,
    // not about palette width.
    write_addr(console.ppu, 0x3F, 0xFF);
    console.ppu.write(PPU::PPUDATA, 0x37);

    EXPECT_EQ(0x37, console.ppu.ppu_bus_read(0x3FFF));

    // CHANGED: this used to expect v == $0000, on the reasoning that the
    // address "wraps rather than running past the end of the VRAM array". The
    // safety being described is real, but it comes from somewhere else:
    // ppu_bus_read/write fold every access with vram_addr_mask, so VRAM cannot
    // be over-indexed whatever v holds - which is why the assertion above
    // still passes unchanged.
    //
    // v itself is FIFTEEN bits, because bits 12-14 are fine Y (NESdev, "PPU
    // scrolling"). $3FFF + 1 is $4000, and discarding that carry would clear
    // the top bit of fine Y. The old expectation conflated the register with
    // the 14-bit bus.
    EXPECT_EQ(0x4000, console.ppu.registers.PPUADDR);
}

// --- $2005 two-write sequence -------------------------------------------

GTEST_TEST(testPPURegisters, ppuscroll_keeps_both_writes)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    console.ppu.write(PPU::PPUSCROLL, 0x7D);  // X
    console.ppu.write(PPU::PPUSCROLL, 0x5E);  // Y

    EXPECT_EQ(0x7D, console.ppu.scroll_x());
    EXPECT_EQ(0x5E, console.ppu.scroll_y());

    // CHANGED: this used to assert registers.PPUSCROLL == 0x7D5E, a register
    // hardware does not have. $2005 writes into t and x:
    //
    //   first  ($7D = 0111 1101):  t coarse X <- 0x7D >> 3 = $0F
    //                              x          <- 0x7D & 7  = 5
    //   second ($5E = 0101 1110):  t fine Y   <- 0x5E & 7  = 6  -> bits 12-14
    //                              t coarse Y <- 0x5E >> 3 = $0B -> bits 5-9
    //
    //   t = (6 << 12) | (0x0B << 5) | 0x0F = $6000 | $0160 | $000F = $616F
    EXPECT_EQ(0x616F, console.ppu.temp_addr);
    EXPECT_EQ(0x05, console.ppu.fine_x);

    // ...and v is untouched. $2005 stages the scroll; only $2006's second
    // write, dot 257 and pre-render 280-304 move it into v.
    EXPECT_EQ(0x0000, console.ppu.registers.PPUADDR);
}

GTEST_TEST(testPPURegisters, ppuscroll_and_ppuaddr_share_the_write_toggle)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    console.ppu.write(PPU::PPUSCROLL, 0x11);  // consumes the first write slot
    console.ppu.write(PPU::PPUADDR, 0x22);    // lands in the second slot

    EXPECT_EQ(0x0022, console.ppu.registers.PPUADDR);
    EXPECT_EQ(0x11, console.ppu.scroll_x());
}

// --- $2002 read resets the write toggle ---------------------------------

GTEST_TEST(testPPURegisters, ppustatus_read_resets_the_write_toggle)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    EXPECT_TRUE(console.ppu.high_byte_input);

    console.ppu.write(PPU::PPUADDR, 0x21);
    EXPECT_FALSE(console.ppu.high_byte_input);

    console.ppu.read(PPU::PPUSTATUS);
    EXPECT_TRUE(console.ppu.high_byte_input);

    // Because the toggle was reset, this is treated as a first write, so it
    // only stages the high byte.
    console.ppu.write(PPU::PPUADDR, 0x08);
    EXPECT_EQ(0x0800, console.ppu.temp_addr);
    EXPECT_EQ(0x0000, console.ppu.registers.PPUADDR);

    console.ppu.write(PPU::PPUADDR, 0x08);
    EXPECT_EQ(0x0808, console.ppu.registers.PPUADDR);
}

GTEST_TEST(testPPURegisters, ppuaddr_is_only_committed_by_the_second_write)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    write_addr(console.ppu, 0x20, 0x00);

    // A half-finished write pair must not disturb the address PPUDATA uses.
    console.ppu.write(PPU::PPUADDR, 0x3F);
    EXPECT_EQ(0x2000, console.ppu.registers.PPUADDR);

    console.ppu.write(PPU::PPUDATA, 0xC3);
    EXPECT_EQ(0xC3, console.ppu.ppu_bus_read(0x2000));
    EXPECT_EQ(0x00, console.ppu.ppu_bus_read(0x3F00));

    console.ppu.write(PPU::PPUADDR, 0x00);
    EXPECT_EQ(0x3F00, console.ppu.registers.PPUADDR);
}

GTEST_TEST(testPPURegisters, ppustatus_read_preserves_the_addresses)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    write_addr(console.ppu, 0x21, 0x08);
    console.ppu.write(PPU::PPUSCROLL, 0x33);
    console.ppu.write(PPU::PPUSCROLL, 0x44);

    console.ppu.read(PPU::PPUSTATUS);

    // Hardware clears only the write toggle; it leaves v and t alone.
    EXPECT_EQ(0x2108, console.ppu.registers.PPUADDR);

    // CHANGED: this used to assert registers.PPUSCROLL == 0x3344. The scroll
    // has no register of its own; the two $2005 writes went into t, on top of
    // the $2108 the $2006 pair had just put there:
    //
    //   after $2006 $21,$08:       t = $2108
    //   $2005 first  ($33):        t coarse X <- $33 >> 3 = 6, x <- 3
    //                              t = ($2108 & ~$001F) | 6 = $2106
    //   $2005 second ($44):        t fine Y <- $44 & 7 = 4, coarse Y <- 8
    //                              t = ($2106 & ~$73E0) | (4 << 12) | (8 << 5)
    //                                = $0006 | $4000 | $0100 = $4106
    EXPECT_EQ(0x4106, console.ppu.temp_addr);
    EXPECT_EQ(0x03, console.ppu.fine_x);

    // Which still reads back as the bytes that were written.
    EXPECT_EQ(0x33, console.ppu.scroll_x());
    EXPECT_EQ(0x44, console.ppu.scroll_y());

    // The retained address is still the one PPUDATA uses.
    console.ppu.write(PPU::PPUDATA, 0x5A);
    EXPECT_EQ(0x5A, console.ppu.ppu_bus_read(0x2108));
}

// --- VRAM address increment ---------------------------------------------

GTEST_TEST(testPPURegisters, vertical_vram_step_is_32)
{
    EXPECT_EQ(32, PPU::Vertical);
    EXPECT_EQ(1, PPU::Horizontal);
}

GTEST_TEST(testPPURegisters, ppuctrl_selects_the_vram_step)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    console.ppu.write(PPU::PPUCTRL, 0x00);
    EXPECT_EQ(PPU::Horizontal, console.ppu.vram_step);

    console.ppu.write(PPU::PPUCTRL, 0x04);
    EXPECT_EQ(PPU::Vertical, console.ppu.vram_step);
}

GTEST_TEST(testPPURegisters, ppudata_write_honours_the_vertical_step)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    console.ppu.write(PPU::PPUCTRL, 0x04);  // increment by 32
    write_addr(console.ppu, 0x20, 0x00);

    console.ppu.write(PPU::PPUDATA, 0x01);
    EXPECT_EQ(0x2020, console.ppu.registers.PPUADDR);
    console.ppu.write(PPU::PPUDATA, 0x02);
    EXPECT_EQ(0x2040, console.ppu.registers.PPUADDR);
    console.ppu.write(PPU::PPUDATA, 0x03);
    EXPECT_EQ(0x2060, console.ppu.registers.PPUADDR);

    EXPECT_EQ(0x01, console.ppu.ppu_bus_read(0x2000));
    EXPECT_EQ(0x02, console.ppu.ppu_bus_read(0x2020));
    EXPECT_EQ(0x03, console.ppu.ppu_bus_read(0x2040));
    // Nothing should have landed at the horizontal-step positions.
    EXPECT_EQ(0x00, console.ppu.ppu_bus_read(0x2001));
    EXPECT_EQ(0x00, console.ppu.ppu_bus_read(0x2002));
}

GTEST_TEST(testPPURegisters, ppudata_read_and_write_use_the_same_step)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    for (const uint8_t ctrl : {uint8_t{0x00}, uint8_t{0x04}}) {
        console.ppu.write(PPU::PPUCTRL, ctrl);
        const uint16_t step = console.ppu.vram_step;

        write_addr(console.ppu, 0x20, 0x00);
        console.ppu.write(PPU::PPUDATA, 0xEE);
        const uint16_t after_write = console.ppu.registers.PPUADDR;

        // $2007 reads below $3F00 are delayed by one: the first read returns
        // whatever the latch held and refills it, so the byte just written
        // only comes back on the second read. This test is about the ADDRESS
        // INCREMENT, so the priming read gets its own address setup and the
        // increment is measured across the second one alone.
        write_addr(console.ppu, 0x20, 0x00);
        console.ppu.read(PPU::PPUDATA);  // priming read: fills the latch

        write_addr(console.ppu, 0x20, 0x00);
        EXPECT_EQ(0xEE, console.ppu.read(PPU::PPUDATA)) << "ctrl " << std::hex << (unsigned)ctrl;
        const uint16_t after_read = console.ppu.registers.PPUADDR;

        EXPECT_EQ(0x2000 + step, after_write) << "ctrl " << std::hex << (unsigned)ctrl;
        EXPECT_EQ(after_write, after_read) << "ctrl " << std::hex << (unsigned)ctrl;
    }
}

// --- post-reset write lockout -------------------------------------------

GTEST_TEST(testPPURegisters, reset_lockout_is_expressed_in_ppu_cycles)
{
    // ~29658 CPU cycles, and the PPU runs three times faster than the CPU.
    EXPECT_EQ(29658u, PPU::reset_lockout_cpu_cycles);
    EXPECT_EQ(88974u, PPU::reset_lockout_cycles);
}

GTEST_TEST(testPPURegisters, ppuctrl_writes_are_ignored_until_the_lockout_ends)
{
    Bus console;

    EXPECT_TRUE(console.ppu.in_reset_write_lockout());
    console.ppu.write(PPU::PPUCTRL, 0xFF);
    EXPECT_EQ(0x00, console.ppu.registers.PPUCTRL);
    // update_flags() must not have run either.
    EXPECT_FALSE(console.ppu.MMI_on_V_Blank);

    while (console.ppu.total_cycles < PPU::reset_lockout_cycles - 1) {
        console.ppu.clock();
    }
    console.ppu.write(PPU::PPUCTRL, 0xFF);
    EXPECT_EQ(0x00, console.ppu.registers.PPUCTRL) << "still one cycle short";

    console.ppu.clock();
    EXPECT_EQ(PPU::reset_lockout_cycles, console.ppu.total_cycles);
    EXPECT_FALSE(console.ppu.in_reset_write_lockout());

    console.ppu.write(PPU::PPUCTRL, 0xFF);
    EXPECT_EQ(0xFF, console.ppu.registers.PPUCTRL);
    EXPECT_TRUE(console.ppu.MMI_on_V_Blank);
}

GTEST_TEST(testPPURegisters, lockout_covers_mask_scroll_and_addr)
{
    Bus console;
    ASSERT_TRUE(console.ppu.in_reset_write_lockout());

    console.ppu.write(PPU::PPUMASK, 0xFF);
    EXPECT_EQ(0x00, console.ppu.registers.PPUMASK);
    EXPECT_FALSE(console.ppu.show_background);

    // CHANGED: this used to assert registers.PPUSCROLL == 0x0000. $2005 writes
    // into t and x, so those are what must be untouched by a locked-out write.
    console.ppu.write(PPU::PPUSCROLL, 0xFF);
    console.ppu.write(PPU::PPUSCROLL, 0xFF);
    EXPECT_EQ(0x0000, console.ppu.temp_addr);
    EXPECT_EQ(0x00, console.ppu.fine_x);

    write_addr(console.ppu, 0x21, 0x08);
    EXPECT_EQ(0x0000, console.ppu.registers.PPUADDR);

    // An ignored write must not consume a write-toggle slot either.
    EXPECT_TRUE(console.ppu.high_byte_input);
}

GTEST_TEST(testPPURegisters, lockout_does_not_cover_oam_or_ppudata)
{
    Bus console;
    ASSERT_TRUE(console.ppu.in_reset_write_lockout());

    console.ppu.write(PPU::OAMADDR, 0x10);
    EXPECT_EQ(0x10, console.ppu.registers.OAMADDR);

    console.ppu.write(PPU::OAMDATA, 0x99);
    EXPECT_EQ(0x99, console.ppu.OAM_memory[0x10]);

    console.ppu.write(PPU::PPUDATA, 0x88);
    EXPECT_EQ(0x88, console.ppu.ppu_bus_read(0x0000));
}

// --- open bus ------------------------------------------------------------

GTEST_TEST(testPPURegisters, ppustatus_merges_open_bus_into_the_low_five_bits)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    console.ppu.write(PPU::PPUCTRL, 0xFF);  // puts $FF on the I/O bus
    console.ppu.set_vblank();

    // Bits 7-5 come from the PPU, bits 4-0 from the bus latch.
    EXPECT_EQ(0x9F, console.ppu.read(PPU::PPUSTATUS));
}

GTEST_TEST(testPPURegisters, ppustatus_read_clears_vblank)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    console.ppu.write(PPU::PPUCTRL, 0x00);  // clear the bus latch
    console.ppu.set_vblank();

    EXPECT_EQ(0x80, console.ppu.read(PPU::PPUSTATUS));
    // Reading it clears vblank but leaves the other flags alone.
    EXPECT_EQ(0x00, console.ppu.read(PPU::PPUSTATUS));
}

GTEST_TEST(testPPURegisters, ppustatus_read_keeps_sprite_flags)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    console.ppu.write(PPU::PPUCTRL, 0x00);
    console.ppu.set_vblank();
    console.ppu.set_sprite0_hit();
    console.ppu.set_sprite_overflow();

    EXPECT_EQ(0xE0, console.ppu.read(PPU::PPUSTATUS));
    EXPECT_EQ(0x60, console.ppu.read(PPU::PPUSTATUS));
}

GTEST_TEST(testPPURegisters, write_only_registers_read_back_the_open_bus)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    console.ppu.write(PPU::PPUCTRL, 0x3C);

    for (const uint16_t addr : {PPU::PPUCTRL, PPU::PPUMASK, PPU::OAMADDR, PPU::PPUSCROLL, PPU::PPUADDR, PPU::OAMDMA}) {
        EXPECT_EQ(0x3C, console.ppu.read(addr)) << "register " << std::hex << addr;
    }
}

GTEST_TEST(testPPURegisters, reads_refresh_the_open_bus)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    console.ppu.write(PPU::PPUCTRL, 0x00);
    write_addr(console.ppu, 0x20, 0x00);
    console.ppu.write(PPU::PPUDATA, 0x6B);

    // Two reads: $2007 below $3F00 is buffered, so the first returns the stale
    // latch and refills it, and the byte written above only arrives on the
    // second. What reaches the open bus is the value the read RETURNED, not the
    // one it fetched.
    write_addr(console.ppu, 0x20, 0x00);
    console.ppu.read(PPU::PPUDATA);  // priming read

    write_addr(console.ppu, 0x20, 0x00);
    EXPECT_EQ(0x6B, console.ppu.read(PPU::PPUDATA));

    // The value just read is now what a write-only register hands back.
    EXPECT_EQ(0x6B, console.ppu.read(PPU::PPUMASK));
}

GTEST_TEST(testPPURegisters, writing_ppustatus_only_moves_the_open_bus)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    console.ppu.set_vblank();
    console.ppu.write(PPU::PPUSTATUS, 0x1F);

    // The flags are untouched, but the latch took the value.
    EXPECT_EQ(0x9F, console.ppu.read(PPU::PPUSTATUS));
}

// --- PPUCTRL decoding ----------------------------------------------------

GTEST_TEST(testPPURegisters, ppuctrl_decodes_nametable_and_pattern_tables)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    console.ppu.write(PPU::PPUCTRL, 0x00);
    EXPECT_EQ(0x2000, console.ppu.base_nametable_addr);
    EXPECT_EQ(0x0000, console.ppu.sprite_pattern_8x8_table_addr);
    EXPECT_EQ(0x0000, console.ppu.bg_pattern_table_address);

    console.ppu.write(PPU::PPUCTRL, 0x03);
    EXPECT_EQ(0x2C00, console.ppu.base_nametable_addr);

    // Bit 3 selects the sprite pattern table, bit 4 the background one:
    // 0 -> $0000, 1 -> $1000.
    console.ppu.write(PPU::PPUCTRL, 0x08);
    EXPECT_EQ(0x1000, console.ppu.sprite_pattern_8x8_table_addr);
    EXPECT_EQ(0x0000, console.ppu.bg_pattern_table_address);

    console.ppu.write(PPU::PPUCTRL, 0x10);
    EXPECT_EQ(0x0000, console.ppu.sprite_pattern_8x8_table_addr);
    EXPECT_EQ(0x1000, console.ppu.bg_pattern_table_address);
}

GTEST_TEST(testPPURegisters, ppumask_decodes_its_flags)
{
    Bus console;
    run_past_reset_lockout(console.ppu);

    console.ppu.write(PPU::PPUMASK, 0xFF);
    EXPECT_TRUE(console.ppu.greyscale);
    EXPECT_TRUE(console.ppu.show_bg_in_leftmost);
    EXPECT_TRUE(console.ppu.show_sprites_in_leftmost);
    EXPECT_TRUE(console.ppu.show_background);
    EXPECT_TRUE(console.ppu.show_sprites);
    EXPECT_TRUE(console.ppu.emphasize_red);
    EXPECT_TRUE(console.ppu.emphasize_green);
    EXPECT_TRUE(console.ppu.emphasize_blue);

    console.ppu.write(PPU::PPUMASK, 0x00);
    EXPECT_FALSE(console.ppu.greyscale);
    EXPECT_FALSE(console.ppu.show_background);
    EXPECT_FALSE(console.ppu.show_sprites);
    EXPECT_FALSE(console.ppu.emphasize_blue);
}

// --- frame state machine -------------------------------------------------
//
// PPU::clock used to declare `scanline` as a local, reset on every call, so
// the state machine never left scanline 1: vblank was never entered and NMI
// never raised. These pin the frame structure that fix restored.

namespace
{
// Clocks until (scanline, cycle) is reached, with a bound so a broken state
// machine fails instead of spinning forever.
bool clock_until_dot(PPU& ppu, int target_scanline, int target_cycle)
{
    const uint64_t limit = 2ull * PPU::dots_per_scanline * PPU::scanlines_per_frame;
    for (uint64_t i = 0; i < limit; ++i) {
        if (ppu.scanline == target_scanline && ppu.cycle == target_cycle) {
            return true;
        }
        ppu.clock();
    }
    return false;
}
}  // namespace

GTEST_TEST(testPPUFrame, dot_counter_advances_and_wraps)
{
    Bus console;
    PPU& ppu = console.ppu;

    EXPECT_EQ(0, ppu.scanline);
    EXPECT_EQ(0, ppu.cycle);

    ppu.clock();
    EXPECT_EQ(0, ppu.scanline);
    EXPECT_EQ(1, ppu.cycle) << "cycle must advance between clocks";

    // Run out the rest of the scanline; the next dot rolls onto scanline 1.
    for (int i = 1; i < PPU::dots_per_scanline; ++i) {
        ppu.clock();
    }
    EXPECT_EQ(1, ppu.scanline);
    EXPECT_EQ(0, ppu.cycle);
}

GTEST_TEST(testPPUFrame, frame_wraps_after_262_scanlines)
{
    Bus console;
    PPU& ppu = console.ppu;

    // Rendering disabled, so no odd-frame dot is skipped and a frame is
    // exactly 341 * 262 dots.
    ASSERT_FALSE(ppu.rendering_enabled());
    for (uint64_t i = 0; i < 1ull * PPU::dots_per_scanline * PPU::scanlines_per_frame; ++i) {
        ppu.clock();
    }

    EXPECT_EQ(0, ppu.scanline);
    EXPECT_EQ(0, ppu.cycle);
    EXPECT_EQ(1u, ppu.frame);
}

GTEST_TEST(testPPUFrame, vblank_is_set_at_241_1_and_cleared_at_261_1)
{
    Bus console;
    PPU& ppu = console.ppu;

    ASSERT_TRUE(clock_until_dot(ppu, PPU::vblank_start_scanline, 1));
    EXPECT_EQ(0x00, ppu.registers.PPUSTATUS & 0x80) << "vblank must not be set before dot 1 is processed";

    ppu.clock();  // process (241, 1)
    EXPECT_EQ(0x80, ppu.registers.PPUSTATUS & 0x80) << "vblank must be set at scanline 241 dot 1";

    ASSERT_TRUE(clock_until_dot(ppu, PPU::pre_render_scanline, 1));
    EXPECT_EQ(0x80, ppu.registers.PPUSTATUS & 0x80) << "vblank must persist through the vblank lines";

    ppu.clock();  // process (261, 1)
    EXPECT_EQ(0x00, ppu.registers.PPUSTATUS & 0x80) << "pre-render line must clear vblank";
}

GTEST_TEST(testPPUFrame, exactly_one_vblank_period_per_frame)
{
    Bus console;
    PPU& ppu = console.ppu;

    int rising_edges = 0;
    bool previous = false;
    const int frames = 3;
    for (uint64_t i = 0; i < 1ull * frames * PPU::dots_per_scanline * PPU::scanlines_per_frame; ++i) {
        ppu.clock();
        const bool now = ppu.registers.PPUSTATUS & 0x80;
        if (now && !previous) {
            ++rising_edges;
        }
        previous = now;
    }

    EXPECT_EQ(frames, rising_edges);
    EXPECT_EQ(static_cast<uint64_t>(frames), ppu.frame);
}

GTEST_TEST(testPPUFrame, pre_render_line_clears_sprite_flags)
{
    Bus console;
    PPU& ppu = console.ppu;

    ppu.set_sprite0_hit();
    ppu.set_sprite_overflow();

    ASSERT_TRUE(clock_until_dot(ppu, PPU::pre_render_scanline, 1));
    ppu.clock();

    EXPECT_EQ(0x00, ppu.registers.PPUSTATUS & 0x60);
}

GTEST_TEST(testPPUFrame, nmi_is_not_raised_while_ppuctrl_bit7_is_clear)
{
    Bus console;
    PPU& ppu = console.ppu;

    ASSERT_FALSE(ppu.MMI_on_V_Blank);
    for (uint64_t i = 0; i < 2ull * PPU::dots_per_scanline * PPU::scanlines_per_frame; ++i) {
        ppu.clock();
        ASSERT_FALSE(console.cpu.nmi_pending()) << "NMI raised with PPUCTRL bit 7 clear, at scanline " << ppu.scanline;
    }
    EXPECT_EQ(2u, ppu.frame);
}

GTEST_TEST(testPPUFrame, nmi_is_raised_at_vblank_when_enabled)
{
    Bus console;
    PPU& ppu = console.ppu;

    // Get past the reset lockout so PPUCTRL is writable, then enable NMI
    // outside of vblank so the enable itself does not raise one.
    while (ppu.in_reset_write_lockout()) {
        ppu.clock();
    }
    ASSERT_TRUE(clock_until_dot(ppu, 0, 0));
    ppu.write(PPU::PPUCTRL, 0x80);
    ASSERT_TRUE(ppu.MMI_on_V_Blank);
    ASSERT_FALSE(console.cpu.nmi_pending());

    ASSERT_TRUE(clock_until_dot(ppu, PPU::vblank_start_scanline, 1));
    EXPECT_FALSE(console.cpu.nmi_pending()) << "not yet: dot 1 has not been processed";

    ppu.clock();
    EXPECT_TRUE(console.cpu.nmi_pending()) << "NMI must be raised at scanline 241 dot 1";
}

GTEST_TEST(testPPUFrame, enabling_nmi_during_vblank_raises_one_immediately)
{
    Bus console;
    PPU& ppu = console.ppu;

    while (ppu.in_reset_write_lockout()) {
        ppu.clock();
    }
    ASSERT_TRUE(clock_until_dot(ppu, PPU::vblank_start_scanline, 1));
    ppu.clock();
    ASSERT_EQ(0x80, ppu.registers.PPUSTATUS & 0x80);
    ASSERT_FALSE(console.cpu.nmi_pending()) << "bit 7 still clear";

    ppu.write(PPU::PPUCTRL, 0x80);
    EXPECT_TRUE(console.cpu.nmi_pending()) << "enabling bit 7 while vblank is set must raise an NMI";
}

GTEST_TEST(testPPUFrame, enabling_nmi_outside_vblank_does_not_raise_one)
{
    Bus console;
    PPU& ppu = console.ppu;

    while (ppu.in_reset_write_lockout()) {
        ppu.clock();
    }
    // Land on a visible scanline, where the vblank flag is clear.
    ASSERT_TRUE(clock_until_dot(ppu, 10, 0));
    ASSERT_EQ(0x00, ppu.registers.PPUSTATUS & 0x80);

    ppu.write(PPU::PPUCTRL, 0x80);
    EXPECT_FALSE(console.cpu.nmi_pending());
}

};  // namespace tests
