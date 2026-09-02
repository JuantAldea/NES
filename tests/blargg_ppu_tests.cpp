// Blargg's ppu_vbl_nmi test ROMs, run through the shared $6000 harness.
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
// Four properties these ROMs pin down, every one of which has to be right
// before most of them report a pass at all:
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
// What these failures are NOT is CPU::clock applying an instruction's whole
// memory effect on its last cycle. That is the plausible diagnosis and it is
// wrong: the core is cycle-stepped at one bus access per cycle, and has been
// throughout. The four properties above are the gap.
#include <cstdint>
#include <string>

#include "../include/bus.h"
#include "blargg_rom_harness.h"
#include "gtest/gtest.h"
#include "rom_fixture.h"

namespace tests
{
namespace blargg
{
namespace ppu_vbl_nmi
{
namespace
{

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

constexpr const char* kFetch = "run tests/test_files/fetch_blargg_ppu.sh";

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/blargg_ppu/" + name + ".nes";
}

RomResult run(const std::string& name) { return run_rom(rom_path(name), kMaxFrames); }

}  // namespace

class BlarggPpuVblNmi : public ::testing::TestWithParam<std::string>
{
};

TEST_P(BlarggPpuVblNmi, reports_pass)
{
    const std::string name = GetParam();
    REQUIRE_ROM(rom_path(name), kFetch);

    const RomResult result = run(name);

    if (!result.completed && !result.needs_reset) {
        // Distinguish the two very different reasons for a timeout, because
        // they point at opposite ends of the emulator.
        if (!result.saw_signature) {
            FAIL() << name << ": timed out after " << result.frames_run
                   << " frames without ever writing the $DE $B0 $61 signature.\n"
                      "  The ROM did not get far enough to initialise. Suspect the CPU,\n"
                      "  the cartridge mapping, or the $6000-$7FFF PRG-RAM window -- not\n"
                      "  the PPU. CPU ran "
                   << result.cpu_cycles << " cycles, ending at PC=$" << std::hex << result.final_pc << std::dec << ".";
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
                  "  clears at (261,1) once per frame, so suspect the CPU/PPU timing\n"
                  "  RELATIONSHIP instead - /NMI held as a level, the interrupt poll on\n"
                  "  the penultimate cycle, the one-dot gap between a bus access and that\n"
                  "  poll, or the odd-frame skip sampling a dot early. Those four are what\n"
                  "  these ROMs measure, to single-cycle resolution.";
    }

    // Reachable only if a ROM asks for more resets than the shared harness will
    // drive. None of these ten asks even once - it is cpu_reset and apu_reset
    // that do - so this firing means something is repeating the request.
    if (result.needs_reset) {
        FAIL() << name << ": asked for more than " << kMaxResets << " soft resets, which is a defect rather than a\n"
               << "  verdict. Last message:\n  " << result.message;
    }

    EXPECT_EQ(0, result.status) << name << " failed with code " << static_cast<int>(result.status) << ":\n  "
                                << result.message;
}

INSTANTIATE_TEST_SUITE_P(PpuVblNmi,
                         BlarggPpuVblNmi,
                         ::testing::Values("01-vbl_basics",
                                           "02-vbl_set_time",
                                           "03-vbl_clear_time",
                                           "04-nmi_control",
                                           "05-nmi_timing",
                                           "06-suppression",
                                           "07-nmi_on_timing",
                                           "08-nmi_off_timing",
                                           "09-even_odd_frames",
                                           "10-even_odd_timing"),
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
    EXPECT_EQ(MapperId::nrom, console.rom.mapper_id);
}

}  // namespace ppu_vbl_nmi
}  // namespace blargg
}  // namespace tests
