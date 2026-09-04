// What DMC DMA does to the CPU. All seven ROMs here are on hardware's values.
//
// THE AUTHORITY IS tests/test_files/fetch_dmc_dma.sh, not this file. That header
// carries Mesen2 b9fa69d's reference table for both ROMs, read out with
// tools/run_mesen_dump.sh --screen, and the eliminations it lists are not
// repeated here.
//
//   sprdma_and_dmc_dma       check_crc $FBADA48D
//   sprdma_and_dmc_dma_512   check_crc $F1A58F55
//   dma_4016_read            check_crc $F0AB808C
//   dma_2007_write           prints its own name and Passed
//
// Those CRCs are the ROMs' own, so the values pinned below are hardware's rather
// than a match against another emulator. dma_2007_read only PRINTS its CRC, so
// it cannot say Passed; its 5E3DF9C4 is one of the two AprNes catalogues, and
// double_2007_read's 85CFD627 is one of that catalogue's four.
//
// MATCHING ROWS ARE WEAK EVIDENCE, which is why this file argues from mechanism
// rather than from how many rows agree. Within a regime the printed value does
// not depend on the sweep offset, so only a row at a TRANSITION can see an error
// - an implementation can be wrong throughout and match everywhere but the
// boundary. sprdma_and_dmc_dma has exactly one such row out of sixteen, so an
// implementation matching the other fifteen says almost nothing.
//
// WHAT THE SWEEP VARIES, from blargg's own source (sprdma.s and dma_timing.inc in
// nes-test-roms). The row index n is split across delay_a_25_clocks(255-n) before
// the timed block and delay_a_25_clocks(n) after, so the measured span is fixed
// while the DMC's free-running fetch - period 3424 = 428*8 - slides 25 CPU cycles
// per row against the `sta $4014` that starts the OAM DMA. 25 is ODD, which is the
// entire reason consecutive rows alternate: every row flips get/put parity.
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
//   Both halves matter; either one alone is wrong.
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
//   attached. Stating it as an independent mechanism, rather than as a
//   consequence of the deferral, gets the timing wrong in both directions.
//
//   UNVERIFIED, AND WIDELY REPEATED: "DMC DMAs appear to try to halt during the
//   'put' phase" and "alignment can be skipped" are not in the Fiskbit threads
//   they are attributed to. His located statement is narrower and consistent
//   with the above: "DMA landing on that write will take 3 cycles instead of 4",
//   a parity effect.
//
//   COLLISION IS NOT A FLAT 2. NESdev and AprNes both give 2 in the middle of an
//   OAM DMA, 1 at the second-to-last put, and 3 at the last - where it extends
//   past the end of the transfer. Fiskbit gives the mechanism: OAM DMA keeps
//   running through halt/dummy/alignment and has no cost there, is suspended 1
//   cycle for the DMC read, then spends 1 realigning.
//
//   THIS IS WHAT sprdma_and_dmc_dma_512 SWEEPS, and why the other ROM cannot see
//   it. Logging our fetch cycle against each OAM transfer shows ONE fetch walking
//   out through the tail, +1 a row, not the several a first reading of the table
//   suggests:
//
//     rows   fetch lands       what that position is
//     00-03  inside, end-5/-3  mid-transfer, costs 2
//     04-05  inside, end-1     the second-to-last put: costs 1
//     06-07  +1 past the end   the last: costs 3, extends past it
//     08-09  +5 past the end   clear of the transfer
//     0A-0B  +7 past the end   clear of it too - the halt meets `sta $100`
//     0C-0F  +9/+11 past end   clear of the transfer
//
//   Acceptance lands on remaining_dma_cycles 7, 5, 3 and 1 at rows 00-07 and on 0
//   for the other 325 fetches in the run, so 3 and 1 are the two positions the
//   rule names and the cost is keyed on them rather than swept.
//
//   NOT IMPLEMENTED, and not yet needed by these ROMs: a halt during the FIRST
//   write of a read-modify-write still costs 4, and across the three writes of
//   an interrupt it costs 3, 4, 3.
//
//   PHANTOM READS are modelled - the repeats follow the access the CPU is
//   ATTEMPTING. See the dmc_dma_during_read4 section below.
//
// --- rows 0A and 0B: the two waits were one deferral ----------------------
//
// Serving the write refusal and the phase wait in sequence makes rows 0A and 0B
// of the _512 ROM read 527 and 528 against hardware's 526 and 527, leaving the
// other fourteen correct.
//
// LOCALIZED FIRST, MECHANISM SECOND. The fetch sits at +5 past the transfer at
// rows 08-09, +7 at 0A-0B and +9 at 0C-0D. After the transfer the CPU resumes
// into the `sta $100` that blargg places right after `sta $4014` - absolute, four
// cycles, the write last - so the halt arrives BEFORE that write at 08-09, ON it
// at 0A-0B, and AFTER it at 0C-0D. Only the coinciding row diverges, with a
// correct neighbour either side.
//
// Measured from the transfer's last cycle to $E384, a PC visited exactly once a
// row, past both the store and the fetch:
//
//     row      08     0A     0C
//     ours    +61    +63    +65
//     mesen   +61    +62    +65
//
// Equal at both neighbours and one over at 0A, while the span from the timed
// section's start to the transfer's last cycle is a flat +1 across all three. So
// the extra cycle is downstream of the transfer, in the window `sta $100`
// occupies.
//
// THE WRITE REFUSAL AND THE PHASE WAIT ARE THE SAME ONE-CYCLE DEFERRAL. Serving
// both in sequence - refusing the halt for the store's write, then making the
// retry wait again for the right phase - spends two delays on one cause. The
// retry skips the phase wait.
//
// THE GATE ITSELF CANNOT BE DELETED INSTEAD. It is load-bearing three times
// over: it picks the DMA's length, it decides which cycles the write refusal is
// even reached on, and it is what puts acceptance on the ODD
// remaining_dma_cycles values the collision costs above are calibrated against.
// Removing it moves every row of both ROMs by +1 - all 32, uniformly - where a
// pure timing shift would be phase-dependent. A uniform shift is the signature
// of the calibration moving, not of a delay, and reading it as a delay points at
// a compensation elsewhere that does not exist.
//
// --- eliminated, with how ------------------------------------------------
//
// Each of these was measured, not argued. Redoing one is a day.
//
//   the parity gate as a    dropping it and supplying the delay elsewhere: as a
//   pure delay              flat one-cycle wait before accepting, and as a
//                           deferred reload in apu.cpp. BOTH HANG BOTH ROMS - no
//                           table at all. The gate is not a delay, so nothing
//                           that only delays can replace it.
//   a reload start delay    Mesen has NEITHER a parity gate NOR a reload delay:
//                           its reload path calls StartDmcTransfer() the instant
//                           the buffer empties, and _transferStartDelay applies
//                           only to the $4015 load. Splitting the fetch REQUEST
//                           out of the buffer-empty handling so only it is
//                           deferred is a no-op with the gate in place, because
//                           the gate absorbs it exactly.
//   the request raised      the account that a reference with no gate and no
//   one cycle early         delay implies our request is early. Refuted by the
//                           above: the compensation is not a delay at all.
//   the collision latch     Bus::advance_dmc_dma samples ppu.dma_in_progress()
//                           once, at the Idle transition, and never revisits it -
//                           so an OAM DMA starting DURING the halt window would
//                           be charged as standalone. It does not happen: at row
//                           05 the DMC ends at cycle 2064796 and the OAM DMA
//                           starts at 2064797. They miss by one.
//   load/reload at the      all four boundary acceptances are reloads at cost 4,
//   boundary                uniformly. A misclassification would have supplied
//                           exactly a missing cycle, and it is not there.
//   fetch count             counted end to end from the $4010 = $00 that opens
//                           the timed section: exactly 1 per iteration, walking
//                           from before the OAM DMA to inside it at iteration 6.
//   cost per DMA            47 cycles stolen across 12 counted fetches, which is
//                           3 + 11 x 4 - one load and eleven reloads, at the
//                           documented costs.
//   byte period             3424 CPU cycles between consecutive fetches, twelve
//                           in a row: the ROM's own dmc_timer_modulo, 8 x 428 at
//                           rate index 0.
//   buffer occupancy        every $4015 enable over 200 frames: 2335 full with no
//                           bytes remaining (reload), 32 empty (load), 32 full
//                           with bytes (no restart), 0 empty with bytes. Pinned
//                           by tests/dmc_buffer_tests.cpp.
//   load/reload split       implemented; the table is byte-identical. 32 loads in
//                           2367 enables is 1.3%, worth at most a cycle in a
//                           section containing one fetch.
//   period-at-next-reload   making a $4010 write reload the timer immediately
//                           instead sends row 00 from 527 to 125.
//   cycles actually lost    517 with a 513-cycle OAM DMA, 515 when the DMC
//                           collides - 4 and 2, as documented.
//   cost swept 1-6          with the collision cost swept 0-3 alongside. Nothing
//                           passes. 4 gives the right shape and wrong offset; 1
//                           gives a flat 514 with no alternation and no step; 3,
//                           5 and 6 hang the ROM; 2 reproduces the original 783.
//
// SWEEPING CONSTANTS UNTIL A ROM PASSES IS THE FAILURE MODE THIS REPOSITORY
// EXISTS TO PREVENT. It cannot distinguish a lucky number from a correct one.
// Every value above is keyed to a position the documentation names.
//
// --- how to measure against Mesen, if a row ever moves again --------------
//
// NEVER ALIGN ON CYCLE NUMBERS. Mesen's counter has no fixed origin, because the
// emulator is already running on its own thread when the debugger attaches and
// the first Step pauses it wherever the scheduler left it. Measured across three
// identical runs: 27279, 27279, 14914. So an offset between the two counters
// measures the scheduler. Comparing two separately-captured traces by cycle
// number produced a "34182-cycle drift" between sweep rows and an argument from
// offset parity that this emulator's get/put labelling is inverted; both were
// artefacts, and both are withdrawn. See tools/mesen_cycle_trace.cpp.
//
// tools/lockstep_compare.cpp links this emulator's static libraries AND Mesen's
// InteropDLL into one process, aligns them on CONTENT - a PC value plus a
// matching register file - and steps both one CPU cycle at a time. There is no
// offset arithmetic anywhere in it.
//
// MEASURE EACH SIDE AGAINST ITS OWN TRANSFER, and compare only derived offsets.
// A table here once read our fetch as a constant +1 behind Mesen's and concluded
// Mesen fetches one cycle later, always. But "our get" was DERIVED as the DMA
// start plus 3 while Mesen's was MEASURED from BytesRemaining - two different
// events, so the constant was the length of our own derivation. A CONSTANT OFFSET
// IS EVIDENCE ABOUT THE INSTRUMENT UNTIL PROVEN OTHERWISE. With both sides
// reading their own fetch, the fetches coincide.
//
// A TRACE CANNOT TELL A DMC STALL FROM AN OAM DMA. PC is frozen during ANY halt,
// so both look identical from outside; an earlier reading of "Mesen has no DMC
// halt at $E503 at all" was that mistake. Mesen's DMC fetch is visible only
// through ApuDmcState::BytesRemaining.
//
// DO NOT MEASURE A DMA STALL BY PC CONSTANCY OUTSIDE A TRANSFER. It works during
// one, where nothing else holds PC for hundreds of cycles, and fails after it: a
// store's write cycle holds PC too, so a "stall" containing the fetch reads as 9
// cycles where the DMA is 4 or 5. That figure is the detector, not the DMA.
//
// DO NOT PROBE IN A WINDOW THAT ENDS BEFORE THE SUSPECT. A prediction about the
// write deferral was tested against a span ending at the transfer's last cycle,
// but `sta $100` runs AFTER the transfer, so the window could not have shown the
// defect whatever the answer. Ask what the probe would report if the hypothesis
// were true BEFORE running it.
//
// DO NOT LOOK FOR A CYCLE IN THE GAP BETWEEN TRANSFERS. The interval from one
// transfer's last write to the next differs between the two emulators by hundreds
// to tens of thousands of cycles - 09->0A by -3340, 0A->0B by +33292 - because
// the ROM's code_timer spans only time_code_begin..time_code_end and everything
// outside it is free to diverge. Any finer probe has to be anchored INSIDE the
// timed section on each side separately.
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
// DELAYING A TRANSFER BY BORROWING transfer_start_delay NEEDS ITS KIND CARRIED.
// Only a $4015 load uses that countdown, so a hardcoded dmc_start_transfer(TRUE)
// in it reads as correct: a reload routed through it silently becomes a load, 3
// cycles instead of 4, and dmc_transfer_is_load() then answers the parity gate
// with the wrong value. dmc.delayed_transfer_is_load exists to keep that
// impossible.
//
// double_2007_read IS NOT A DMC PROBLEM - it sets up no DMA at all. `lda $20F7,x`
// with x=$10 crosses a page, so the CPU reads $2007 twice one cycle apart, and
// the second sees a buffer the first has not yet refilled. See
// PPU::kReadBufferRefillDots; fetch_dmc_dma.sh carries the rest.
//
// --- claims to distrust --------------------------------------------------
//
//   "the get cycle is the odd half of cpu_cycles"   STILL fitted rather than
//     documented, but no longer merely suspected: inverting the alignment
//     parity in the Dummy transition AND removing the gate together - the pair
//     that would expose an inverted labelling the gate was masking - hangs both
//     ROMs. So the labelling is not inverted, and the gate is not compensating
//     for one. Inferred from sync_dmc
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

// The printed screen with trailing blanks and empty lines removed, so a pin can
// be an exact string rather than a search. These ROMs print few enough lines
// that the whole screen IS the result, and matching it exactly is what makes a
// single changed digit fail loudly.
std::string screen_lines(const std::string& screen)
{
    std::string out;
    std::istringstream lines(screen);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
            line.pop_back();
        }
        const size_t first = line.find_first_not_of(' ');
        if (first == std::string::npos) {
            continue;
        }
        out += line.substr(first);
        out += '\n';
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

// --- the sweep ---------------------------------------------------------------

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

    // BOTH ROMS SELF-CHECK, with check_crc $FBADA48D and $F1A58F55 - values blargg
    // computed on hardware. An emulator producing a WRONG table that lands on the
    // right 32-bit CRC is a 2^-32 coincidence, so a pass means these thirty-two
    // values ARE the hardware's, not a match against another emulator.
    //
    // The table is pinned as well as the verdict, so a regression names the row
    // that moved instead of only reporting a failed CRC.
    //
    // WHY EXACT VALUES RATHER THAN "512 OR 513". This pin used to require every
    // row to be a plain uninterrupted OAM DMA length, on the reasoning that the
    // DMC's CPU stall was absent. It is not absent - it is present, so every row
    // reads 524-528, and the pin failed on all sixteen.
    //
    // That failure was left standing, and it turned CI red on every push for six
    // weeks. A permanently-red pipeline is worse than none: it hides the next
    // real regression behind a failure everyone has learned to scroll past,
    // which is the same false signal this project rejects in the other
    // direction.
    //
    // Each row is parsed, rather than searching the whole screen for "512" and
    // "513" as an earlier version did. That search passed on any screen
    // containing one row of each - a table of fourteen 783s with a single 512
    // and 513 satisfied it - and let a wrong stall through undetected in an
    // adversarial review.
    //
    // Do not "update" these numbers to make a red run green. They are hardware's:
    // a row moving means the emulator changed, not the pin.
    static const std::map<std::string, std::vector<int>> kExpected = {
        {"sprdma_and_dmc_dma", {527, 528, 527, 528, 527, 526, 525, 526, 525, 526, 525, 526, 525, 526, 525, 526}},
        {"sprdma_and_dmc_dma_512", {525, 526, 525, 526, 524, 525, 526, 527, 527, 528, 526, 527, 527, 528, 527, 528}},
    };

    const std::vector<int> lengths = table_lengths(screen);
    ASSERT_EQ(16u, lengths.size()) << name << ": expected 16 table rows, parsed " << lengths.size()
                                   << ". Full screen:\n"
                                   << screen;

    const auto expected = kExpected.find(name);
    ASSERT_NE(kExpected.end(), expected) << name << " has no recorded table; add one rather than skipping the check";

    EXPECT_TRUE(screen_says(screen, "Passed"))
        << name << " no longer passes its own CRC, which is a regression against hardware.\n"
        << "  Full screen:\n"
        << screen;

    for (size_t row = 0; row < lengths.size(); ++row) {
        EXPECT_EQ(expected->second[row], lengths[row])
            << name << " row " << row << " reads " << lengths[row] << ", not the expected " << expected->second[row]
            << ".\n"
               "  These are hardware's values, confirmed by the ROM's own CRC - a row moving\n"
               "  here is a regression in the DMC/OAM collision, not a divergence to record.\n"
               "  Full screen:\n"
            << screen;
    }
}

INSTANTIATE_TEST_SUITE_P(DmcDma,
                         SprdmaAndDmcDma,
                         ::testing::Values("sprdma_and_dmc_dma", "sprdma_and_dmc_dma_512"),
                         [](const ::testing::TestParamInfo<const char*>& info) { return std::string(info.param); });

// --- phantom reads: the last DMC DMA behaviour not implemented ---------------

// While the CPU is halted for a DMA it does not idle. NESdev's DMA page: "When
// RDY is deasserted, the 6502 core repeats the last read cycle indefinitely,
// making no forward progress nor handling interrupts. On 2A03 CPUs, these
// repeated reads are externally visible on any no-operation DMA cycle, causing
// data loss if reading a register with side effects." The no-operation cycles
// are the halt, the DMC's dummy, and the optional alignment.
//
// So a DMA landing on `lda $2007` makes the PPU see the read two or three extra
// times, each swapping the read buffer and stepping the VRAM address. Same for
// $2002 and $4015. The register that does NOT work this way is the controller:
// "Joypads are clocked via direct lines from the CPU, called joypad 1 /OE and
// joypad 2 /OE, rather than going over the address bus", which is why the count
// there is 0-4 rather than simply matching the no-op cycles.
//
// THESE FOUR ROMS WERE RECORDED AS DRAWING NOTHING, AND THEY DO NOT. That entry
// was measured without cpu.reset() after load_cartridge, so the CPU ran from
// $0000 - PC comes from $FFFC, which is open bus until a cartridge is present,
// and Bus's constructor resets before one is. The ROM then BRK-slid through the
// zeroed RAM and the $00 padding ahead of blargg's `.align 64` routines, two
// bytes at a time, into the IRQ handler at $E742 (`bit $4015` / `rti`) and back.
// The slide is what made them look hung: PC oscillating over a handful of
// addresses is exactly what a wait loop looks like from outside.
//
// The tell was that the last BRK before sync_dmc consumed its own padding byte
// AND $E040, so execution resumed at $E041, one byte into `lda #$80`, and
// `sta $4010` was never executed - the DMC timer period stayed 0, clock_dmc
// returned early forever, and $4015 bit 4 never cleared. A missing reset, not a
// missing feature.
class DmcDmaDuringRead : public ::testing::TestWithParam<const char*>
{
};

TEST_P(DmcDmaDuringRead, prints_what_phantom_reads_would_change)
{
    const std::string name = GetParam();
    REQUIRE_DMC_DMA_ROM(name);

    const std::string screen = screen_lines(run_until_settled(name));

    // PHANTOM READS, and all four ROMs now on hardware's values. Each expected
    // value comes from blargg's own source in
    // nes-test-roms/dmc_dma_during_read4/source/, which states the answer in a
    // comment at the top of each .s file.
    //
    // A DMA's halt, dummy and optional alignment cycles steal the bus without
    // using it, and the 6502 does not idle through them. NESdev's DMA page:
    // "When RDY is deasserted, the 6502 core repeats the last read cycle
    // indefinitely, making no forward progress nor handling interrupts. On 2A03
    // CPUs, these repeated reads are externally visible on any no-operation DMA
    // cycle, causing data loss if reading a register with side effects."
    //
    // IT IS THE ACCESS THE CPU IS ATTEMPTING THAT REPEATS, not the last one it
    // completed, and that distinction is the whole feature. Measured in Mesen
    // with tools/mesen_2007_trace.cpp, reading MemoryReadBuffer and
    // VideoRamAddr every CPU cycle across dma_2007_read's five iterations:
    //
    //   a stall landing AFTER the read   buf and v do not move for its whole
    //                                    length - the CPU is waiting on an
    //                                    opcode fetch and the repeats land in
    //                                    ROM where nothing observes them
    //   a stall landing ON the read      three reads of $2007, and the CPU
    //                                    keeps the LAST
    //
    // Repeating the last COMPLETED read gets both wrong at once: it re-reads
    // $2007 in the rows hardware leaves alone, and keeps the first value in the
    // row where it does not. Bus::clock therefore runs the CPU for one cycle and
    // un-runs it, so the CPU computes its own address - predicting it would mean
    // a second model of every addressing mode's arithmetic, since the address
    // comes from PC and fetched_operand rather than from (Schedule, cycle) the
    // way cycle_writes does.
    //
    // THE JOYPAD IS NOT CLOCKED BY THE ADDRESS BUS. Same page: "Joypads are
    // clocked via direct lines from the CPU, called joypad 1 /OE and joypad 2
    // /OE, rather than going over the address bus", with the NES-001 and AV
    // Famicom clocking once per CONTIGUOUS SET of reads. Only a DMA's repeats
    // put two controller reads back to back - a game's `lda $4016` are separated
    // by opcode fetches - so without that rule each repeat clocks the pad and
    // dma_4016_read reads 05 where hardware gives 07. See
    // Bus::controller_read_is_continuation.
    //
    // THE TWO ROMS DISAGREE ABOUT WHICH ALIGNMENT THEY SEE, and both are right.
    // A 3-cycle DMA gives dma_2007_read 33 44 and a 4-cycle one gives 44 55;
    // blargg accepts either, and AprNes' catalogue lists a CRC for each. Mesen
    // lands on the first, this emulator on the second.
    //
    // NOT IMPLEMENTED: the OAM DMA's own halt and alignment cycles. Those run
    // through PPU::perform_OAM_DMA_cycle rather than the branch in Bus::clock,
    // so they steal the bus without repeating anything.
    static const std::map<std::string, const char*> kExpected = {
        // Already correct, and the only one of the four that is. Its own name
        // and "Passed" are on the screen, so it self-checks.
        {"dma_2007_write",
         "11 11 AA 33 44 55 66 77\n"
         "11 11 AA 33 44 55 66 77\n"
         "11 11 AA 33 44 55 66 77\n"
         "11 11 AA 33 44 55 66 77\n"
         "11 11 AA 33 44 55 66 77\n"
         "dma_2007_write\n"
         "Passed\n"},

        // "DMC DMA during $4016 read causes extra $4016 read." Blargg's header
        // gives 08 08 07 08 08 and check_crc $F0AB808C. We print 08 in the
        // third position: the extra read is missing, so the bit counter is one
        // short of hardware's. ONE VALUE, in a ROM that self-checks - the
        // tightest oracle in this file.
        // HARDWARE'S OWN VALUES: the ROM self-checks with check_crc $F0AB808C and
        // prints Passed on its own, so 08 08 07 08 08 is blargg's measurement
        // rather than a match against another emulator.
        {"dma_4016_read",
         "08 08 07 08 08\n"
         "dma_4016_read\n"
         "Passed\n"},

        // "DMC DMA during $2007 read causes 2-3 extra $2007 reads before real
        // read." Blargg's header gives 11 22 / 11 22 / 33 44 or 44 55 / 11 22 /
        // 11 22, with the third row depending on CPU-PPU alignment at reset -
        // which is why the ROM has TWO accepted CRCs, 159A7A8F and 5E3DF9C4.
        // Our third row is 11 22: no extra reads at all, so the VRAM address
        // never advances past the first pair.
        // HARDWARE'S, via AprNes' catalogue: this ROM only prints its CRC, so it
        // cannot say Passed, and 5E3DF9C4 is one of the two values that
        // catalogue accepts - the 4-cycle alignment. Mesen lands on the 3-cycle
        // one and prints 33 44 with 159A7A8F; both are correct, and which one an
        // emulator gets depends on CPU/PPU sync at reset.
        {"dma_2007_read",
         "11 22\n"
         "11 22\n"
         "44 55\n"
         "11 22\n"
         "11 22\n"
         "5E3DF9C4\n"},

        // NOT A DMC PROBLEM - it sets up no DMA at all - and pinned here only
        // because it ships in the same suite. The second line is the double
        // read, `lda $20F7,x` crossing a page so the CPU reads $2007 twice one
        // cycle apart, and 85CFD627 is the first of the four CRCs blargg
        // accepts. It comes from the buffer refill landing a few dots after the
        // read returns while the address increment does not - see
        // PPU::kReadBufferRefillDots.
        {"double_2007_read",
         "22 33 44 55 66\n"
         "22 44 55 66 77\n"
         "85CFD627\n"},
    };

    const auto expected = kExpected.find(name);
    ASSERT_NE(kExpected.end(), expected) << name << " has no recorded screen; add one rather than skipping the check";

    EXPECT_EQ(std::string(expected->second), screen)
        << name
        << " no longer prints what it did.\n"
           "  These are hardware's values - three of the four ROMs self-check by CRC and the\n"
           "  fourth prints a CRC the AprNes catalogue lists - so a change here is a\n"
           "  regression against hardware rather than a divergence to re-record.\n";
}

INSTANTIATE_TEST_SUITE_P(DmcDma,
                         DmcDmaDuringRead,
                         ::testing::Values("dma_2007_write", "dma_4016_read", "dma_2007_read", "double_2007_read"),
                         [](const ::testing::TestParamInfo<const char*>& info) { return std::string(info.param); });

}  // namespace dmc_dma
}  // namespace tests
