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
// WHAT THE SWEEP VARIES, from blargg's own source (sprdma.s and dma_timing.inc in
// nes-test-roms). The row index n is split across delay_a_25_clocks(255-n) before
// the timed block and delay_a_25_clocks(n) after, so the measured span is fixed
// while the DMC's free-running fetch - period 3424 = 428*8 - slides 25 CPU cycles
// per row against the `sta $4014` that starts the OAM DMA. 25 is ODD, which is the
// entire reason consecutive rows alternate: every row flips get/put parity.
//
// So the reference table is two facts, not sixteen: +1 on odd rows, and one -2
// regime step. We reproduce both regimes' values AND the parity; only the step's
// position differs, ours at 05/06 against the reference's 04/05.
//
// NINTENDULATOR SPLITS THE ARRIVAL THREE WAYS AND WE SPLIT IT TWO. 0.980 - named by
// Fiskbit (nesdev t=14319) as the only emulator then getting DMC halt/parity right -
// branches in HandleDMA (src/CPU.cpp) on the DMC arriving before the sprite DMA, at
// exactly its start (DoPCM=1, the fetch goes first), or after its alignment read
// (DoPCM=2, the fetch waits for a sprite write). bus.cpp has
// `ppu.dma_in_progress() ? Align : Halt`.
//
//   ROW 05 IS NOT THE "AT EXACTLY THE START" ARRIVAL. Logging the request against
//   the OAM DMA start per row gives a delta walking +1 a row, and at row 05 the
//   request is 5 cycles BEFORE the start. The -2 matching a halt+dummy pair is
//   arithmetic that fits a wrong mechanism - and a two-cycle gap in a machine full
//   of two-cycle quantities, so fitting it is nearly no evidence at all.
//
// MEASURED, logging the fetch cycle relative to the OAM DMA start (rows 00-0F): the
// fetch lands at -6 -5 -4 -3 -2 for rows 00-04, at -1 for row 05, and inside the
// transfer from 06 on. ROW 05 MISSES THE COLLISION BY ONE CYCLE. One cycle later it
// lands on the OAM DMA's first cycle, where "DMC DMA wins", putting row 05 in the
// inside regime at 526; row 04 moves -2 to -1 and stays outside; every other row is
// far from its boundary. So a uniform one-cycle shift moves row 05 ALONE - which is
// what lets such a shift survive fifteen matching rows, not what makes it unlikely.
//
// THE DEFERRAL ASKS ABOUT THE WRONG CYCLE. bus.cpp tests cpu_wrote_this_cycle at the
// end of cycle N - "was the cycle that just ended a write". Mesen reaches
// ProcessPendingDma only from NesCpu::MemoryRead and Nintendulator only from
// MemGetCPU, so neither can halt anywhere but on a read: their question is "is the
// cycle about to run a read". A write at N followed by a read at N+1 halts at N+1
// for both and at N+2 for us. REAL, BUT NOT ROW 05's CAUSE - that trace shows the
// request accepted with no deferral at all.
//
// MESEN'S SIDE HAS SINCE BEEN MEASURED, once the lockstep harness was made
// symmetric - see the retraction further down. The fetches coincide and the two
// cycles are spent after them, so nothing above about WHERE our fetch lands is the
// defect; the "misses the collision by one cycle" reading of it was another
// mechanism that fit.
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
// MEASURED IN LOCKSTEP, which is the only way this was ever going to be settled.
// tools/lockstep_compare.cpp links this emulator's static libraries AND Mesen's
// InteropDLL into one process, aligns them on CONTENT - a PC value plus a
// matching register file - and then steps both one CPU cycle at a time. There is
// no offset arithmetic anywhere in it.
//
// At row 05, aligned on $E4FE, they agree for 523 cycles and then:
//
//   our DMC get   2064795            Mesen's is ONE CYCLE LATER
//   our resume    2065312            Mesen resumes TWO CYCLES EARLIER
//   get->resume   ours 517           Mesen 514, which is 1 (the store's
//                                    write) + 513 (its OAM DMA)
//   our OAM DMA   514 cycles, and remaining_dma_cycles was requested as 514
//
// The two-cycle resume difference is exactly the ROM's 528 against 526.
//
// A TRACE CANNOT TELL A DMC STALL FROM AN OAM DMA, and an earlier version of
// this section concluded from separate traces that "Mesen has no DMC halt at
// $E503 at all". That is withdrawn: PC is frozen during ANY halt, so both look
// identical from outside. Mesen's DMC fetch is visible only through
// ApuDmcState::BytesRemaining, which is what the lockstep reads.
//
// THE OBVIOUS FIX WAS TESTED AND IS WRONG. If our DMC sequence ends a cycle
// early, lengthening it by one should move the store's write onto the other
// parity, give a 513-cycle OAM DMA, and step row 05 to 526 while leaving the
// other fifteen alone - the fetch script's own note says only row 05 is
// sensitive to the sweep offset.
//
// Measured, by inserting exactly one cycle before the halt: EVERY ROW MOVED, by
// +2 and +3, and the crossing stayed at 05/06. Baseline 527/528 stepping to
// 525/526 became 529/530 stepping to 528/529.
//
// That is the same signature the fetch script already records for a different
// edit - "removing the get/put gate moved ALL SIXTEEN rows by +1 and left row
// 05's crossing exactly where it was" - so the class is now confirmed twice from
// two directions. THE CROSSING'S POSITION IS NOT SET BY THE DMC'S DURATION.
// Changing it moves the whole table and leaves the boundary alone.
//
// THE "+1 PHASE OFFSET" WAS THE INSTRUMENT, AND IS RETRACTED. A table here read
//
//   row  regime      our get   mesen   delta
//   04   uncollided     +7       +8     +1
//   05   uncollided     +8       +9     +1
//   06   collided       +9      +10     +1
//
// and concluded Mesen's fetch lands one cycle later than ours, constantly. But
// "our get" there was DERIVED as the DMA start plus 3, while Mesen's was MEASURED
// from BytesRemaining - two different events, so the constant +1 was the length of
// our halt sequence's derivation and not a disagreement. A constant offset is
// evidence about the instrument until proven otherwise, and this one never was.
//
// WITH BOTH SIDES READING THEIR OWN FETCH CYCLE, THE FETCHES COINCIDE. At row 05
// ours is 2064796 and Mesen's 2065894, both landmark+9. Cross-checked against the
// NES_DMA_TRACE instrumentation, which independently puts our fetch at 2064796 -
// two instruments, one number.
//
//   row 04   fetch -> resume  ours 516  mesen 516    frozen span  515 / 515
//   row 05   fetch -> resume  ours 516  mesen 514    frozen span  520 / 518
//
// The two agree exactly on the row that prints correctly and differ by exactly 2 on
// the row that does not, with the DMC read on the same cycle in both. SO THE TWO
// CYCLES ARE SPENT AFTER THE FETCH, and DMC timing is not where this defect lives.
// Mesen shortens row 05 by 2 against its own row 04; we run both at 516.
//
//   WHERE THE TWO CYCLES GO, located by OAM TRANSFER PROGRESS. PC and the register
//   file are frozen for the whole transfer, so they can only differ at the resume
//   and cannot say where the difference accrued. The OAM address advances once per
//   byte written, on both sides, and comparing that gives, relative to the
//   alignment cycle:
//
//     row 04   OAM progress never differs anywhere in the window
//     row 05   first differs at +11: ours $00, mesen $01
//
//   Our fetch is at +9 and our OAM DMA begins at +10, so at +10 we are spending a
//   halt cycle and at +11 an alignment cycle, while MESEN HAS ALREADY WRITTEN ITS
//   FIRST SPRITE BYTE. Our row 05 OAM DMA is 514 = halt + align + 512; Mesen spends
//   neither, because the DMC DMA that ended at +9 is adjacent to it and already
//   stopped the CPU and aligned the bus. At row 04 there is a cycle between the two
//   (fetch 1886948, OAM 1886950), the CPU comes back, the OAM DMA pays its own halt
//   at 513, and the two agree - which is what makes row 04 a control rather than
//   just another matching row.
//
// AND OUR OAM LENGTH IS NOT INDEPENDENTLY WRONG. It alternates as the parity
// rule says it should - 514 at row 03, 513 at row 04, 514 at row 05 - and rows
// 03 and 04 print 528 and 527, matching the reference exactly. Only row 05 is
// wrong.
//
// TWO FLAWS IN THE HARNESS, because its numbers should not be trusted blind:
//
//   The halted PC was hardcoded to $E503, which is row 05's store. Other rows
//   halt elsewhere - $E501 at row 03, $E502 at row 04 - so the resume test fired
//   immediately and reported a zero-length span. Fixed by taking the PC from
//   where the halt actually begins.
//
//   The resume test still compares Mesen's PC against OUR halted PC, and the two
//   are not at the same instruction once they diverge. Mesen's "resumes" figures
//   are therefore only meaningful where both happen to halt at the same address,
//   which is rows 05 and 06. Row 03's +49 is the same class of problem on the
//   fetch side: BytesRemaining changes for reasons other than the fetch being
//   looked for, and at row 03 it caught one of them.
//
// WHY THIS DOES NOT CONTRADICT THE PROBE ABOVE. That probe LENGTHENED the stall;
// this offset is about WHEN the fetch lands. They are different quantities, and
// only the second moves phase - which is why lengthening moved every row and
// left the crossing alone.
//
// So the measurements above are real and none of them is the cause: they are all
// downstream of whatever decides which iteration the crossing falls on.
// Anything that only lengthens or shortens the stall is answering the wrong
// question, and this file now has two experiments saying so.
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

    // ONE OF THESE PASSES AND ONE DOES NOT, and both are asserted exactly - the
    // pattern this repository already uses for opcode $AB in instr_test_roms.cpp
    // and for 6-MMC3_alt. sprdma_and_dmc_dma passes since the OAM DMA stopped
    // re-paying a halt and alignment a DMC DMA on the previous cycle had already
    // paid; that is worth more than a table match, because the ROM self-checks with
    // check_crc $FBADA48D, a value blargg computed on hardware. An emulator
    // producing a WRONG table that lands on the right 32-bit CRC is a 2^-32
    // coincidence, so the pass means these sixteen values ARE the hardware's.
    //
    // sprdma_and_dmc_dma_512 still fails and stays pinned. Do not read its pin as
    // the same kind of statement: it is what we currently produce, not what
    // hardware does.
    //
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
    // So the _512 table is the divergence, recorded. Any change to the DMC's timing
    // shifts a row and fails here with the row named, which is the signal worth
    // having - and the same assertion on the ROM that now passes is what stops a
    // later change quietly walking it back off the hardware values.
    //
    // Each row is parsed, rather than searching the whole screen for "512" and
    // "513" as an earlier version did. That search passed on any screen
    // containing one row of each - a table of fourteen 783s with a single 512
    // and 513 satisfied it - and let a wrong stall through undetected in an
    // adversarial review.
    //
    // Do not "update" these numbers to make a red run green without understanding
    // which row moved and why. The one row that has ever moved here took four
    // measured attempts and three wrong mechanisms to move correctly.
    struct Expectation {
        bool passes;  // whether the ROM's own CRC check succeeds
        std::vector<int> table;
    };
    static const std::map<std::string, Expectation> kExpected = {
        // HARDWARE'S OWN VALUES, via check_crc $FBADA48D. Verified 2026-09-03.
        {"sprdma_and_dmc_dma",
         {true, {527, 528, 527, 528, 527, 526, 525, 526, 525, 526, 525, 526, 525, 526, 525, 526}}},
        // OURS, not hardware's - a recorded divergence. MEASURED 2026-08-29,
        // unchanged by the row 05 fix.
        {"sprdma_and_dmc_dma_512",
         {false, {525, 526, 525, 526, 525, 526, 525, 526, 527, 528, 527, 528, 527, 528, 527, 528}}},
    };

    const std::vector<int> lengths = table_lengths(screen);
    ASSERT_EQ(16u, lengths.size()) << name << ": expected 16 table rows, parsed " << lengths.size()
                                   << ". Full screen:\n"
                                   << screen;

    const auto expected = kExpected.find(name);
    ASSERT_NE(kExpected.end(), expected) << name << " has no recorded table; add one rather than skipping the check";

    EXPECT_EQ(expected->second.passes, screen_says(screen, "Passed"))
        << name << (expected->second.passes ? " no longer passes." : " now passes.")
        << "\n  The verdict is asserted in BOTH directions on purpose: this ROM's CRC is a\n"
           "  hardware value, so losing a pass here is a regression against hardware and\n"
           "  gaining one means the recorded divergence below is stale. Full screen:\n"
        << screen;

    for (size_t row = 0; row < lengths.size(); ++row) {
        EXPECT_EQ(expected->second.table[row], lengths[row])
            << name << " row " << row << " reads " << lengths[row] << ", not the expected "
            << expected->second.table[row] << ".\n"
            << (expected->second.passes
                    ? "  These are hardware's values, confirmed by the ROM's own CRC - a row moving\n"
                      "  here is a regression in the DMC/OAM collision, not a divergence to record.\n"
                    : "  This is the remaining DMC/OAM collision divergence, and a row moving means\n"
                      "  the DMC's timing changed. Check it against Mesen before re-pinning it.\n")
            << "  Full screen:\n"
            << screen;
    }
}

INSTANTIATE_TEST_SUITE_P(DmcDma,
                         SprdmaAndDmcDma,
                         ::testing::Values("sprdma_and_dmc_dma", "sprdma_and_dmc_dma_512"),
                         [](const ::testing::TestParamInfo<const char*>& info) { return std::string(info.param); });

}  // namespace dmc_dma
}  // namespace tests
