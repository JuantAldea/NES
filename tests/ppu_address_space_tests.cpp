// PPU address-space test ROMs.
//
// The headless part of the oracle for the PPU's memory interface: OAM and open
// bus.
//
// The address space itself - pattern tables, nametable and palette mirroring,
// the $2007 read buffer - IS implemented, and is covered by
// tests/ppu_memory_tests.cpp rather than by a ROM, for the reason below.
//
//   oam_read       OAM through $2003/$2004, including its read quirks
//   oam_stress     OAM under heavy access patterns
//   ppu_open_bus   what the open bus returns for write-only registers and
//                  unused bits, and how it decays
//
// All three are asserted to pass, unconditionally. ppu_open_bus needs the
// per-bit decay in PPU::decay_open_bus; oam_stress needs OAM correct under
// access patterns well beyond a $2003/$2004 round trip. Do NOT weaken these
// assertions to accommodate a regression in either.
//
// These three are the ones that report through $6000. Blargg's 2005 PPU suite -
// palette_ram, vram_access, sprite_ram - covers the palette and $2007-buffer
// behaviour instead, and predates that protocol: those ROMs draw their results
// rather than writing them, so pointed at a $6000 harness they report "timed
// out without initialising" however correct the emulator is, which is worse
// than no coverage because it is coverage that lies. They are read off the
// nametable by tests/blargg_ppu_2005_tests.cpp, through
// tests/nametable_screen.h.
//
// Palette mirroring, nametable mirroring and the $2007 read buffer are ALSO
// pinned by unit tests in tests/ppu_memory_tests.cpp, which is the finer
// instrument: a ROM says which subtest failed, a unit test says which address.
#include <string>

#include "blargg_rom_harness.h"
#include "gtest/gtest.h"

namespace tests
{
namespace blargg
{
namespace ppu_address_space
{

// The cap is a hang detector, nothing else. Measured frame at which each ROM
// writes its final status:
//
//   oam_read         2
//   ppu_open_bus    22
//   oam_stress    1703   <- writes and reads all 256 OAM bytes many times over
//
// I have now set this cap too low three times on this codebase - 60 frames for
// ppu_vbl_nmi, 300 for cpu_interrupts_v2, and 400 here - and each time a
// PASSING ROM was reported as a hang, which reads as a broken emulator. So:
// measure the slowest ROM, then leave real headroom. A wrong number here does
// not merely lose coverage, it actively misdirects.
constexpr uint64_t kMaxFrames = 2500;

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/ppu_address_space/" + name + ".nes";
}

class PpuAddressSpaceRoms : public ::testing::TestWithParam<std::string>
{
};

TEST_P(PpuAddressSpaceRoms, reports_pass)
{
    const std::string name = GetParam();
    const RomResult result = run_rom(rom_path(name), kMaxFrames);

    if (!result.completed && !result.needs_reset) {
        if (!result.saw_signature) {
            FAIL() << name << ": timed out after " << result.frames_run
                   << " frames without writing the $DE $B0 $61 signature.\n"
                      "  The ROM did not get far enough to initialise - suspect the CPU,\n"
                      "  the cartridge mapping, or the $6000-$7FFF PRG-RAM window, not the\n"
                      "  PPU. CPU ran "
                   << result.cpu_cycles << " cycles, ending at PC=$" << std::hex << result.final_pc << std::dec << ".";
        }

        FAIL() << name << ": timed out after " << result.frames_run << " frames still reporting status $" << std::hex
               << static_cast<int>(result.last_status) << std::dec
               << " (running).\n"
                  "  The ROM initialised and is waiting on the PPU. All three of these\n"
                  "  passed when last measured, so this is a REGRESSION, not a missing\n"
                  "  feature - suspect OAM under stress, per-bit open-bus decay, or the\n"
                  "  rendering pipeline, in that order.";
    }

    if (result.needs_reset) {
        FAIL() << name << ": ROM asked for more than " << kMaxResets
               << " soft resets. The harness does drive them, so this is a ROM stuck\n"
                  "  in a reset loop rather than one waiting on a feature.";
    }

    EXPECT_EQ(0, result.status) << name << " failed with code " << static_cast<int>(result.status) << ":\n  "
                                << result.message;
}

INSTANTIATE_TEST_SUITE_P(PpuAddressSpace,
                         PpuAddressSpaceRoms,
                         ::testing::Values("oam_read", "oam_stress", "ppu_open_bus"),
                         [](const ::testing::TestParamInfo<std::string>& info) { return info.param; });

// Guards the harness rather than the emulator: if the ROMs are missing or the
// PRG-RAM window regresses, every test above fails identically and it is not
// obvious which of the two happened.
GTEST_TEST(ppuAddressSpaceHarness, roms_present_and_prg_ram_readable)
{
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom_path("oam_read")))
        << "PPU address-space ROMs absent - run tests/test_files/fetch_ppu_address_space.sh";

    console.write(kStatusAddr, kStatusRunning);
    console.write(kSignatureAddr, kSignature[0]);
    console.write(static_cast<uint16_t>(kSignatureAddr + 1), kSignature[1]);
    console.write(static_cast<uint16_t>(kSignatureAddr + 2), kSignature[2]);
    EXPECT_EQ(kStatusRunning, console.read(kStatusAddr));
    EXPECT_TRUE(signature_present(console));

    EXPECT_EQ(MapperId::nrom, console.rom.mapper_id);
}

}  // namespace ppu_address_space
}  // namespace blargg
}  // namespace tests
