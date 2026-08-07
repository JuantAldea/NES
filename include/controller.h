// The two standard controller ports, as one device.
//
// They are modelled together rather than as two independent devices because
// hardware wires them that way: a single strobe line, driven by writing $4016,
// latches BOTH controllers at once. Two separate devices would each need their
// own copy of the strobe and could drift apart, which is a bug no game would
// reveal until it read port 2.
//
// Read/write asymmetry, which is the awkward part of the NES I/O map:
//
//   $4016 write   strobe, to both ports
//   $4016 read    port 1 data
//   $4017 write   APU frame counter - NOT this device
//   $4017 read    port 2 data
//
// Only $4017 is split, and Bus::decode takes an access direction to express it.
#pragma once

#include <cstdint>

#include "device.h"

class Controllers : public Device
{
public:
    Controllers(Bus* b) : Device{b} {};

    // Bit positions in the order the shift register reports them, which is the
    // order NESdev lists: "0 - A, 1 - B, 2 - Select, 3 - Start, 4 - Up,
    // 5 - Down, 6 - Left, 7 - Right".
    enum Button : uint8_t {
        A = 1 << 0,
        B = 1 << 1,
        Select = 1 << 2,
        Start = 1 << 3,
        Up = 1 << 4,
        Down = 1 << 5,
        Left = 1 << 6,
        Right = 1 << 7,
    };

    // What the frontend pushes in. Held separately from the shift registers so
    // that a key pressed mid-read does not corrupt a latch already in progress:
    // hardware samples the buttons at the latch, not at each read.
    uint8_t buttons[2] = {0, 0};

    void write(const uint16_t addr, const uint8_t data) override
    {
        if (addr != 0x4016) {
            return;
        }

        // Only bit 0 is the strobe.
        const bool now = (data & 0x01) != 0;

        // "While S (strobe) is high, the shift registers in the controllers are
        // continuously reloaded from the button states" - so the reload happens
        // throughout, not only on the edge. Reading while strobe is high
        // therefore keeps returning A, which is what makes the 1-then-0 write
        // sequence necessary to read the rest.
        if (now) {
            reload();
        }
        strobe = now;
    }

    uint8_t read(const uint16_t addr) override
    {
        const int port = (addr == 0x4017) ? 1 : 0;

        if (strobe) {
            // Still latching: every read sees button A again.
            reload();
        }

        const uint8_t bit = static_cast<uint8_t>(shift[port] & 0x01);

        if (!strobe) {
            // Shifting a 1 in at the top is not a trick to save a counter: it
            // is what the hardware reports. "After 8 bits are read, all
            // subsequent bits will report 1 on a standard NES controller."
            shift[port] = static_cast<uint8_t>((shift[port] >> 1) | 0x80);
        }

        // NESdev: "In the NES and Famicom, the top three (or five) bits are not
        // driven, and so retain the bits of the previous byte on the bus." That
        // is the real CPU open-bus latch now; this used to hardcode $40, which
        // happens to be what the bus holds during `LDA $4016` and nothing else.
        return static_cast<uint8_t>(bit | (open_bus() & 0xE0));
    }

    // Convenience for the frontend and for tests.
    void press(const int port, const uint8_t mask) { buttons[port] |= mask; }
    void release(const int port, const uint8_t mask) { buttons[port] &= static_cast<uint8_t>(~mask); }
    void set_port(const int port, const uint8_t mask) { buttons[port] = mask; }

    bool strobe_active() const { return strobe; }

private:
    void reload()
    {
        shift[0] = buttons[0];
        shift[1] = buttons[1];
    }

    bool strobe = false;
    uint8_t shift[2] = {0, 0};
};
