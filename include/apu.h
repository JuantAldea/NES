#pragma once
#include "device.h"

class APU : public Device
{
public:
    APU(Bus* b) : Device{b} {};
    void write(const uint16_t addr, const uint8_t data){};
    uint8_t read(const uint16_t addr) { return 0; };
};
