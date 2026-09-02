#pragma once
// Shared runner for Blargg's test ROMs.
//
// All of his suites report through PRG-RAM using the same protocol, so they
// differ only in which directory they load from, how long they are allowed to
// run, and - for the cpu_reset ROMs alone - whether they start from a power-on
// or a reset. Every suite in this repo that speaks the protocol goes through
// here. A private copy of this loop drifts in ways invisible from inside the
// suite that has it: driving no resets at all, lacking the stale-$6000 guard
// below, or reporting a ROM's $81 reset REQUEST as "failed with code 129" - a
// request misread as a verdict. All three have happened here.
//
//   $6001-$6003  validation signature $DE $B0 $61; until it reads back exactly
//                this, $6000 holds no meaningful value
//   $6000        $80 = still running, $81 = ROM wants a soft reset (which
//                run_rom drives, after the required 100ms delay),
//                anything else = final result code, 0 meaning pass
//   $6004...     NUL-terminated ASCII description of the result
#include <cstdint>
#include <string>

#include "../include/bus.h"
#include "gtest/gtest.h"

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

struct RomResult {
    bool completed = false;  // signature appeared and status left the running state
    // Set only when a ROM asked for MORE resets than kMaxResets allows. A ROM
    // that asks once or twice and finishes is a normal completed run, not this.
    bool needs_reset = false;
    uint32_t resets_driven = 0;
    bool saw_signature = false;
    uint8_t status = 0xFF;
    uint8_t last_status = 0xFF;
    std::string message;
    uint64_t frames_run = 0;
    // Sampled on a timed-out run, to separate "the ROM never got going" from
    // "the ROM is alive and waiting on something".
    uint64_t cpu_cycles = 0;
    uint16_t final_pc = 0;
    // Read directly out of the register rather than through PPU::read, which
    // would clear the vblank flag it is being sampled to report.
    uint8_t final_ppustatus = 0;
};

// Which entry point a suite needs before its ROM starts running.
//
// `Reset` suits every suite that only needs the ROM started: Bus's constructor
// has already reset the CPU once, and these ROMs cannot tell that this is the
// second.
//
// `PowerOn` exists because two ROMs CAN tell. That constructor reset ran before
// a cartridge was mapped, so resetting again here would be the second one,
// leaving S at $FA - and blargg's registers.nes checks for $FD at power. That
// distinction is the entire reason CPU::power_on and CPU::reset are separate
// functions, and it did not exist until this ROM forced it.
enum class Start {
    Reset,
    PowerOn,
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

// $81 is a REQUEST, and answering it is the harness's job. blargg's readme:
// "$81 means the test needs the reset button pressed, but delayed by at least
// 100 msec from now."
//
// 100 ms is 6.01 frames at 60.0988 Hz, so 7 is the true minimum and this is 10
// for margin.
//
// The exact value is deliberately NOT load-bearing, and that was worth
// establishing: measured at 6, 7, 8, 10, 12, 15, 20 and 30 frames, all five
// passing ROMs pass at every one. An earlier version needed 20 or more, which
// looked like a timing requirement and was really the stale-$6000 race below -
// so if this constant ever starts mattering again, the bug is there, not here.
constexpr uint64_t kResetDelayFrames = 10;

// A cap, not a budget. The apu_reset ROMs ask once or twice; a ROM asking
// forever is a defect, and without a limit it would hang the suite instead of
// reporting one. Reaching it sets RomResult::needs_reset.
constexpr uint32_t kMaxResets = 4;

// Runs one ROM to completion or until `max_frames` elapse, pressing RESET
// whenever the ROM asks. The caller supplies the full path so each suite can
// keep its own fixtures directory.
inline RomResult run_rom(const std::string& path, uint64_t max_frames, Start start = Start::Reset)
{
    RomResult result;

    Bus console;
    if (!console.load_cartridge(path)) {
        ADD_FAILURE() << "could not load " << path;
        return result;
    }

    if (start == Start::PowerOn) {
        console.cpu.power_on();
    } else {
        console.cpu.reset();
    }

    // Counts down only while a reset is pending, so an ordinary run never
    // touches it.
    uint64_t frames_until_reset = 0;

    // $6000 survives a reset, because PRG-RAM does. So immediately after one it
    // still holds the $81 the ROM wrote BEFORE it, and believing that value
    // schedules a second reset the ROM never asked for - which is exactly what
    // happened here: len_ctrs_enabled and 4017_written failed with one spurious
    // extra reset each. Raising the reset delay appeared to fix it, because a
    // ROM given longer had usually republished by the time it was read, but
    // that only moved the race. The status is trusted again once the ROM writes
    // $80 (running) from its reset handler.
    //
    // Still load-bearing, and re-measured when cpu_behaviour folded in: with
    // this guard defeated those two ROMs fail again, and so does registers.nes
    // (Failed #3, 2 resets driven, S=$F1). Its old private loop carried a
    // 15-frame delay for exactly this reason.
    bool status_is_stale = false;

    // Stepped a frame at a time rather than by a cycle budget. The old loop
    // multiplied out a fixed 341*262*4 per frame, which is one dot too many on
    // every odd frame once rendering is enabled, so "max_frames" drifted longer
    // the further a ROM ran. Sampling once per frame is also cheaper than the
    // every-65536-cycles compromise it replaces.
    for (uint64_t frame = 0; frame < max_frames; ++frame) {
        console.run_frame();

        if (frames_until_reset > 0 && --frames_until_reset == 0) {
            console.reset();
            ++result.resets_driven;
            status_is_stale = true;
            continue;
        }

        if (!signature_present(console)) {
            continue;
        }
        result.saw_signature = true;

        const uint8_t status = console.read(kStatusAddr);
        result.last_status = status;
        if (status == kStatusRunning) {
            // The ROM has republished, so whatever it says next is its own.
            status_is_stale = false;
            continue;
        }

        if (status == kStatusNeedsReset) {
            // Left over from before the reset we just drove, not a new request.
            if (status_is_stale) {
                continue;
            }

            // Already waiting - the status stays $81 across the whole delay, so
            // without this the request would be re-armed every frame and the
            // reset would never actually land.
            if (frames_until_reset > 0) {
                continue;
            }
            if (result.resets_driven < kMaxResets) {
                frames_until_reset = kResetDelayFrames;
                continue;
            }
            // Out of resets: report it as such rather than as a verdict.
            result.frames_run = frame + 1;
            result.status = status;
            result.message = read_message(console);
            result.needs_reset = true;
            return result;
        }

        result.frames_run = frame + 1;
        result.status = status;
        result.message = read_message(console);
        result.completed = true;
        return result;
    }

    result.frames_run = max_frames;
    result.cpu_cycles = console.cpu.total_cycles;
    result.final_pc = console.cpu.registers.PC;
    result.final_ppustatus = console.ppu.registers.PPUSTATUS;
    // A ROM that ran out of frames has usually already printed which subtest it
    // reached, which is the most useful line in the failure message. Guarded on
    // the signature because before that appears $6004 is uninitialised PRG-RAM
    // and would decode as noise presented as a quotation from the ROM.
    if (result.saw_signature) {
        result.message = read_message(console);
    }
    return result;
}

}  // namespace blargg
}  // namespace tests
