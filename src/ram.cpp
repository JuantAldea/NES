#include "devices/ram.h"

void RAM::write(const uint16_t addr, const uint8_t data)
{
    memory[addr] = data;
}

uint8_t RAM::read(const uint16_t addr)
{
    return memory[addr];
}
