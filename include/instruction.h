#pragma once

#include <functional>
#include <vector>
#include <valarray>
#include <string>
#include "device.h"
#include "addressing_types.h"
#include "cpu.h"

struct Instruction
{
    std::string name;
    AddressingTypes addr_type;
    std::function<void(CPU&)> operation = nullptr;
    std::function<uint16_t(CPU&)> addressing = nullptr;
    uint8_t cycles;

    static const std::valarray<Instruction> instruction_set;
};
