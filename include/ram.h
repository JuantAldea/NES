#pragma once

#include <array>

#include "device.h"
#include <iostream>
class RAM : public Device
{
public:
    RAM(Bus *b) : Device {b} {};
    void write(const uint16_t addr, const uint8_t data) { memory[addr] = data; };
    void write(const uint16_t start_addr, const size_t n_bytes, const uint8_t *bytes) {
        memcpy(memory.data() + start_addr, bytes, n_bytes);
    }

    uint8_t read(const uint16_t addr) {
        return memory[addr]; };

    std::array<uint8_t, 64*1024> memory = { 0 };

};
