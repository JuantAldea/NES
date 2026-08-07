#pragma once
#include "apu.h"
#include "controller.h"
#include "cpu.h"
#include "device.h"
#include "ppu.h"
#include "ram.h"
#include "rom.h"

class Bus
{
public:
    Bus();
    void write(const uint16_t addr, const uint8_t data);
    void write_ram(const uint16_t start_addr, const size_t n_bytes, const uint8_t* bytes);
    uint8_t read(const uint16_t addr);
    bool load_cartridge(const std::string& path);
    uint64_t total_cycles = 0;

    // CPU cycles elapsed, counting the ones OAM DMA steals - which CPU::clock
    // never sees, so CPU::total_cycles does not count them.
    //
    // This is the divide-by-two everything phase-sensitive on the CPU bus hangs
    // off: the APU frame counter's get/put alignment and OAM DMA's are the same
    // one. Deriving DMA's from CPU::total_cycles made the two disagree by the
    // length of every DMA that had already run.
    uint64_t cpu_cycles = 0;

    // Returns true on the master tick that completed a CPU instruction, which
    // is the only moment an outside observer can safely inspect CPU state: in
    // between, the schedule has a half-resolved address and a latched operand
    // that belong to no architectural register.
    //
    // Every existing caller ignores the return and is unaffected.
    bool clock();
    bool clock_CPU();
    void clock_PPU();

    // Passed to CPU::clock, which prints one line per instruction to stdout.
    // Off by default so the headless harnesses stay silent; the frontend's
    // Trace checkbox is the only thing that sets it.
    bool trace_cpu = false;

    // Runs until the CPU completes the instruction it is in the middle of.
    // Returns false if it gave up instead - see the definition for why that is
    // a real outcome and not defensive padding.
    bool step_instruction();

    // Runs until the PPU finishes the frame it is in the middle of.
    //
    // This is not the same as clocking a fixed number of times, which is what
    // everything that needed a frame used to do. An NTSC frame is 341x262
    // dots and the master clock runs four times per dot, so 341*262*4 looks
    // exact - but odd frames with rendering enabled skip their last dot, so
    // that count is one dot too many every other frame. It was harmless while
    // nothing rendered and the skip never armed; it is not now.
    //
    // Watching the PPU's own frame counter is right by construction and cannot
    // drift, whatever the PPU decides a frame is.
    void run_frame();

    CPU cpu;
    APU apu;
    PPU ppu;
    SystemRAM ram;
    PrgRAM prg_ram;
    ROM rom;
    Controllers controllers;

protected:
    // Reads and writes go through ONE decode function, so the two paths cannot
    // drift apart on any address where hardware agrees - which is every address
    // but one.
    //
    // $4017 is the exception, and it is a real property of the NES rather than
    // an emulator convenience: writing it sets the APU frame counter, reading
    // it returns controller port 2. They are different devices. That is why
    // decode takes a direction instead of the read and write paths each growing
    // their own special case, which is exactly how the two would come to
    // disagree about the addresses where hardware does NOT.
    enum class Access { Read, Write };

    struct DecodedAddress {
        Device* device;  // nullptr for open-bus ranges (no device backs them)
        uint16_t effective_addr;
    };
    DecodedAddress decode(const uint16_t addr, const Access access);
};
