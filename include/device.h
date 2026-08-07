#pragma once
#include <cstdint>
class Bus;
struct Device
{
    Device() = delete;
    Device(Bus* b) : bus{b} {};

    // A base with virtual functions needs a virtual destructor: deleting a
    // derived device through a Device* is undefined behaviour without one.
    // Nothing does that today, but the Bus holds its devices polymorphically
    // and this is the kind of trap that only surfaces once ownership changes.
    virtual ~Device() = default;

    virtual void write(const uint16_t addr, const uint8_t data) = 0;
    virtual uint8_t read(const uint16_t addr) = 0;

protected:
    Bus* bus;

    // The last value driven on the CPU's data bus.
    //
    // A read of an address nothing drives - a write-only register, or unmapped
    // space - returns whatever was last on the bus, because nothing pulled it
    // anywhere else. Defined in device.cpp rather than inline, because reaching
    // into Bus needs its definition and bus.h already includes this header.
    uint8_t open_bus() const;
};
