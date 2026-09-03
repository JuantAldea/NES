// What DMC DMA does to the CPU, and the one row still wrong.
//
// THE AUTHORITY IS tests/test_files/fetch_dmc_dma.sh, not this file. That header
// carries Mesen2 b9fa69d's reference table for both ROMs, read out with
// tools/run_mesen_dump.sh --screen, and the eliminations it lists are not
// repeated here.
//
//   sprdma_and_dmc_dma       ROW 05 ONLY. We print 528, the reference 526: the
//                            outside->inside crossing is one iteration late.
//   sprdma_and_dmc_dma_512   the harder case, and where the work is. Mesen emits
//                            524 at row 04, a value we never produce at all.
//
// FIFTEEN MATCHING ROWS IS WEAK EVIDENCE. Rows 00-04 are all "outside" and 06-0F
// all "inside", so within each regime the printed value does not depend on the
// sweep offset. Only row 05, the transition, is sensitive to it - an
// implementation can be wrong throughout and match everywhere but the boundary.
//
// --- the hardware model, from documentation ------------------------------
//
// The DMC's memory reader halts the CPU for a halt cycle, an always-present
// dummy, an optional alignment cycle, and the get that performs the read.
//
//   3 OR 4 CYCLES IS NOT A RULE OF ITS OWN - it falls out of the PHASE the halt
//   lands on. A load halts on a GET, so its dummy lands on a put and the next
//   get needs no alignment: three. A reload halts on a PUT, so its dummy lands
//   on a get, one alignment cycle is spent, then the read: four. Loads are
//   additionally scheduled for the 2nd APU cycle after the $4015 write.
//
//   A HALT ON A WRITE CYCLE IS DEFERRED, AND THAT IS ALSO WHY IT CAN COST LESS.
//   Both halves matter and this file has previously asserted each of them alone.
//
//   The deferral is the mechanism, quoted from the wiki's raw source: "The CPU
//   only allows this on read cycles. If the CPU is writing, it ignores the halt
//   and the DMA unit waits until the next cycle to try again, repeating until
//   successful. Delays of up to 3 cycles are possible, with read-modify-write
//   instructions having 2 consecutive writes and interrupts having 3."
//   Mesen2 implements exactly this structurally - ProcessPendingDma is called
//   only from NesCpu::MemoryRead, never from the write path, so the halt cannot
//   fire on a write at all.
//
//   The 3-versus-4 outcome is a CONSEQUENCE of that deferral, not a rule beside
//   it: get and put alternate, so delaying the halt flips which phase the dummy
//   cycle lands on, which decides whether the alignment cycle is spent. Same
//   page: "load DMAs take 3 cycles and reload DMAs take 4 unless the halt is
//   delayed by an odd number of cycles".
//
//   So "a write makes it cheaper" is a true observation with the wrong cause
//   attached, and stating it as an independent mechanism is what sent earlier
//   attempts here wrong in both directions.
//
//   UNVERIFIED: the Fiskbit forum wording this file used to quote - "DMC DMAs
//   appear to try to halt during the 'put' phase" and "alignment can be skipped"
//   - was not found verbatim on a search of the relevant threads. Fiskbit's
//   located statement is narrower and consistent with the above: "DMA landing on
//   that write will take 3 cycles instead of 4" as a parity effect.
//
//   COLLISION IS NOT A FLAT 2. NESdev and AprNes both give 2 in the middle of an
//   OAM DMA, 1 at the second-to-last put, and 3 at the last - where it extends
//   past the end of the transfer. A flat 2 is implemented here. Fiskbit gives
//   the mechanism: OAM DMA keeps running through halt/dummy/alignment and has no
//   cost there, is suspended 1 cycle for the DMC read, then spends 1 realigning.
//
//   THE TRANSFER START DELAY IS IMPLEMENTED, and this file said for a long time
//   that it was not - "ours starts the instant dmc_wants_sample_byte() goes
//   true", offered as the leading explanation for the residual. It is at
//   src/apu.cpp, `dmc.transfer_start_delay = (apu_cycles % 2 == 0) ? 2 : 3`,
//   which is Mesen2's _transferStartDelay rule parity for parity, counted down
//   in clock_dmc.
//
//   IT APPLIES TO LOADS ONLY, and that is correct rather than an omission. A
//   load is scheduled by the $4015 write and starts 2 or 3 cycles later; a
//   reload is requested the instant the sample buffer empties. The wiki's
//   "Reload DMA (delayed 1/2/3 cycles)" listings are not a scheduled delay at
//   all - they are the write deferral, the halt being refused on consecutive
//   CPU writes. Adding a scheduled delay to reloads would be modelling the same
//   cycles twice.
//
//   NOT IMPLEMENTED, and not yet needed by this ROM: a halt during the FIRST
//   write of a read-modify-write still costs 4, and across the three writes of
//   an interrupt it costs 3, 4, 3.
//
//   PHANTOM READS are not modelled: while halted the CPU repeatedly reads
//   whatever address is on the bus, $4016/$4017 only on the halt cycle and other
//   registers on every no-op cycle. AprNes names this as the cause of
//   dma_4016_read, one of the three ROMs that draw nothing here.
//
// --- eliminated, with how ------------------------------------------------
//
// Each of these was measured, not argued. Redoing one is a day.
//
//   the write-cycle rule   whichever way it resolves, it cannot move row 05.
//                          Instrumented every DMC acceptance over 200 frames:
//                          the write gate fires ONCE in 349, and at none of the
//                          four boundary rows. Worth knowing before spending a
//                          day on the deferred-versus-cheaper question, which
//                          two files here answered oppositely.
//   the collision latch    Bus::advance_dmc_dma samples ppu.dma_in_progress()
//                          once, at the Idle transition, and never revisits it -
//                          so an OAM DMA starting DURING the halt window would
//                          be charged as standalone. It does not happen: at row
//                          05 the DMC ends at cycle 2064796 and the OAM DMA
//                          starts at 2064797. They miss by one.
//   load/reload at the     all four boundary acceptances are reloads at cost 4,
//   boundary               uniformly. A misclassification would have supplied
//                          exactly the missing cycle, and it is not there.
//   fetch count            counted end to end from the $4010 = $00 that opens
//                          the timed section: exactly 1 per iteration, walking
//                          from before the OAM DMA to inside it at iteration 6.
//   cost per DMA           47 cycles stolen across 12 counted fetches, which is
//                          3 + 11 x 4 - one load and eleven reloads, at the
//                          documented costs.
//   byte period            3424 CPU cycles between consecutive fetches, twelve
//                          in a row: the ROM's own dmc_timer_modulo, 8 x 428 at
//                          rate index 0.
//   buffer occupancy       every $4015 enable over 200 frames: 2335 full with no
//                          bytes remaining (reload), 32 empty (load), 32 full
//                          with bytes (no restart), 0 empty with bytes. Pinned
//                          by tests/dmc_buffer_tests.cpp.
//   load/reload split      implemented; the table is byte-identical. 32 loads in
//                          2367 enables is 1.3%, worth at most a cycle in a
//                          section containing one fetch.
//   period-at-next-reload  making a $4010 write reload the timer immediately
//                          instead sends row 00 from 527 to 125.
//   cycles actually lost   517 with a 513-cycle OAM DMA, 515 when the DMC
//                          collides - 4 and 2, as documented.
//   cost swept 1-6         with the collision cost swept 0-3 alongside. Nothing
//                          passes. 4 gives the right shape and wrong offset; 1
//                          gives a flat 514 with no alternation and no step; 3,
//                          5 and 6 hang the ROM; 2 reproduces the original 783.
//
// SWEEPING CONSTANTS UNTIL A ROM PASSES IS THE FAILURE MODE THIS REPOSITORY
// EXISTS TO PREVENT. It cannot distinguish a lucky number from a correct one.
// The 4-cycle version is the one the documentation and the shape both support;
// what is left needs an explanation, not a fitted parameter.
//
// --- where row 05 actually diverges --------------------------------------
//
// Traced on both emulators and aligned on the INSTRUCTION STREAM. Never on cycle
// numbers: Mesen's counter has no fixed origin, because the emulator is already
// running on its own thread when the debugger attaches and the first Step pauses
// it wherever the scheduler left it. Measured across three identical runs: 27279,
// 27279, 14914.
//
// So an offset between the two counters measures the scheduler. Comparing two
// separately-captured traces by cycle number produced a "34182-cycle drift"
// between sweep rows and an argument from offset parity that this emulator's
// get/put labelling is inverted; both were artefacts of two processes starting
// at different random points, and both are withdrawn. See the origin note in
// tools/mesen_cycle_trace.cpp.
//
// Everything below is a within-trace comparison and needs no alignment.
//
// THE TWO CPUs ARE CYCLE-IDENTICAL INTO THE STORE. Sixteen cycles from $E3AE to
// $E503, with $E3AF and $E3B0 each held 4, on both sides:
//
//   ours    $E3AE 2064776 ... $E503 2064792
//   Mesen   $E3AE 2244095 ... $E503 2244111
//
// THEY DIVERGE AT $E503, WHICH IS THE STORE. We insert a 4-cycle DMC halt there
// and then start the OAM DMA at 2064797, so the DMC is entirely OUTSIDE the
// transfer: 528. Mesen has no DMC halt at $E503 at all - it goes straight into
// the long freeze, so its DMC lands INSIDE the transfer: 526.
//
// SO THE DMC FIRES EARLY RELATIVE TO THE CPU, and it is a PHASE error, not a
// rate one - the 3424-cycle byte period is measured exact above. One cycle the
// other way and the order inverts: the store completes, the OAM DMA begins, and
// the halt lands inside it. That is the whole of the -5 -> +2 discontinuity and
// the whole of 528 against 526.
//
// WHAT IS LEFT IS THE OUTPUT UNIT, not the DMA. A reload is requested the
// instant the sample buffer empties, so the reload's timing is set by when the
// output unit empties it - and every part of the DMA machinery downstream of
// that request is now measured correct. That is a different subsystem from
// anything eliminated above, and it is where the next attempt should start.
//
// --- traps ---------------------------------------------------------------
//
// DO NOT RECOVER THE EXPECTED TABLE FROM THE CRC. The sixteen values are three
// ASCII digits each, so the message length is fixed and CRC-32 is linear over
// it - which makes a meet-in-the-middle search look tractable. It is a trap:
// allowing each row a delta of 0-5 gives 6^16 candidates against a 32-bit
// constraint, so ~657 tables satisfy the target. A match carries almost no
// information - the first one found required a DMC DMA to cost nothing at all on
// four consecutive rows.
//
// AND THE PRINTED 8-HEX VALUE IS NOT THE CHECKSUM check_crc COMPARES. The CRC
// model here is validated to the byte - its running value tracks the ROM's own
// zero-page checksum with zero divergence across all 4203 updates and reproduces
// the end state exactly, EB161E94 both ways - and with that established, every
// checksum state the ROM passes through was recorded. The printed value appears
// in none of them. What it is remains open.
//
// Reusable if anyone returns to it: update_crc is at $E788, found by searching
// PRG for the reflected polynomial's own bytes, EOR #$ED and EOR #$B8. Its guard
// is "bit checksum_off_ / bmi" with checksum_off_ at zero page $17, and $E78C is
// the entry past that guard - callers use $E78C almost exclusively, 4207 bytes
// against 2.
//
// double_2007_read IS NOT A DMC PROBLEM. Its CRC D84F6815 is the value AprNes
// reported before fixing a PPU bug: a missing ~6-dot cooldown after a $2007
// read, during which a second read returns open bus without swapping the buffer
// or incrementing the VRAM address.
//
// --- claims to distrust --------------------------------------------------
//
//   "the get cycle is the odd half of cpu_cycles"   Inferred from sync_dmc
//     converging with one phase and hanging with the other - with it inverted,
//     its 433-cycle loop runs exactly 432 against a 432-cycle sample, so the
//     one-cycle-per-iteration creep it relies on never happens. Strong evidence
//     of a phase relationship, but it is OUR convention fitted to the ROM, not a
//     documented mapping.
//
//   "the checksum starts at $FFFFFFFF"   Observed once, before the first
//     update_crc call. May equally be uninitialised RAM; reset_crc as published
//     sets zero.
//
// An earlier figure here read "exactly 4.00 cycles, 48 stolen across 12
// fetches". The probe behind it computed the fetch count as stolen/4 and then
// printed a hard-coded 4.00 - it could not have produced another answer. The
// corrected 47 over 12 is better evidence, because it distinguishes the two
// kinds of DMA.
//
// read_write_2007 already passes and is asserted to keep passing: a stall
// implementation that breaks what already works should not be able to hide
// behind the ROM it was written to fix.
// ---------------------------------------------------------------------------
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nametable_screen.h"
#include "rom_fixture.h"

namespace tests
{
namespace dmc_dma
{
namespace
{

std::string rom_path(const std::string& name) { return std::string(NES_TEST_FILES_DIR) + "/dmc_dma/" + name + ".nes"; }

constexpr const char* kFetch = "run tests/test_files/fetch_dmc_dma.sh";

// Measured: read_write_2007 settles at frame 14, the sprdma pair at 157. 900 is
// far above that on purpose - these are a FLOOR and rise as ROMs get further.
constexpr uint64_t kMaxFrames = 900;
constexpr uint64_t kSettleFrames = 90;

#define REQUIRE_DMC_DMA_ROM(name) REQUIRE_ROM(rom_path(name), kFetch)

// Runs until the whole screen stops changing. The whole screen, not the first
// row: these ROMs print a table and then a verdict underneath it, so a reader
// watching one row would stop while the table was still filling.
std::string run_until_settled(const std::string& name)
{
    Bus console;
    EXPECT_TRUE(console.load_cartridge(rom_path(name))) << "the ROM is present but did not load: " << rom_path(name);
    console.cpu.reset();

    std::string last;
    uint64_t stable = 0;
    for (uint64_t frame = 0; frame < kMaxFrames; ++frame) {
        nametable_screen::run_one_frame(console);

        std::string text = nametable_screen::read_text(console);
        if (text == last) {
            if (++stable >= kSettleFrames) {
                break;
            }
            continue;
        }
        stable = 0;
        last = std::move(text);
    }
    return last;
}

bool screen_says(const std::string& screen, const std::string& word) { return screen.find(word) != std::string::npos; }

// The sixteen "NN NNN" rows of the table, as integers. Returns what it found
// rather than asserting, so the caller can report the whole screen when the row
// count is wrong - a ROM that printed nothing and one that printed a bad table
// are different failures.
std::vector<int> table_lengths(const std::string& screen)
{
    std::vector<int> out;
    std::istringstream lines(screen);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string index;
        int length = 0;
        // "0A 513" - a two-digit hex index then the decimal length.
        if ((fields >> index >> length) && index.size() == 2 && std::isxdigit(static_cast<unsigned char>(index[0])) &&
            std::isxdigit(static_cast<unsigned char>(index[1]))) {
            out.push_back(length);
        }
    }
    return out;
}

}  // namespace

// --- what already works ------------------------------------------------------

GTEST_TEST(dmcDma, a_2007_read_and_write_around_a_dma_still_pass)
{
    REQUIRE_DMC_DMA_ROM("read_write_2007");

    const std::string screen = run_until_settled("read_write_2007");

    EXPECT_TRUE(screen_says(screen, "Passed")) << "read_write_2007 passed before the DMC stall existed and must keep\n"
                                                  "  passing after it. Full screen:\n"
                                               << screen;
}

// --- the queue ---------------------------------------------------------------

// Pinned to its current failure, like every other unfinished feature here. When
// the stall lands this fails, and the message says to promote it.
class SprdmaAndDmcDma : public ::testing::TestWithParam<const char*>
{
};

TEST_P(SprdmaAndDmcDma, is_blocked_on_the_cpu_stall)
{
    const std::string name = GetParam();
    REQUIRE_DMC_DMA_ROM(name);

    const std::string screen = run_until_settled(name);

    ASSERT_TRUE(screen_says(screen, "Failed") || screen_says(screen, "Passed"))
        << name
        << " reached no verdict at all, which is neither the recorded failure nor a\n"
           "  pass. Full screen:\n"
        << screen;

    EXPECT_TRUE(screen_says(screen, "Failed"))
        << name
        << " now PASSES. The DMC's CPU stall must be implemented: delete this pin\n"
           "  and assert the pass instead. Full screen:\n"
        << screen;

    // The failure is pinned to its CAUSE, not just its verdict: every row of the
    // table is asserted at the value this emulator currently produces.
    //
    // WHY EXACT VALUES RATHER THAN "512 OR 513". This pin used to require every
    // row to be a plain uninterrupted OAM DMA length, on the reasoning that the
    // DMC's CPU stall was absent. It is not absent - it is present and
    // imperfect, so every row reads 525-528, and the pin failed on all sixteen.
    //
    // That failure was left standing, and it turned CI red on every push for six
    // weeks. A permanently-red pipeline is worse than none: it hides the next
    // real regression behind a failure everyone has learned to scroll past,
    // which is the same false signal this project rejects in the other
    // direction. Pinning the measured values is what the rest of this repository
    // already does with a known divergence - opcode $AB in instr_test_roms.cpp
    // asserts blargg's ROM fails on exactly ATX, and 6-MMC3_alt is asserted to
    // fail on exactly its subtest. Neither is "ignored"; both are nailed down so
    // that any MOVEMENT surfaces.
    //
    // So these tables are the divergence, recorded. Hardware would give 512 or
    // 513 with no DMC interference and a correct stall gives documented longer
    // figures; this gives neither. Any change to the DMC's timing shifts a row
    // and fails here with the row named, which is the signal worth having.
    //
    // Each row is parsed, rather than searching the whole screen for "512" and
    // "513" as an earlier version did. That search passed on any screen
    // containing one row of each - a table of fourteen 783s with a single 512
    // and 513 satisfied it - and let a wrong stall through undetected in an
    // adversarial review.
    //
    // MEASURED 2026-08-29. Delete the whole pin when the stall is implemented
    // properly, and assert the pass instead; do not "update" these numbers to
    // make a red run green without understanding which row moved and why.
    static const std::map<std::string, std::vector<int>> kMeasured = {
        {"sprdma_and_dmc_dma", {527, 528, 527, 528, 527, 528, 525, 526, 525, 526, 525, 526, 525, 526, 525, 526}},
        {"sprdma_and_dmc_dma_512", {525, 526, 525, 526, 525, 526, 525, 526, 527, 528, 527, 528, 527, 528, 527, 528}},
    };

    const std::vector<int> lengths = table_lengths(screen);
    ASSERT_EQ(16u, lengths.size()) << name << ": expected 16 table rows, parsed " << lengths.size()
                                   << ". Full screen:\n"
                                   << screen;

    const auto expected = kMeasured.find(name);
    ASSERT_NE(kMeasured.end(), expected) << name << " has no recorded table; add one rather than skipping the check";

    for (size_t row = 0; row < lengths.size(); ++row) {
        EXPECT_EQ(expected->second[row], lengths[row])
            << name << " row " << row << " reads " << lengths[row] << ", not the recorded " << expected->second[row]
            << ".\n"
               "  This is the parked DMC/OAM collision divergence, and a row moving means the\n"
               "  DMC's timing changed. Hardware gives 512 or 513 with no interference; if the\n"
               "  stall is now correct, delete this pin and assert the ROM's pass instead.\n"
               "  Full screen:\n"
            << screen;
    }
}

INSTANTIATE_TEST_SUITE_P(DmcDma,
                         SprdmaAndDmcDma,
                         ::testing::Values("sprdma_and_dmc_dma", "sprdma_and_dmc_dma_512"),
                         [](const ::testing::TestParamInfo<const char*>& info) { return std::string(info.param); });

}  // namespace dmc_dma
}  // namespace tests
