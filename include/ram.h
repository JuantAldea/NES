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

// 32KB, not the 8KB the $6000-$7FFF window shows at once. SOROM and SXROM carry
// 16KB and 32KB of work RAM and page it through that window, so a device sized
// to the window cannot hold what the board has - Holy Mapperel's M1_P512K_S32K
// reported 8KB against a header declaring 32KB for exactly that reason.
//
// Sizing to the largest board and letting ROM::prg_ram_offset fold every access
// into the cartridge's REAL size is what keeps a smaller cartridge honest: an
// 8KB board's bank lines are not connected, so a bank select there has to wrap
// back onto the single chip rather than reach the other 24KB. That fold is the
// enforcement, not this number.
using PrgRAM = RAM<32 * 1024>;
