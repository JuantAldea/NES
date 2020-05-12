#pragma once

#include <functional>
#include <string>
#include <valarray>
#include <vector>

#include "addressing_types.h"
#include "cpu.h"
#include "device.h"

struct Instruction
{
    std::string name;
    Addressing addr_type;
    std::function<void(CPU&)> operation = nullptr;
    std::function<void(CPU&)> addressing = nullptr;
    uint8_t cycles;

    static const std::valarray<Instruction> instruction_set;
};
