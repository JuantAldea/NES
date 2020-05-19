#include "bus.h"

Bus::Bus()
    : cpu{std::bind(&Bus::read, this, std::placeholders::_1),
          std::bind(&Bus::write, this, std::placeholders::_1, std::placeholders::_2)},
      apu{this},
      ppu{this},
      ram{this}
{
    cpu.reset();
}

Device& Bus::get_device_from_addr(const uint16_t addr)
{
    if (addr < 0x2000) {
        std::cout << "RAM " << std::hex << addr << "\n";
        return ram;
    } else if ((addr >= 0x2000 && addr < 0x3FFF) || addr == 0x4014) {
        std::cout << "PPU " << std::hex << addr << "\n";
        return ppu;
    } else if (addr >= 0x4000 && addr < 0x4016 && addr != 0x4014) {
        std::cout << "APU " << std::hex << addr << "\n";
        return apu;
    } else if (addr >= 0x4020 && addr < 0x6000) {
        std::cout << "ROM " << std::hex << addr << "\n";
        //expansion rom;
    } else if (addr >= 0x6000 && addr < 0x8000) {
        //sram;
    }

    std::cout << "DEFAULT RAM\n";
    return ram;
}

//Device& Bus::get_device_from_addr(const uint16_t addr) { return ram; }

void Bus::write(const uint16_t addr, const uint8_t data)
{
    Device& device = get_device_from_addr(addr);
    device.write(addr, data);
}

void Bus::write_ram(const uint16_t start_addr, const size_t n_bytes, const uint8_t* bytes)
{
    ram.write(start_addr, n_bytes, bytes);
}

uint8_t Bus::read(const uint16_t addr)
{
    if (addr < 0x2000) {
        const uint16_t effective_addr = addr % 0x0800;
        return ram.read(effective_addr);
    } else if (addr >= 0x2000 && addr < 0x4000) {
        const uint16_t addr_index = addr % 0x8;
        return ppu.read(0x2000 + addr_index);
    } else if (addr >= 0x4000 && addr < 0x4014) {
        return apu.read(addr);
    } else if (addr == 0x4014) {
        return ppu.read(addr);
    } else if (addr == 0x4015) {
        return apu.read(addr);
#if 0
    } else if (addr == 0x4016) {
        return pad1.read(addr);
    } else if (addr == 0x4017) {
        return pad2.read(addr);
#endif
    } else if (addr >= 0x4020 && addr < 0x6000) {
        //expansion rom;
    } else if (addr >= 0x6000 && addr < 0x8000) {
        //sram;
    }

    return 0;
}

void Bus::clock()
{
    if (!ppu.dma_in_progress()) {
        cpu.clock(false);
    }

    ppu.clock();
}
