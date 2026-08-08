// Cycle-accuracy and undocumented-opcode tests for the 6502 core.
//
// Three independent layers, deliberately not sharing a source of truth with
// src/:
//
//  1. SingleStepTests 65x02 vectors (an external oracle: 10,000 randomized
//     cases per opcode, generated from real hardware behaviour). This is what
//     validates the illegal opcodes. Asserting that "SLO does ASL then ORA"
//     against an implementation written as ASL-then-ORA would prove nothing;
//     these vectors were produced without reference to this codebase.
//
//  2. A cycle-count table transcribed by hand from a published 6502 reference
//     (see kReferenceTimings). Deliberately NOT derived from
//     src/instruction.cpp, which would test that file against itself.
//
//  3. Targeted regression tests pinning the specific hazards this core got
//     wrong: the page-cross penalty applied to stores, branches executing
//     twice, and the (zp),Y pointer wrap.
//
// Layer 1 needs ~360 MB of fetched vectors and skips (loudly) without them; see
// tests/test_files/fetch_single_step_tests.sh. Layers 2 and 3 always run.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../include/cpu.h"
#include "../include/instruction.h"

namespace tests
{
namespace
{

// ---------------------------------------------------------------------------
// A flat 64 KB address space. No mirroring, no mapped registers: the vectors
// assume a bare 6502 with RAM everywhere, and so does the timing table.
// ---------------------------------------------------------------------------

// One bus access: the 6502 performs exactly one per cycle, so a trace of these
// is equivalent to a cycle-by-cycle description of the instruction. Verified
// against all 900,000 vectors: accesses-per-instruction always equals the
// documented cycle count.
struct BusAccess {
    uint16_t addr = 0;
    uint8_t value = 0;
    bool is_write = false;

    bool operator==(const BusAccess&) const = default;
};

std::string to_string(const BusAccess& a)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%s $%04X = $%02X", a.is_write ? "write" : "read ", a.addr, a.value);
    return buf;
}

struct FlatMemory {
    std::array<uint8_t, 64 * 1024> memory{};

    // Every access the CPU makes, in order. Recording here rather than inside
    // the CPU keeps the observation completely outside the code under test.
    std::vector<BusAccess> trace;
    bool recording = false;

    uint8_t read(const uint16_t addr)
    {
        const uint8_t value = memory[addr];
        if (recording) {
            trace.push_back(BusAccess{addr, value, false});
        }
        return value;
    }

    void write(const uint16_t addr, const uint8_t data)
    {
        memory[addr] = data;
        if (recording) {
            trace.push_back(BusAccess{addr, data, true});
        }
    }
};

CPU make_cpu(FlatMemory& mem)
{
    return CPU([&mem](uint16_t addr) { return mem.read(addr); },
               [&mem](uint16_t addr, uint8_t data) { mem.write(addr, data); });
}

// Runs exactly one instruction and returns how many clock() calls it took.
// clock() returns true on the final cycle of an instruction, so the number of
// calls up to and including that one is the instruction's cycle count.
size_t run_one_instruction(CPU& cpu)
{
    size_t cycles = 0;
    bool complete = false;

    while (!complete) {
        complete = cpu.clock(false);
        ++cycles;

        // A wrong cycle count is a test failure, not a reason to hang the
        // suite. No 6502 instruction exceeds 8 cycles (7 plus the oops cycle).
        if (cycles > 64) {
            throw std::runtime_error("instruction did not complete within 64 cycles");
        }
    }

    return cycles;
}

// ---------------------------------------------------------------------------
// Layer 2: cycle counts transcribed from a published reference.
//
// Source: the opcode timing tables at
// https://www.masswerk.at/6502/6502_instruction_set.html, cross-checked against
// http://www.oxyron.de/html/opcodes02.html for the undocumented opcodes. Typed
// in by hand from those tables. This must NOT be regenerated from
// src/instruction.cpp - the entire point is that it disagrees loudly if that
// table is wrong.
//
// `base` is the cost with no page cross. `oops` marks the instructions that
// spend one extra cycle when indexing crosses a page boundary: the read forms
// only. Stores and read-modify-writes always perform the address fix-up, so
// their published cost already includes it and never varies - STA abs,X is 5
// cycles whether or not it crosses a page, ASL abs,X is always 7.
// ---------------------------------------------------------------------------

struct ReferenceTiming {
    uint8_t opcode;
    const char* mnemonic;
    uint8_t base;
    bool oops;
};

// clang-format off: the table is laid out in aligned columns on purpose.
constexpr ReferenceTiming kReferenceTimings[] = {
    // Loads: read forms pay the oops cycle.
    {0xA9, "LDA #", 2, false},   {0xA5, "LDA zp", 3, false},    {0xB5, "LDA zp,X", 4, false},
    {0xAD, "LDA abs", 4, false}, {0xBD, "LDA abs,X", 4, true},  {0xB9, "LDA abs,Y", 4, true},
    {0xA1, "LDA (zp,X)", 6, false}, {0xB1, "LDA (zp),Y", 5, true},
    {0xA2, "LDX #", 2, false},   {0xAE, "LDX abs", 4, false},   {0xBE, "LDX abs,Y", 4, true},
    {0xA0, "LDY #", 2, false},   {0xAC, "LDY abs", 4, false},   {0xBC, "LDY abs,X", 4, true},

    // Stores: fixed cost, never an oops cycle.
    {0x85, "STA zp", 3, false},  {0x95, "STA zp,X", 4, false},  {0x8D, "STA abs", 4, false},
    {0x9D, "STA abs,X", 5, false}, {0x99, "STA abs,Y", 5, false}, {0x81, "STA (zp,X)", 6, false},
    {0x91, "STA (zp),Y", 6, false},
    {0x86, "STX zp", 3, false},  {0x8E, "STX abs", 4, false},   {0x84, "STY zp", 3, false},

    // Read-modify-write: fixed cost, never an oops cycle.
    {0x0A, "ASL A", 2, false},   {0x06, "ASL zp", 5, false},    {0x16, "ASL zp,X", 6, false},
    {0x0E, "ASL abs", 6, false}, {0x1E, "ASL abs,X", 7, false},
    {0x2A, "ROL A", 2, false},   {0x3E, "ROL abs,X", 7, false},
    {0x4A, "LSR A", 2, false},   {0x5E, "LSR abs,X", 7, false},
    {0x6A, "ROR A", 2, false},   {0x7E, "ROR abs,X", 7, false},
    {0xE6, "INC zp", 5, false},  {0xFE, "INC abs,X", 7, false},
    {0xC6, "DEC zp", 5, false},  {0xDE, "DEC abs,X", 7, false},

    // ALU read forms.
    {0x69, "ADC #", 2, false},   {0x7D, "ADC abs,X", 4, true},  {0x79, "ADC abs,Y", 4, true},
    {0x71, "ADC (zp),Y", 5, true},
    {0xE9, "SBC #", 2, false},   {0xFD, "SBC abs,X", 4, true},  {0xF1, "SBC (zp),Y", 5, true},
    {0xC9, "CMP #", 2, false},   {0xDD, "CMP abs,X", 4, true},  {0xD1, "CMP (zp),Y", 5, true},
    {0x29, "AND #", 2, false},   {0x3D, "AND abs,X", 4, true},
    {0x09, "ORA #", 2, false},   {0x1D, "ORA abs,X", 4, true},
    {0x49, "EOR #", 2, false},   {0x5D, "EOR abs,X", 4, true},
    {0x24, "BIT zp", 3, false},  {0x2C, "BIT abs", 4, false},
    {0xE0, "CPX #", 2, false},   {0xC0, "CPY #", 2, false},

    // Implied / transfers.
    {0xEA, "NOP", 2, false},     {0xAA, "TAX", 2, false},       {0xA8, "TAY", 2, false},
    {0x8A, "TXA", 2, false},     {0x98, "TYA", 2, false},       {0xBA, "TSX", 2, false},
    {0x9A, "TXS", 2, false},     {0xE8, "INX", 2, false},       {0xC8, "INY", 2, false},
    {0xCA, "DEX", 2, false},     {0x88, "DEY", 2, false},       {0x18, "CLC", 2, false},
    {0x38, "SEC", 2, false},     {0x58, "CLI", 2, false},       {0x78, "SEI", 2, false},
    {0xB8, "CLV", 2, false},     {0xD8, "CLD", 2, false},       {0xF8, "SED", 2, false},

    // Stack and control flow.
    {0x48, "PHA", 3, false},     {0x08, "PHP", 3, false},       {0x68, "PLA", 4, false},
    {0x28, "PLP", 4, false},     {0x40, "RTI", 6, false},       {0x60, "RTS", 6, false},
    {0x20, "JSR", 6, false},     {0x4C, "JMP abs", 3, false},   {0x6C, "JMP (ind)", 5, false},
    {0x00, "BRK", 7, false},

    // Undocumented, read forms: pay the oops cycle.
    {0xA3, "LAX (zp,X)", 6, false}, {0xA7, "LAX zp", 3, false}, {0xAF, "LAX abs", 4, false},
    {0xB3, "LAX (zp),Y", 5, true},  {0xB7, "LAX zp,Y", 4, false}, {0xBF, "LAX abs,Y", 4, true},

    // Undocumented stores: fixed cost.
    {0x83, "SAX (zp,X)", 6, false}, {0x87, "SAX zp", 3, false}, {0x8F, "SAX abs", 4, false},
    {0x97, "SAX zp,Y", 4, false},

    // Undocumented read-modify-writes: fixed cost, never an oops cycle. These
    // are the rows most at risk of being wrongly given the penalty.
    {0x03, "SLO (zp,X)", 8, false}, {0x07, "SLO zp", 5, false}, {0x0F, "SLO abs", 6, false},
    {0x13, "SLO (zp),Y", 8, false}, {0x17, "SLO zp,X", 6, false}, {0x1B, "SLO abs,Y", 7, false},
    {0x1F, "SLO abs,X", 7, false},
    {0x23, "RLA (zp,X)", 8, false}, {0x27, "RLA zp", 5, false}, {0x2F, "RLA abs", 6, false},
    {0x33, "RLA (zp),Y", 8, false}, {0x37, "RLA zp,X", 6, false}, {0x3B, "RLA abs,Y", 7, false},
    {0x3F, "RLA abs,X", 7, false},
    {0x43, "SRE (zp,X)", 8, false}, {0x47, "SRE zp", 5, false}, {0x4F, "SRE abs", 6, false},
    {0x53, "SRE (zp),Y", 8, false}, {0x57, "SRE zp,X", 6, false}, {0x5B, "SRE abs,Y", 7, false},
    {0x5F, "SRE abs,X", 7, false},
    {0x63, "RRA (zp,X)", 8, false}, {0x67, "RRA zp", 5, false}, {0x6F, "RRA abs", 6, false},
    {0x73, "RRA (zp),Y", 8, false}, {0x77, "RRA zp,X", 6, false}, {0x7B, "RRA abs,Y", 7, false},
    {0x7F, "RRA abs,X", 7, false},
    {0xC3, "DCP (zp,X)", 8, false}, {0xC7, "DCP zp", 5, false}, {0xCF, "DCP abs", 6, false},
    {0xD3, "DCP (zp),Y", 8, false}, {0xD7, "DCP zp,X", 6, false}, {0xDB, "DCP abs,Y", 7, false},
    {0xDF, "DCP abs,X", 7, false},
    {0xE3, "ISC (zp,X)", 8, false}, {0xE7, "ISC zp", 5, false}, {0xEF, "ISC abs", 6, false},
    {0xF3, "ISC (zp),Y", 8, false}, {0xF7, "ISC zp,X", 6, false}, {0xFB, "ISC abs,Y", 7, false},
    {0xFF, "ISC abs,X", 7, false},

    // Undocumented immediate forms.
    {0x0B, "ANC #", 2, false},   {0x2B, "ANC #", 2, false},     {0x4B, "ALR #", 2, false},

    // Undocumented NOPs, including the abs,X reads that do pay the oops cycle.
    {0x1A, "NOP impl", 2, false}, {0x80, "NOP #", 2, false},    {0x04, "NOP zp", 3, false},
    {0x14, "NOP zp,X", 4, false}, {0x0C, "NOP abs", 4, false},  {0x1C, "NOP abs,X", 4, true},
    {0x3C, "NOP abs,X", 4, true}, {0x5C, "NOP abs,X", 4, true}, {0x7C, "NOP abs,X", 4, true},
    {0xDC, "NOP abs,X", 4, true}, {0xFC, "NOP abs,X", 4, true},
};
// clang-format on

// ---------------------------------------------------------------------------
// Layer 1: a minimal parser for the SingleStepTests JSON schema.
//
// Purpose-built rather than generic, but it validates as it goes: anything that
// does not match the expected shape throws, so a truncated or malformed
// download surfaces as a parse error instead of a silently-skipped assertion.
// ---------------------------------------------------------------------------

struct CpuState {
    uint16_t pc = 0;
    uint8_t s = 0;
    uint8_t a = 0;
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t p = 0;
    std::vector<std::pair<uint16_t, uint8_t>> ram;
};

struct VectorCase {
    std::string name;
    CpuState initial;
    CpuState expected;
    // The full expected bus trace. Its length is the instruction's cycle count,
    // since the 6502 performs exactly one bus access per cycle.
    std::vector<BusAccess> cycles;

    size_t cycle_count() const { return cycles.size(); }
};

class JsonParser
{
public:
    explicit JsonParser(std::string text) : text_{std::move(text)} {}

    std::vector<VectorCase> parse_cases()
    {
        std::vector<VectorCase> cases;

        skip_ws();
        expect('[');
        skip_ws();

        if (peek() == ']') {
            advance();
            return cases;
        }

        while (true) {
            cases.push_back(parse_case());
            skip_ws();

            if (peek() == ',') {
                advance();
                skip_ws();
                continue;
            }

            expect(']');
            break;
        }

        return cases;
    }

private:
    [[noreturn]] void fail(const std::string& what) const
    {
        std::ostringstream oss;
        oss << "malformed vector JSON at byte " << pos_ << ": " << what;
        throw std::runtime_error(oss.str());
    }

    char peek() const
    {
        if (pos_ >= text_.size()) {
            throw std::runtime_error("unexpected end of vector JSON");
        }
        return text_[pos_];
    }

    void advance() { ++pos_; }

    void skip_ws()
    {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' || text_[pos_] == '\t')) {
            ++pos_;
        }
    }

    void expect(const char c)
    {
        if (pos_ >= text_.size() || text_[pos_] != c) {
            fail(std::string("expected '") + c + "'");
        }
        ++pos_;
    }

    uint32_t parse_number()
    {
        skip_ws();

        const size_t start = pos_;
        while (pos_ < text_.size() && (text_[pos_] == '-' || (text_[pos_] >= '0' && text_[pos_] <= '9'))) {
            ++pos_;
        }

        if (pos_ == start) {
            fail("expected a number");
        }

        return static_cast<uint32_t>(std::stol(text_.substr(start, pos_ - start)));
    }

    std::string parse_string()
    {
        skip_ws();
        expect('"');

        const size_t start = pos_;
        while (pos_ < text_.size() && text_[pos_] != '"') {
            ++pos_;
        }

        if (pos_ >= text_.size()) {
            fail("unterminated string");
        }

        std::string value = text_.substr(start, pos_ - start);
        advance();
        return value;
    }

    // [ addr, value ] pairs, or [ addr, value, "read"/"write" ] for cycles.
    // Only the count matters for cycles, so the third element is skipped.
    std::vector<std::pair<uint16_t, uint8_t>> parse_ram()
    {
        std::vector<std::pair<uint16_t, uint8_t>> ram;

        skip_ws();
        expect('[');
        skip_ws();

        if (peek() == ']') {
            advance();
            return ram;
        }

        while (true) {
            skip_ws();
            expect('[');
            const uint32_t addr = parse_number();
            skip_ws();
            expect(',');
            const uint32_t value = parse_number();
            skip_ws();
            expect(']');

            ram.emplace_back(static_cast<uint16_t>(addr), static_cast<uint8_t>(value));

            skip_ws();
            if (peek() == ',') {
                advance();
                continue;
            }

            expect(']');
            break;
        }

        return ram;
    }

    // Parses the whole per-cycle bus trace rather than just its length. Each
    // entry is [address, value, "read"|"write"] and describes the single bus
    // access the 6502 performs on that cycle.
    std::vector<BusAccess> parse_cycle_trace()
    {
        std::vector<BusAccess> trace;

        skip_ws();
        expect('[');
        skip_ws();

        if (peek() == ']') {
            advance();
            return trace;
        }

        while (true) {
            skip_ws();
            expect('[');
            const long addr = parse_number();
            skip_ws();
            expect(',');
            const long value = parse_number();
            skip_ws();
            expect(',');
            const std::string kind = parse_string();
            skip_ws();
            expect(']');

            if (kind != "read" && kind != "write") {
                fail("unrecognised bus access kind: " + kind);
            }
            trace.push_back(BusAccess{static_cast<uint16_t>(addr), static_cast<uint8_t>(value), kind == "write"});

            skip_ws();
            if (peek() == ',') {
                advance();
                continue;
            }

            expect(']');
            break;
        }

        return trace;
    }

    CpuState parse_state()
    {
        CpuState state;
        bool seen_pc = false;
        bool seen_ram = false;

        skip_ws();
        expect('{');
        skip_ws();

        while (true) {
            const std::string key = parse_string();
            skip_ws();
            expect(':');

            if (key == "pc") {
                state.pc = static_cast<uint16_t>(parse_number());
                seen_pc = true;
            } else if (key == "s") {
                state.s = static_cast<uint8_t>(parse_number());
            } else if (key == "a") {
                state.a = static_cast<uint8_t>(parse_number());
            } else if (key == "x") {
                state.x = static_cast<uint8_t>(parse_number());
            } else if (key == "y") {
                state.y = static_cast<uint8_t>(parse_number());
            } else if (key == "p") {
                state.p = static_cast<uint8_t>(parse_number());
            } else if (key == "ram") {
                state.ram = parse_ram();
                seen_ram = true;
            } else {
                fail("unknown key in CPU state: " + key);
            }

            skip_ws();
            if (peek() == ',') {
                advance();
                skip_ws();
                continue;
            }

            expect('}');
            break;
        }

        if (!seen_pc || !seen_ram) {
            fail("CPU state is missing pc or ram");
        }

        return state;
    }

    VectorCase parse_case()
    {
        VectorCase test_case;
        bool seen_cycles = false;

        skip_ws();
        expect('{');
        skip_ws();

        while (true) {
            const std::string key = parse_string();
            skip_ws();
            expect(':');

            if (key == "name") {
                test_case.name = parse_string();
            } else if (key == "initial") {
                test_case.initial = parse_state();
            } else if (key == "final") {
                test_case.expected = parse_state();
            } else if (key == "cycles") {
                test_case.cycles = parse_cycle_trace();
                seen_cycles = true;
            } else {
                fail("unknown key in test case: " + key);
            }

            skip_ws();
            if (peek() == ',') {
                advance();
                skip_ws();
                continue;
            }

            expect('}');
            break;
        }

        if (!seen_cycles) {
            fail("test case is missing its cycles array");
        }

        return test_case;
    }

    std::string text_;
    size_t pos_ = 0;
};

std::string vectors_path(const std::string& opcode_hex)
{
    return std::string(NES_TEST_FILES_DIR) + "/single_step_tests/" + opcode_hex + ".json";
}

}  // namespace

// ---------------------------------------------------------------------------
// Layer 1: run every SingleStepTests case for one opcode.
// ---------------------------------------------------------------------------

class SingleStepVectors : public ::testing::TestWithParam<std::string>
{
};

TEST_P(SingleStepVectors, matches_hardware_vectors)
{
    const std::string opcode_hex = GetParam();
    const std::string path = vectors_path(opcode_hex);

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        GTEST_SKIP() << "vectors not present: " << path << "\n"
                     << "run tests/test_files/fetch_single_step_tests.sh to enable this oracle";
    }

    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(text.empty()) << "empty vector file: " << path;

    const std::vector<VectorCase> cases = JsonParser(text).parse_cases();
    ASSERT_FALSE(cases.empty()) << "no test cases parsed from " << path;

    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    size_t failures = 0;
    constexpr size_t kMaxReported = 5;

    for (const VectorCase& test_case : cases) {
        mem.memory.fill(0);
        for (const auto& [addr, value] : test_case.initial.ram) {
            mem.memory[addr] = value;
        }

        cpu.registers.PC = test_case.initial.pc;
        cpu.registers.SP = test_case.initial.s;
        cpu.registers.A = test_case.initial.a;
        cpu.registers.X = test_case.initial.x;
        cpu.registers.Y = test_case.initial.y;
        cpu.registers.P = test_case.initial.p;
        cpu.cycles_left = 0;

        const size_t cycles = run_one_instruction(cpu);

        // Collect every mismatch for this case, then report the case once.
        std::ostringstream problems;

        if (cycles != test_case.cycle_count()) {
            problems << "\n  cycles:   expected " << test_case.cycle_count() << ", got " << cycles;
        }
        if (cpu.registers.PC != test_case.expected.pc) {
            problems << "\n  PC:       expected " << std::hex << test_case.expected.pc << ", got " << cpu.registers.PC
                     << std::dec;
        }
        if (cpu.registers.SP != test_case.expected.s) {
            problems << "\n  SP:       expected " << std::hex << static_cast<unsigned>(test_case.expected.s) << ", got "
                     << static_cast<unsigned>(cpu.registers.SP) << std::dec;
        }
        if (cpu.registers.A != test_case.expected.a) {
            problems << "\n  A:        expected " << std::hex << static_cast<unsigned>(test_case.expected.a) << ", got "
                     << static_cast<unsigned>(cpu.registers.A) << std::dec;
        }
        if (cpu.registers.X != test_case.expected.x) {
            problems << "\n  X:        expected " << std::hex << static_cast<unsigned>(test_case.expected.x) << ", got "
                     << static_cast<unsigned>(cpu.registers.X) << std::dec;
        }
        if (cpu.registers.Y != test_case.expected.y) {
            problems << "\n  Y:        expected " << std::hex << static_cast<unsigned>(test_case.expected.y) << ", got "
                     << static_cast<unsigned>(cpu.registers.Y) << std::dec;
        }
        if (cpu.registers.P != test_case.expected.p) {
            problems << "\n  P:        expected " << std::hex << static_cast<unsigned>(test_case.expected.p) << ", got "
                     << static_cast<unsigned>(cpu.registers.P) << std::dec;
        }

        for (const auto& [addr, value] : test_case.expected.ram) {
            if (mem.memory[addr] != value) {
                problems << "\n  mem[" << std::hex << addr << "]: expected " << static_cast<unsigned>(value) << ", got "
                         << static_cast<unsigned>(mem.memory[addr]) << std::dec;
            }
        }

        const std::string detail = problems.str();
        if (!detail.empty()) {
            ++failures;
            if (failures <= kMaxReported) {
                ADD_FAILURE() << "opcode $" << opcode_hex << " case \"" << test_case.name << "\"" << detail;
            }
        }
    }

    EXPECT_EQ(failures, 0u) << failures << " of " << cases.size() << " vectors failed for opcode $" << opcode_hex
                            << (failures > kMaxReported ? " (only the first 5 shown)" : "");
}

// ---------------------------------------------------------------------------
// Per-cycle bus trace.
//
// The test above checks where an instruction ENDS UP. This one checks HOW IT
// GETS THERE: the address, value and direction of the single bus access the
// 6502 performs on each of its cycles.
//
// That distinction is the whole point. This core performs an instruction's
// memory effect in two lumps -- all reads during addressing on the first cycle,
// the rest on the last -- which lands on the correct final state via the wrong
// accesses at the wrong times. Real hardware also issues accesses that this
// core does not make at all: the dummy read at the un-carried address when an
// indexed read crosses a page, and the dummy write of the OLD value that every
// read-modify-write performs before writing the new one. Those phantom accesses
// are observable: a dummy read of $2002 clears vblank, a dummy write to $2007
// corrupts VRAM. They are why Blargg's PPU timing ROMs fail.
//
// EXPECTED TO FAIL BROADLY until the CPU is cycle-stepped. The number of
// opcodes passing here is the scalar that work drives upward. Do NOT weaken
// this to make the suite green.
// ---------------------------------------------------------------------------

class SingleStepBusTrace : public ::testing::TestWithParam<std::string>
{
};

// All 256 opcodes. Unlike the final-state suite above, which is restricted to
// the opcodes this core reworked, the trace suite covers everything: the cycle
// schedule is a property of the addressing mode and access class, so a gap
// anywhere hides a whole family of wrong schedules.
std::vector<std::string> all_opcodes()
{
    std::vector<std::string> out;
    out.reserve(256);
    for (int i = 0; i < 256; ++i) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", i);
        out.emplace_back(buf);
    }
    return out;
}

TEST_P(SingleStepBusTrace, matches_hardware_bus_trace)
{
    const std::string opcode_hex = GetParam();
    const std::string path = vectors_path(opcode_hex);

    std::ifstream file(path);
    if (!file.is_open()) {
        GTEST_SKIP() << "vectors absent: " << path << " (run tests/test_files/fetch_single_step_tests.sh)";
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::vector<VectorCase> cases = JsonParser(buffer.str()).parse_cases();
    ASSERT_FALSE(cases.empty()) << "no cases parsed from " << path;

    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    size_t failures = 0;
    constexpr size_t kMaxReported = 2;

    for (const VectorCase& test_case : cases) {
        mem.memory.fill(0);
        for (const auto& [addr, value] : test_case.initial.ram) {
            mem.memory[addr] = value;
        }

        cpu.registers.PC = test_case.initial.pc;
        cpu.registers.SP = test_case.initial.s;
        cpu.registers.A = test_case.initial.a;
        cpu.registers.X = test_case.initial.x;
        cpu.registers.Y = test_case.initial.y;
        cpu.registers.P = test_case.initial.p;
        cpu.cycles_left = 0;

        mem.trace.clear();
        mem.recording = true;
        run_one_instruction(cpu);
        mem.recording = false;

        if (mem.trace == test_case.cycles) {
            continue;
        }

        ++failures;
        if (failures <= kMaxReported) {
            std::ostringstream detail;
            const size_t n = std::max(mem.trace.size(), test_case.cycles.size());
            for (size_t i = 0; i < n; ++i) {
                const bool have_exp = i < test_case.cycles.size();
                const bool have_got = i < mem.trace.size();
                const bool same = have_exp && have_got && test_case.cycles[i] == mem.trace[i];
                detail << "\n    cycle " << (i + 1) << (same ? "   " : " * ")
                       << (have_exp ? to_string(test_case.cycles[i]) : std::string("(none)")) << "   |   "
                       << (have_got ? to_string(mem.trace[i]) : std::string("(none)"));
            }
            ADD_FAILURE() << "opcode $" << opcode_hex << " case \"" << test_case.name << "\""
                          << "\n  expected " << test_case.cycles.size() << " accesses, made " << mem.trace.size()
                          << "\n    cycle    EXPECTED (hardware)      |   ACTUAL (this core)" << detail.str();
        }
    }

    EXPECT_EQ(failures, 0u) << failures << " of " << cases.size() << " vectors had a wrong bus trace for opcode $"
                            << opcode_hex << (failures > kMaxReported ? " (only the first 2 shown)" : "");
}

INSTANTIATE_TEST_SUITE_P(AllOpcodes,
                         SingleStepBusTrace,
                         ::testing::ValuesIn(all_opcodes()),
                         [](const ::testing::TestParamInfo<std::string>& info) { return "op_" + info.param; });

// Every opcode implemented or restructured by this core that has vectors
// fetched for it. Kept in sync with tests/test_files/fetch_single_step_tests.sh.
// Every opcode, not a hand-maintained subset.
//
// This suite used to list ~90 opcodes chosen by which ones a past change had
// touched. That let $AB sit wrong indefinitely: it was mapped to LAX (A = imm;
// X = A) instead of LXA ((A | magic) & imm), and 4422 of its 10,000 vectors
// disagreed - but "ab" was not in the list, and its bus trace is a bare operand
// fetch that is correct either way, so nothing caught it.
//
// A hand-maintained list of what to verify will always drift behind the code it
// verifies. Running all 256 costs about a minute and removes the failure mode.
INSTANTIATE_TEST_SUITE_P(AllOpcodes,
                         SingleStepVectors,
                         ::testing::ValuesIn(all_opcodes()),
                         [](const ::testing::TestParamInfo<std::string>& info) { return "op_" + info.param; });

// ---------------------------------------------------------------------------
// Layer 2: cycle counts against the hand-transcribed reference table.
// ---------------------------------------------------------------------------

namespace
{

// Lays out `opcode` at $1000 with two operand bytes and runs it, with the
// operand chosen so the effective address does NOT cross a page.
size_t measure_without_page_cross(const uint8_t opcode)
{
    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    // Indexing by 1 from $2000 stays inside the page; zero-page pointers are
    // set up to resolve to $2000 as well.
    mem.memory[0x1000] = opcode;
    mem.memory[0x1001] = 0x00;
    mem.memory[0x1002] = 0x20;

    // Zero-page pointer at $00/$01 -> $2000, for the (zp,X) and (zp),Y forms.
    mem.memory[0x0000] = 0x00;
    mem.memory[0x0001] = 0x20;

    cpu.registers.PC = 0x1000;
    cpu.registers.SP = 0xFD;
    cpu.registers.A = 0;
    cpu.registers.X = 0;
    cpu.registers.Y = 1;
    cpu.registers.P = 0x24;
    cpu.cycles_left = 0;

    return run_one_instruction(cpu);
}

// Runs `opcode` with an absolute base address and an index, returning the cycle
// count. The index goes in both X and Y so the same helper serves the abs,X and
// abs,Y forms.
size_t measure_indexed(const uint8_t opcode, const uint16_t base_address, const uint8_t index)
{
    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    mem.memory[0x1000] = opcode;
    mem.memory[0x1001] = static_cast<uint8_t>(base_address & 0xFF);
    mem.memory[0x1002] = static_cast<uint8_t>(base_address >> 8);

    cpu.registers.PC = 0x1000;
    cpu.registers.SP = 0xFD;
    cpu.registers.A = 0;
    cpu.registers.X = index;
    cpu.registers.Y = index;
    cpu.registers.P = 0x24;
    cpu.cycles_left = 0;

    return run_one_instruction(cpu);
}

// Places a branch at `address` with the given offset and returns its cycle
// count. Only the carry flag is configurable, which covers BCC/BCS; the other
// six are exercised against the vectors and by the agreement test below.
size_t measure_branch(const uint8_t opcode, const bool carry, const uint16_t address, const uint8_t offset)
{
    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    mem.memory[address] = opcode;
    mem.memory[static_cast<uint16_t>(address + 1)] = offset;

    cpu.registers.PC = address;
    cpu.registers.SP = 0xFD;
    cpu.registers.P = 0x24;
    cpu.set_flag(CPU::FLAGS::C, carry);
    cpu.cycles_left = 0;

    return run_one_instruction(cpu);
}

}  // namespace

TEST(CpuCycleCounts, base_cost_matches_published_reference)
{
    for (const ReferenceTiming& timing : kReferenceTimings) {
        // Branches are covered separately: their cost depends on the condition
        // and the target, so there is no single "base" measurement here.
        if (InstructionSet::Table[timing.opcode].addr_type == Addressing::relative) {
            continue;
        }

        const size_t measured = measure_without_page_cross(timing.opcode);

        EXPECT_EQ(measured, timing.base) << "opcode $" << std::hex << static_cast<unsigned>(timing.opcode) << std::dec
                                         << " (" << timing.mnemonic << ") took " << measured
                                         << " cycles with no page cross, reference says "
                                         << static_cast<unsigned>(timing.base);
    }
}

TEST(CpuCycleCounts, page_cross_penalty_applies_only_to_read_forms)
{
    for (const ReferenceTiming& timing : kReferenceTimings) {
        const Addressing mode = InstructionSet::Table[timing.opcode].addr_type;

        const bool is_indexed =
            mode == Addressing::absolute_X || mode == Addressing::absolute_Y || mode == Addressing::indirect_indexed;
        if (!is_indexed) {
            // Anything not indexed cannot cross a page, so the reference must
            // not be claiming a penalty for it.
            EXPECT_FALSE(timing.oops) << "reference table marks a non-indexed opcode $" << std::hex
                                      << static_cast<unsigned>(timing.opcode) << std::dec
                                      << " as paying the oops cycle";
            continue;
        }

        FlatMemory mem;
        CPU cpu = make_cpu(mem);

        // Base address $20FF indexed by 1 lands at $2100: a page cross.
        mem.memory[0x1000] = timing.opcode;
        mem.memory[0x1001] = 0xFF;
        mem.memory[0x1002] = 0x20;

        // For the (zp),Y forms the operand byte above is the zero-page address
        // of the pointer, i.e. $FF - so the pointer's low byte lives at $FF and
        // its high byte wraps round to $00, giving $20FF. Indexed by 1 that is
        // $2100, the same page cross the absolute forms above see.
        mem.memory[0x00FF] = 0xFF;
        mem.memory[0x0000] = 0x20;

        cpu.registers.PC = 0x1000;
        cpu.registers.SP = 0xFD;
        cpu.registers.A = 0;
        cpu.registers.X = 1;
        cpu.registers.Y = 1;
        cpu.registers.P = 0x24;
        cpu.cycles_left = 0;

        const size_t measured = run_one_instruction(cpu);
        const size_t expected = timing.base + (timing.oops ? 1u : 0u);

        EXPECT_EQ(measured, expected) << "opcode $" << std::hex << static_cast<unsigned>(timing.opcode) << std::dec
                                      << " (" << timing.mnemonic << ") took " << measured
                                      << " cycles across a page boundary, reference says " << expected;
    }
}

// The specific regression: a store must not pay the read form's penalty.
TEST(CpuCycleCounts, store_does_not_pay_the_page_cross_penalty)
{
    // LDA $C0FF,X with X=1 reads across a page: 4 base + 1 = 5.
    EXPECT_EQ(measure_indexed(0xBD, 0xC0FF, 1), 5u) << "LDA abs,X across a page should cost 5 cycles";

    // LDA $C000,X with X=1 stays on the page: 4.
    EXPECT_EQ(measure_indexed(0xBD, 0xC000, 1), 4u) << "LDA abs,X within a page should cost 4 cycles";

    // STA $C0FF,X with X=1 also crosses, but stores have a fixed cost: 5, NOT 6.
    EXPECT_EQ(measure_indexed(0x9D, 0xC0FF, 1), 5u)
        << "STA abs,X must cost 5 cycles even across a page boundary - the oops cycle is already "
           "included in a store's fixed cost";

    // STA $C000,X with X=1: still 5.
    EXPECT_EQ(measure_indexed(0x9D, 0xC000, 1), 5u) << "STA abs,X should cost 5 cycles regardless of page crossing";

    // ASL $C0FF,X read-modify-write: always 7.
    EXPECT_EQ(measure_indexed(0x1E, 0xC0FF, 1), 7u) << "ASL abs,X must cost 7 cycles even across a page boundary";
    EXPECT_EQ(measure_indexed(0x1E, 0xC000, 1), 7u) << "ASL abs,X should cost 7 cycles regardless of page crossing";

    // The undocumented read-modify-writes behave the same way.
    EXPECT_EQ(measure_indexed(0x1F, 0xC0FF, 1), 7u) << "SLO abs,X must cost 7 cycles even across a page boundary";
    EXPECT_EQ(measure_indexed(0xFF, 0xC0FF, 1), 7u) << "ISC abs,X must cost 7 cycles even across a page boundary";
    EXPECT_EQ(measure_indexed(0xDF, 0xC0FF, 1), 7u) << "DCP abs,X must cost 7 cycles even across a page boundary";
}

// ---------------------------------------------------------------------------
// Layer 3: branch timing, and the double-execution regression.
// ---------------------------------------------------------------------------

TEST(CpuBranches, timing_depends_on_taken_and_target_page)
{
    // BCS with carry clear: not taken, 2 cycles.
    EXPECT_EQ(measure_branch(0xB0, /*carry=*/false, 0x1000, 0x10), 2u) << "a branch that is not taken costs 2 cycles";

    // BCS with carry set, target on the same page: 3 cycles.
    EXPECT_EQ(measure_branch(0xB0, /*carry=*/true, 0x1000, 0x10), 3u)
        << "a taken branch within the same page costs 3 cycles";

    // BCS at $10F0: PC after the branch is $10F2, +$70 = $1162, a different
    // page, so 4 cycles.
    EXPECT_EQ(measure_branch(0xB0, /*carry=*/true, 0x10F0, 0x70), 4u)
        << "a taken branch onto a different page costs 4 cycles";

    // Backwards across a page boundary is the same: PC after is $1001,
    // -$10 = $0FF1, a different page.
    EXPECT_EQ(measure_branch(0xB0, /*carry=*/true, 0x0FFF, 0xF0), 4u)
        << "a taken backwards branch onto a different page costs 4 cycles";
}

// The trap this core was structurally exposed to: an operation that extends its
// own cycle budget pushes cycles_left from 0 back to 1 after it has already
// run. clock() then reports the instruction complete, but the next clock() sees
// a non-zero cycles_left, skips fetch/decode, and runs the same instruction a
// second time. Nothing fails visibly - the CPU just silently executes twice.
//
// INC $20 is used as the witness: if the branch instruction were executed
// twice, the counter would advance by two rather than one.
TEST(CpuBranches, taken_branch_executes_exactly_once)
{
    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    // $1000: BCS +2   -> jumps over the INC at $1002, landing on the INC at $1004
    // $1002: INC $20  (must NOT run)
    // $1004: INC $21  (must run exactly once)
    mem.memory[0x1000] = 0xB0;  // BCS
    mem.memory[0x1001] = 0x02;
    mem.memory[0x1002] = 0xE6;  // INC zp
    mem.memory[0x1003] = 0x20;
    mem.memory[0x1004] = 0xE6;  // INC zp
    mem.memory[0x1005] = 0x21;

    mem.memory[0x0020] = 0;
    mem.memory[0x0021] = 0;

    cpu.registers.PC = 0x1000;
    cpu.registers.SP = 0xFD;
    cpu.registers.P = 0x24;
    cpu.set_flag(CPU::FLAGS::C, true);
    cpu.cycles_left = 0;

    const size_t branch_cycles = run_one_instruction(cpu);
    EXPECT_EQ(branch_cycles, 3u) << "taken branch within a page should take 3 cycles";
    EXPECT_EQ(cpu.registers.PC, 0x1004) << "the branch should have landed at $1004";

    // Run the instruction the branch landed on.
    const size_t inc_cycles = run_one_instruction(cpu);
    EXPECT_EQ(inc_cycles, 5u) << "INC zp should take 5 cycles";

    EXPECT_EQ(mem.memory[0x0020], 0u) << "the branched-over INC must not have run";
    EXPECT_EQ(mem.memory[0x0021], 1u)
        << "the instruction at the branch target ran " << static_cast<unsigned>(mem.memory[0x0021])
        << " times; exactly one is required. More than one means clock() re-entered an instruction "
           "that had already executed.";
    EXPECT_EQ(cpu.registers.PC, 0x1006) << "PC should have advanced past the INC exactly once";
}

// Cross-check for the decode-time branch prediction in CPU::branch_is_taken()
// against the BPL()/BMI()/... implementations, which are the ones that actually
// move PC. The two encode the same condition in different ways, so this pins
// them together: for all eight branches in both flag states, the cycle count
// (which comes from the prediction) must agree with whether PC actually moved
// (which comes from the operation).
TEST(CpuBranches, predicted_cost_agrees_with_whether_the_branch_was_taken)
{
    struct BranchCase {
        uint8_t opcode;
        const char* mnemonic;
        CPU::FLAGS flag;
        bool branches_when_flag_set;
    };

    constexpr BranchCase branches[] = {
        {0x10, "BPL", CPU::FLAGS::N, false}, {0x30, "BMI", CPU::FLAGS::N, true},  {0x50, "BVC", CPU::FLAGS::V, false},
        {0x70, "BVS", CPU::FLAGS::V, true},  {0x90, "BCC", CPU::FLAGS::C, false}, {0xB0, "BCS", CPU::FLAGS::C, true},
        {0xD0, "BNE", CPU::FLAGS::Z, false}, {0xF0, "BEQ", CPU::FLAGS::Z, true},
    };

    for (const BranchCase& branch : branches) {
        for (const bool flag_value : {false, true}) {
            FlatMemory mem;
            CPU cpu = make_cpu(mem);

            mem.memory[0x1000] = branch.opcode;
            mem.memory[0x1001] = 0x10;

            cpu.registers.PC = 0x1000;
            cpu.registers.SP = 0xFD;
            cpu.registers.P = 0x24;
            cpu.set_flag(branch.flag, flag_value);
            cpu.cycles_left = 0;

            const size_t cycles = run_one_instruction(cpu);
            const bool pc_moved = cpu.registers.PC == 0x1012;
            const bool should_branch = (flag_value == branch.branches_when_flag_set);

            EXPECT_EQ(pc_moved, should_branch)
                << branch.mnemonic << " with its flag " << (flag_value ? "set" : "clear") << " branched the wrong way";

            // The cycle count is derived from the decode-time prediction; PC is
            // moved by the operation. They must tell the same story.
            EXPECT_EQ(cycles, pc_moved ? 3u : 2u)
                << branch.mnemonic << " with its flag " << (flag_value ? "set" : "clear") << " took " << cycles
                << " cycles but " << (pc_moved ? "did" : "did not")
                << " branch; the decode-time prediction in "
                   "CPU::branch_is_taken() disagrees with the operation itself";
        }
    }
}

// ---------------------------------------------------------------------------
// Layer 3: the (zp),Y pointer wrap.
// ---------------------------------------------------------------------------

// The pointer for (zp),Y lives in zero page and its high byte comes from the
// next zero-page location, wrapping within the page. For a pointer of $FF the
// high byte must come from $00, not from $0100.
TEST(CpuAddressing, indirect_indexed_pointer_wraps_within_zero_page)
{
    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    // LDA ($FF),Y with Y=0.
    mem.memory[0x1000] = 0xB1;
    mem.memory[0x1001] = 0xFF;

    // The pointer: low byte at $FF, high byte wrapping round to $00.
    mem.memory[0x00FF] = 0x34;
    mem.memory[0x0000] = 0x12;

    // The address the wrap should NOT produce: $0100 holds a decoy high byte.
    mem.memory[0x0100] = 0xAB;

    mem.memory[0x1234] = 0x42;  // via the correct pointer $1234
    mem.memory[0xAB34] = 0x99;  // via the incorrect pointer $AB34

    cpu.registers.PC = 0x1000;
    cpu.registers.SP = 0xFD;
    cpu.registers.Y = 0;
    cpu.registers.P = 0x24;
    cpu.cycles_left = 0;

    run_one_instruction(cpu);

    EXPECT_EQ(cpu.registers.A, 0x42)
        << "LDA ($FF),Y should read through pointer $1234 (high byte wrapping to $00), not $AB34 "
           "(high byte taken from $0100)";
}

// The same wrap, in the indexed-indirect direction, which was already correct.
// Kept so a future change cannot regress one while fixing the other.
TEST(CpuAddressing, indexed_indirect_pointer_wraps_within_zero_page)
{
    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    // LDA ($FE,X) with X=1, so the pointer is $FF and its high byte wraps to $00.
    mem.memory[0x1000] = 0xA1;
    mem.memory[0x1001] = 0xFE;

    mem.memory[0x00FF] = 0x34;
    mem.memory[0x0000] = 0x12;
    mem.memory[0x0100] = 0xAB;

    mem.memory[0x1234] = 0x42;
    mem.memory[0xAB34] = 0x99;

    cpu.registers.PC = 0x1000;
    cpu.registers.SP = 0xFD;
    cpu.registers.X = 1;
    cpu.registers.P = 0x24;
    cpu.cycles_left = 0;

    run_one_instruction(cpu);

    EXPECT_EQ(cpu.registers.A, 0x42) << "LDA ($FE,X) with X=1 should read through pointer $1234";
}

// ---------------------------------------------------------------------------
// Layer 3: interrupt timing and power-on state.
// ---------------------------------------------------------------------------

// The CPU polls the interrupt lines on an instruction's PENULTIMATE cycle, and
// acts on that poll at the following instruction boundary. An interrupt raised
// with no instruction in flight is therefore not something hardware can be in:
// there is no penultimate cycle for the poll to happen on, and nothing would
// ever latch it. Both tests below consequently assert the line DURING a
// preceding NOP - raised before its first clock() call, which for a two-cycle
// instruction is its penultimate cycle - and then check the sequence that
// follows. What they assert about the sequence itself is unchanged.
//
// The NOP at $1001 is load-bearing rather than padding. FlatMemory reads back
// $00 where nothing was written, which is BRK: a 7-cycle sequence vectoring
// through $FFFE/$FFFF. Without it, an IRQ that was silently never taken would
// still produce "7 cycles, PC = $9000" and the test would pass on the wrong
// instruction entirely. The pushed copy of P is checked for the same reason -
// B is set by BRK and clear for a hardware interrupt, and it is the only thing
// in the two sequences that differs.

TEST(CpuInterrupts, nmi_takes_seven_cycles)
{
    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    mem.memory[0x1000] = 0xEA;  // NOP, the instruction the NMI arrives during
    mem.memory[0x1001] = 0xEA;  // NOP, so "no interrupt taken" is 2 cycles, not 7
    mem.memory[0xFFFA] = 0x00;
    mem.memory[0xFFFB] = 0x90;

    cpu.registers.PC = 0x1000;
    cpu.registers.SP = 0xFD;
    cpu.registers.P = 0x24;
    cpu.cycles_left = 0;

    cpu.raise_NMI();
    ASSERT_EQ(run_one_instruction(cpu), 2u) << "the NOP the NMI arrived during still runs to completion";
    ASSERT_EQ(cpu.registers.PC, 0x1001) << "an NMI must not displace the instruction it arrived during";

    EXPECT_EQ(run_one_instruction(cpu), 7u) << "the NMI sequence takes 7 cycles on hardware, not 8";
    EXPECT_EQ(cpu.registers.PC, 0x9000) << "NMI should vector through $FFFA/$FFFB";
    EXPECT_EQ(mem.memory[0x01FB] & static_cast<uint8_t>(CPU::FLAGS::B), 0)
        << "a hardware interrupt pushes P with B clear; a BRK would have set it";
}

TEST(CpuInterrupts, irq_takes_seven_cycles)
{
    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    mem.memory[0x1000] = 0xEA;  // NOP, the instruction the IRQ arrives during
    mem.memory[0x1001] = 0xEA;  // NOP, so "no interrupt taken" is 2 cycles, not 7
    mem.memory[0xFFFE] = 0x00;
    mem.memory[0xFFFF] = 0x90;

    cpu.registers.PC = 0x1000;
    cpu.registers.SP = 0xFD;
    cpu.registers.P = 0x24;  // I clear, so the IRQ is taken
    cpu.set_flag(CPU::FLAGS::I, false);
    cpu.cycles_left = 0;

    cpu.raise_IRQ();
    ASSERT_EQ(run_one_instruction(cpu), 2u) << "the NOP the IRQ arrived during still runs to completion";
    ASSERT_EQ(cpu.registers.PC, 0x1001) << "an IRQ must not displace the instruction it arrived during";

    EXPECT_EQ(run_one_instruction(cpu), 7u) << "the IRQ sequence takes 7 cycles";
    EXPECT_EQ(cpu.registers.PC, 0x9000) << "IRQ should vector through $FFFE/$FFFF";
    EXPECT_EQ(mem.memory[0x01FB] & static_cast<uint8_t>(CPU::FLAGS::B), 0)
        << "a hardware interrupt pushes P with B clear; a BRK would have set it";
}

// An interrupt sequence performs no interrupt polling of its own, which is what
// guarantees at least one instruction of the handler runs before another
// interrupt is serviced. Without that, an NMI arriving during a BRK is taken at
// the BRK's own boundary and the handler never executes an instruction.
//
// Only the Blargg ROMs covered this, and only indirectly. Injecting the edge at
// every cycle of the sequence is cheap and pins it directly.
//
// The sequence does look at /NMI exactly once, on cycle 4, and that is a
// different question from "is there another interrupt to take after this one".
// An NMI latched by then HIJACKS the sequence already in flight: the vector
// fetched on cycles 6/7 becomes $FFFA/$FFFB. So which vector this BRK ends at
// is not constant across the seven injection points, and the table below spells
// out both halves:
//
//   inject on cycle 1-4   hijacked, vectors to $FFFA, edge consumed
//   inject on cycle 5-7   too late, vectors to $FFFE, edge still pending
//
// The five-cycle-wide hijack window is Blargg's 2-nmi_and_brk result, whose ten
// printed rows walk the NMI one clock at a time across the last cycle of the
// instruction before the BRK and the BRK's own cycles 1-6, and show exactly
// five of them taking the NMI vector. Cycles 1-4 here are the four of those
// five that fall inside the sequence; the fifth is covered by
// an_edge_on_the_final_cycle_is_deferred_one_instruction.
//
// Whichever vector is taken, the property this test exists for is unchanged and
// is asserted in both halves: the handler's first instruction runs before any
// further interrupt is serviced.
TEST(CpuInterrupts, an_interrupt_sequence_does_not_poll)
{
    for (int inject_at = 1; inject_at <= 7; ++inject_at) {
        FlatMemory mem;
        CPU cpu = make_cpu(mem);

        mem.memory[0x1000] = 0x00;  // BRK
        mem.memory[0x8000] = 0xEA;  // BRK/IRQ handler: NOP, NOP
        mem.memory[0x8001] = 0xEA;
        mem.memory[0x9000] = 0xEA;  // NMI handler: NOP, NOP
        mem.memory[0x9001] = 0xEA;
        mem.memory[0xFFFA] = 0x00;  // NMI   -> $9000
        mem.memory[0xFFFB] = 0x90;
        mem.memory[0xFFFE] = 0x00;  // BRK   -> $8000
        mem.memory[0xFFFF] = 0x80;

        cpu.registers.PC = 0x1000;
        cpu.registers.SP = 0xFD;
        cpu.registers.P = 0x24;
        cpu.cycles_left = 0;

        // Step the BRK, raising /NMI on the chosen cycle of its sequence.
        size_t sequence_cycles = 0;
        for (int cycle = 1;; ++cycle) {
            if (cycle == inject_at) {
                cpu.raise_NMI();
            }
            ++sequence_cycles;
            if (cpu.clock(false)) {
                break;
            }
            ASSERT_LT(cycle, 32) << "BRK did not complete";
        }

        const bool hijacked = inject_at <= 4;

        // Literal, not derived: a hijack redirects the vector fetch that the
        // sequence was going to make anyway, so it cannot cost extra cycles.
        EXPECT_EQ(sequence_cycles, 7u) << "BRK is seven cycles whether or not an NMI stole its vector"
                                       << " (NMI raised on cycle " << inject_at << ")";

        EXPECT_EQ(cpu.registers.PC, hijacked ? 0x9000 : 0x8000)
            << "NMI raised on BRK cycle " << inject_at << " should" << (hijacked ? " " : " not ")
            << "have redirected the vector fetch to $FFFA";

        // What B records is which instruction STARTED the sequence, not where
        // it ended up: a hijacked BRK still reaches the NMI handler with B set,
        // which is exactly the $36 Blargg's 2-nmi_and_brk prints for its five
        // hijacked rows. SP was $FD, so BRK pushed PCH at $01FD, PCL at $01FC
        // and P at $01FB.
        EXPECT_EQ(mem.memory[0x01FB] & static_cast<uint8_t>(CPU::FLAGS::B), static_cast<uint8_t>(CPU::FLAGS::B))
            << "a hijacked BRK still pushes P with B set (NMI raised on cycle " << inject_at << ")";

        // And the frame is still BRK's: the return address is the byte after
        // BRK's padding byte, $1000 + 2 = $1002.
        EXPECT_EQ(mem.memory[0x01FC], 0x02) << "pushed PCL should be $02 (NMI raised on cycle " << inject_at << ")";
        EXPECT_EQ(mem.memory[0x01FD], 0x10) << "pushed PCH should be $10 (NMI raised on cycle " << inject_at << ")";

        // The property under test, in both halves.
        EXPECT_EQ(run_one_instruction(cpu), 2u)
            << "NMI raised on BRK cycle " << inject_at
            << " was serviced before the handler ran an instruction; an interrupt sequence must not poll";
        EXPECT_EQ(cpu.registers.PC, hijacked ? 0x9001 : 0x8001)
            << "the handler's first instruction should have executed";

        if (hijacked) {
            // The hijack consumed the edge. The second NOP of the NMI handler
            // runs; nothing takes a second NMI.
            EXPECT_EQ(run_one_instruction(cpu), 2u) << "the hijack consumed the /NMI edge, so no second NMI is due";
            EXPECT_EQ(cpu.registers.PC, 0x9002) << "the NMI handler should still be running";
        } else {
            // Too late to hijack, so the edge is still pending and is taken at
            // the first boundary the handler offers.
            EXPECT_EQ(run_one_instruction(cpu), 7u)
                << "an NMI too late to hijack is still pending and due after the handler's first instruction";
            EXPECT_EQ(cpu.registers.PC, 0x9000) << "and it vectors through $FFFA/$FFFB";
        }
    }
}

// The IRQ half of the same window. An IRQ sequence has no BRK opcode behind it,
// so the copy of P it pushes has B CLEAR - and stays clear when an NMI steals
// the vector, for the same reason a hijacked BRK's stays set. Blargg's
// 3-nmi_and_irq prints $20 for its hijacked rows against the $36 of
// 2-nmi_and_brk, and the two differ in nothing but that bit.
TEST(CpuInterrupts, an_nmi_hijacks_an_irq_sequence_without_setting_b)
{
    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    mem.memory[0x1000] = 0xEA;  // NOP: the instruction the IRQ arrives during
    mem.memory[0x1001] = 0xEA;
    mem.memory[0x9000] = 0xEA;  // NMI handler
    mem.memory[0x9001] = 0xEA;
    mem.memory[0xFFFA] = 0x00;  // NMI -> $9000
    mem.memory[0xFFFB] = 0x90;
    mem.memory[0xFFFE] = 0x00;  // IRQ -> $8000
    mem.memory[0xFFFF] = 0x80;

    cpu.registers.PC = 0x1000;
    cpu.registers.SP = 0xFD;
    cpu.registers.P = 0x24;
    cpu.set_flag(CPU::FLAGS::I, false);  // so the IRQ is taken at all
    cpu.cycles_left = 0;

    cpu.raise_IRQ();
    ASSERT_EQ(run_one_instruction(cpu), 2u) << "the NOP the IRQ arrived during still runs to completion";
    ASSERT_EQ(cpu.registers.PC, 0x1001);

    // Now step the IRQ sequence, raising /NMI on its cycle 3 - inside the
    // window, and before the cycle-4 look at the line.
    size_t cycles = 0;
    for (int cycle = 1;; ++cycle) {
        if (cycle == 3) {
            cpu.raise_NMI();
        }
        ++cycles;
        if (cpu.clock(false)) {
            break;
        }
        ASSERT_LT(cycle, 32) << "the IRQ sequence did not complete";
    }

    EXPECT_EQ(cycles, 7u) << "the sequence is seven cycles whether or not the NMI stole its vector";
    EXPECT_EQ(cpu.registers.PC, 0x9000) << "the NMI should have redirected the vector fetch to $FFFA/$FFFB";
    EXPECT_EQ(mem.memory[0x01FB] & static_cast<uint8_t>(CPU::FLAGS::B), 0)
        << "a hijacked IRQ pushes P with B clear; only a BRK sets it";
    // The IRQ arrived during the NOP at $1000, so the sequence pushes $1001 -
    // no padding byte, unlike BRK.
    EXPECT_EQ(mem.memory[0x01FC], 0x01) << "pushed PCL should be $01";
    EXPECT_EQ(mem.memory[0x01FD], 0x10) << "pushed PCH should be $10";
}

// An NMI sequence cannot hijack itself.
//
// poll_interrupt_hijack returns early when servicing_nmi is already set. That
// guard is what preserves a SECOND /NMI edge arriving inside an NMI sequence's
// hijack window: the hijack only selects a vector, it does not consume an edge -
// the latch was already cleared when the sequence began, so a new edge survives
// and is taken after the handler's first instruction.
//
// Without the guard the second edge is silently swallowed, and a review found
// that removing `servicing_nmi ||` left the entire 640-test suite passing.
// Nothing anywhere pinned it, including all five cpu_interrupts_v2 ROMs.
TEST(CpuInterrupts, an_nmi_sequence_does_not_consume_a_second_nmi_edge)
{
    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    mem.memory[0x1000] = 0xEA;  // NOP the first NMI arrives during
    mem.memory[0x1001] = 0xEA;
    mem.memory[0x9000] = 0xEA;  // NMI handler
    mem.memory[0x9001] = 0xEA;
    mem.memory[0xFFFA] = 0x00;  // NMI -> $9000
    mem.memory[0xFFFB] = 0x90;

    cpu.registers.PC = 0x1000;
    cpu.registers.SP = 0xFD;
    cpu.registers.P = 0x24;
    cpu.cycles_left = 0;

    cpu.raise_NMI();
    ASSERT_EQ(run_one_instruction(cpu), 2u);
    ASSERT_EQ(cpu.registers.PC, 0x1001);

    // Step the NMI sequence, raising a SECOND edge on cycle 3 - inside the
    // window where a BRK or IRQ sequence would be hijacked.
    for (int cycle = 1;; ++cycle) {
        if (cycle == 3) {
            cpu.raise_NMI();
        }
        if (cpu.clock(false)) {
            break;
        }
        ASSERT_LT(cycle, 32) << "the NMI sequence did not complete";
    }
    ASSERT_EQ(cpu.registers.PC, 0x9000) << "the first NMI should have vectored to its handler";

    // A sequence does not poll, so the second edge cannot be taken at this
    // boundary - the handler's first instruction runs.
    EXPECT_EQ(run_one_instruction(cpu), 2u) << "the handler's first instruction must run";
    EXPECT_EQ(cpu.registers.PC, 0x9001);

    // And only then is the retained edge serviced.
    EXPECT_EQ(run_one_instruction(cpu), 7u)
        << "the second /NMI edge was swallowed by the sequence it arrived in; a hijack "
           "selects a vector, it does not consume an edge";
    EXPECT_EQ(cpu.registers.PC, 0x9000) << "the retained edge should vector through $FFFA again";
}

// The corollary of polling on the penultimate cycle: an edge arriving on an
// instruction's *final* cycle is not seen until the next instruction's poll, so
// it is deferred by a whole instruction. This is what 04-nmi_control means by
// "Immediate occurence should be after NEXT instruction".
TEST(CpuInterrupts, an_edge_on_the_final_cycle_is_deferred_one_instruction)
{
    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    mem.memory[0x1000] = 0xEA;  // NOP - raise on its final (2nd) cycle
    mem.memory[0x1001] = 0xEA;  // NOP - runs anyway
    mem.memory[0x1002] = 0xEA;
    mem.memory[0xFFFA] = 0x00;
    mem.memory[0xFFFB] = 0x90;

    cpu.registers.PC = 0x1000;
    cpu.registers.SP = 0xFD;
    cpu.registers.P = 0x24;
    cpu.cycles_left = 0;

    ASSERT_FALSE(cpu.clock(false)) << "a 2-cycle NOP should not complete on cycle 1";
    cpu.raise_NMI();                // arrives on the final cycle
    ASSERT_TRUE(cpu.clock(false));  // completes it
    ASSERT_EQ(cpu.registers.PC, 0x1001);

    EXPECT_EQ(run_one_instruction(cpu), 2u) << "an edge on the final cycle must not be taken at that boundary";
    EXPECT_EQ(cpu.registers.PC, 0x1002) << "the following instruction should have run first";

    EXPECT_EQ(run_one_instruction(cpu), 7u) << "and only then is the NMI serviced";
    EXPECT_EQ(cpu.registers.PC, 0x9000);
}

// The one instruction that does not poll on its penultimate cycle.
//
// Hardware polls before a branch's operand fetch and then NOT before the third
// cycle of a taken branch (NESdev, CPU interrupts: "Interrupts are always
// polled before the second CPU cycle (the operand fetch), but not before the
// third CPU cycle on a taken branch"). In this core's terms - where a sample
// runs after each cycle's bus access, so "polled before cycle N" is the sample
// at the end of cycle N-1 - that suppresses the end-of-cycle-2 sample and
// nothing else:
//
//   not taken        2 cycles   samples after cycle 1        (= penultimate)
//   taken            3 cycles   samples after cycle 1 only   (one EARLIER than
//                                                             penultimate)
//   taken, crossing  4 cycles   samples after cycles 1 and 3 (= penultimate)
//
// The distinction is what Blargg's 5-branch_delays_irq measures with its
// test_branch_taken / test_branch_taken_pagecross / test_branch_not_taken /
// test_jmp sub-tests.
//
// Only TWO of the four tests below can actually detect a change to this rule,
// and it is worth knowing which:
//
//   a_taken_branch_does_not_poll_on_its_second_cycle
//       the load-bearing one - fails if the rule is deleted or moved
//   a_page_crossing_taken_branch_polls_on_its_penultimate_cycle
//       fails if the suppression is widened to cycle 3 as well
//
//   an_untaken_branch_polls_like_any_other_instruction
//   a_three_cycle_non_branch_does_poll_on_its_second_cycle
//       CONTROLS, not detectors. An untaken branch's cycle-2 sample is already
//       suppressed by the completed_instruction_this_cycle guard, so no
//       mutation of the branch rule can reach either of them. A review
//       confirmed both survive every such mutation. They are here to show the
//       rule does not over-apply, which is a different claim from pinning it.
//
// All four raise /IRQ between cycle 1 and cycle 2, i.e. after the only sample a
// three-cycle taken branch takes.

TEST(CpuInterrupts, a_taken_branch_does_not_poll_on_its_second_cycle)
{
    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    // BCS +0 at $1000. Operand fetch leaves PC at $1002 and the offset is 0, so
    // the target is $1002: same page, three cycles.
    mem.memory[0x1000] = 0xB0;
    mem.memory[0x1001] = 0x00;
    mem.memory[0x1002] = 0xEA;  // NOP - must run BEFORE the IRQ
    mem.memory[0x1003] = 0xEA;
    mem.memory[0xFFFE] = 0x00;  // IRQ -> $9000
    mem.memory[0xFFFF] = 0x90;

    cpu.registers.PC = 0x1000;
    cpu.registers.SP = 0xFD;
    cpu.registers.P = 0x21;  // U set, I clear (IRQ enabled), C set (branch taken)
    cpu.cycles_left = 0;

    ASSERT_FALSE(cpu.clock(false)) << "cycle 1 of a branch is the opcode fetch";
    cpu.raise_IRQ();  // arrives during cycle 2, after the branch's only sample
    ASSERT_FALSE(cpu.clock(false)) << "cycle 2 is the operand fetch; a taken branch has more to do";
    ASSERT_TRUE(cpu.clock(false)) << "a taken branch that stays on its page is three cycles";
    ASSERT_EQ(cpu.registers.PC, 0x1002) << "BCS +0 should land at $1002";

    EXPECT_EQ(run_one_instruction(cpu), 2u)
        << "the IRQ was taken at the branch's boundary; a taken branch does not poll on cycle 2";
    EXPECT_EQ(cpu.registers.PC, 0x1003) << "the instruction after the branch must run first";

    EXPECT_EQ(run_one_instruction(cpu), 7u) << "and only then is the IRQ serviced";
    EXPECT_EQ(cpu.registers.PC, 0x9000) << "IRQ vectors through $FFFE/$FFFF";
}

// The control: a three-cycle instruction that is not a branch polls on its
// penultimate cycle as usual, so the same IRQ IS taken at its boundary.
TEST(CpuInterrupts, a_three_cycle_non_branch_does_poll_on_its_second_cycle)
{
    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    mem.memory[0x1000] = 0xA5;  // LDA $10 - zero page, three cycles
    mem.memory[0x1001] = 0x10;
    mem.memory[0x1002] = 0xEA;  // NOP - must NOT run before the IRQ
    mem.memory[0xFFFE] = 0x00;
    mem.memory[0xFFFF] = 0x90;

    cpu.registers.PC = 0x1000;
    cpu.registers.SP = 0xFD;
    cpu.registers.P = 0x20;  // U set, I clear
    cpu.cycles_left = 0;

    ASSERT_FALSE(cpu.clock(false));
    cpu.raise_IRQ();  // same injection point as the branch test above
    ASSERT_FALSE(cpu.clock(false));
    ASSERT_TRUE(cpu.clock(false)) << "LDA zero page is three cycles";
    ASSERT_EQ(cpu.registers.PC, 0x1002);

    EXPECT_EQ(run_one_instruction(cpu), 7u)
        << "an ordinary instruction polls on its penultimate cycle, so the IRQ is due immediately";
    EXPECT_EQ(cpu.registers.PC, 0x9000);
}

// A taken branch that crosses a page is four cycles and does poll on its
// penultimate one (cycle 3), so the delay is NOT a property of taken branches
// in general - only of the three-cycle form.
TEST(CpuInterrupts, a_page_crossing_taken_branch_polls_on_its_penultimate_cycle)
{
    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    // BCS at $1080 with offset $7F. The operand fetch leaves PC at $1082 and
    // $1082 + $7F = $1101, which is on the next page: four cycles.
    mem.memory[0x1080] = 0xB0;
    mem.memory[0x1081] = 0x7F;
    mem.memory[0x1101] = 0xEA;  // NOP - must NOT run before the IRQ
    mem.memory[0xFFFE] = 0x00;
    mem.memory[0xFFFF] = 0x90;

    cpu.registers.PC = 0x1080;
    cpu.registers.SP = 0xFD;
    cpu.registers.P = 0x21;  // U set, I clear, C set
    cpu.cycles_left = 0;

    ASSERT_FALSE(cpu.clock(false));
    cpu.raise_IRQ();  // again during cycle 2
    ASSERT_FALSE(cpu.clock(false));
    ASSERT_FALSE(cpu.clock(false)) << "a page-crossing taken branch has a fourth cycle";
    ASSERT_TRUE(cpu.clock(false));
    ASSERT_EQ(cpu.registers.PC, 0x1101) << "BCS $7F from $1082 should land at $1101";

    EXPECT_EQ(run_one_instruction(cpu), 7u) << "the cycle-3 sample sees the IRQ, so it is due at the branch's boundary";
    EXPECT_EQ(cpu.registers.PC, 0x9000);
}

// A branch that is not taken is two cycles, and its cycle 1 sample IS its
// penultimate one, so it behaves like every other instruction.
TEST(CpuInterrupts, an_untaken_branch_polls_like_any_other_instruction)
{
    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    mem.memory[0x1000] = 0xB0;  // BCS +0 with C clear: not taken, two cycles
    mem.memory[0x1001] = 0x00;
    mem.memory[0x1002] = 0xEA;  // NOP - must NOT run before the IRQ
    mem.memory[0xFFFE] = 0x00;
    mem.memory[0xFFFF] = 0x90;

    cpu.registers.PC = 0x1000;
    cpu.registers.SP = 0xFD;
    cpu.registers.P = 0x20;  // U set, I clear, C CLEAR so BCS falls through
    cpu.cycles_left = 0;

    cpu.raise_IRQ();  // before cycle 1, so the cycle-1 (= penultimate) sample sees it
    ASSERT_FALSE(cpu.clock(false));
    ASSERT_TRUE(cpu.clock(false)) << "an untaken branch is two cycles";
    ASSERT_EQ(cpu.registers.PC, 0x1002);

    EXPECT_EQ(run_one_instruction(cpu), 7u) << "an untaken branch polls on its penultimate cycle like anything else";
    EXPECT_EQ(cpu.registers.PC, 0x9000);
}

TEST(CpuReset, power_on_state_matches_hardware)
{
    FlatMemory mem;
    CPU cpu = make_cpu(mem);

    mem.memory[0xFFFC] = 0x00;
    mem.memory[0xFFFD] = 0xC0;

    cpu.reset();

    // Three dummy stack accesses during reset decrement SP without writing.
    EXPECT_EQ(cpu.registers.SP, 0xFD) << "SP should settle at $FD after reset, not $FF";

    // U set, I set, B clear: this is the $24 that nestest's first log line shows.
    EXPECT_EQ(cpu.registers.P, 0x24) << "P should be $24 after reset (U and I set, B clear)";
    EXPECT_TRUE(cpu.get_flag(CPU::FLAGS::U)) << "the unused flag always reads as set";
    EXPECT_TRUE(cpu.get_flag(CPU::FLAGS::I)) << "interrupts are masked on reset";
    EXPECT_FALSE(cpu.get_flag(CPU::FLAGS::B)) << "B is not a real bit of P; it only exists in pushed copies";

    EXPECT_EQ(cpu.registers.PC, 0xC000) << "reset should vector through $FFFC/$FFFD";
}

// B must not survive a pull, and U must always come back set - the two must
// agree between RTI and PLP.
TEST(CpuFlags, rti_and_plp_agree_on_the_b_and_u_bits)
{
    for (const uint8_t pushed : {uint8_t{0x00}, uint8_t{0xFF}, uint8_t{0x30}, uint8_t{0xCF}}) {
        // PLP
        {
            FlatMemory mem;
            CPU cpu = make_cpu(mem);

            mem.memory[0x1000] = 0x28;  // PLP
            mem.memory[0x01FE] = pushed;

            cpu.registers.PC = 0x1000;
            cpu.registers.SP = 0xFD;
            cpu.registers.P = 0x24;
            cpu.cycles_left = 0;

            run_one_instruction(cpu);

            EXPECT_FALSE(cpu.get_flag(CPU::FLAGS::B))
                << "PLP must discard B (pushed value " << std::hex << static_cast<unsigned>(pushed) << ")";
            EXPECT_TRUE(cpu.get_flag(CPU::FLAGS::U))
                << "PLP must leave U set (pushed value " << std::hex << static_cast<unsigned>(pushed) << ")";
        }

        // RTI
        {
            FlatMemory mem;
            CPU cpu = make_cpu(mem);

            mem.memory[0x1000] = 0x40;  // RTI
            mem.memory[0x01FE] = pushed;
            mem.memory[0x01FF] = 0x00;  // PCL
            mem.memory[0x0100] = 0x90;  // PCH (SP wraps)

            cpu.registers.PC = 0x1000;
            cpu.registers.SP = 0xFD;
            cpu.registers.P = 0x24;
            cpu.cycles_left = 0;

            run_one_instruction(cpu);

            EXPECT_FALSE(cpu.get_flag(CPU::FLAGS::B))
                << "RTI must discard B (pushed value " << std::hex << static_cast<unsigned>(pushed) << ")";
            EXPECT_TRUE(cpu.get_flag(CPU::FLAGS::U))
                << "RTI must leave U set (pushed value " << std::hex << static_cast<unsigned>(pushed) << ")";
        }
    }
}

}  // namespace tests
