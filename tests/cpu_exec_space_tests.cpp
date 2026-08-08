// blargg's cpu_exec_space: the CPU executing code out of I/O space.
//
// From its readme: "These tests verify that the CPU can execute code from any
// possible memory location, even if that is mapped as I/O space." The program
// counter walks THROUGH $4000-$40FF and $2000-$2007, so every byte fetched
// there is whatever the hardware leaves on the data bus for a write-only
// register.
//
// That makes these the only ROMs here that measure CPU OPEN BUS, and they found
// two real defects on first contact. Bus::read returned a constant 0 for
// write-only registers and unmapped space, where hardware leaves the last value
// that was on the bus; and $4014 returned the PPU's internal latch, when OAM
// DMA is not a PPU register at all - it lives in the 2A03 beside the CPU, and
// only Bus::decode routes it to that device.
//
// The unit tests alongside these live in ppu_register_tests.cpp
// (reading_oamdma_returns_the_cpu_bus_not_the_ppu_latch), controller_tests.cpp
// (the_undriven_bits_follow_the_cpu_data_bus) and memory_tests.cpp
// (unmapped_ranges_are_open_bus). Those say precisely what is wrong when they
// fail; these say whether real hardware agrees.
#include <cstdint>
#include <string>

#include "../include/bus.h"
#include "gtest/gtest.h"
#include "nametable_screen.h"

namespace tests
{
namespace cpu_exec_space
{
namespace
{

using nametable_screen::run_one_frame;

constexpr uint16_t kStatusAddr = 0x6000;
constexpr uint16_t kSignatureAddr = 0x6001;
constexpr uint16_t kMessageAddr = 0x6004;
constexpr uint8_t kStatusRunning = 0x80;

// Measured: both report within a few frames. The cap only exists so a hang
// fails with a diagnosis instead of stalling the suite.
constexpr int kMaxFrames = 600;

struct Result {
    bool completed = false;
    uint8_t status = 0xFF;
    std::string message;
};

Result run_rom(const std::string& name)
{
    Result result;

    Bus console;
    const std::string path = std::string(NES_TEST_FILES_DIR) + "/cpu_exec_space/" + name + ".nes";
    if (!console.load_cartridge(path)) {
        ADD_FAILURE() << "could not load " << path << " - run tests/test_files/fetch_cpu_exec_space.sh";
        return result;
    }
    console.cpu.power_on();

    for (int frame = 0; frame < kMaxFrames; ++frame) {
        run_one_frame(console);

        if (console.read(kSignatureAddr) != 0xDE || console.read(static_cast<uint16_t>(kSignatureAddr + 1)) != 0xB0 ||
            console.read(static_cast<uint16_t>(kSignatureAddr + 2)) != 0x61) {
            continue;
        }

        const uint8_t status = console.read(kStatusAddr);
        if (status == kStatusRunning) {
            continue;
        }

        result.completed = true;
        result.status = status;
        // These ROMs print ANSI colour codes into the message; kept verbatim so
        // a failure report is exactly what the ROM said.
        for (uint16_t i = 0; i < 600; ++i) {
            const uint8_t c = console.read(static_cast<uint16_t>(kMessageAddr + i));
            if (c == 0x00) {
                break;
            }
            result.message.push_back(static_cast<char>(c));
        }
        return result;
    }
    return result;
}

}  // namespace

class CpuExecSpaceRoms : public ::testing::TestWithParam<std::string>
{
};

TEST_P(CpuExecSpaceRoms, reports_pass)
{
    const std::string name = GetParam();
    const Result result = run_rom(name);

    ASSERT_TRUE(result.completed) << name << ": no verdict within " << kMaxFrames << " frames";

    // The failure text is worth printing in full. When this ROM fails it names
    // the address it got stuck at - "0022 4000 4001 ... 4013 4014 ERROR" - and
    // that address is the whole diagnosis.
    EXPECT_EQ(0, result.status) << name << " failed with code " << static_cast<int>(result.status) << ":\n"
                                << result.message;
}

INSTANTIATE_TEST_SUITE_P(CpuExecSpace,
                         CpuExecSpaceRoms,
                         ::testing::Values("test_cpu_exec_space_apu", "test_cpu_exec_space_ppuio"),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                             std::string s = info.param;
                             for (char& c : s) {
                                 if (!std::isalnum(static_cast<unsigned char>(c))) {
                                     c = '_';
                                 }
                             }
                             return s;
                         });

}  // namespace cpu_exec_space
}  // namespace tests
