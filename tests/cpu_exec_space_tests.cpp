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

#include "blargg_rom_harness.h"
#include "gtest/gtest.h"
#include "rom_fixture.h"

namespace tests
{
namespace cpu_exec_space
{
namespace
{

// Measured: both report within a few frames. The cap only exists so a hang
// fails with a diagnosis instead of stalling the suite.
constexpr uint64_t kMaxFrames = 600;

constexpr const char* kFetch = "run tests/test_files/fetch_cpu_exec_space.sh";

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/cpu_exec_space/" + name + ".nes";
}

// Start::PowerOn, not the default reset: Bus's constructor already reset this
// CPU before a cartridge was mapped, and a reset subtracts 3 from S rather than
// reloading it.
//
// Through the shared harness, which answers $81. Neither of these two ROMs asks
// for a reset today, so a loop that mishandled $81 looks correct here.
blargg::RomResult run_rom(const std::string& name)
{
    return blargg::run_rom(rom_path(name), kMaxFrames, blargg::Start::PowerOn);
}

using Result = blargg::RomResult;

}  // namespace

class CpuExecSpaceRoms : public ::testing::TestWithParam<std::string>
{
};

TEST_P(CpuExecSpaceRoms, reports_pass)
{
    const std::string name = GetParam();
    REQUIRE_ROM(rom_path(name), kFetch);

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
