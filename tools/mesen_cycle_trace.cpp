// Cycle-stepped trace of Mesen2, for direct comparison against our own.
//
// WHY THIS EXISTS, and why the tool beside it does not replace it.
// tools/mesen_event_dump.cpp reads Mesen's EVENT VIEWER, which is scoped to a
// frame (ClearFrameEvents on StartFrame, Debugger.cpp:596) and has to be
// snapshotted before it is read. That tool polls for snapshots on a WALL CLOCK,
// so any frame the core finishes between two polls is never captured and its
// row is simply absent. Measured, on sprdma_and_dmc_dma: a 20s run caught 12 of
// the ROM's 16 rows and a 60s run caught 13. Tripling the wall time bought one
// row, because the limit was never duration - it was sampling rate.
//
// That is not a tuning problem, it is the wrong data path. The rows it dropped
// were not random either: rows with a DMC inside the transfer survived in pairs
// while rows with the DMC outside arrived as singletons, which silently
// re-aligned the table against ours. Reasoning over the holes produced a
// confident refutation of a finding that was in fact TRUE (see findings 11 and
// 13 in tests/dmc_sweep_diag.cpp). A lossy instrument is worse than no
// instrument, because its output is well-formed.
//
// MESEN HAS SUPPORTED CYCLE STEPPING ALL ALONG. StepType::CpuCycleStep
// (Core/Debugger/DebugTypes.h:365) is advertised by the NES debugger itself -
// features.CpuCycleStep = true, NesDebugger.cpp:407 - and handled at
// NesDebugger.cpp:277. The interop surface is one call:
//
//     Step(CpuType cpuType, uint32_t count, StepType type)
//
// Stepping is deterministic and loses nothing, so this tool replaces sampling
// with stepping and the frame disappears from the measurement entirely.
//
// WHAT IT PRINTS. One line per CPU cycle in the requested window:
//
//     cycle 2064792  pc $E2B0  a $00 x $07 y $00  sp $FB  p $24
//
// plus a summary line whenever the PC stops advancing, which is how a DMA is
// detected without any DMA-specific API: the CPU is halted while either DMA
// holds the bus, so CycleCount advances and PC does not. A run of ~513 stalled
// cycles is an OAM DMA and a run of 3-4 is a DMC DMA. That is exactly the
// comparison the investigation needs - where the DMC's short stall sits
// relative to the OAM DMA's long one - and it is measured the same way on both
// emulators rather than inferred from different quantities.
//
// SEEKING IS ALSO CYCLE-BASED. The window of interest is ~2 million cycles in,
// and single-stepping there would mean two million thread round-trips. The
// coarse seek therefore uses the SAME CpuCycleStep with a large count, so the
// position is exact rather than approximate; nothing here is frame-scoped.
//
// THE CYCLE COUNTERS DO NOT SHARE AN ORIGIN. MEASURED, not assumed: our own
// trace has the row-05 DMC halt accepted at cycle 2064792, and Mesen at its own
// 2064792 is in a seven-cycle delay loop at $E640-$E646 with A decrementing by 7
// per pass, with no stall of any kind in the surrounding 45 cycles. Mesen counts
// from a different zero, so ABSOLUTE CYCLE NUMBERS ARE NOT COMPARABLE and
// lining the two traces up by number produces fiction.
//
// Align on a LANDMARK instead. The obvious one is the Nth OAM DMA, visible on
// both sides as a ~513-cycle run with the PC frozen - our diagnostic numbers
// those rows already, and the stall detector below finds them here. That is
// what the next change to this tool should add: a "seek to the Nth long stall"
// mode, so the window is chosen by event rather than by a number that means
// something different on each side.
//
// THE dmaN LANDMARK MODE DOES NOT WORK YET. Measured: `--cycles ROM dma5` runs
// its whole 4,000,000-cycle guard without ever seeing a PC frozen for 100
// cycles, and exits 0 having printed nothing. OAM DMAs are ~178,000 cycles
// apart, so ~22 should have been inside that window. Either the coarse counter
// leaves the emulator somewhere unexpected, or the stall detector does not fire
// the way the absolute-cycle mode's does.
//
// The absolute-cycle mode IS verified working - it produced a correct
// cycle-by-cycle trace at 100000 and at 2064780, instruction boundaries and
// register changes and all. Only the landmark seek is broken.
//
// Two things to fix before trusting anything it prints:
//   1. the guard exhausting MUST be an error, not a silent return 0. A tool
//      that reports nothing and succeeds is the exact failure this file's
//      header was written to warn about.
//   2. verify the two phases separately - print the cycle at which each of the
//      first few DMAs is detected in the coarse phase, and check the stall
//      detector against the absolute mode at a window known to contain a DMA.
//
// Usage:
//   mesen_cycle_trace <MesenCore.so> <rom.nes> <from-cycle> [count]
//
//   from-cycle   CPU cycle to begin printing at (Mesen's own CycleCount)
//   count        cycles to print, default 64
//
// Build and run it through tools/run_mesen_dump.sh --cycles, which supplies the
// include paths, LD_LIBRARY_PATH and working directory that Mesen needs.
#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Core/NES/NesTypes.h"

namespace
{
// CpuType::Nes. Same constant the event dump pins, for the same reason: the
// enum is not exported and a wrong value here reads as "the debugger returned
// nothing" rather than as an error.
constexpr int kCpuTypeNes = 8;

// StepType::CpuCycleStep, Core/Debugger/DebugTypes.h:365. Position in the enum,
// counting from Step=0: Step, StepOut, StepOver, CpuCycleStep.
constexpr int kStepCpuCycle = 3;

// A stall shorter than this is not worth reporting; 3 is the shortest real DMC
// DMA (halt, dummy, get, with no alignment needed).
constexpr uint64_t kMinStall = 3;
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <MesenCore.so> <rom.nes> <from-cycle> [count]\n", argv[0]);
        return 2;
    }

    // "dmaN" selects the Nth OAM DMA (0-based, matching the row numbering in
    // tests/dmc_sweep_diag.cpp) instead of an absolute cycle. This is the only
    // form that is comparable across emulators - see the origin note above.
    const bool landmarkMode = std::strncmp(argv[3], "dma", 3) == 0;
    const int wantDma = landmarkMode ? std::atoi(argv[3] + 3) : 0;
    const uint64_t fromCycle = landmarkMode ? 0 : std::strtoull(argv[3], nullptr, 10);
    const uint64_t printCount = (argc > 4) ? std::strtoull(argv[4], nullptr, 10) : 64;

    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    auto InitDll = reinterpret_cast<void (*)()>(dlsym(lib, "InitDll"));
    auto InitializeEmu =
        reinterpret_cast<void (*)(const char*, void*, void*, bool, bool, bool, bool)>(dlsym(lib, "InitializeEmu"));
    auto LoadRom = reinterpret_cast<bool (*)(char*, char*)>(dlsym(lib, "LoadRom"));
    auto InitializeDebugger = reinterpret_cast<void (*)()>(dlsym(lib, "InitializeDebugger"));
    auto Step = reinterpret_cast<void (*)(int, uint32_t, int)>(dlsym(lib, "Step"));
    auto GetCpuState = reinterpret_cast<void (*)(BaseState&, int)>(dlsym(lib, "GetCpuState"));
    auto IsPaused = reinterpret_cast<bool (*)()>(dlsym(lib, "IsPaused"));
    auto IsDebuggerRunning = reinterpret_cast<bool (*)()>(dlsym(lib, "IsDebuggerRunning"));
    auto Stop = reinterpret_cast<void (*)()>(dlsym(lib, "Stop"));

    // EVERY symbol is required and the missing one is named. A null function
    // pointer called through reinterpret_cast is a segfault with no clue in it,
    // and this tool exists precisely because a previous one failed quietly.
    const struct {
        const char* name;
        void* p;
    } required[] = {
        {"InitDll", reinterpret_cast<void*>(InitDll)},
        {"InitializeEmu", reinterpret_cast<void*>(InitializeEmu)},
        {"LoadRom", reinterpret_cast<void*>(LoadRom)},
        {"InitializeDebugger", reinterpret_cast<void*>(InitializeDebugger)},
        {"Step", reinterpret_cast<void*>(Step)},
        {"GetCpuState", reinterpret_cast<void*>(GetCpuState)},
        {"IsPaused", reinterpret_cast<void*>(IsPaused)},
        {"IsDebuggerRunning", reinterpret_cast<void*>(IsDebuggerRunning)},
        {"Stop", reinterpret_cast<void*>(Stop)},
    };
    for (const auto& sym : required) {
        if (!sym.p) {
            std::fprintf(stderr, "dlsym failed: %s\n", sym.name);
            return 1;
        }
    }

    InitDll();
    InitializeEmu("./MesenHome", nullptr, nullptr, true, true, true, true);

    std::string romArg(argv[2]);
    std::vector<char> rom(romArg.begin(), romArg.end());
    rom.push_back('\0');
    std::vector<char> patch{'\0'};
    if (!LoadRom(rom.data(), patch.data())) {
        std::fprintf(stderr, "Mesen could not load %s\n", argv[2]);
        return 1;
    }

    InitializeDebugger();
    if (!IsDebuggerRunning()) {
        std::fprintf(stderr, "debugger did not start; nothing below would be measured\n");
        return 1;
    }

    NesCpuState cpu{};
    auto readCycle = [&]() -> uint64_t {
        GetCpuState(cpu, kCpuTypeNes);
        return cpu.CycleCount;
    };

    // Step() hands the request to the emulator thread and returns, so the state
    // read straight afterwards is the state from BEFORE the step. Waiting on
    // IsPaused is what makes each step observable; without it every line below
    // would report the same cycle and look like a stalled CPU.
    auto stepCycles = [&](uint32_t n) {
        Step(kCpuTypeNes, n, kStepCpuCycle);
        for (int spin = 0; spin < 100000000 && !IsPaused(); ++spin) {
        }
    };

    if (landmarkMode) {
        // Phase 1, coarse: step in blocks and watch whether the PC moved across
        // the block. Nothing in normal code holds the PC still for 256 cycles,
        // so a block with an unchanged PC is inside an OAM DMA and cannot be
        // anything else. This costs ~8000 round trips to reach the region
        // instead of the two million single steps it would otherwise take.
        constexpr uint32_t kProbe = 256;
        int seen = 0;
        bool inDma = false;
        std::fprintf(stderr, "seeking to OAM DMA #%d...\n", wantDma);
        while (seen < wantDma) {
            GetCpuState(cpu, kCpuTypeNes);
            const uint16_t before = cpu.PC;
            stepCycles(kProbe);
            GetCpuState(cpu, kCpuTypeNes);
            const bool nowInDma = (cpu.PC == before);
            if (nowInDma && !inDma) {
                ++seen;
            }
            inDma = nowInDma;
        }

        // Phase 2, exact: single-step into the next DMA, keeping the last 64
        // cycles so the window BEFORE it is available. The DMC fetch being
        // hunted happens shortly before the transfer starts, so a trace that
        // began at the transfer would miss the entire point.
        struct Rec {
            uint64_t cycle;
            uint16_t pc;
            uint8_t a, x, y, sp, ps;
        };
        constexpr size_t kRing = 64;
        std::vector<Rec> ring;
        ring.reserve(kRing);
        size_t head = 0;
        uint16_t prev = 0;
        uint64_t frozen = 0;
        bool first = true;

        std::fprintf(stderr, "single-stepping to OAM DMA #%d...\n", wantDma);
        for (uint64_t guard = 0; guard < 4000000; ++guard) {
            GetCpuState(cpu, kCpuTypeNes);
            const Rec r{cpu.CycleCount, cpu.PC, cpu.A, cpu.X, cpu.Y, cpu.SP, cpu.PS};
            if (ring.size() < kRing) {
                ring.push_back(r);
            } else {
                ring[head] = r;
                head = (head + 1) % kRing;
            }
            frozen = (!first && cpu.PC == prev) ? frozen + 1 : 0;
            prev = cpu.PC;
            first = false;

            if (frozen >= 100) {
                std::printf("OAM DMA #%d found; %zu cycles of lead-in, then the transfer:\n", wantDma, ring.size());
                for (size_t i = 0; i < ring.size(); ++i) {
                    const Rec& e = ring[(head + i) % ring.size()];
                    std::printf("  cycle %llu  pc $%04X  a $%02X x $%02X y $%02X  sp $%02X  p $%02X\n",
                                static_cast<unsigned long long>(e.cycle), e.pc, e.a, e.x, e.y, e.sp, e.ps);
                }
                break;
            }
            stepCycles(1);
        }

        Stop();
        return 0;
    }

    // Coarse seek, in cycles. Chunked because a step is a thread round-trip and
    // two million of them is not a measurement, it is a wait. The final approach
    // is single-stepped so the window starts on the exact cycle asked for.
    std::fprintf(stderr, "seeking to cycle %llu...\n", static_cast<unsigned long long>(fromCycle));
    constexpr uint32_t kChunk = 20000;
    while (readCycle() + kChunk < fromCycle) {
        stepCycles(kChunk);
    }
    while (readCycle() < fromCycle) {
        stepCycles(1);
    }

    std::fprintf(stderr, "at cycle %llu, tracing %llu cycles\n", static_cast<unsigned long long>(readCycle()),
                 static_cast<unsigned long long>(printCount));

    uint16_t previousPc = 0;
    uint64_t stallStart = 0;
    uint64_t stallLength = 0;
    bool havePrevious = false;

    for (uint64_t i = 0; i < printCount; ++i) {
        GetCpuState(cpu, kCpuTypeNes);
        std::printf("  cycle %llu  pc $%04X  a $%02X x $%02X y $%02X  sp $%02X  p $%02X\n",
                    static_cast<unsigned long long>(cpu.CycleCount), cpu.PC, cpu.A, cpu.X, cpu.Y, cpu.SP, cpu.PS);

        // A halted CPU is the only thing that advances CycleCount without
        // advancing PC, so a run of unchanged PC is a DMA holding the bus. No
        // DMA-specific API is involved, which is what makes this directly
        // comparable to our own trace rather than to a different quantity.
        if (havePrevious && cpu.PC == previousPc) {
            if (stallLength == 0) {
                stallStart = cpu.CycleCount;
            }
            ++stallLength;
        } else {
            if (stallLength >= kMinStall) {
                std::printf("    ^ stalled %llu cycles from %llu  (%s)\n", static_cast<unsigned long long>(stallLength),
                            static_cast<unsigned long long>(stallStart), stallLength > 100 ? "OAM DMA" : "DMC DMA");
            }
            stallLength = 0;
        }
        previousPc = cpu.PC;
        havePrevious = true;

        stepCycles(1);
    }
    if (stallLength >= kMinStall) {
        std::printf("    ^ stalled %llu cycles from %llu  (%s, still running at window end)\n",
                    static_cast<unsigned long long>(stallLength), static_cast<unsigned long long>(stallStart),
                    stallLength > 100 ? "OAM DMA" : "DMC DMA");
    }

    Stop();  // the emulator thread outlives main() otherwise, and segfaults on exit
    return 0;
}
