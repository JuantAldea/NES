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

    struct {
        uint8_t PPUCTRL;
        uint8_t PPUMASK;
        uint8_t PPUSTATUS;
        uint8_t OAMADDR;
        uint8_t OAMDATA;
        uint8_t PPUSCROLL;
        uint8_t PPUADDR;
        uint8_t PPUDATA;
        uint8_t OAMDMA;
    } registers;

    uint16_t remaining_dma_cycles = 0;
    uint16_t dma_current_memory_source_addr = 0;

    void request_OAM_DMA();
    void perform_OAM_DMA_cycle();
    bool dma_in_progress() { return remaining_dma_cycles != 0; }

    uint8_t OAM_memory[256] = {0};
    uint8_t VRAM[256] = {0};

    uint8_t& get_register(const RegisterMMap reg);

    enum VRAMStep : uint8_t { Horizontal = 0x1, Vertical = 0x32 };
    uint16_t base_nametable_addr = 0;
    uint16_t sprite_pattern_8x8_table_addr = 0;
    uint16_t bg_pattern_table_address = 0;
    uint64_t total_cycles = 0;

    bool MMI_on_V_Blank = false;
    bool increase_vertical = false;
    bool big_sprites = false;
    uint8_t vram_step = 1;
    bool high_byte_input = true;

    bool greyscale = false;
    bool show_bg_in_leftmost = false;
    bool show_sprites_in_leftmost = false;
    bool show_background = false;
    bool show_sprites = false;
    bool emphasize_red = false;
    bool emphasize_green = false;
    bool emphasize_blue = false;

    uint8_t low_last_written;

    void update_flags();
};
