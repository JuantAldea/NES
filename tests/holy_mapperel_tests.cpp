// Damian Yerrick's Holy Mapperel - the oracle MMC1 is implemented against, and
// since the two mapper-4 builds were added, the only test here that checks an
// MMC3 answers mirroring writes the way the hardware does. See kMapper4Roms.
//
// WHY THIS SUITE AND NOT MMC1_A12. nes-test-roms carries an MMC1 test that
// looks like the obvious choice and is not usable: it asks a human to walk a
// delay counter with the D-pad and judge the width of a grayscale bar. There is
// no verdict in it to read. The reasoning is written out in full at the top of
// tests/test_files/fetch_holy_mapperel.sh.
//
// WHAT IT CHECKS, and why a set of images rather than one. The ROM detects which
// board it is on from how the mapper answers, then measures PRG size, CHR size
// and kind, work-RAM size and battery, and finally runs a per-mapper detailed
// test. The mapper-1 images differ ONLY in their headers and payload sizes, so
// between them they walk the whole SxROM family - SGROM, SFROM, SJROM, SLROM,
// SKROM, SUROM, SXROM - and a mapper bug tends to show up in one board and not
// its neighbour. That is the diagnostic value: the failing SET names the
// feature, which is how both CHR-RAM faults here were located.
//
// Counts are deliberately not written into this prose. Every table below states
// its own, and a total stated here would be a fourth place to forget.
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
//   M4_P128K            87    M4_P256K_C256K       22
//   M0_P32K_C8K_V       14    M3_P32K_C32K_H       14    M2_P128K_V           85
//   M7_P128K            85
//
// One line splits the whole table: CHR-RAM images pay for a write-and-verify
// sweep of the RAM and CHR-ROM ones skip it. That is why M2_P128K_V takes 85
// where the two discrete CHR-ROM boards take 14, and why M4_P256K_C256K is
// faster than M4_P128K despite being eight times the cartridge. The SUROMs take
// 166 because 512KB is four times the PRG to walk on top of that.
// The M4 pair splits the same way and for the same reason - M4_P128K is CHR-RAM
// and pays for the sweep, M4_P256K_C256K is CHR-ROM and skips it - which is why
// the larger ROM is the faster of the two.
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

// The board line, trimmed. Holy Mapperel draws the report block first and the
// board is its top line, so this finds it without assuming what the ROM decided
// the board WAS - see the note at the EXPECT_EQ that uses it.
std::string first_non_empty_row(const std::vector<std::string>& rows)
{
    for (const std::string& row : rows) {
        const size_t start = row.find_first_not_of(' ');
        if (start != std::string::npos) {
            return row.substr(start);
        }
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
// CHR, in which zero is normal. ALL ELEVEN ROWS ARE 0000 - do not add a
// deliberately non-zero one without saying here how many there are and why.
//
// This sentence has been wrong twice, both times by naming a count. It read
// "two images are pinned at a NON-zero value" when three rows carried 0003, and
// it survived 58f1ce6 taking those three to 0000, at which point it described a
// set that was empty. The count is the part that rots, so it is stated once,
// here, rather than in prose that has to be found and updated.
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
    // The last two differ only in work RAM, and the BOARD NAMES are the check
    // that matters. Both images declare 512K of PRG, so both are SUROM as far
    // as the PRG side can tell; SXROM is SUROM plus 32K of banked work RAM, and
    // the only way the ROM can tell them apart is by writing tags to all four
    // 8KB banks and reading them back. So "SXROM" on the second row is not a
    // relabelling of the first - it is the banking working, stated in the one
    // field that cannot be satisfied by getting the header right.
    //
    // Both read 8K and SUROM until PrgRAM stopped being an 8KB device sized to
    // the window rather than to the board.
    {"M1_P512K_S8K", "001 SUROM (MMC1)", "512K PRG ROM", "8K PRG RAM OK", "8K CHR RAM OK", "0000"},
    {"M1_P512K_S32K", "001 SXROM (MMC1)", "512K PRG ROM", "32K PRG RAM OK", "8K CHR RAM OK", "0000"},
};

// The two mapper-4 builds. These were added to settle one specific question -
// whether $A000 bit 0 selects vertical or horizontal - and they answered it on
// the first run, which is the argument for reaching for an oracle instead of a
// doc citation. Under the old polarity M4_P128K reported "002 UNROM": Holy
// Mapperel identifies the board by writing mirroring and seeing where the
// nametables land, so an inverted bit makes an MMC3 answer exactly like a
// discrete mapper. M4_P256K_C256K never rendered a report at all.
//
// WHY THE EXISTING MMC3 SUITE MISSED IT. All six blargg mmc3_test_2 ROMs are
// IRQ-counter tests: they write mirroring once during init and never read the
// nametables back, so the bug survived a fully green mmc3_rom_tests.cpp. These
// two rows are what closes that gap, and they cross-check the PRG and CHR
// banking at the same time.
//
// THEY SETTLED A SECOND QUESTION TOO, which is the better argument for them.
// Mmc3::chr_offset used to leave CHR-RAM unbanked, with a comment saying real
// MMC3 does drive those lines so paging it was "very likely right" - but that
// nothing in the suite measured it, and naming these two images as what would.
// They did: M4_P128K arrived reporting detail 0003, the CHR digit, which is
// bit-for-bit the signature the three MMC1 CHR-RAM boards showed before 58f1ce6
// banked theirs - reproduced on a board sharing no mapper code with MMC1.
// Banking MMC3's CHR-RAM the same way takes it to 0000, and M4_P256K_C256K is
// CHR-ROM, was already 0000, and stays there, so the change is confined to the
// RAM path. Both rows are pinned at 0000 below; a regression in either surfaces
// as the digit that names the subsystem.
const Expected kMapper4Roms[] = {
    {"M4_P128K", "004 TGROM (MMC3)", "128K PRG ROM", "PRG RAM MISSING", "8K CHR RAM OK", "0000"},
    {"M4_P256K_C256K", "004 TLROM (MMC3)", "256K PRG ROM", "PRG RAM MISSING", "256K CHR ROM OK", "0000"},
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
    // The board line is located by POSITION, not by matching "(MMC1)" as this
    // used to. A misidentified board is the single most informative failure this
    // suite produces, and it is exactly the case a needle naming the expected
    // chip cannot report: the mirroring-polarity bug made M4_P128K print
    // "002 UNROM", which contains no "(MMC" at all, so the row would have come
    // back empty and read as "the ROM printed nothing" - a hang - rather than
    // "the ROM decided this was a different board". The report block is the
    // first thing drawn, so the first non-blank row is the board line whatever
    // the ROM concluded.
    EXPECT_EQ(expected.board, first_non_empty_row(run.rows)) << screen;
    EXPECT_EQ(expected.prg, find_row(run.rows, "PRG ROM")) << screen;
    EXPECT_EQ(expected.wram, find_row(run.rows, "PRG RAM")) << screen;
    EXPECT_EQ(expected.chr, find_row(run.rows, "CHR R")) << screen;
    EXPECT_EQ(std::string(kDetailLine) + " " + expected.detail, find_row(run.rows, kDetailLine)) << screen;
}

// The three discrete boards - NROM, UNROM and CNROM. These found nothing, and
// that is the result rather than a reason to leave them out.
//
// They were added because this oracle had been aimed at exactly two mappers and
// found three real faults in them: MMC1 indexing CHR-RAM flat, MMC3 inverting
// $A000 bit 0, MMC3 leaving CHR-RAM unbanked. NROM, UNROM and CNROM predate the
// Mapper refactor and had been carried through two structural changes on the
// strength of suites largely written from the implementation, so they were the
// obvious place to look next. They are clean. Now that is measured rather than
// assumed, and the rows stay as the regression net for the next refactor.
//
// TWO OF THE BOARD NAMES LOOK WRONG AND ARE NOT. Holy Mapperel reports "066"
// for NROM and CNROM because its README groups board 066 as "NROM, CNROM,
// GNROM" - mappers it cannot separate by how they answer, since none of them
// changes mirroring. The number is the detection GROUP and the word after it is
// the board within it. Only UNROM, which switches PRG, gets its own line.
//
// M2_P128K_V is the useful one for a reason beyond UNROM itself: it is CHR-RAM,
// and it reports 0000. 58f1ce6 routed CHR-RAM through Mapper::chr_offset so
// MMC1 could page it; this is the check that boards which must NOT page it were
// left alone, since UNROM takes the flat default.
const Expected kDiscreteRoms[] = {
    {"M0_P32K_C8K_V", "066 NROM", "32K PRG ROM", "PRG RAM MISSING", "8K CHR ROM OK", "0000"},
    {"M2_P128K_V", "002 UNROM", "128K PRG ROM", "PRG RAM MISSING", "8K CHR RAM OK", "0000"},
    {"M3_P32K_C32K_H", "066 CNROM", "32K PRG ROM", "PRG RAM MISSING", "32K CHR ROM OK", "0000"},
};

INSTANTIATE_TEST_SUITE_P(Discrete,
                         HolyMapperel,
                         ::testing::ValuesIn(kDiscreteRoms),
                         [](const ::testing::TestParamInfo<Expected>& info) { return std::string(info.param.name); });

// AxROM, and the board name is the whole test.
//
// Holy Mapperel identifies a board by writing mirroring and seeing where the
// nametables land. AxROM is the case where BOTH answers are one screen and the
// register picks which - so "007" is not a label the ROM read off the header,
// it is bit 4 of the latch demonstrably moving all four nametable slots
// together. The same probe reported "002 UNROM" for an MMC3 with an inverted
// mirroring bit, which is how much weight this one field carries.
//
// ANROM rather than AOROM is the size detection: AOROM is the 256KB variant and
// this image is 128KB, so the ROM walked the bank tags and got the right answer
// through a 32KB window with no fixed half to stand on.
const Expected kAxRomRoms[] = {
    {"M7_P128K", "007 ANROM", "128K PRG ROM", "PRG RAM MISSING", "8K CHR RAM OK", "0000"},
};

INSTANTIATE_TEST_SUITE_P(Mapper7,
                         HolyMapperel,
                         ::testing::ValuesIn(kAxRomRoms),
                         [](const ::testing::TestParamInfo<Expected>& info) { return std::string(info.param.name); });

INSTANTIATE_TEST_SUITE_P(Mapper1,
                         HolyMapperel,
                         ::testing::ValuesIn(kRoms),
                         [](const ::testing::TestParamInfo<Expected>& info) { return std::string(info.param.name); });

INSTANTIATE_TEST_SUITE_P(Mapper4,
                         HolyMapperel,
                         ::testing::ValuesIn(kMapper4Roms),
                         [](const ::testing::TestParamInfo<Expected>& info) { return std::string(info.param.name); });

}  // namespace holy_mapperel
}  // namespace tests
