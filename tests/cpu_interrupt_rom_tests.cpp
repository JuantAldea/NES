// Blargg cpu_interrupts_v2 test-ROM harness.
//
// The oracle for CPU interrupt behaviour, and currently the ONLY one: the
// SingleStepTests vectors carry no interrupts at all (each case is a single
// instruction from a fixed state), and the ppu_vbl_nmi ROMs only ever reach
// NMI. Everything these cover is otherwise unverified.
//
//   1-cli_latency        CLI/SEI/PLP take effect one instruction late
//   2-nmi_and_brk        an NMI arriving during a BRK sequence hijacks its
//                        vector fetch
//   3-nmi_and_irq        NMI and IRQ arriving together, and during each other
//   4-irq_and_dma        an IRQ asserted while OAM DMA holds the bus
//   5-branch_delays_irq  a taken branch polls earlier than the uniform
//                        penultimate-cycle rule
//
// SOME OF THESE ARE EXPECTED TO FAIL. They were fetched deliberately as the
// instrument for known-unfixed defects, before any attempt to fix them, so the
// count that passes is a scalar rather than a guess:
//   - IRQ is a one-shot latch with no lower_IRQ, asymmetric with the level
//     model /NMI now has. Nothing drives an IRQ line here yet.
//   - BRK/IRQ sequences are not hijacked by an NMI arriving before cycle 4.
//   - A page-crossing taken branch polls on its own penultimate cycle where
//     hardware polls on cycle 2.
//
// Do NOT weaken these assertions. A ROM going from fail to pass is progress; a
// ROM going the other way is a regression.
#include <string>

#include "gtest/gtest.h"

#include "blargg_rom_harness.h"

namespace tests
{
namespace blargg
{
namespace interrupts
{

// These ROMs settle much faster than the PPU ones, but the cap is generous for
// the same reason: a genuinely stuck CPU should fail with a diagnosis rather
// than hang the suite.
constexpr uint64_t kMaxFrames = 300;

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/cpu_interrupts/" + name + ".nes";
}

class CpuInterruptRoms : public ::testing::TestWithParam<std::string>
{
};

TEST_P(CpuInterruptRoms, reports_pass)
{
    const std::string name = GetParam();
    const RomResult result = run_rom(rom_path(name), kMaxFrames);

    if (!result.completed && !result.needs_reset) {
        if (!result.saw_signature) {
            FAIL() << name << ": timed out after " << result.frames_run
                   << " frames without writing the $DE $B0 $61 signature.\n"
                      "  The ROM did not get far enough to initialise - suspect the CPU,\n"
                      "  the cartridge mapping, or the $6000-$7FFF PRG-RAM window. CPU ran "
                   << result.cpu_cycles << " cycles, ending at PC=$" << std::hex << result.final_pc << std::dec << ".";
        }

        FAIL() << name << ": timed out after " << result.frames_run << " frames still reporting status $" << std::hex
               << static_cast<int>(result.last_status) << std::dec
               << " (running).\n"
                  "  The ROM initialised and is waiting on an interrupt that never\n"
                  "  arrives. The most likely cause is that nothing drives an IRQ line:\n"
                  "  raise_IRQ is a one-shot latch with no lower_IRQ, and there is no\n"
                  "  APU frame counter, DMC or mapper to assert one.";
    }

    if (result.needs_reset) {
        FAIL() << name << ": ROM requested a soft reset, which this harness does not drive yet";
    }

    EXPECT_EQ(0, result.status) << name << " failed with code " << static_cast<int>(result.status) << ":\n  "
                               << result.message;
}

INSTANTIATE_TEST_SUITE_P(CpuInterruptsV2, CpuInterruptRoms,
                         ::testing::Values("1-cli_latency", "2-nmi_and_brk", "3-nmi_and_irq", "4-irq_and_dma",
                                           "5-branch_delays_irq"),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                             std::string sanitized = info.param;
                             for (char& c : sanitized) {
                                 if (!std::isalnum(static_cast<unsigned char>(c))) {
                                     c = '_';
                                 }
                             }
                             return sanitized;
                         });

// Guards the harness rather than the emulator: if the ROMs are missing or the
// PRG-RAM window regresses, every parameterized test above fails identically
// and it is not obvious which of the two happened.
GTEST_TEST(cpuInterruptHarness, roms_present_and_prg_ram_readable)
{
    Bus console;
    ASSERT_TRUE(console.load_cartridge(rom_path("1-cli_latency")))
        << "cpu_interrupts_v2 ROMs absent - run tests/test_files/fetch_cpu_interrupts.sh";

    console.write(kStatusAddr, kStatusRunning);
    console.write(kSignatureAddr, kSignature[0]);
    console.write(static_cast<uint16_t>(kSignatureAddr + 1), kSignature[1]);
    console.write(static_cast<uint16_t>(kSignatureAddr + 2), kSignature[2]);
    EXPECT_EQ(kStatusRunning, console.read(kStatusAddr));
    EXPECT_TRUE(signature_present(console));

    EXPECT_EQ(32768u, console.rom.prg_rom.size());
    EXPECT_EQ(0, console.rom.mapper_id);
}

}  // namespace interrupts
}  // namespace blargg
}  // namespace tests
