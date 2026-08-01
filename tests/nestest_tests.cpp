// nestest golden-log differ.
//
// This is a MEASUREMENT INSTRUMENT, not a CPU fix. It compares this emulator's
// CPU trace against the canonical nestest.log line by line and reports the
// first line where they diverge. The CPU is known to have cycle-accuracy and
// illegal-opcode bugs, so the harness test at the bottom of this file (the one
// that actually clocks the CPU) is EXPECTED TO FAIL. Do not weaken the
// assertion and do not "fix" the CPU here -- the first-divergence line number
// is the metric another agent drives upward.
//
// Part 1 (parser + comparator + their own unit tests) has zero dependency on
// CPU correctness and must be airtight: an off-by-one here would masquerade
// as a phantom CPU bug.
#include <array>
#include <cstdint>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "../include/bus.h"
#include "../include/cpu.h"


namespace tests
{
namespace nestest
{

// ---------------------------------------------------------------------------
// Part 1: log line model, parser, comparator.
// ---------------------------------------------------------------------------

struct LogEntry {
    uint16_t pc = 0;
    std::vector<uint8_t> opcode_bytes;
    uint8_t a = 0;
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t p = 0;
    uint8_t sp = 0;
    uint64_t cyc = 0;
};

// Field-level divergence result. NONE means the two entries compare equal on
// every field this harness cares about (PC, opcode bytes, A, X, Y, P, SP, CYC
// -- explicitly NOT the disassembly text and NOT the PPU dot/scanline column,
// since the PPU is not clocked by this harness).
enum class DivergenceField {
    NONE,
    PC,
    OPCODE_BYTES,
    A,
    X,
    Y,
    P,
    SP,
    CYC,
};

std::string to_string(DivergenceField field)
{
    switch (field) {
    case DivergenceField::NONE:
        return "NONE";
    case DivergenceField::PC:
        return "PC";
    case DivergenceField::OPCODE_BYTES:
        return "opcode_bytes";
    case DivergenceField::A:
        return "A";
    case DivergenceField::X:
        return "X";
    case DivergenceField::Y:
        return "Y";
    case DivergenceField::P:
        return "P";
    case DivergenceField::SP:
        return "SP";
    case DivergenceField::CYC:
        return "CYC";
    }
    return "UNKNOWN";
}

// Matches a Nintendulator-style nestest.log line, e.g.:
//   "C000  4C F5 C5  JMP $C5F5                       A:00 X:00 Y:00 P:24 SP:FD PPU:  0, 21 CYC:7"
//   "C5F5  A2 00     LDX #$00                        A:00 X:00 Y:00 P:24 SP:FD PPU:  0, 30 CYC:10"
//   "C6BD  04 A9    *NOP $A9 = 00                    A:AA X:97 Y:4E P:EF SP:F9 PPU:128, 89 CYC:14579"
//
// Fields captured (1-indexed groups):
//   1: PC (4 hex digits)
//   2: opcode bytes, 1 to 3 space-separated hex pairs
//   3: A, 4: X, 5: Y, 6: P, 7: SP  (all 2 hex digits)
//   8: CYC (decimal, arbitrary width)
//
// Deliberately NOT captured: the disassembly/mnemonic text (may contain an
// undocumented-opcode '*' marker and the codebase's ISC vs the log's ISB
// naming disagreement) and the PPU column (not clocked by this harness).
const std::regex& log_line_pattern()
{
    static const std::regex pattern(
        R"(^([0-9A-F]{4})  )"                      // PC
        R"(([0-9A-F]{2}(?: [0-9A-F]{2}){0,2}) *)"  // opcode bytes (1-3), right-padded
        R"( \*?\S.*?)"                             // disassembly column, ignored
        R"(A:([0-9A-F]{2}) X:([0-9A-F]{2}) Y:([0-9A-F]{2}) P:([0-9A-F]{2}) SP:([0-9A-F]{2}) )"
        R"(PPU:\s*\d+,\s*\d+ CYC:(\d+)\s*$)");
    return pattern;
}

uint8_t parse_hex_byte(const std::string& s) { return static_cast<uint8_t>(std::stoul(s, nullptr, 16)); }

std::optional<LogEntry> parse_log_line(const std::string& line)
{
    // nestest.log is CRLF-terminated; be tolerant of a stray trailing '\r' or
    // '\n' regardless of how the caller split the file into lines.
    std::string trimmed = line;
    while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == '\n')) {
        trimmed.pop_back();
    }

    std::smatch match;
    if (!std::regex_match(trimmed, match, log_line_pattern())) {
        return std::nullopt;
    }

    LogEntry entry;
    entry.pc = static_cast<uint16_t>(std::stoul(match[1].str(), nullptr, 16));

    std::istringstream bytes_stream(match[2].str());
    std::string byte_token;
    while (bytes_stream >> byte_token) {
        entry.opcode_bytes.push_back(parse_hex_byte(byte_token));
    }

    entry.a = parse_hex_byte(match[3].str());
    entry.x = parse_hex_byte(match[4].str());
    entry.y = parse_hex_byte(match[5].str());
    entry.p = parse_hex_byte(match[6].str());
    entry.sp = parse_hex_byte(match[7].str());
    entry.cyc = std::stoull(match[8].str());

    return entry;
}

// Returns the first field (in the order PC, opcode bytes, A, X, Y, P, SP,
// CYC) that differs between expected and actual. DivergenceField::NONE means
// every compared field matches.
DivergenceField compare_entries(const LogEntry& expected, const LogEntry& actual)
{
    if (expected.pc != actual.pc) {
        return DivergenceField::PC;
    }
    if (expected.opcode_bytes != actual.opcode_bytes) {
        return DivergenceField::OPCODE_BYTES;
    }
    if (expected.a != actual.a) {
        return DivergenceField::A;
    }
    if (expected.x != actual.x) {
        return DivergenceField::X;
    }
    if (expected.y != actual.y) {
        return DivergenceField::Y;
    }
    if (expected.p != actual.p) {
        return DivergenceField::P;
    }
    if (expected.sp != actual.sp) {
        return DivergenceField::SP;
    }
    if (expected.cyc != actual.cyc) {
        return DivergenceField::CYC;
    }
    return DivergenceField::NONE;
}

// ---------------------------------------------------------------------------
// Part 1 unit tests: parser and comparator, no CPU involved.
// ---------------------------------------------------------------------------

GTEST_TEST(nestestDiffer, parses_three_byte_opcode_line)
{
    const auto entry =
        parse_log_line("C000  4C F5 C5  JMP $C5F5                       A:00 X:00 Y:00 P:24 SP:FD PPU:  0, 21 CYC:7");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->pc, 0xC000);
    EXPECT_EQ(entry->opcode_bytes, (std::vector<uint8_t>{0x4C, 0xF5, 0xC5}));
    EXPECT_EQ(entry->a, 0x00);
    EXPECT_EQ(entry->x, 0x00);
    EXPECT_EQ(entry->y, 0x00);
    EXPECT_EQ(entry->p, 0x24);
    EXPECT_EQ(entry->sp, 0xFD);
    EXPECT_EQ(entry->cyc, 7u);
}

GTEST_TEST(nestestDiffer, parses_two_byte_opcode_line)
{
    const auto entry =
        parse_log_line("C5F5  A2 00     LDX #$00                        A:00 X:00 Y:00 P:24 SP:FD PPU:  0, 30 CYC:10");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->pc, 0xC5F5);
    EXPECT_EQ(entry->opcode_bytes, (std::vector<uint8_t>{0xA2, 0x00}));
    EXPECT_EQ(entry->cyc, 10u);
}

GTEST_TEST(nestestDiffer, parses_one_byte_opcode_line)
{
    const auto entry =
        parse_log_line("C72D  EA        NOP                             A:00 X:00 Y:00 P:26 SP:FB PPU:  0, 81 CYC:27");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->pc, 0xC72D);
    EXPECT_EQ(entry->opcode_bytes, (std::vector<uint8_t>{0xEA}));
    EXPECT_EQ(entry->a, 0x00);
    EXPECT_EQ(entry->p, 0x26);
    EXPECT_EQ(entry->sp, 0xFB);
    EXPECT_EQ(entry->cyc, 27u);
}

GTEST_TEST(nestestDiffer, parses_undocumented_opcode_line_with_asterisk_marker)
{
    // The '*' marks an undocumented opcode. The disassembly text itself
    // (including the codebase's ISC vs the log's ISB naming disagreement) is
    // never compared, so the parser only needs to tolerate the marker, not
    // interpret the mnemonic.
    const auto entry = parse_log_line(
        "C6BD  04 A9    *NOP $A9 = 00                    A:AA X:97 Y:4E P:EF SP:F9 PPU:128, 89 CYC:14579");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->pc, 0xC6BD);
    EXPECT_EQ(entry->opcode_bytes, (std::vector<uint8_t>{0x04, 0xA9}));
    EXPECT_EQ(entry->a, 0xAA);
    EXPECT_EQ(entry->x, 0x97);
    EXPECT_EQ(entry->y, 0x4E);
    EXPECT_EQ(entry->p, 0xEF);
    EXPECT_EQ(entry->sp, 0xF9);
    EXPECT_EQ(entry->cyc, 14579u);
}

GTEST_TEST(nestestDiffer, parses_isb_undocumented_opcode_disassembly)
{
    const auto entry = parse_log_line(
        "EB9E  E3 45    *ISB ($45,X) @ 47 = 0647 = EB    A:40 X:02 Y:AA P:64 SP:FB PPU:160,331 CYC:18297");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->pc, 0xEB9E);
    EXPECT_EQ(entry->opcode_bytes, (std::vector<uint8_t>{0xE3, 0x45}));
    EXPECT_EQ(entry->cyc, 18297u);
}

GTEST_TEST(nestestDiffer, rejects_malformed_line)
{
    EXPECT_FALSE(parse_log_line("").has_value());
    EXPECT_FALSE(parse_log_line("this is not a log line").has_value());
    EXPECT_FALSE(parse_log_line("C000  4C F5 C5  JMP $C5F5").has_value());  // missing register/CYC columns
    EXPECT_FALSE(parse_log_line("C000  ZZ F5 C5  JMP $C5F5   A:00 X:00 Y:00 P:24 SP:FD PPU:  0, 21 CYC:7")
                     .has_value());  // non-hex opcode byte
    EXPECT_FALSE(
        parse_log_line("C000  4C F5 C5  JMP $C5F5                       A:00 X:00 Y:00 P:24 SP:FD PPU:  0, 21 CYC:")
            .has_value());  // empty CYC
}

GTEST_TEST(nestestDiffer, tolerates_trailing_crlf)
{
    const auto entry = parse_log_line(
        "C000  4C F5 C5  JMP $C5F5                       A:00 X:00 Y:00 P:24 SP:FD PPU:  0, 21 CYC:7\r\n");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->pc, 0xC000);
    EXPECT_EQ(entry->cyc, 7u);
}

namespace
{
LogEntry baseline_entry()
{
    LogEntry entry;
    entry.pc = 0xC000;
    entry.opcode_bytes = {0x4C, 0xF5, 0xC5};
    entry.a = 0x00;
    entry.x = 0x00;
    entry.y = 0x00;
    entry.p = 0x24;
    entry.sp = 0xFD;
    entry.cyc = 7;
    return entry;
}
}  // namespace

GTEST_TEST(nestestDiffer, comparator_reports_no_divergence_for_identical_entries)
{
    const auto expected = baseline_entry();
    const auto actual = baseline_entry();
    EXPECT_EQ(compare_entries(expected, actual), DivergenceField::NONE);
}

GTEST_TEST(nestestDiffer, comparator_identifies_pc_divergence)
{
    const auto expected = baseline_entry();
    auto actual = baseline_entry();
    actual.pc = 0xC001;
    EXPECT_EQ(compare_entries(expected, actual), DivergenceField::PC);
}

GTEST_TEST(nestestDiffer, comparator_identifies_opcode_bytes_divergence)
{
    const auto expected = baseline_entry();
    auto actual = baseline_entry();
    actual.opcode_bytes = {0x4C, 0xF5, 0xC6};
    EXPECT_EQ(compare_entries(expected, actual), DivergenceField::OPCODE_BYTES);
}

GTEST_TEST(nestestDiffer, comparator_identifies_a_divergence)
{
    const auto expected = baseline_entry();
    auto actual = baseline_entry();
    actual.a = 0x01;
    EXPECT_EQ(compare_entries(expected, actual), DivergenceField::A);
}

GTEST_TEST(nestestDiffer, comparator_identifies_x_divergence)
{
    const auto expected = baseline_entry();
    auto actual = baseline_entry();
    actual.x = 0x01;
    EXPECT_EQ(compare_entries(expected, actual), DivergenceField::X);
}

GTEST_TEST(nestestDiffer, comparator_identifies_y_divergence)
{
    const auto expected = baseline_entry();
    auto actual = baseline_entry();
    actual.y = 0x01;
    EXPECT_EQ(compare_entries(expected, actual), DivergenceField::Y);
}

GTEST_TEST(nestestDiffer, comparator_identifies_p_divergence)
{
    const auto expected = baseline_entry();
    auto actual = baseline_entry();
    actual.p = 0x25;
    EXPECT_EQ(compare_entries(expected, actual), DivergenceField::P);
}

GTEST_TEST(nestestDiffer, comparator_identifies_sp_divergence)
{
    const auto expected = baseline_entry();
    auto actual = baseline_entry();
    actual.sp = 0xFC;
    EXPECT_EQ(compare_entries(expected, actual), DivergenceField::SP);
}

GTEST_TEST(nestestDiffer, comparator_identifies_cyc_divergence)
{
    const auto expected = baseline_entry();
    auto actual = baseline_entry();
    actual.cyc = 8;
    EXPECT_EQ(compare_entries(expected, actual), DivergenceField::CYC);
}

GTEST_TEST(nestestDiffer, comparator_reports_first_divergence_when_several_fields_differ)
{
    // PC is checked first, so it should win even though A also differs.
    const auto expected = baseline_entry();
    auto actual = baseline_entry();
    actual.pc = 0xC001;
    actual.a = 0x01;
    EXPECT_EQ(compare_entries(expected, actual), DivergenceField::PC);
}

GTEST_TEST(nestestDiffer, parser_matches_every_line_of_the_real_log)
{
    const std::string path = std::string(NES_TEST_FILES_DIR) + "/nestest.log";
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open()) << "could not open nestest log: " << path;

    std::string line;
    size_t line_number = 0;
    size_t parse_failures = 0;
    while (std::getline(file, line)) {
        ++line_number;
        if (!parse_log_line(line).has_value()) {
            ++parse_failures;
            ADD_FAILURE() << "parser rejected well-formed log line " << line_number << ": " << line;
            if (parse_failures > 5) {
                break;
            }
        }
    }
    EXPECT_GT(line_number, 0u) << "log file appears to be empty";
}

// ---------------------------------------------------------------------------
// Part 2: flat-memory harness.
//
// Deliberately independent of Bus/RAM/ROM/PPU: those have known bugs (Bus
// mirroring, aborting asserts in ppu.cpp) that belong to a different agent's
// lane. This harness is a plain 64KB array with iNES PRG-ROM mapped NROM-style
// at both $8000 and $C000, wired straight into the CPU's read/write callbacks.
// ---------------------------------------------------------------------------

namespace
{

constexpr size_t INES_HEADER_SIZE = 16;
constexpr size_t PRG_ROM_SIZE = 16384;
// The canonical nestest.log is exactly this many lines; nestest's official
// automated run is the same number of instructions.
constexpr size_t EXPECTED_LOG_LINES = 8991;
constexpr uint16_t PRG_BANK_1_BASE = 0x8000;
constexpr uint16_t PRG_BANK_2_BASE = 0xC000;

struct FlatMemory {
    std::array<uint8_t, 64 * 1024> memory{0};

    void write(const uint16_t addr, const uint8_t data)
    {
        // $8000-$FFFF is cartridge PRG-ROM and is not writable on NROM. Without
        // this guard, a CPU bug that computes a wrong store address could
        // rewrite the golden program in place, turning one root-cause bug into
        // a cascade of misattributed divergences further down the log.
        if (addr >= 0x8000) {
            return;
        }
        memory[addr] = data;
    }

    uint8_t read(const uint16_t addr) { return memory[addr]; }
};

// Loads nestest.nes into `mem`, mapping the (single) 16KB PRG-ROM bank at both
// $8000 and $C000 per NROM mirroring. Fails loudly (via ADD_FAILURE, and by
// returning false so the caller aborts the test) rather than silently running
// against zeroed memory.
bool load_nestest_rom(FlatMemory& mem)
{
    const std::string path = std::string(NES_TEST_FILES_DIR) + "/nestest.nes";
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        ADD_FAILURE() << "could not open nestest ROM: " << path << " (run tests/test_files/fetch_nestest.sh)";
        return false;
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    if (data.size() < INES_HEADER_SIZE + PRG_ROM_SIZE) {
        ADD_FAILURE() << "nestest ROM is too small (" << data.size() << " bytes): " << path;
        return false;
    }

    if (!(data[0] == 'N' && data[1] == 'E' && data[2] == 'S' && data[3] == 0x1A)) {
        ADD_FAILURE() << "nestest ROM has no iNES header (expected \"NES\\x1a\"): " << path;
        return false;
    }

    const uint8_t prg_rom_units = data[4];
    if (prg_rom_units != 1) {
        ADD_FAILURE() << "nestest ROM header reports " << static_cast<int>(prg_rom_units)
                      << " x 16KB PRG-ROM units, expected exactly 1 for NROM mirroring: " << path;
        return false;
    }

    for (size_t i = 0; i < PRG_ROM_SIZE; ++i) {
        const uint8_t byte = data[INES_HEADER_SIZE + i];
        mem.memory[PRG_BANK_1_BASE + i] = byte;
        mem.memory[PRG_BANK_2_BASE + i] = byte;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Part 3: reporting contract.
// ---------------------------------------------------------------------------

struct DivergenceReport {
    bool diverged = false;
    size_t line_number = 0;  // 1-based
    DivergenceField field = DivergenceField::NONE;
    LogEntry expected;
    LogEntry actual;
    std::string log_line_text;
};

std::string format_bytes(const std::vector<uint8_t>& bytes)
{
    std::ostringstream oss;
    oss << std::hex << std::uppercase;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            oss << ' ';
        }
        oss.width(2);
        oss.fill('0');
        oss << static_cast<unsigned>(bytes[i]);
    }
    return oss.str();
}

void print_divergence_report(const DivergenceReport& report)
{
    std::cout << std::dec;
    std::cout << "nestest: first divergence at line " << report.line_number << std::endl;

    std::ostringstream field_line;
    field_line << "  field: " << to_string(report.field) << "   ";

    switch (report.field) {
    case DivergenceField::PC:
        field_line << "expected " << std::hex << std::uppercase << report.expected.pc << "   actual "
                   << report.actual.pc;
        break;
    case DivergenceField::OPCODE_BYTES:
        field_line << "expected " << format_bytes(report.expected.opcode_bytes) << "   actual "
                   << format_bytes(report.actual.opcode_bytes);
        break;
    case DivergenceField::A:
        field_line << "expected " << std::hex << std::uppercase << static_cast<unsigned>(report.expected.a)
                   << "   actual " << static_cast<unsigned>(report.actual.a);
        break;
    case DivergenceField::X:
        field_line << "expected " << std::hex << std::uppercase << static_cast<unsigned>(report.expected.x)
                   << "   actual " << static_cast<unsigned>(report.actual.x);
        break;
    case DivergenceField::Y:
        field_line << "expected " << std::hex << std::uppercase << static_cast<unsigned>(report.expected.y)
                   << "   actual " << static_cast<unsigned>(report.actual.y);
        break;
    case DivergenceField::P:
        field_line << "expected " << std::hex << std::uppercase << static_cast<unsigned>(report.expected.p)
                   << "   actual " << static_cast<unsigned>(report.actual.p);
        break;
    case DivergenceField::SP:
        field_line << "expected " << std::hex << std::uppercase << static_cast<unsigned>(report.expected.sp)
                   << "   actual " << static_cast<unsigned>(report.actual.sp);
        break;
    case DivergenceField::CYC:
        field_line << std::dec << "expected " << report.expected.cyc << "   actual " << report.actual.cyc;
        break;
    case DivergenceField::NONE:
        field_line << "(no divergence)";
        break;
    }
    std::cout << field_line.str() << std::endl;
    std::cout << "  context: " << report.log_line_text << std::endl;
}

// Runs the CPU against the nestest.log oracle, one instruction per log line.
// Stops at the first divergence (or at end of log / end of file, whichever
// comes first).
// Templated on the memory backend so the identical comparison can be driven
// either against FlatMemory (isolating the CPU) or against the real Bus
// (additionally exercising the address decode, RAM mirroring, cartridge
// mapping and PPU register stubs). Memory only needs `uint8_t read(uint16_t)`.
template <typename Memory>
DivergenceReport run_nestest(CPU& cpu, Memory& mem, std::ifstream& log_file)
{
    DivergenceReport report;

    std::string line;
    size_t line_number = 0;

    while (std::getline(log_file, line)) {
        ++line_number;

        const auto expected = parse_log_line(line);
        if (!expected.has_value()) {
            ADD_FAILURE() << "differ bug: could not parse nestest.log line " << line_number << ": " << line;
            report.diverged = true;
            report.line_number = line_number;
            report.log_line_text = line;
            return report;
        }

        // Snapshot state BEFORE the instruction runs. clock() does its entire
        // fetch+decode (including advancing PC past every opcode/operand
        // byte) on the very first cycle of an instruction, so registers.PC at
        // this point is exactly what the log's PC column records.
        LogEntry actual;
        actual.pc = cpu.registers.PC;
        actual.a = cpu.registers.A;
        actual.x = cpu.registers.X;
        actual.y = cpu.registers.Y;
        actual.p = cpu.registers.P;
        actual.sp = cpu.registers.SP;
        actual.cyc = cpu.total_cycles;

        // Capture the opcode+operand bytes from memory at the PC the
        // instruction started at, before running it. Reading these up front
        // (rather than trying to infer a byte count from how far clock()
        // moves PC, which execute-time PC changes like branches/JMP/JSR would
        // corrupt) is what lets this comparison stay correct regardless of
        // what the instruction does.
        const uint16_t pc_before = cpu.registers.PC;
        std::vector<uint8_t> opcode_bytes;
        for (size_t i = 0; i < expected->opcode_bytes.size(); ++i) {
            opcode_bytes.push_back(mem.read(static_cast<uint16_t>(pc_before + i)));
        }
        actual.opcode_bytes = opcode_bytes;

        // Run exactly one instruction to completion.
        bool complete = cpu.clock(false);
        while (!complete) {
            complete = cpu.clock(false);
        }

        const DivergenceField field = compare_entries(*expected, actual);
        if (field != DivergenceField::NONE) {
            report.diverged = true;
            report.line_number = line_number;
            report.field = field;
            report.expected = *expected;
            report.actual = actual;
            report.log_line_text = line;
            return report;
        }
    }

    report.diverged = false;
    report.line_number = line_number;
    return report;
}

}  // namespace

GTEST_TEST(nestestDiffer, cpu_trace_matches_golden_log)
{
    FlatMemory mem;
    ASSERT_TRUE(load_nestest_rom(mem)) << "aborting: cannot measure divergence without a valid ROM image";

    auto read = std::bind(&FlatMemory::read, &mem, std::placeholders::_1);
    auto write = std::bind(&FlatMemory::write, &mem, std::placeholders::_1, std::placeholders::_2);
    CPU cpu(read, write);

    // Prologue contract: nestest's automated (no-video-needed) entry point is
    // $C000, NOT the ROM's real reset vector ($C004), so we must not call
    // cpu.reset() here.
    cpu.registers.PC = 0xC000;
    cpu.registers.A = 0;
    cpu.registers.X = 0;
    cpu.registers.Y = 0;
    cpu.registers.P = 0x24;
    cpu.registers.SP = 0xFD;
    cpu.total_cycles = 7;

    const std::string log_path = std::string(NES_TEST_FILES_DIR) + "/nestest.log";
    std::ifstream log_file(log_path);
    ASSERT_TRUE(log_file.is_open()) << "could not open nestest log: " << log_path;

    const DivergenceReport report = run_nestest(cpu, mem, log_file);

    if (report.diverged) {
        print_divergence_report(report);
    } else {
        std::cout << "nestest: full log matched, " << report.line_number << " lines" << std::endl;
    }

    if (!report.diverged) {
        // The $02/$03 result bytes alone do NOT make a match trustworthy: flat
        // memory starts zeroed and nestest only writes non-zero on failure, so
        // both bytes read 0 long before the ROM reaches its result-writing code.
        // A truncated log would therefore "match" every line it had and pass.
        // The fixtures are gitignored, so their integrity is environment state
        // rather than repo state - pin the count explicitly.
        EXPECT_EQ(report.line_number, EXPECTED_LOG_LINES)
            << "log ended early: compared " << report.line_number << " of " << EXPECTED_LOG_LINES
            << " lines. The fixture is truncated - re-run tests/test_files/fetch_nestest.sh";
        EXPECT_EQ(mem.read(0x0002), 0) << "nestest reported a failure via byte $02 despite a full log match";
        EXPECT_EQ(mem.read(0x0003), 0) << "nestest reported a failure via byte $03 despite a full log match";
    }

    // The measurement. If this ever regresses, the line number printed above
    // localises the failure to a single instruction -- do not weaken it.
    EXPECT_FALSE(report.diverged) << "first divergence at line " << report.line_number << ", field "
                                  << to_string(report.field);
}

// Same comparison, but driven through the real Bus instead of a flat array, so
// it additionally exercises the address decode, RAM mirroring, cartridge
// mapping and the PPU register stubs. The flat-memory test above isolates the
// CPU; this one is the integration counterpart. A divergence here that does NOT
// appear above indicts the memory map rather than the CPU.
GTEST_TEST(nestestDiffer, cpu_trace_matches_golden_log_through_bus)
{
    Bus console;
    ASSERT_TRUE(console.load_cartridge(std::string(NES_TEST_FILES_DIR) + "/nestest.nes"))
        << "aborting: cannot measure divergence without a valid ROM image";

    // reset() reads the ROM's real vector ($C004); nestest's automated entry
    // point is $C000, so override PC afterwards. SP/P/cycles come from reset().
    console.cpu.reset();
    ASSERT_EQ(console.cpu.registers.PC, 0xC004) << "cartridge reset vector not visible through the Bus";
    console.cpu.registers.PC = 0xC000;
    console.cpu.total_cycles = 7;

    const std::string log_path = std::string(NES_TEST_FILES_DIR) + "/nestest.log";
    std::ifstream log_file(log_path);
    ASSERT_TRUE(log_file.is_open()) << "could not open nestest log: " << log_path;

    const DivergenceReport report = run_nestest(console.cpu, console, log_file);

    if (report.diverged) {
        print_divergence_report(report);
    } else {
        std::cout << "nestest (via Bus): full log matched, " << report.line_number << " lines" << std::endl;
    }

    if (!report.diverged) {
        EXPECT_EQ(report.line_number, EXPECTED_LOG_LINES)
            << "log ended early: compared " << report.line_number << " of " << EXPECTED_LOG_LINES
            << " lines. The fixture is truncated - re-run tests/test_files/fetch_nestest.sh";
        // Unlike the flat-memory case these bytes go through RAM mirroring, so
        // this also confirms the decode routes $0002/$0003 to internal RAM.
        EXPECT_EQ(console.read(0x0002), 0) << "nestest reported a failure via byte $02";
        EXPECT_EQ(console.read(0x0003), 0) << "nestest reported a failure via byte $03";
    }

    EXPECT_FALSE(report.diverged) << "first divergence at line " << report.line_number << ", field "
                                  << to_string(report.field);
}

}  // namespace nestest
}  // namespace tests
