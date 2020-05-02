#pragma once
#include "device.h"
#include "bus.h"
class Bus;
class APU : public Device
{
public:
    APU(Bus &bus) : Device { bus } {};
    void write(const uint16_t addr, const uint8_t data) { };
    uint8_t read(const uint16_t addr) { return 0u; };
};
