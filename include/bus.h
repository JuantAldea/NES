#pragma once
#include "apu.h"
#include "cpu.h"
#include "device.h"
#include "ppu.h"
#include "ram.h"
#include "rom.h"

class Bus
{
public:
    Bus();
    void write(const uint16_t addr, const uint8_t data);
    void write_ram(const uint16_t start_addr, const size_t n_bytes, const uint8_t* bytes);
    uint8_t read(const uint16_t addr);
    bool load_cartridge(const std::string& path);
    uint64_t total_cycles = 0;

    // CPU cycles elapsed, counting the ones OAM DMA steals - which CPU::clock
    // never sees, so CPU::total_cycles does not count them.
    //
    // This is the divide-by-two everything phase-sensitive on the CPU bus hangs
    // off: the APU frame counter's get/put alignment and OAM DMA's are the same
    // one. Deriving DMA's from CPU::total_cycles made the two disagree by the
    // length of every DMA that had already run.
    uint64_t cpu_cycles = 0;

    void clock();
    void clock_CPU();
    void clock_PPU();

    CPU cpu;
    APU apu;
    PPU ppu;
    SystemRAM ram;
    PrgRAM prg_ram;
    ROM rom;

protected:
    // Single address-decode table shared by read() and write() so the two
    // paths can never disagree about which device (and which mirrored
    // effective address) a given CPU address maps to.
    struct DecodedAddress {
        Device* device;  // nullptr for open-bus ranges (no device backs them)
        uint16_t effective_addr;
    };
    DecodedAddress decode(const uint16_t addr);
};
