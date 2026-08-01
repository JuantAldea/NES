#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "device.h"

// Flat, fixed-size RAM block. Address mirroring is a Bus-level address-decode
// concern (see Bus::decode); a RAM instance only ever sees already-mirrored,
// in-range addresses.
//
// N_BYTES is a template parameter so the same implementation backs both the
// 2KB internal work RAM at $0000-$07FF (mirrored through $1FFF) and the 8KB
// cartridge PRG-RAM at $6000-$7FFF.
template <size_t N_BYTES>
class RAM : public Device
{
public:
    static constexpr size_t SIZE = N_BYTES;

    RAM(Bus* b) : Device{b} {};
    void write(const uint16_t addr, const uint8_t data) { memory[addr] = data; };

    // Bulk helper used by Bus::write_ram (e.g. to seed a test program). Bytes
    // that would fall outside the array are silently truncated rather than
    // overrunning it.
    void write(const uint16_t start_addr, const size_t n_bytes, const uint8_t* bytes)
    {
        if (start_addr >= memory.size()) {
            return;
        }
        const size_t available = memory.size() - start_addr;
        const size_t to_copy = std::min(n_bytes, available);
        memcpy(memory.data() + start_addr, bytes, to_copy);
    }

    uint8_t read(const uint16_t addr) { return memory[addr]; };

    std::array<uint8_t, SIZE> memory = {0};
};

using SystemRAM = RAM<2 * 1024>;
using PrgRAM = RAM<8 * 1024>;
