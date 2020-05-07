#pragma once
#include "device.h"

#include "cpu.h"
#include "apu.h"
#include "ppu.h"
#include "ram.h"

class Bus
{
public:
    Bus();
    void write(const uint16_t addr, const uint8_t data);
    void write_ram(const uint16_t start_addr, const size_t n_bytes, const uint8_t *bytes);
    uint8_t read(const uint16_t addr);

    CPU cpu;
    APU apu;
    PPU ppu;
    RAM ram;

protected:
    const Device& get_device_from_addr(const uint16_t addr) const;
    Device& get_device_from_addr(const uint16_t addr);

};
