// Blargg ppu_vbl_nmi test-ROM harness.
//
// These ROMs are the oracle for PPU frame timing: vblank set/clear timing, NMI
// enable/suppression, and odd-frame behaviour. They report their result through
// PRG-RAM rather than the screen, so they are fully headless.
//
// ALL TEN PASS. This file is now a regression guard rather than a target: it
// measures to single-cycle resolution, so almost any drift in the CPU/PPU
// timing relationship surfaces here first. A failure is a real regression, not
// an expected gap. Do NOT weaken these assertions to make the suite green.
//
// Four properties these ROMs are the only tests to pin down, all of which had
// to be right before the last seven went green:
//   - /NMI is a level, not an edge. The PPU drives a line the CPU samples, so
//     an assertion can be revoked before the CPU acts on it. That is what makes
//     NMI suppression (06-suppression) expressible at all.
//   - Interrupts are polled on an instruction's penultimate cycle, not at the
//     instruction boundary. 04-nmi_control measures this directly, and says so:
//     "Immediate occurence should be after NEXT instruction".
//   - The CPU's bus access and its interrupt sample are one PPU dot apart, not
//     simultaneous (05-nmi_timing, 07/08-nmi_on/off_timing).
//   - The odd-frame clock skip samples rendering state one dot before the skip
//     rather than at it (10-even_odd_timing).
//
// An earlier version of this comment blamed the failures on CPU::clock applying
// an instruction's whole memory effect on its last cycle. That was wrong - the
// core was already cycle-stepped, one bus access per cycle. The four properties
// above were the actual gap, which is worth remembering: a plausible diagnosis
// that survives because nobody re-derives it is expensive.
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "../include/bus.h"

namespace tests
{
namespace blargg
{

// Blargg's shared test protocol (see his readme.txt):
//   $6001-$6003  validation signature $DE $B0 $61; until it reads back exactly
//                this, $6000 holds no meaningful value
//   $6000        $80 = still running, $81 = ROM wants a soft reset,
//                anything else = final result code, 0 meaning pass
//   $6004...     NUL-terminated ASCII description of the result
constexpr uint16_t kStatusAddr = 0x6000;
constexpr uint16_t kSignatureAddr = 0x6001;
constexpr uint16_t kMessageAddr = 0x6004;

constexpr uint8_t kSignature[3] = {0xDE, 0xB0, 0x61};
constexpr uint8_t kStatusRunning = 0x80;
constexpr uint8_t kStatusNeedsReset = 0x81;

// Frames are stepped with Bus::run_frame rather than counted in cycles; see
// the comment on its declaration for why a frame is not a fixed length.
//
// The cap must be generous. These ROMs spend many frames measuring before they
// report: 01-vbl_basics does not answer until frame 142 and 02-vbl_set_time not
// until 161. An earlier 60-frame cap turned both into "timed out", which read
// as a dead PPU when in fact one of them passes -- the harness was
// under-reporting real progress. The cap exists only so a genuinely stuck PPU
// fails with a diagnosis instead of hanging the suite.
constexpr uint64_t kMaxFrames = 400;

struct BlarggResult {
    bool completed = false;    // signature appeared and status left the running state
    bool needs_reset = false;  // ROM asked for a soft reset (we do not drive one)
    bool saw_signature = false;
    uint8_t status = 0xFF;
    uint8_t last_status = 0xFF;  // last $6000 seen while the signature was valid
    std::string message;
    uint64_t frames_run = 0;
    // Sampled at the end of a timed-out run, to distinguish "the ROM never got
    // going" from "the ROM is alive but waiting on something the PPU owes it".
    uint64_t cpu_cycles = 0;
    uint16_t final_pc = 0;
    uint8_t final_ppustatus = 0;
};

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/blargg_ppu/" + name + ".nes";
}

bool signature_present(Bus& console)
{
    for (uint16_t i = 0; i < 3; ++i) {
        if (console.read(static_cast<uint16_t>(kSignatureAddr + i)) != kSignature[i]) {
            return false;
        }
    }
    return true;
}

std::string read_message(Bus& console)
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

BlarggResult run_rom(const std::string& name)
{
    BlarggResult result;

    Bus console;
    if (!console.load_cartridge(rom_path(name))) {
        ADD_FAILURE() << "could not load " << rom_path(name) << " - run tests/test_files/fetch_blargg_ppu.sh";
        return result;
    }

    console.cpu.reset();

    // Stepped a frame at a time. The old loop multiplied out a fixed
    // 341*262*4 per frame, which overshoots by a dot on every odd frame once
    // rendering is enabled - and these ROMs enable it. Sampling once per frame
    // is also cheaper than the every-65536-cycles compromise it replaces.
    for (uint64_t frame = 0; frame < kMaxFrames; ++frame) {
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

        if (status == kStatusNeedsReset) {
            result.needs_reset = true;
            return result;
        }

        result.completed = true;
        return result;
    }

    result.frames_run = kMaxFrames;
    result.cpu_cycles = console.cpu.total_cycles;
    result.final_pc = console.cpu.registers.PC;
    result.final_ppustatus = console.ppu.registers.PPUSTATUS;
    return result;
}

class BlarggPpuVblNmi : public ::testing::TestWithParam<std::string>
{
};

TEST_P(BlarggPpuVblNmi, reports_pass)
{
    const std::string name = GetParam();
    const BlarggResult result = run_rom(name);

    if (!result.completed && !result.needs_reset) {
        // Distinguish the two very different reasons for a timeout, because
        // they point at opposite ends of the emulator.
        if (!result.saw_signature) {
            FAIL() << name << ": timed out after " << result.frames_run
                   << " frames without ever writing the $DE $B0 $61 signature.\n"
                      "  The ROM did not get far enough to initialise. Suspect the CPU,\n"
                      "  the cartridge mapping, or the $6000-$7FFF PRG-RAM window -- not\n"
                      "  the PPU. CPU ran " << result.cpu_cycles << " cycles, ending at PC=$" << std::hex
                   << result.final_pc << std::dec << ".";
        }

        FAIL() << name << ": timed out after " << result.frames_run << " frames still reporting status $" << std::hex
               << static_cast<int>(result.last_status) << std::dec
               << " (running).\n"
                  "  The ROM initialised correctly (signature written) and is now waiting\n"
                  "  on the PPU. PPUSTATUS=$"
               << std::hex << static_cast<int>(result.final_ppustatus) << std::dec << ", CPU ran " << result.cpu_cycles
               << " cycles, ending at PC=$" << std::hex << result.final_pc << std::dec
               << ".\n"
                  "  The frame state machine itself works: vblank sets at (241,1) and\n"
                  "  clears at (261,1) once per frame. Expected while CPU::clock applies\n"
                  "  an instruction's entire memory effect on its last cycle instead of\n"
                  "  per-cycle, which these ROMs measure to single-cycle resolution.";
    }

    if (result.needs_reset) {
        FAIL() << name << ": ROM requested a soft reset, which this harness does not drive yet";
    }

    EXPECT_EQ(0, result.status) << name << " failed with code " << static_cast<int>(result.status) << ":\n  "
                               << result.message;
}

INSTANTIATE_TEST_SUITE_P(PpuVblNmi, BlarggPpuVblNmi,
                         ::testing::Values("01-vbl_basics", "02-vbl_set_time", "03-vbl_clear_time", "04-nmi_control",
                                           "05-nmi_timing", "06-suppression", "07-nmi_on_timing", "08-nmi_off_timing",
                                           "09-even_odd_frames", "10-even_odd_timing"),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                             // gtest requires alphanumeric/underscore test names.
                             std::string sanitized = info.param;
                             for (char& c : sanitized) {
                                 if (!std::isalnum(static_cast<unsigned char>(c))) {
                                     c = '_';
                                 }
                             }
                             return sanitized;
                         });

// Guards the harness itself rather than the emulator: if the ROMs are missing
// or the PRG-RAM window regresses, the parameterized tests above would all fail
// identically and it would not be obvious which of the two happened.
GTEST_TEST(blarggHarness, prg_ram_window_is_readable_and_roms_are_present)
{
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom_path("01-vbl_basics")))
        << "Blargg ROMs absent - run tests/test_files/fetch_blargg_ppu.sh";

    // The whole $6000-$7FFF reporting window must round-trip, since the entire
    // protocol lives there.
    console.write(kStatusAddr, 0x80);
    console.write(kSignatureAddr, 0xDE);
    console.write(static_cast<uint16_t>(kSignatureAddr + 1), 0xB0);
    console.write(static_cast<uint16_t>(kSignatureAddr + 2), 0x61);
    EXPECT_EQ(0x80, console.read(kStatusAddr));
    EXPECT_TRUE(signature_present(console));

    // And the cartridge must actually be visible, i.e. this is a 32KB NROM
    // image filling $8000-$FFFF rather than a 16KB one mirrored into it.
    EXPECT_EQ(32768u, console.rom.prg_rom.size());
    EXPECT_EQ(0, console.rom.mapper_id);
}

}  // namespace blargg
}  // namespace tests
