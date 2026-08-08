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

// <cstddef> for size_t, which the storage sizes further down use. This header
// was not self-contained without it and got away with it: every .cpp that
// included it happened to include a C++ header first. Sorting it to the top of
// its own .cpp, which is what IncludeIsMainRegex does, ends that.
#include <cstddef>
#include <cstdint>
#include <limits>

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
        // PPUADDR is the PPU's current VRAM address - loopy's `v`. It is 15
        // bits, not 8: the low 12 double as the scroll position while
        // rendering (coarse X, coarse Y, nametable select) and the top three
        // are fine Y. See temp_addr (`t`) and fine_x (`x`) below.
        //
        // There is deliberately no PPUSCROLL member. $2005 is a WRITE PORT
        // into t and x; hardware has no register that holds "the scroll" as
        // an X/Y pair, and modelling one made $2005 and $2006 independent
        // when in fact they share t and the write toggle.
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

    // The latch is not permanent: it is stray capacitance, and it bleeds away.
    // Each bit decays to 0 independently, roughly 600ms after the last time
    // ANYTHING drove that particular bit - which matters because most reads
    // drive only some of the eight. A PPUSTATUS read drives bits 5-7, a palette
    // read drives 0-5, and a read of a write-only register drives none at all.
    //
    // ppu_open_bus subtest 3 is "Decay value should become zero by one second".
    static constexpr uint64_t open_bus_decay_cycles = 3'220'000;  // ~600ms of PPU dots

    uint64_t open_bus_refreshed_at[8] = {0};

    // Drives `value` onto the bits selected by `mask`, refreshing only those
    // bits' decay timers. Bits outside the mask keep whatever they held, and
    // keep ageing.
    void drive_open_bus(const uint8_t value, const uint8_t mask = 0xFF);
    void decay_open_bus();
    void refresh_open_bus_next_decay();

    // Cycle at which the earliest-expiring set bit decays; UINT64_MAX when the
    // latch is empty. Lets the per-tick check be a single comparison.
    uint64_t open_bus_next_decay = UINT64_MAX;

    uint16_t remaining_dma_cycles = 0;
    uint16_t dma_current_memory_source_addr = 0;

    // The byte read on a DMA read cycle, held until the write cycle that
    // follows it. DMA alternates read and write cycles and the two are a whole
    // CPU cycle apart, which only matters when the source is something that
    // changes in between - i.e. the PPU register file, which advances three
    // dots per CPU cycle.
    uint8_t dma_latch = 0;

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

    // Whether rendering was enabled when the pre-render line last passed the
    // dot at which the odd-frame skip is decided. Latched rather than read at
    // the jump, because the two are one dot apart; see PPU::advance_dot.
    bool odd_frame_skip_armed = false;

    void set_sprite0_hit() { registers.PPUSTATUS |= 0x40; }
    void clear_sprite0_hit() { registers.PPUSTATUS &= ~0x40; }

    void set_sprite_overflow() { registers.PPUSTATUS |= 0x20; }
    void clear_sprite_overflow() { registers.PPUSTATUS &= ~0x20; }

    uint8_t OAM_memory[256] = {0};

    // The PPU's address space is not a flat array. It is three distinct
    // memories behind one 14-bit bus:
    //
    //   $0000-$1FFF  pattern tables - on the CARTRIDGE. CHR-ROM if the iNES
    //                header declares CHR banks, otherwise 8KB of CHR-RAM the
    //                console supplies (which is why chr_ram lives here).
    //   $2000-$2FFF  nametables - 2KB internal, arranged into four logical
    //                1KB screens by the cartridge's mirroring wiring.
    //   $3000-$3EFF  a mirror of $2000-$2EFF.
    //   $3F00-$3F1F  palette RAM, mirrored every $20 up to $3FFF, and with
    //                $3F10/$14/$18/$1C aliasing $3F00/$04/$08/$0C.
    //
    // Reads and writes go through ppu_bus_read/ppu_bus_write, which is the one
    // place that decode lives.
    static constexpr size_t nametable_size = 2 * 1024;
    static constexpr size_t chr_ram_size = 8 * 1024;
    static constexpr size_t palette_size = 32;

    uint8_t nametable_ram[nametable_size] = {0};
    uint8_t chr_ram[chr_ram_size] = {0};

    // Power-on palette contents are not specified by the hardware, but a
    // consistent starting point keeps reads deterministic before software
    // writes one.
    uint8_t palette_ram[palette_size] = {0};

    uint8_t ppu_bus_read(const uint16_t addr);
    void ppu_bus_write(const uint16_t addr, const uint8_t data);

    // Offers an address to the cartridge, which is where a scanline-counting
    // mapper such as MMC3 watches address line A12. Called by the two functions
    // above and by the $2006 write that commits a new v; see the definition for
    // why palette accesses are excluded.
    void observe_ppu_address_bus(const uint16_t addr);

    // $2007 reads are delayed by one: the value returned is what the PREVIOUS
    // read fetched, and the current fetch refills the latch. Palette reads are
    // the exception - they come back immediately, while the latch is still
    // refilled from the nametable underneath.
    uint8_t vram_read_buffer = 0;

private:
    uint16_t nametable_offset(const uint16_t addr) const;

public:
    // Public because anything that displays palette RAM has to apply the same
    // fold. $3F10/$3F14/$3F18/$3F1C are aliases of $3F00/$04/$08/$0C, so the
    // cells sitting behind them are never written and never read - a viewer
    // that indexed palette_ram raw would show four entries the PPU does not
    // use, in place of the four it does. Re-deriving the rule in the frontend
    // instead would leave two copies free to drift apart.
    static uint16_t palette_offset(const uint16_t addr);

    uint8_t& get_register(const RegisterMMap reg);

    // PPUCTRL bit 2 selects an increment of 1 or 32. These are decimal
    // quantities, not bit patterns: 0x32 would be 50.
    enum VRAMStep : uint8_t { Horizontal = 1, Vertical = 32 };

    // The PPU's address BUS is 14 bits: this is what an access is folded into,
    // and it is what makes $3000-$3EFF mirror the nametables.
    static constexpr uint16_t vram_addr_mask = 0x3FFF;

    // The v REGISTER is 15 bits, because bits 12-14 hold fine Y. The two are
    // easy to conflate - v is "the VRAM address" - but masking v with the bus
    // width would clear the top bit of fine Y, so the $2007 increment has to
    // wrap modulo $8000 rather than $4000.
    static constexpr uint16_t vram_register_mask = 0x7FFF;

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
    // The dot on the pre-render line at which the odd-frame clock skip is
    // decided, one ahead of the (261,339) -> (0,0) jump that carries it out.
    static constexpr int odd_frame_skip_decision_dot = 338;

    int scanline = 0;
    int cycle = 0;
    uint64_t frame = 0;

    bool rendering_enabled() const { return show_background || show_sprites; }
    void advance_dot();

    bool MMI_on_V_Blank = false;
    bool increase_vertical = false;
    bool big_sprites = false;
    uint8_t vram_step = 1;
    // Loopy's `w`, the shared write toggle for $2005 and $2006, with the
    // opposite sense: w == !high_byte_input. true means "the next write is the
    // first of the pair".
    bool high_byte_input = true;
    bool write_toggle_w() const { return !high_byte_input; }

    // Loopy's `t`: the address/scroll being assembled. BOTH $2005 and $2006
    // write into it - that is why they share the toggle - and it is copied
    // into v by the second $2006 write, at dot 257 (horizontally) and during
    // pre-render dots 280-304 (vertically).
    //
    //   t and v layout:  yyy NN YYYYY XXXXX
    //                    ||| || ||||| +++++-- coarse X scroll
    //                    ||| || +++++-------- coarse Y scroll
    //                    ||| ++-------------- nametable select
    //                    +++----------------- fine Y scroll
    uint16_t temp_addr = 0;

    // Loopy's `x`: the fine X scroll, three bits, written only by the first
    // $2005 write. It never enters t or v - it selects which bit of the
    // pattern shift registers is output.
    uint8_t fine_x = 0;

    // --- the v/t/x scroll operations, NESdev "PPU scrolling" ---------------
    void increment_coarse_x();
    void increment_y();
    // What a $2007 access does to v afterwards. Normally +1 or +32, but during
    // rendering it is a coarse-X increment and a Y increment instead.
    void advance_vram_address();
    void copy_horizontal_from_t();
    void copy_vertical_from_t();

    uint16_t tile_address() const { return static_cast<uint16_t>(0x2000 | (registers.PPUADDR & 0x0FFF)); }
    uint16_t attribute_address() const
    {
        return static_cast<uint16_t>(0x23C0 | (registers.PPUADDR & 0x0C00) | ((registers.PPUADDR >> 4) & 0x38) |
                                     ((registers.PPUADDR >> 2) & 0x07));
    }
    uint8_t fine_y() const { return static_cast<uint8_t>((registers.PPUADDR >> 12) & 0x07); }

    bool greyscale = false;
    bool show_bg_in_leftmost = false;
    bool show_sprites_in_leftmost = false;
    bool show_background = false;
    bool show_sprites = false;
    bool emphasize_red = false;
    bool emphasize_green = false;
    bool emphasize_blue = false;

    bool in_reset_write_lockout() const { return total_cycles < reset_lockout_cycles; }

    // Derived, not stored. The scroll position is spread across t (coarse X in
    // bits 0-4, coarse Y in bits 5-9, fine Y in bits 12-14) and x (fine X), so
    // "the X scroll" is coarse X * 8 + fine X and "the Y scroll" is
    // coarse Y * 8 + fine Y. These reassemble the byte that was written to
    // $2005, which is all any caller ever wanted from the old PPUSCROLL.
    uint8_t scroll_x() const { return static_cast<uint8_t>(((temp_addr & 0x001F) << 3) | fine_x); }
    uint8_t scroll_y() const { return static_cast<uint8_t>(((temp_addr & 0x03E0) >> 2) | ((temp_addr >> 12) & 0x07)); }

    // --- background rendering pipeline -------------------------------------
    //
    // The PPU fetches one tile every eight dots, in four two-dot accesses:
    // nametable byte, attribute byte, pattern low, pattern high. Those land in
    // the latches below, and are transferred into the shift registers on the
    // eight-dot boundary - two tiles ahead of the pixel being drawn, which is
    // why the shifters are 16 bits and why dots 321-336 prefetch the next
    // line's first two tiles.
    uint8_t nametable_latch = 0;
    uint8_t attribute_latch = 0;
    uint8_t pattern_low_latch = 0;
    uint8_t pattern_high_latch = 0;

    uint16_t bg_pattern_shift_low = 0;
    uint16_t bg_pattern_shift_high = 0;
    // The attribute bits are per-tile, but they have to be selectable per pixel
    // by the same fine-X tap as the pattern bits, so each of the two bits is
    // smeared across a whole byte as it is loaded.
    uint16_t bg_attribute_shift_low = 0;
    uint16_t bg_attribute_shift_high = 0;

    void shift_background_registers();
    void reload_background_shifters();
    void fetch_background_byte();
    uint16_t background_pattern_address() const
    {
        return static_cast<uint16_t>(bg_pattern_table_address + (nametable_latch << 4) + fine_y());
    }
    void render_pixel();

    // The finished picture: one 6-bit palette index per pixel, already through
    // the greyscale mask, ready to be looked up in nes_palette.
    //
    // KNOWN GAPS, deliberate:
    //  - PPUMASK colour emphasis (bits 5-7) is not applied. The framebuffer is
    //    a palette index, and emphasis is a property of the video signal; it
    //    belongs to the display path, not here.
    //  - The "forced backdrop" case - rendering disabled while v points into
    //    $3F00-$3FFF, where hardware outputs the colour v addresses rather than
    //    the backdrop - is not modelled. Nothing tests it and it needs the
    //    palette read to be driven from v.
    static constexpr int screen_width = 256;
    static constexpr int screen_height = 240;
    uint8_t framebuffer[screen_width * screen_height] = {0};

    // The 2C02's 64 colours as 0xRRGGBB. Indexed by the palette entry the
    // framebuffer holds; nothing in the PPU reads this, it is for the display.
    static constexpr uint32_t nes_palette[64] = {
        0x666666, 0x002A88, 0x1412A7, 0x3B00A4, 0x5C007E, 0x6E0040, 0x6C0600, 0x561D00, 0x333500, 0x0B4800, 0x005200,
        0x004F08, 0x00404D, 0x000000, 0x000000, 0x000000, 0xADADAD, 0x155FD9, 0x4240FF, 0x7527FE, 0xA01ACC, 0xB71E7B,
        0xB53120, 0x994E00, 0x6B6D00, 0x388700, 0x0C9300, 0x008F32, 0x007C8D, 0x000000, 0x000000, 0x000000, 0xFFFEFF,
        0x64B0FF, 0x9290FF, 0xC676FF, 0xF36AFF, 0xFE6ECC, 0xFE8170, 0xEA9E22, 0xBCBE00, 0x88D800, 0x5CE430, 0x45E082,
        0x48CDDE, 0x4F4F4F, 0x000000, 0x000000, 0xFFFEFF, 0xC0DFFF, 0xD3D2FF, 0xE8C8FF, 0xFBC2FF, 0xFEC4EA, 0xFECCC5,
        0xF7D8A5, 0xE4E594, 0xCFEF96, 0xBDF4AB, 0xB3F3CC, 0xB5EBF2, 0xB8B8B8, 0x000000, 0x000000,
    };

    // --- sprite rendering ---------------------------------------------------
    //
    // The sprite pipeline runs one scanline AHEAD of the pixels it produces,
    // in three phases that do not overlap:
    //
    //   dots   1- 64  secondary OAM cleared to $FF
    //   dots  65-256  sprite evaluation: scan primary OAM for sprites on the
    //                 NEXT line, copy up to 8 into secondary OAM, set the
    //                 overflow flag on the 9th
    //   dots 257-320  pattern fetch: eight groups of eight dots load secondary
    //                 OAM's sprites into the eight output units below
    //
    // So the units are loaded during line L and drawn during line L+1. That is
    // what makes it safe for the fetch to overwrite them at dot 257: line L's
    // own pixels (dots 1-256) are already finished.

    // 32 bytes: eight sprite records of {Y, tile, attributes, X}.
    uint8_t secondary_oam[32] = {0};

    // Sprite evaluation walks primary OAM as (sprite index n, byte index m),
    // one OAM read on each odd dot and one secondary-OAM write on each even
    // dot. It is a per-dot state machine rather than a batch loop because the
    // moment the overflow flag is set is itself observable: 3.Timing measures
    // it to within a CPU clock or two, and 5.Emulator exists specifically to
    // defeat implementations that compute the flag once and cache it.
    enum class SpriteEvalState : uint8_t {
        // sprite_eval_latch holds a candidate Y read from OAM[4n + m].
        ScanY,
        // Copying bytes 1-3 of an in-range sprite into secondary OAM.
        CopySprite,
        // The three dummy reads hardware performs after the 9th in-range
        // sprite is found (NESdev sprite evaluation, step 3a).
        OverflowCopy,
        // n has run off the end of OAM; the scan is over for this line.
        Done,
    };

    SpriteEvalState sprite_eval_state = SpriteEvalState::Done;

    // The sprite index (0-63) and byte index (0-3) the scan is looking at.
    // Together they form the OAM address 4n + m, which is the ONE address the
    // read phase ever uses - the difference between a normal scan and the
    // buggy overflow scan is only in how the pair advances.
    uint8_t sprite_eval_n = 0;
    uint8_t sprite_eval_m = 0;

    // Sprites copied into secondary OAM so far, 0-8. Doubles as the write
    // cursor: the next sprite lands at secondary_oam[4 * found].
    //
    // The known gap that used to be recorded here - fetch_sprite_pattern
    // returning early for slots past sprite_count, where hardware always
    // performs eight fetches - is CLOSED. It was unobservable until a mapper
    // watched the PPU address bus, and MMC3 is that mapper; see the dummy-fetch
    // branch in fetch_sprite_pattern.
    //
    // Two sibling findings from the same adversarial review against the NESdev
    // pages are also implemented: the 2C02G/H OAM refresh bug (oam_refresh_bug)
    // and the glitchy OAMADDR increment on a $2004 write during rendering.
    //
    // Destination offset within the secondary-OAM slot being filled, counted
    // separately from sprite_eval_m (the SOURCE byte index). They agree only
    // when OAMADDR was record-aligned.
    uint8_t sprite_eval_copy = 0;

    uint8_t sprite_eval_found = 0;

    // The byte read on an odd dot, consumed by the even dot that follows it.
    uint8_t sprite_eval_latch = 0;

    // Whether secondary OAM slot 0 holds OAM sprite 0. This is the whole of
    // "which sprite can raise the hit flag": with a non-zero OAMADDR the first
    // sprite evaluated is not OAM[0], so slot 0 is some other sprite and no
    // hit is possible from it.
    bool sprite_zero_in_secondary = false;

    // OAMADDR as it stood when sprite evaluation began, aligned down to a
    // sprite record. This is where the scan starts.
    //
    // It has to be latched rather than read at evaluation time, because
    // hardware forces OAMADDR to 0 across dots 257-320 of every rendering
    // scanline - so by the time the sprites are fetched, the value that chose
    // the starting sprite is already gone.
    uint8_t sprite_eval_oamaddr = 0;

    // --- the eight sprite output units --------------------------------------
    //
    // Loaded during dots 257-320 of the previous line, consumed by
    // render_pixel on dots 1-256 of this one.
    //
    // The pattern registers really do shift: on each dot a unit whose X
    // counter has reached zero shifts one pixel out of the top. That is
    // self-limiting - after eight shifts both bytes are zero, so the sprite
    // simply stops being opaque - which is why nothing here has to track how
    // many pixels of a sprite have been drawn.
    uint8_t sprite_pattern_shift_low[8] = {0};
    uint8_t sprite_pattern_shift_high[8] = {0};
    uint8_t sprite_attribute_latch[8] = {0};
    uint8_t sprite_x_counter[8] = {0};

    // How many of the eight units hold a real sprite for the line being drawn,
    // and whether unit 0 is OAM sprite 0. Snapshotted from the evaluation
    // state at dot 257.
    uint8_t sprite_count = 0;
    bool sprite_zero_in_units = false;

    // Is a sprite whose OAM Y byte is `y` on the line currently being
    // evaluated? Sprites are delayed one scanline by evaluation running a line
    // ahead: OAM holds Y one LESS than the first line the sprite appears on,
    // so a sprite with Y = 239 appears only on line 239 and one with Y = 255
    // can never be drawn at all.
    //
    // big_sprites is read live rather than latched, because 5.Emulator's test
    // 5 changes sprite height mid-frame and expects the flag time to move.
    bool sprite_in_range(const uint8_t y) const;

    void clear_secondary_oam_dot();
    void begin_sprite_evaluation();
    void oam_refresh_bug();
    void tick_sprite_evaluation();
    void sprite_evaluation_read();
    void sprite_evaluation_write();
    void sprite_evaluation_advance_n();
    void sprite_evaluation_advance_byte();
    void load_sprite_units();
    void fetch_sprite_pattern();

    void update_flags();
};
