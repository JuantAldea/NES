#include "devices/bus.h"
Bus::Bus() :
    cpu {*this},
//    apu(*this),
//    ppu(*this),
    ram {*this}
{

}

const Device& Bus::get_device_from_addr(const uint16_t addr) const
{
/*
    if (addr >= 0x0000 && addr < 0x2000) {
        return ram;
    } else if (addr >= 0x2000 && addr < 0x2008 || addr == 0x2014) {
        return ppu;
    } else if (addr >= 0x4000 && addr < 0x4016 && addr != 0x2014) {
        return apu;
    } else if (addr >= 0x40 && addr < 0x6000) {
        //expansion rom;
    } else if (addr >= 0x6000 && addr < 0x8000) {
        //sram;
    }

*/
    return ram;
}

Device& Bus::get_device_from_addr(const uint16_t addr)
{
    return ram;
}

void Bus::write(const uint16_t addr, const uint8_t data)
{
    Device &device = get_device_from_addr(addr);
    device.write(addr, data);
}

uint8_t Bus::read(const uint16_t addr)
{
    Device &device = get_device_from_addr(addr);
    return device.read(addr);
}
