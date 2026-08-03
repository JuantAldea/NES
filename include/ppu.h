#pragma once
#include "device.h"

/*

PPUCTRL 	$2000 	VPHB SINN 	NMI enable (V), PPU master/slave (P), sprite height (H), background tile select (B), sprite tile select (S), increment mode (I), nametable select (NN)
PPUMASK 	$2001 	BGRs bMmG 	color emphasis (BGR), sprite enable (s), background enable (b), sprite left column enable (M), background left column enable (m), greyscale (G)
PPUSTATUS 	$2002 	VSO- ---- 	vblank (V), sprite 0 hit (S), sprite overflow (O); read resets write pair for $2005/$2006
OAMADDR 	$2003 	aaaa aaaa 	OAM read/write address
OAMDATA 	$2004 	dddd dddd 	OAM data read/write
PPUSCROLL 	$2005 	xxxx xxxx 	fine scroll position (two writes: X scroll, Y scroll)
PPUADDR 	$2006 	aaaa aaaa 	PPU read/write address (two writes: most significant byte, least significant byte)
PPUDATA 	$2007 	dddd dddd 	PPU data read/write
OAMDMA 	    $4014   aaaa aaaa 	OAM DMA high address

*/

#include <cstdint>

//_RP2A03
class PPU : public Device
{
public:
    PPU(Bus* b) : Device{b} {};
    void write(const uint16_t addr, const uint8_t data);
    uint8_t read(const uint16_t addr);
    void clock();

    /*
    uint8_t& PPUCTRL();
    uint8_t& PPUMASK();
    uint8_t PPUSTATUS();
    uint8_t& OAMADDR();
    uint8_t& OAMDATA();
    uint8_t& PPUSCROLL();
    uint8_t& PPUADDR();
    uint8_t& PPUDATA();
    uint8_t& OAMDMA();
*/
    enum RegisterMMap : uint16_t {
        PPUCTRL = 0x2000,
        PPUMASK = 0x2001,
        PPUSTATUS = 0x2002,
        OAMADDR = 0x2003,  //The OAM (Object Attribute Memory) is internal memory inside the PPU
        OAMDATA = 0x2004,
        PPUSCROLL = 0x2005,
        PPUADDR = 0x2006,
        PPUDATA = 0x2007,
        OAMDMA = 0x4014,
    };

    // Value-initialized: without this the registers hold indeterminate memory,
    // so a read of $2002 on a freshly constructed PPU could report a spurious
    // vblank and sprite-0 hit. The open-bus model below only makes reads
    // deterministic if the underlying registers start from a known state.
    struct {
        uint8_t PPUCTRL;
        uint8_t PPUMASK;
        uint8_t PPUSTATUS;
        uint8_t OAMADDR;
        uint8_t OAMDATA;
        // Two-write registers. Both hold the value assembled from the write
        // pair, so they need more than 8 bits: PPUADDR is a 14-bit VRAM
        // address, PPUSCROLL packs X (first write) in the high byte and Y
        // (second write) in the low byte. As uint8_t these could only reach
        // the first 256 bytes of VRAM, leaving the nametables and palette
        // unaddressable.
        uint16_t PPUSCROLL;
        uint16_t PPUADDR;
        uint8_t PPUDATA;
        uint8_t OAMDMA;
    } registers = {};

    // Approximates the PPU's internal data bus latch: every register access
    // (read or write) through PPU::read/PPU::write drives this byte. Real
    // hardware decays this value over time; we keep the simpler "last value
    // seen" approximation, which is enough to make open-bus bits (PPUSTATUS
    // bits 0-4, and reads of write-only registers) return something
    // deterministic instead of aborting or fabricating state.
    uint8_t open_bus = 0;

    uint16_t remaining_dma_cycles = 0;
    uint16_t dma_current_memory_source_addr = 0;

    void request_OAM_DMA();
    void perform_OAM_DMA_cycle();
    bool dma_in_progress() { return remaining_dma_cycles != 0; }

    void process_visible_scanline();

    void set_vblank() { registers.PPUSTATUS |= 0x80; }
    void clear_vblank() { registers.PPUSTATUS &= ~0x80; }

    // /NMI is pulled low while the vblank flag is set and PPUCTRL bit 7 asks
    // for an interrupt. The CPU cares only about the edges, which
    // update_nmi_line reports; `nmi_line` remembers the level so it can tell
    // an edge from a repeat.
    bool nmi_line_asserted() const { return MMI_on_V_Blank && (registers.PPUSTATUS & 0x80); }
    void update_nmi_line();
    bool nmi_line = false;

    // Set by a $2002 read that lands on the dot the vblank flag would be set,
    // and consumed by that same dot's tick. Nothing else ever sees it.
    bool suppress_vblank_flag_set = false;

    void set_sprite0_hit() { registers.PPUSTATUS |= 0x40; }
    void clear_sprite0_hit() { registers.PPUSTATUS &= ~0x40; }

    void set_sprite_overflow() { registers.PPUSTATUS |= 0x20; }
    void clear_sprite_overflow() { registers.PPUSTATUS &= ~0x20; }

    uint8_t OAM_memory[256] = {0};
    uint8_t VRAM[0x4000] = {0};

    uint8_t& get_register(const RegisterMMap reg);

    // PPUCTRL bit 2 selects an increment of 1 or 32. These are decimal
    // quantities, not bit patterns: 0x32 would be 50.
    enum VRAMStep : uint8_t { Horizontal = 1, Vertical = 32 };

    static constexpr uint16_t vram_addr_mask = 0x3FFF;

    // Writes to PPUCTRL, PPUMASK, PPUSCROLL and PPUADDR are ignored for about
    // 29658 CPU cycles after power/reset. total_cycles counts PPU cycles, and
    // the PPU runs three times faster than the CPU, so the threshold has to be
    // converted into that clock domain.
    static constexpr uint64_t reset_lockout_cpu_cycles = 29658;
    static constexpr uint64_t reset_lockout_cycles = reset_lockout_cpu_cycles * 3;

    uint16_t base_nametable_addr = 0;
    uint16_t sprite_pattern_8x8_table_addr = 0;
    uint16_t bg_pattern_table_address = 0;
    uint64_t total_cycles = 0;

    // Position within the frame. These have to persist across clock() calls:
    // as locals the state machine could never advance, so vblank was never
    // entered and NMI never raised.
    //
    // An NTSC frame is 262 scanlines (0-261) of 341 dots (0-340): 0-239 are
    // visible, 240 is post-render, 241 starts vblank, and 261 is the
    // pre-render line that clears the status flags.
    static constexpr int dots_per_scanline = 341;
    static constexpr int scanlines_per_frame = 262;
    static constexpr int post_render_scanline = 240;
    static constexpr int vblank_start_scanline = 241;
    static constexpr int pre_render_scanline = 261;

    int scanline = 0;
    int cycle = 0;
    uint64_t frame = 0;

    bool rendering_enabled() const { return show_background || show_sprites; }
    void advance_dot();

    bool MMI_on_V_Blank = false;
    bool increase_vertical = false;
    bool big_sprites = false;
    uint8_t vram_step = 1;
    bool high_byte_input = true;
    // Staging register for the $2006 write pair. The first write only updates
    // this; registers.PPUADDR is committed from it by the second write, so a
    // PPUDATA access in between still uses the previous address.
    uint16_t temp_addr = 0;

    bool greyscale = false;
    bool show_bg_in_leftmost = false;
    bool show_sprites_in_leftmost = false;
    bool show_background = false;
    bool show_sprites = false;
    bool emphasize_red = false;
    bool emphasize_green = false;
    bool emphasize_blue = false;

    bool in_reset_write_lockout() const { return total_cycles < reset_lockout_cycles; }

    uint8_t scroll_x() const { return registers.PPUSCROLL >> 8; }
    uint8_t scroll_y() const { return registers.PPUSCROLL & 0xFF; }

    void update_flags();
};
