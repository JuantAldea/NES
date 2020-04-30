#pragma once
#include "device.h"

#include <array>

class RAM : public Device
{
    public:
        void write(const uint16_t addr, const uint8_t data);
        uint8_t read(const uint16_t addr);

    public:
        std::array<uint8_t, 64 * 1024> memory = { 0 };

};
