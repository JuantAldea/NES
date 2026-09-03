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
// THE CYCLE COUNTERS DO NOT SHARE AN ORIGIN, AND MESEN'S IS NOT EVEN THE SAME
// TWICE. This is stronger than it was first written and the difference matters.
//
// InitializeEmu and LoadRom start Mesen on its own thread. It is already
// executing by the time InitializeDebugger attaches, so the first Step pauses it
// wherever the scheduler happened to leave it. MEASURED across three identical
// invocations: cycle 27279 at pc $EA2B, 27279 at $EA2B, then 14914 at $E95C.
// The starting point is nondeterministic.
//
// SO AN OFFSET BETWEEN THE TWO COUNTERS IS AN ARTEFACT OF THE SCHEDULER, and
// nothing may be concluded from it - not its value, and not its parity. A
// retraction is recorded here because that trap has already been walked into:
// offsets of 145137 and 179319 were measured at two sweep rows, their difference
// read as 34182 cycles of "drift", and their shared oddness used to argue that
// this emulator's get/put labelling is inverted relative to Mesen2's. Those were
// two SEPARATE PROCESSES with independent random origins. There was no drift and
// the parity was noise.
//
// What survives that retraction is anything compared WITHIN one trace: the
// instruction stream approaching row 05 is 16 cycles from $E3AE to $E503 on both
// sides, with $E3AF and $E3B0 each held 4, and then this emulator inserts a
// 4-cycle DMC halt at $E503 where Mesen inserts none. That is structural and
// needs no alignment at all.
//
// ALIGN ON CONTENT, NEVER ON A NUMBER. The Nth OAM DMA works because the
// handover lands around 15000-27000 cycles and the ROM's first OAM DMA is past
// 1.2 million, so Mesen cannot have passed one before the debugger attached and
// both sides count the same transfers. A tool that aligns this way is immune to
// where the thread happened to stop; one that takes a cycle pair from a previous
// run is measuring the scheduler.
//
// Align on a LANDMARK instead. The obvious one is the Nth OAM DMA, visible on
// both sides as a ~513-cycle run with the PC frozen - our diagnostic numbers
// those rows already, and the stall detector below finds them here. That is
// what the next change to this tool should add: a "seek to the Nth long stall"
// mode, so the window is chosen by event rather than by a number that means
// something different on each side.
//
// THE dmaN LANDMARK MODE WORKS, and both halves of it were wrong before. Each
// failure is recorded at the code it belongs to; what is worth having here is
// how they hid.
//
// "IT COMPLETES" IS NOT "IT WORKS". Phase 1 was cleared of suspicion because it
// finished and printed its progress line - and it was the bug: its block test
// compares only the endpoints of a 256-cycle window, so the ROM's init wait
// loop at $E95C read as a halted CPU. It reported eight OAM DMAs before the
// first frame had ended. Phase 2 then searched from cycle 10759, correctly
// found nothing, and took the blame.
//
// AN INSTRUMENT CAN BE BLIND TO EXACTLY WHAT IT IS FOR. Phase 2's ring held 64
// cycles against a freeze threshold of 100, so every entry it could ever hold
// was already inside the DMA. It printed "64 cycles of lead-in" that were 64
// frozen cycles - the lead-in it exists to capture was unreachable by
// construction, and the header above it claimed the opposite.
//
// A STEP IS 2.01 ms, measured by differential timing - 100 vs 2000 traced
// steps, 28.4s vs 32.2s, so ~498 steps/sec. That is the constraint on every
// number here: the 4,000,000-step guard this carried originally would take 134
// MINUTES to exhaust, which is why it was never observed doing so.
//
// VERIFIED END TO END: `--cycles ROM dma3 16` finds OAM DMA #3 at cycle
// 1884544, with 192 cycles of genuine lead-in across 30 distinct PCs - the
// seven-cycle delay loop at $E640-$E646, A decrementing by 7 a pass, which is
// the same loop the origin note below describes. The absolute-cycle mode is
// unchanged and still traces correctly from 100000 and 2064780.
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
        // the block, so the region costs ~8000 round trips to reach instead of
        // the two million single steps it would otherwise take.
        //
        // "NOTHING IN NORMAL CODE HOLDS THE PC STILL FOR 256 CYCLES" IS FALSE,
        // and believing it is what broke this mode. A block test only compares
        // the ENDPOINTS, so any loop that returns to the same address between
        // two samples reads as frozen. MEASURED, before the confirmation below
        // existed: eight "OAM DMAs" all at $E95C, at cycles 1031 through 10759 -
        // gaps of 768 to 2560 where real ones are ~178,000 apart, and every one
        // of them inside the ROM's init, before the first frame ended. Phase 2
        // then single-stepped from there and found no DMA in 250,000 cycles,
        // which is correct: the sweep had not started.
        //
        // A HALTED CPU HOLDS PC ON EVERY CYCLE, not merely at the endpoints, so
        // a candidate is confirmed by single-stepping and requiring PC never to
        // move. That is the rule phase 2 already applied - the two phases
        // disagreeing is what let this sit broken.
        //
        // 40 is well over any loop's period and well under the ~513 cycles of an
        // OAM DMA, so a real one passes from anywhere in its first 470 cycles.
        // Cheap despite the single-stepping: a loop fails on the first step or
        // two, and only a genuine DMA pays the full 40.
        //
        // MEASURED after: hits at $E503 from cycle 1233596, gap #2 to #3 of
        // 177999 - the documented spacing, at frame ~41 where the ROM is
        // actually running its sweep.
        constexpr uint32_t kProbe = 256;
        constexpr int kConfirmCycles = 40;
        int seen = 0;
        bool inDma = false;
        std::fprintf(stderr, "seeking to OAM DMA #%d...\n", wantDma);
        while (seen < wantDma) {
            GetCpuState(cpu, kCpuTypeNes);
            const uint16_t before = cpu.PC;
            stepCycles(kProbe);
            GetCpuState(cpu, kCpuTypeNes);

            bool nowInDma = false;
            if (cpu.PC == before) {
                const uint16_t held = cpu.PC;
                nowInDma = true;
                for (int i = 0; i < kConfirmCycles; ++i) {
                    stepCycles(1);
                    GetCpuState(cpu, kCpuTypeNes);
                    if (cpu.PC != held) {
                        nowInDma = false;
                        break;
                    }
                }
            }

            if (nowInDma && !inDma) {
                ++seen;
            }
            inDma = nowInDma;
        }

        // STEP OUT OF THE DMA JUST COUNTED, or phase 2 re-detects it.
        //
        // Confirming a candidate leaves the emulator ~40 cycles inside the
        // transfer, and phase 2 looks for the NEXT freeze from wherever it is
        // handed. Left inside, it finds this same DMA immediately and every
        // entry in its ring is a frozen cycle - so it reports a lead-in that
        // contains none.
        //
        // MEASURED, and the reason this is not a theoretical tidy-up: `dma5`
        // printed "101 cycles of lead-in" across ONE distinct PC, $E503, while
        // `dma3` on the same build printed 192 across 30. Whether the ring holds
        // real context depended on where the coarse blocks happened to land, so
        // the mode worked for some N and silently did not for others - which is
        // worse than failing outright, and is what made a broken trace look like
        // a verified one.
        //
        // 1200 bounds it at roughly twice the ~513 cycles of an OAM DMA. Failing
        // to leave in that many is not something to continue past.
        GetCpuState(cpu, kCpuTypeNes);
        const uint16_t heldPc = cpu.PC;
        bool steppedOut = false;
        for (int i = 0; i < 1200; ++i) {
            stepCycles(1);
            GetCpuState(cpu, kCpuTypeNes);
            if (cpu.PC != heldPc) {
                steppedOut = true;
                break;
            }
        }
        if (!steppedOut) {
            std::fprintf(stderr,
                         "the PC never moved in 1200 cycles after OAM DMA #%d, so the emulator is not\n"
                         "where the coarse phase thinks it is - a trace from here would be meaningless.\n",
                         wantDma);
            Stop();
            return 1;
        }

        // Phase 2, exact: single-step into the next DMA, keeping a ring so the
        // window BEFORE it is available. The DMC fetch being hunted happens
        // shortly before the transfer starts, so a trace that began at the
        // transfer would miss the entire point.
        //
        // THE RING MUST BE LONGER THAN THE FREEZE THRESHOLD, and at 64 against a
        // threshold of 100 it was not - so every entry it held was already
        // inside the DMA and the lead-in it exists to capture could not appear.
        // MEASURED: dma3 printed "64 cycles of lead-in" that were 64 consecutive
        // frozen cycles at $E503. The instrument was structurally blind to the
        // one thing it was built to show.
        //
        // 192 leaves ~92 cycles genuinely before the halt, which covers the
        // 3-or-4 cycle DMC sequence and the instruction that provoked it.
        struct Rec {
            uint64_t cycle;
            uint16_t pc;
            uint8_t a, x, y, sp, ps;
        };
        constexpr size_t kRing = 192;
        std::vector<Rec> ring;
        ring.reserve(kRing);
        size_t head = 0;
        uint16_t prev = 0;
        uint64_t frozen = 0;
        bool first = true;

        std::fprintf(stderr, "single-stepping to OAM DMA #%d...\n", wantDma);
        // MEASURED: one Step()+IsPaused() round trip costs 2.01 ms (differential
        // timing, 100 vs 2000 traced steps: 28.4s vs 32.2s), so ~498 steps/sec.
        // The 4,000,000 that stood here is 134 MINUTES - long enough that nobody
        // reaches it, which makes it a hang rather than a guard.
        //
        // Sized from the spacing instead: OAM DMAs are ~178,000 CPU cycles apart,
        // so one full gap plus margin is all this phase can need. 250,000 steps
        // is ~8 minutes worst case and terminates with a verdict either way.
        bool found = false;
        constexpr uint64_t kGuard = 250000;
        for (uint64_t guard = 0; guard < kGuard; ++guard) {
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
                found = true;
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

        // Exhausting the guard is a FAILURE, not an empty result. OAM DMAs are
        // ~178,000 cycles apart, so a window this size should contain ~22 of
        // them; finding none means the seek or the detector is wrong, and this
        // tool exists because its predecessor reported nothing and succeeded.
        if (!found) {
            std::fprintf(stderr,
                         "no OAM DMA found in %llu cycles after the coarse seek to #%d.\n"
                         "The stall detector never saw a PC frozen for 100 cycles. OAM DMAs\n"
                         "are ~178,000 cycles apart, so a window this size should contain at\n"
                         "least one. Do not read this as 'the ROM performs no DMA' - the\n"
                         "absolute-cycle mode is verified working, so compare against it at a\n"
                         "window known to contain one.\n",
                         static_cast<unsigned long long>(kGuard), wantDma);
            return 1;
        }
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
