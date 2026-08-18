// blargg's cpu_reset and cpu_dummy_reads/writes ROMs, plus direct assertions on
// what a reset does.
//
// This file exists because of a hole with a particular shape: CPU::reset() was
// called as SETUP by almost every test in this suite and asserted on by none.
// It was implemented as a power-on - zeroing A/X/Y and forcing S to $FD - and
// nothing could tell, because a second call was indistinguishable from the
// first. registers.nes found it immediately and states the rule in one line:
//
//     "Reset should set I flag, subtract 3 from S, nothing more"
//
// The unit tests below assert that directly, and the ROMs assert it against
// hardware. Both are here on purpose: the unit tests say precisely what is
// wrong when they fail, and the ROMs do not care what I believe the rule is.
#include <cstdint>
#include <cstring>
#include <string>

#include "../include/bus.h"
#include "gtest/gtest.h"
#include "nametable_screen.h"

namespace tests
{
namespace cpu_behaviour
{
namespace
{

using nametable_screen::read_text;
using nametable_screen::run_one_frame;

constexpr uint16_t kStatusAddr = 0x6000;
constexpr uint16_t kSignatureAddr = 0x6001;
constexpr uint16_t kMessageAddr = 0x6004;
constexpr uint8_t kStatusRunning = 0x80;
constexpr uint8_t kStatusNeedsReset = 0x81;

// Measured: registers.nes still reports Failed #3 with a 7-frame delay and
// passes from 12 onward. blargg's readme asks for roughly 100ms of held RESET;
// 15 frames is ~250ms, comfortably past the threshold without being slow.
constexpr int kFramesBeforeReset = 15;
constexpr int kMaxFrames = 900;

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/cpu_behaviour/" + name + ".nes";
}

bool signature_present(Bus& console)
{
    return console.read(kSignatureAddr) == 0xDE && console.read(static_cast<uint16_t>(kSignatureAddr + 1)) == 0xB0 &&
           console.read(static_cast<uint16_t>(kSignatureAddr + 2)) == 0x61;
}

std::string read_message(Bus& console)
{
    std::string out;
    for (uint16_t i = 0; i < 512; ++i) {
        const uint8_t c = console.read(static_cast<uint16_t>(kMessageAddr + i));
        if (c == 0x00) {
            break;
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

struct RomResult {
    bool completed = false;
    uint8_t status = 0xFF;
    std::string message;
    int resets = 0;
};

// Runs a $6000-protocol ROM, DRIVING A SOFT RESET whenever it asks for one.
//
// This used to be the only harness that did, which is why it is a local copy.
// blargg_rom_harness.h now drives resets too, so this exists only for the
// power_on() below - these two ROMs are the reason CPU::power_on and CPU::reset
// are separate, and the shared harness resets rather than powers on. Folding
// the two together needs that difference kept, not smoothed over.
RomResult run_with_resets(const std::string& name)
{
    RomResult result;

    Bus console;
    if (!console.load_cartridge(rom_path(name))) {
        ADD_FAILURE() << "could not load " << rom_path(name) << " - run tests/test_files/fetch_cpu_behaviour.sh";
        return result;
    }

    // power_on(), not reset(). Bus's constructor already reset this CPU before
    // a cartridge was mapped, so a reset here would be the SECOND - leaving S
    // at $FA, and registers.nes checks for $FD at power. That distinction did
    // not exist until this ROM forced it.
    console.cpu.power_on();

    for (int frame = 0; frame < kMaxFrames; ++frame) {
        run_one_frame(console);

        if (!signature_present(console)) {
            continue;
        }

        const uint8_t status = console.read(kStatusAddr);
        if (status == kStatusNeedsReset) {
            for (int w = 0; w < kFramesBeforeReset; ++w) {
                run_one_frame(console);
            }
            console.reset();
            ++result.resets;
            continue;
        }
        if (status == kStatusRunning) {
            continue;
        }

        result.completed = true;
        result.status = status;
        result.message = read_message(console);
        return result;
    }

    result.message = read_message(console);
    return result;
}

}  // namespace

// --- what a reset does, asserted directly ---------------------------------

GTEST_TEST(cpuReset, preserves_the_registers_and_only_subtracts_three_from_s)
{
    Bus console;
    console.cpu.power_on();

    console.cpu.registers.A = 0x34;
    console.cpu.registers.X = 0x56;
    console.cpu.registers.Y = 0x78;
    const uint8_t sp_before = console.cpu.registers.SP;
    const uint8_t p_before = console.cpu.registers.P;

    console.cpu.reset();

    EXPECT_EQ(0x34, console.cpu.registers.A) << "a reset does not clear A";
    EXPECT_EQ(0x56, console.cpu.registers.X) << "a reset does not clear X";
    EXPECT_EQ(0x78, console.cpu.registers.Y) << "a reset does not clear Y";
    EXPECT_EQ(static_cast<uint8_t>(sp_before - 3), console.cpu.registers.SP) << "S is decremented by 3, not reloaded";
    EXPECT_EQ(static_cast<uint8_t>(p_before | 0x04), console.cpu.registers.P) << "P is ORed with $04, nothing more";
}

// "S decremented by 3, but nothing written to stack" - the three accesses the
// reset sequence performs are reads. This is also what lets ram_after_reset
// check that RAM survives.
GTEST_TEST(cpuReset, writes_nothing_to_ram)
{
    Bus console;
    console.cpu.power_on();

    for (uint16_t i = 0; i < 0x0800; ++i) {
        console.ram.memory[i] = static_cast<uint8_t>(i ^ 0x5A);
    }
    uint8_t before[0x0800];
    std::memcpy(before, console.ram.memory.data(), sizeof(before));

    console.cpu.reset();

    EXPECT_EQ(0, std::memcmp(before, console.ram.memory.data(), sizeof(before)))
        << "reset modified RAM; the three stack accesses must be reads";
}

GTEST_TEST(cpuReset, two_resets_subtract_six)
{
    Bus console;
    console.cpu.power_on();
    ASSERT_EQ(0xFD, console.cpu.registers.SP) << "power-on leaves S at $FD";

    console.cpu.reset();
    EXPECT_EQ(0xFA, console.cpu.registers.SP);
    console.cpu.reset();
    EXPECT_EQ(0xF7, console.cpu.registers.SP) << "resets accumulate; they do not reload S";
}

GTEST_TEST(cpuPowerOn, establishes_the_documented_state)
{
    // blargg's readme: "At power: A, X, Y = 0, P = $34, S = $FD".
    //
    // P is $24 here rather than $34 because bit 4 is not a register bit at all:
    // it only appears in the copy PHP and BRK push. The ROM observes $34
    // because it reads P through PHP.
    Bus console;
    console.cpu.registers.A = 0xFF;
    console.cpu.registers.X = 0xFF;
    console.cpu.registers.Y = 0xFF;

    console.cpu.power_on();

    EXPECT_EQ(0x00, console.cpu.registers.A);
    EXPECT_EQ(0x00, console.cpu.registers.X);
    EXPECT_EQ(0x00, console.cpu.registers.Y);
    EXPECT_EQ(0xFD, console.cpu.registers.SP);
    EXPECT_EQ(0x24, console.cpu.registers.P) << "U and I set; B is not a register bit";
}

// --- the ROMs --------------------------------------------------------------

class CpuResetRoms : public ::testing::TestWithParam<std::string>
{
};

TEST_P(CpuResetRoms, reports_pass)
{
    const std::string name = GetParam();
    const RomResult result = run_with_resets(name);

    ASSERT_TRUE(result.completed) << name << ": no verdict within " << kMaxFrames << " frames after " << result.resets
                                  << " reset(s).\n  last message: " << result.message;

    EXPECT_EQ(0, result.status) << name << " failed with code " << static_cast<int>(result.status) << " after "
                                << result.resets << " reset(s):\n  " << result.message;
}

INSTANTIATE_TEST_SUITE_P(CpuReset,
                         CpuResetRoms,
                         ::testing::Values("registers", "ram_after_reset"),
                         [](const ::testing::TestParamInfo<std::string>& info) { return info.param; });

class CpuDummyWriteRoms : public ::testing::TestWithParam<std::string>
{
};

TEST_P(CpuDummyWriteRoms, reports_pass)
{
    const std::string name = GetParam();
    const RomResult result = run_with_resets(name);

    ASSERT_TRUE(result.completed) << name << ": no verdict within " << kMaxFrames << " frames";
    EXPECT_EQ(0, result.status) << name << " failed with code " << static_cast<int>(result.status) << ":\n  "
                                << result.message;
}

INSTANTIATE_TEST_SUITE_P(CpuDummyWrites,
                         CpuDummyWriteRoms,
                         ::testing::Values("cpu_dummy_writes_oam", "cpu_dummy_writes_ppumem"),
                         [](const ::testing::TestParamInfo<std::string>& info) { return info.param; });

// cpu_dummy_reads reports on screen rather than through $6000, so it needs the
// nametable reader instead of the protocol above.
GTEST_TEST(cpuDummyReads, reports_passed_on_screen)
{
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom_path("cpu_dummy_reads"))) << "run tests/test_files/fetch_cpu_behaviour.sh";
    console.cpu.power_on();

    std::string screen;
    for (int frame = 0; frame < 400; ++frame) {
        run_one_frame(console);
        screen = read_text(console);
        if (screen.find("Passed") != std::string::npos || screen.find("Failed") != std::string::npos) {
            break;
        }
    }

    EXPECT_NE(std::string::npos, screen.find("Passed")) << "cpu_dummy_reads did not report Passed:\n" << screen;
}

}  // namespace cpu_behaviour
}  // namespace tests
