// Damian Yerrick's Holy Mapperel, the mapper-1 builds - the oracle MMC1 is
// implemented against.
//
// WHY THIS SUITE AND NOT MMC1_A12. nes-test-roms carries an MMC1 test that
// looks like the obvious choice and is not usable: it asks a human to walk a
// delay counter with the D-pad and judge the width of a grayscale bar. There is
// no verdict in it to read. The reasoning is written out in full at the top of
// tests/test_files/fetch_holy_mapperel.sh.
//
// WHAT IT CHECKS, and why nine images rather than one. The ROM detects which
// board it is on from how the mapper answers, then measures PRG size, CHR size
// and kind, work-RAM size and battery, and finally runs a per-mapper detailed
// test. The nine differ ONLY in their headers and payload sizes, so between
// them they walk the whole SxROM family - SGROM, SFROM, SJROM, SLROM, SKROM and
// SUROM - and a mapper bug tends to show up in one board and not its neighbour.
// That is the diagnostic value: the failing set names the feature.
//
// HOW IT REPORTS. On screen, so tests/nametable_screen.h does the reading - but
// NOT with the identity mapping the blargg 2005 ROMs use. Holy Mapperel's puts
// is `and #$3F` before `sta PPUDATA`, folding ASCII into six bits, so 'A' is
// tile $01 and a reader that assumed identity would score every letter as
// unprintable. decode_tile below undoes it.
#include <cstdint>
#include <string>
#include <vector>

#include "../include/bus.h"
#include "gtest/gtest.h"
#include "nametable_screen.h"
#include "rom_fixture.h"

namespace tests
{
namespace holy_mapperel
{
namespace
{

constexpr const char* kFetch = "run tests/test_files/fetch_holy_mapperel.sh";

// MEASURED, per image, as the frame the detail line stopped changing:
//
//   M1_P128K            86    M1_P128K_C32K        15    M1_P128K_C128K       15
//   M1_P128K_C32K_S8K   92    M1_P128K_C32K_W8K    92
//   M1_P128K_C128K_S8K  93    M1_P128K_C128K_W8K   93
//   M1_P512K_S8K       166    M1_P512K_S32K       166
//
// The two CHR-ROM images without work RAM settle in 15 because they skip both
// RAM sweeps; the SUROMs take 166 because 512KB is four times the PRG to walk.
// The cap is a bit over 2x the slowest, and the loop still waits for the line
// to hold steady rather than trusting any of these numbers - so the cap only
// turns a hang into a diagnosis instead of a stalled suite.
constexpr int kMaxFrames = 400;

std::string rom_path(const std::string& name)
{
    return std::string(NES_TEST_FILES_DIR) + "/holy_mapperel/" + name + ".nes";
}

// Undoes src/main.s's `and #$3F`. ASCII $40-$5F - A-Z and a few symbols - folds
// onto $00-$1F, while $20-$3F, which is space, punctuation and the digits,
// survives unchanged. Tile 0 is the ambiguous one: it is both '@' and the value
// a cleared nametable holds. The ROM never prints '@', so blank wins.
char decode_tile(const uint8_t tile)
{
    if (tile == 0x00) {
        return ' ';
    }
    return static_cast<char>(tile < 0x20 ? 0x40 + tile : tile);
}

std::vector<std::string> read_screen(Bus& console)
{
    std::vector<std::string> rows;
    for (uint16_t row = 0; row < nametable_screen::kRows; ++row) {
        std::string line;
        for (uint16_t col = 0; col < nametable_screen::kColumns; ++col) {
            const uint16_t addr =
                static_cast<uint16_t>(nametable_screen::kNametableBase + row * nametable_screen::kColumns + col);
            line.push_back(decode_tile(console.ppu.ppu_bus_read(addr)));
        }
        const size_t end = line.find_last_not_of(' ');
        rows.push_back(end == std::string::npos ? std::string() : line.substr(0, end + 1));
    }
    return rows;
}

std::string screen_text(const std::vector<std::string>& rows)
{
    std::string text;
    for (const std::string& row : rows) {
        text += row;
        text.push_back('\n');
    }
    return text;
}

// The first row containing `needle`, trimmed of leading spaces, or empty. The
// ROM indents everything by two columns and the report lines are unique, so
// matching on content is steadier than matching on a row number - which would
// move the day a line is added above.
std::string find_row(const std::vector<std::string>& rows, const std::string& needle)
{
    for (const std::string& row : rows) {
        const size_t at = row.find(needle);
        if (at == std::string::npos) {
            continue;
        }
        const size_t start = row.find_first_not_of(' ');
        return row.substr(start == std::string::npos ? 0 : start);
    }
    return {};
}

constexpr const char* kDetailLine = "DETAILED TEST RESULT:";

// Runs until the detail line has been on screen unchanged for ten frames. The
// ROM prints intermediate state while it works - the CHR-RAM pass alone buzzes
// the speaker for dozens of frames - so sampling at a fixed frame reads a
// half-finished report and calls it a verdict.
struct ScreenRun {
    std::vector<std::string> rows;
    int frames = 0;
    bool settled = false;
};

ScreenRun run_until_settled(Bus& console)
{
    ScreenRun result;
    std::string previous;
    int stable = 0;

    for (int frame = 1; frame <= kMaxFrames; ++frame) {
        nametable_screen::run_one_frame(console);
        result.rows = read_screen(console);
        result.frames = frame;

        const std::string detail = find_row(result.rows, kDetailLine);
        if (!detail.empty() && detail == previous) {
            if (++stable >= 10) {
                result.settled = true;
                return result;
            }
        } else {
            stable = 0;
            previous = detail;
        }
    }
    return result;
}

// What each image must report. Written from a measured run with MMC1 newly
// implemented, not from what the mapper is believed to do - see the baseline
// table in the fetch script.
//
// `detail` is the four-digit code, one digit each for WRAM, PRG ROM, IRQ and
// CHR, in which zero is normal. Two images are pinned at a NON-zero value on
// purpose: see the CHR-RAM note on kRoms below.
struct Expected {
    const char* name;
    const char* board;
    const char* prg;
    const char* wram;
    const char* chr;
    const char* detail;
};

// All nine report 0000. Worth recording how the last three got there, because
// the diagnosis came from WHICH images failed rather than from reading code.
//
// The three CHR-RAM boards - SGROM and both SUROMs - reported 0003 at first,
// while all six CHR-ROM boards reported 0000. Digits are WRAM, PRG, IRQ, CHR,
// and the two set bits are the two 4K CHR windows: mmcdrivers.s runs
// mmc1_test_one_chr_window at $0000 and again at $1000, ORing the second in
// shifted left. Identical on a 128K board and a 512K one, which ruled out the
// PRG side, since only the 512K boards route a CHR register bit to PRG A18.
//
// The cause was that CHR-RAM was indexed FLAT - `chr_ram[addr]` - on the
// assumption that RAM the console supplies is not banked. CHR-RAM is not the
// console's: it sits on the cartridge behind the same mapper CHR address lines
// as CHR-ROM, so MMC1 in 4KB mode pages an 8KB chip as two halves. It now goes
// through Mapper::chr_offset like everything else. No CHR-ROM oracle could have
// found this, which is the argument for taking all nine images rather than one.
const Expected kRoms[] = {
    {"M1_P128K", "001 SGROM (MMC1)", "128K PRG ROM", "PRG RAM MISSING", "8K CHR RAM OK", "0000"},
    {"M1_P128K_C32K", "001 SFROM (MMC1)", "128K PRG ROM", "PRG RAM MISSING", "32K CHR ROM OK", "0000"},
    {"M1_P128K_C32K_S8K", "001 SJROM (MMC1)", "128K PRG ROM", "8K PRG RAM OK", "32K CHR ROM OK", "0000"},
    {"M1_P128K_C32K_W8K", "001 SJROM (MMC1)", "128K PRG ROM", "8K PRG RAM OK", "32K CHR ROM OK", "0000"},
    {"M1_P128K_C128K", "001 SLROM (MMC1)", "128K PRG ROM", "PRG RAM MISSING", "128K CHR ROM OK", "0000"},
    {"M1_P128K_C128K_S8K", "001 SKROM (MMC1)", "128K PRG ROM", "8K PRG RAM OK", "128K CHR ROM OK", "0000"},
    {"M1_P128K_C128K_W8K", "001 SKROM (MMC1)", "128K PRG ROM", "8K PRG RAM OK", "128K CHR ROM OK", "0000"},
    // Both SUROMs report 8K of work RAM. For _S8K that is correct; for _S32K it
    // is not, and the reason is the console rather than the mapper - PrgRAM is
    // a fixed 8KB Bus device, so SXROM's four switchable 8KB banks have nowhere
    // to live. Recorded as measured; banking it is a separate change with its
    // own oracle row waiting here for it.
    {"M1_P512K_S8K", "001 SUROM (MMC1)", "512K PRG ROM", "8K PRG RAM OK", "8K CHR RAM OK", "0000"},
    {"M1_P512K_S32K", "001 SUROM (MMC1)", "512K PRG ROM", "8K PRG RAM OK", "8K CHR RAM OK", "0000"},
};

}  // namespace

class HolyMapperel : public ::testing::TestWithParam<Expected>
{
};

TEST_P(HolyMapperel, reports_what_the_board_is_and_that_it_works)
{
    const Expected& expected = GetParam();
    const std::string path = rom_path(expected.name);

    Bus console;
    ASSERT_TRUE(console.load_cartridge(path)) << "could not load " << path << " - " << kFetch;

    // Bus::load_cartridge maps the ROM and nothing else: the CPU was reset by
    // Bus's constructor, before there was a cartridge to fetch vectors from.
    // Without this the console runs from whatever an empty-bus vector fetch
    // produced, which presents as a blank screen - indistinguishable from a
    // mapper bug until you count how many distinct PCs it visited.
    console.cpu.power_on();

    const ScreenRun run = run_until_settled(console);
    ASSERT_TRUE(run.settled) << expected.name << " never settled in " << kMaxFrames
                             << " frames. Suspect the mapper detection phase, which runs from RAM and hangs "
                                "rather than reporting when a bank switch does not take.\n"
                             << screen_text(run.rows);

    const std::string screen = screen_text(run.rows);

    // The board line is the load-bearing one: the ROM works out which of the
    // SxROM boards it is on purely from how the mapper answers, so getting it
    // right means mirroring control, PRG banking and the size detection all
    // behaved. A wrong board name makes every line under it meaningless.
    EXPECT_EQ(expected.board, find_row(run.rows, "(MMC1)")) << screen;
    EXPECT_EQ(expected.prg, find_row(run.rows, "PRG ROM")) << screen;
    EXPECT_EQ(expected.wram, find_row(run.rows, "PRG RAM")) << screen;
    EXPECT_EQ(expected.chr, find_row(run.rows, "CHR R")) << screen;
    EXPECT_EQ(std::string(kDetailLine) + " " + expected.detail, find_row(run.rows, kDetailLine)) << screen;
}

INSTANTIATE_TEST_SUITE_P(Mapper1,
                         HolyMapperel,
                         ::testing::ValuesIn(kRoms),
                         [](const ::testing::TestParamInfo<Expected>& info) { return std::string(info.param.name); });

}  // namespace holy_mapperel
}  // namespace tests
