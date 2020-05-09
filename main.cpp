#include "bus.h"
#include <vector>
#include <fstream>
#include <iostream>
int main(int argc, char **argv)
{
    Bus console;
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
     file.read(reinterpret_cast<char*>(console.ram.memory.data()), size);
    console.cpu.reset();
    while(true) {
       console.cpu.execute_next_instruction(false);
    }
}
