#pragma once
#include <cstdint>
class Bus;
struct Device
{
    Device() = delete;
    Device(Bus *b) : bus{b} {};
    virtual void write(const uint16_t addr, const uint8_t data) = 0;
    virtual uint8_t read(const uint16_t addr) = 0;
    //virtual uint8_t read(const uint16_t addr) const = 0;

protected:
    Bus *bus;
};
