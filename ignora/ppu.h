#pragma once
#include "device.h"

class PPU : public Device
{
public:
    PPU(Bus &bus) : Device(bus) {};
    void write(const uint16_t addr, const uint8_t data) { };
    uint8_t read(const uint16_t addr) {return 0u; };
};
