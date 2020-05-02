#pragma once

#include <array>

#include "device.h"

class RAM : public Device
{
public:
    void write(const uint16_t addr, const uint8_t data) { memory[addr] = data; };
    uint8_t read(const uint16_t addr) { return memory[addr]; };

    std::array<uint8_t, 64 * 1024> memory = { 0 };

};
