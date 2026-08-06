#pragma once
// Shared runner for Blargg's test ROMs.
//
// All of his suites report through PRG-RAM using the same protocol, so the
// ppu_vbl_nmi and cpu_interrupts_v2 harnesses differ only in which directory
// they load from and how long they are allowed to run.
//
//   $6001-$6003  validation signature $DE $B0 $61; until it reads back exactly
//                this, $6000 holds no meaningful value
//   $6000        $80 = still running, $81 = ROM wants a soft reset,
//                anything else = final result code, 0 meaning pass
//   $6004...     NUL-terminated ASCII description of the result
#include <cstdint>
#include <string>

#include "gtest/gtest.h"

#include "../include/bus.h"

namespace tests
{
namespace blargg
{

constexpr uint16_t kStatusAddr = 0x6000;
constexpr uint16_t kSignatureAddr = 0x6001;
constexpr uint16_t kMessageAddr = 0x6004;

constexpr uint8_t kSignature[3] = {0xDE, 0xB0, 0x61};
constexpr uint8_t kStatusRunning = 0x80;
constexpr uint8_t kStatusNeedsReset = 0x81;

// One frame is 341 dots x 262 scanlines and Bus::clock ticks the PPU every 4th
// bus cycle.

struct RomResult {
    bool completed = false;    // signature appeared and status left the running state
    bool needs_reset = false;  // ROM asked for a soft reset (we do not drive one)
    bool saw_signature = false;
    uint8_t status = 0xFF;
    uint8_t last_status = 0xFF;
    std::string message;
    uint64_t frames_run = 0;
    // Sampled on a timed-out run, to separate "the ROM never got going" from
    // "the ROM is alive and waiting on something".
    uint64_t cpu_cycles = 0;
    uint16_t final_pc = 0;
};

inline bool signature_present(Bus& console)
{
    for (uint16_t i = 0; i < 3; ++i) {
        if (console.read(static_cast<uint16_t>(kSignatureAddr + i)) != kSignature[i]) {
            return false;
        }
    }
    return true;
}

inline std::string read_message(Bus& console)
{
    std::string out;
    // Bounded: PRG-RAM is 8KB and a runaway pointer must not spin here.
    for (uint16_t i = 0; i < 1024; ++i) {
        const uint8_t c = console.read(static_cast<uint16_t>(kMessageAddr + i));
        if (c == 0x00) {
            break;
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// Runs one ROM to completion or until `max_frames` elapse. The caller supplies
// the full path so each suite can keep its own fixtures directory.
inline RomResult run_rom(const std::string& path, uint64_t max_frames)
{
    RomResult result;

    Bus console;
    if (!console.load_cartridge(path)) {
        ADD_FAILURE() << "could not load " << path;
        return result;
    }

    console.cpu.reset();

    // Stepped a frame at a time rather than by a cycle budget. The old loop
    // multiplied out a fixed 341*262*4 per frame, which is one dot too many on
    // every odd frame once rendering is enabled, so "max_frames" drifted longer
    // the further a ROM ran. Sampling once per frame is also cheaper than the
    // every-65536-cycles compromise it replaces.
    for (uint64_t frame = 0; frame < max_frames; ++frame) {
        console.run_frame();

        if (!signature_present(console)) {
            continue;
        }
        result.saw_signature = true;

        const uint8_t status = console.read(kStatusAddr);
        result.last_status = status;
        if (status == kStatusRunning) {
            continue;
        }

        result.frames_run = frame + 1;
        result.status = status;
        result.message = read_message(console);
        result.needs_reset = (status == kStatusNeedsReset);
        result.completed = !result.needs_reset;
        return result;
    }

    result.frames_run = max_frames;
    result.cpu_cycles = console.cpu.total_cycles;
    result.final_pc = console.cpu.registers.PC;
    return result;
}

}  // namespace blargg
}  // namespace tests
