#pragma once
#include "device.h"

class APU : public Device
{
    public:
        void write(const uint16_t addr, const uint8_t data) { };
        uint8_t read(const uint16_t addr) {};
};
