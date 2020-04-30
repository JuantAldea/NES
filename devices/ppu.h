#pragma once
#include "device.h"

class PPU : public Device
{
    public:
        void write(const uint16_t addr, const uint8_t data) { };
        uint8_t read(const uint16_t addr) {};
};
