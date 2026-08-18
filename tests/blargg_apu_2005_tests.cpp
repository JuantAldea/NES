// blargg's 2005 APU frame-counter suite - eleven ROMs that reach further into
// the frame counter and length counters than apu_test does.
//
// FETCHED TO AUDIT WORK ALREADY DONE, not to drive new work, and it is the last
// oracle that can do that for the APU. There is no test ROM anywhere for the
// envelope, sweep or triangle linear counter - blargg's own tests.txt says "the
// tests do not test clocking of the envelope, sweep, or triangle's linear
// counter", and his readme admits he never characterised that hardware either.
// So everything after this point needs a different kind of verification. See
// fetch_blargg_apu_2005.sh.
//
// ALL ELEVEN PASS. Nine did on first contact, 09.reset_timing among them -
// "after reset or power-up, APU acts as if $4017 were written with $00 from 9
// to 12 clocks before first instruction begins" - which is APU::power_on() and
// APU::reset() confirmed by a suite that had no part in building them.
//
// The other two were one subject, write-versus-clock ordering in the length
// counter, and are fixed: see LengthCounter::pending_halt and
// APU::apply_pending_loads. They were pinned to their exact codes until then,
// and announced themselves by failing those pins rather than by being noticed.
//
// THESE ROMs REPORT ON SCREEN, NOT THROUGH $6000. They predate that protocol,
// so blargg_rom_harness.h cannot read them and nametable_screen.h does. Two
// consequences worth stating because both have caused mistakes here before:
//
//   - $01 MEANS PASSED. Not 0. A zero screen is an unwritten nametable, which
//     is what a ROM that never ran looks like - so a reader that treated 0 as
//     success would report a blank screen as a pass.
//   - The screen must be allowed to SETTLE. The ROMs print intermediate codes
//     while running, so sampling the first non-blank row too early catches a
//     subtest's code rather than the verdict.
#include <string>

#include "gtest/gtest.h"
#include "nametable_screen.h"
#include "rom_fixture.h"

namespace tests
{
namespace blargg_apu_2005
{
namespace
{

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/blargg_apu_2005/" + name + ".nes";
}

constexpr const char* kFetch = "run tests/test_files/fetch_blargg_apu_2005.sh";

// Measured: every ROM shows its final code between frames 11 and 24, and none
// changes afterwards. The budget is far above that on purpose - these are a
// FLOOR and rise as ROMs get further, so a tight one turns progress into a
// timeout. kSettleFrames is how long the code must hold before it is believed.
constexpr uint64_t kMaxFrames = 600;
constexpr uint64_t kSettleFrames = 60;

// Runs until the reported code stops changing, and returns it verbatim - "$01",
// "$03" - rather than parsing to an integer, because an unexpected screen is
// more useful quoted than turned into a number that lost its context.
std::string run_until_settled(const std::string& name, uint64_t& frame_of_first_report)
{
    Bus console;
    EXPECT_TRUE(console.load_cartridge(rom_path(name))) << "the ROM is present but did not load: " << rom_path(name);
    console.cpu.reset();

    std::string last;
    uint64_t stable = 0;
    frame_of_first_report = 0;

    for (uint64_t frame = 0; frame < kMaxFrames; ++frame) {
        nametable_screen::run_one_frame(console);

        const std::string row = nametable_screen::first_non_blank_row(console);
        if (!row.empty() && row == last) {
            if (++stable >= kSettleFrames) {
                break;
            }
            continue;
        }
        stable = 0;
        last = row;
        frame_of_first_report = frame + 1;
    }

    return last;
}

// The report is a row like "  $01" padded with the blank tile, which
// nametable_screen renders as '.'. Pull out the "$NN" and nothing else.
std::string reported_code(const std::string& screen_row)
{
    const size_t dollar = screen_row.find('$');
    if (dollar == std::string::npos || dollar + 3 > screen_row.size()) {
        return {};
    }
    return screen_row.substr(dollar, 3);
}

#define REQUIRE_APU2005_ROM(name) REQUIRE_ROM(rom_path(name), kFetch)

}  // namespace

// --- what already works ------------------------------------------------------

class BlarggApu2005RomsThatPass : public ::testing::TestWithParam<const char*>
{
};

TEST_P(BlarggApu2005RomsThatPass, reports_code_01)
{
    const std::string name = GetParam();
    REQUIRE_APU2005_ROM(name);

    uint64_t frame = 0;
    const std::string screen = run_until_settled(name, frame);

    EXPECT_EQ("$01", reported_code(screen))
        << name << " no longer reports $01 (passed) - it settled on this screen at frame " << frame << ":\n  ["
        << screen
        << "]\n"
           "  Codes are listed per ROM in blargg's tests.txt; $01 is the ONLY pass.\n"
           "  An empty or all-blank screen means the ROM never ran, not that it failed.";
}

// All eleven. Nine passed on first contact - 09.reset_timing among them, which
// is why the suite was worth fetching at all. The last two arrived when the
// length counter learned that a halt or a reload written on the cycle before a
// length clock lands AFTER it; both were pinned to their exact codes until
// then, and both announced themselves by failing those pins.
INSTANTIATE_TEST_SUITE_P(BlarggApu2005,
                         BlarggApu2005RomsThatPass,
                         ::testing::Values("01.len_ctr",
                                           "02.len_table",
                                           "03.irq_flag",
                                           "04.clock_jitter",
                                           "05.len_timing_mode0",
                                           "06.len_timing_mode1",
                                           "07.irq_flag_timing",
                                           "08.irq_timing",
                                           "09.reset_timing",
                                           "10.len_halt_timing",
                                           "11.len_reload_timing"),
                         [](const ::testing::TestParamInfo<const char*>& info) {
                             std::string name = info.param;
                             for (char& c : name) {
                                 if (!std::isalnum(static_cast<unsigned char>(c))) {
                                     c = '_';
                                 }
                             }
                             return name;
                         });

// Guards the reader rather than the emulator. If the nametable path regressed,
// every test above would fail identically with an empty screen, and it would not
// be obvious that the APU was fine and the screen reader was not.
GTEST_TEST(blarggApu2005Harness, the_screen_reader_recovers_a_code_from_these_roms)
{
    REQUIRE_APU2005_ROM("01.len_ctr");

    uint64_t frame = 0;
    const std::string screen = run_until_settled("01.len_ctr", frame);

    ASSERT_FALSE(screen.empty()) << "the nametable never showed a printable row - the ROM did not draw, or\n"
                                    "  nametable_screen.h regressed. Neither is an APU fault.";
    EXPECT_NE(std::string::npos, screen.find('$')) << "a row was drawn but carries no $NN code: [" << screen << "]";
    EXPECT_GT(frame, 0u);
}

}  // namespace blargg_apu_2005
}  // namespace tests
