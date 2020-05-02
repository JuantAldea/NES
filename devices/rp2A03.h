#pragma once
#include "device.h"

class RP2A03 : public Device
{
public:
    void write(const uint16_t addr, const uint8_t data) { };
    uint8_t read(const uint16_t addr) { return 0; };
};
