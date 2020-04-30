#pragma once
#include "device.h"

#include "cpu.h"
#include "apu.h"
#include "ppu.h"
#include "ram.h"

class Bus : public Device {
    public:
        Bus();
        void write(const uint16_t addr, const uint8_t data);
        uint8_t read(const uint16_t addr);

    protected:
        const Device& get_device_from_addr(const uint16_t addr) const;
        Device& get_device_from_addr(const uint16_t addr);

    protected:
        CPU cpu;
        APU apu;
        PPU ppu;
        RAM ram;
};
