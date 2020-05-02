#pragma once
#include "device.h"

class RP2A03 : public Device
{
public:

    RP2A03(Bus *b) : Device {b} {};
    void write(const uint16_t addr, const uint8_t data) { };
    uint8_t read(const uint16_t addr) { return 0; };
};
