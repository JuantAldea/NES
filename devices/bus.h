#pragma once

#include "cpu.h"
#include "ram.h"

/*
#include "devices/apu.h"
#include "devices/ppu.h"
*/

class Bus
{
public:
    Bus();
    void write(const uint16_t addr, const uint8_t data);
    uint8_t read(const uint16_t addr);

protected:
    const Device& get_device_from_addr(const uint16_t addr) const;
    Device& get_device_from_addr(const uint16_t addr);

protected:
    CPU cpu;
    RAM ram;
    //APU apu;
    //PPU ppu;
};
