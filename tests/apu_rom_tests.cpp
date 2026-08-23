// blargg's apu_test suite - the oracle for building the APU.
//
// Every other ROM suite here guards work that is finished. This one measured
// work in progress: when it was written the frame counter, its /IRQ and the
// length counters existed and nothing else did. The DMC has landed since, minus
// its CPU stall. Still missing: the envelope, the sweep, the triangle's linear
// counter, every channel's OUTPUT, and the mixer. clock_quarter_frame() is
// still empty.
//
// DO NOT REACH FOR apu_mixer OR volume_tests TO COVER WHAT IS MISSING. Both
// look like the obvious next oracles and neither is one. Measured on this
// emulator, with no channels and no mixer implemented at all:
//
//   apu_mixer/square     status 0 (passed) @ frame  970
//   apu_mixer/triangle   status 0 (passed) @ frame  609
//   apu_mixer/noise      status 0 (passed) @ frame 1160
//   apu_mixer/dmc        status 0 (passed) @ frame  721
//
// All four pass against an emulator that cannot produce a triangle wave. They
// DO use the $6000 protocol - signature, status byte, ASCII at $6004 - so
// blargg_rom_harness.h reads them perfectly, and that is exactly what makes the
// trap worth writing down. The message says what the status byte does not:
//
//   1. Should play short tone.
//   2. Should be nearly silent.
//   3. Should play short tone.
//
// Those are instructions to a listener. The tests generate a tone and its
// inverse on the DMC DAC, and correctness is near-silence between two beeps - a
// ROM cannot hear itself, so the status byte only reports that it ran to the
// end. This is blargg's own "done" category: his readme says "Only a test which
// prints 'done' at the end requires that you watch/listen while it runs", and
// these are those. volume_tests is the same shape and ships a recordings/
// directory for the same reason.
//
// Wiring either in as a pass/fail oracle adds tests that are green today and
// cannot go red, which is the failure this repo exists to prevent. If the
// channels and the mixer get written they will be the first substantial
// subsystem here built WITHOUT an oracle, and that should be a decision taken
// on purpose rather than discovered afterwards.
//
// ALL EIGHT NOW PASS, and the file records how it got there rather than only
// where it ended. It began with three passing and five pinned to their exact
// status and message. Pinning rather than skipping is the point - see the
// $AB/ATX precedent in instr_test_roms.cpp. A skipped failure says nothing when
// it changes; a pinned one fails loudly the moment the behaviour moves, in
// either direction, and every one of the five went out that way.
//
// THE PINS EARNED THEIR KEEP TWICE. Implementing the length counters broke
// three of them, and the DMC broke the last two - each time the assertion
// failed and its message said to promote the ROM into the list above, which is
// how a feature announces its own completion. Keep that property for whatever
// is pinned next: a pin that is merely deleted teaches nothing.
//
// WHICH ROMS PASS IS ITSELF A RESULT. 3-irq_flag, 4-jitter and 6-irq_flag_timing
// were passing before any of this work, confirming a frame counter - including
// the write-parity detail in APU::write - that had been reasoned out rather
// than measured, against an oracle it was never run against.
//
// See fetch_apu_test.sh for the measured baseline, and blargg's readme.txt for
// what each numbered failure means; 1-len_ctr alone enumerates seven distinct
// ways length counters go wrong, which is the order to implement them in.
#include <string>

#include "blargg_rom_harness.h"
#include "gtest/gtest.h"
#include "rom_fixture.h"

namespace tests
{
namespace apu_rom
{
namespace
{

std::string rom_path(const std::string& name) { return std::string(NES_TEST_FILES_DIR) + "/apu_test/" + name + ".nes"; }

// Measured: every ROM reports between frames 15 and 22. 600 is far above that
// on purpose - these numbers are a FLOOR and will rise as ROMs get further, so
// a tight budget would turn progress into a timeout.
constexpr uint64_t kMaxFrames = 600;

constexpr const char* kFetch = "run tests/test_files/fetch_apu_test.sh";

blargg::RomResult run(const std::string& name) { return blargg::run_rom(rom_path(name), kMaxFrames); }

}  // namespace

// --- what already works ------------------------------------------------------

class ApuRomsThatPass : public ::testing::TestWithParam<const char*>
{
};

TEST_P(ApuRomsThatPass, reports_pass)
{
    const std::string name = GetParam();
    REQUIRE_ROM(rom_path(name), kFetch);

    const blargg::RomResult result = run(name);

    ASSERT_TRUE(result.completed) << name << ": no verdict within " << kMaxFrames << " frames";
    EXPECT_EQ(0, result.status) << name << " reported:\n" << result.message;
}

// Six of the eight. The frame counter and its IRQ were passing before any of
// this; the three length ROMs joined them when the length counters landed, and
// each did so by FAILING its pin first, which is what those pins are for.
INSTANTIATE_TEST_SUITE_P(ApuTest,
                         ApuRomsThatPass,
                         ::testing::Values("1-len_ctr",
                                           "2-len_table",
                                           "3-irq_flag",
                                           "4-jitter",
                                           "5-len_timing",
                                           "6-irq_flag_timing",
                                           "7-dmc_basics",
                                           "8-dmc_rates"),
                         [](const ::testing::TestParamInfo<const char*>& info) {
                             std::string name = info.param;
                             for (char& c : name) {
                                 if (c == '-' || c == '.') {
                                     c = '_';
                                 }
                             }
                             return name;
                         });

}  // namespace apu_rom
}  // namespace tests
