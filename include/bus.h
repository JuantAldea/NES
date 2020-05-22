#pragma once
#include "apu.h"
#include "cpu.h"
#include "device.h"
#include "ppu.h"
#include "ram.h"

class Bus
{
public:
    Bus();
    void write(const uint16_t addr, const uint8_t data);
    void write_ram(const uint16_t start_addr, const size_t n_bytes, const uint8_t* bytes);
    uint8_t read(const uint16_t addr);
    uint64_t total_cycles = 0;

    void clock();
    void clock_CPU();
    void clock_PPU();

    CPU cpu;
    APU apu;
    PPU ppu;
    RAM ram;

protected:
    const Device& get_device_from_addr(const uint16_t addr) const;
    Device& get_device_from_addr(const uint16_t addr);
};
