// Steps this emulator and Mesen2 together, one CPU cycle at a time, and reports
// the FIRST cycle they disagree on.
//
// WHY THIS AND NOT TWO TRACES. Everything before this dumped each emulator
// separately and compared the files. That works only if you can line the two up,
// and you cannot: the cycle counters share no origin, and worse, they DRIFT -
// measured at 145137 apart at one sweep row and 179319 at another, 34182 cycles
// of divergence across two iterations. Every conclusion drawn from an offset is
// therefore conditional on an alignment that is itself unknown, which is how a
// whole line of reasoning about get/put parity ended up resting on the parity of
// a number that does not mean what it looked like.
//
// Stepping them together removes the problem rather than working around it.
// There is one alignment, at the start, and after that the question is only
// "which cycle is the first to differ" - which is a fact about the emulators
// rather than about the arithmetic used to compare them.
//
// It links both: our own Bus out of the static libraries CMake builds, and
// Mesen through the same InteropDLL the other tools in this directory use.
//
// THE COST IS MESEN'S. One Step()+IsPaused() round trip is ~2.01 ms, so a
// million cycles is about half an hour. Our side is free by comparison. That
// makes this an unattended run, not an interactive one - and the reason it is
// still worth it is that a first divergence is exact, where an offset is a
// guess.
//
// This used to say "build and run it through tools/run_lockstep.sh", which has
// never existed. Mesen must be the working directory - it dlopens its own
// dependencies by relative path - and both include roots are needed:
//
//   M=/home/juen/projects/Mesen2
//   g++ -O2 -std=c++20 -Wall -Wextra -I "$M" -I "$M/Core" -I include
//       -o /tmp/lockstep tools/lockstep_compare.cpp
//       build/liblib{Bus,PPU,CPU,Instruction}.a -ldl
//   cd "$M/bin/linux-x64/Release"
//   LD_LIBRARY_PATH=.:./Dependencies /tmp/lockstep ./MesenCore.so <rom>
//       <our-cycle> <landmark-pc-hex> [max-cycles]
//
// The landmark must be a PC our side actually holds at <our-cycle>; the tool says
// so and stops if it is not.
#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../include/bus.h"
#include "Core/NES/NesTypes.h"

namespace
{
// CpuType::Nes and StepType::CpuCycleStep, as in tools/mesen_cycle_trace.cpp.
constexpr int kCpuTypeNes = 8;
constexpr int kStepCpuCycle = 3;

// ConsoleType::Nes, from Core/Shared/SettingTypes.h. GetConsoleState is what
// makes the two frozen spans DECOMPOSABLE: a halted CPU freezes PC on both
// sides, so PC alone cannot tell a DMC stall from an OAM DMA. The DMC's
// BytesRemaining advances on the cycle it performs its fetch, which locates that
// fetch inside the span exactly.
constexpr int kConsoleTypeNes = 2;

struct Sample {
    uint64_t cycle;
    uint16_t pc;
    uint8_t a, x, y, sp;
};
}  // namespace

int main(int argc, char** argv)
{
    // argv[3] and argv[4] are both read below, so this guarded too few: at argc 3
    // or 4 it read past the end of argv rather than printing the usage.
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <MesenCore.so> <rom.nes> <our-cycle> <landmark-pc-hex|OAM> [max-cycles]\n",
                     argv[0]);
        std::fprintf(stderr, "  landmark: ... sprdma_and_dmc_dma.nes 2064787 E4FE 400\n");
        std::fprintf(stderr, "  structural: ... sprdma_and_dmc_dma_512.nes 1945390 OAM 900\n");
        std::fprintf(stderr, "    OAM aligns both sides on the next OAM DMA after the given cycle,\n");
        std::fprintf(stderr, "    which needs no shared clock - pass the transfer's start cycle.\n");
        return 2;
    }

    // ALIGNED ON CONTENT, NOT ON A CYCLE NUMBER, because Mesen has no fixed
    // origin: it is already running when the debugger attaches, so the handover
    // lands wherever the scheduler left it. Measured across three identical
    // runs: 27279, 27279, 14914. Any offset between the two counters is
    // therefore a measurement of the scheduler, and an earlier version of this
    // file took a cycle PAIR from a previous run, which is exactly that mistake.
    //
    // The landmark is a PC value, which both sides observe identically. Our side
    // seeks to `our-cycle` directly, since our origin is fixed at zero. Mesen's
    // side coarse-steps to a point safely before it, then single-steps until the
    // PC arrives.
    //
    // THE COARSE HINT IS SAFE FOR A REASON WORTH STATING, since it does use our
    // cycle number: the margin below is 60000, the origin uncertainty is under
    // 30000, and consecutive occurrences of a landmark like $E4FE are ~175000
    // cycles apart. So the hint lands inside the gap before the intended
    // occurrence and cannot reach the one before it. If that ever stops holding
    // the alignment check below fails loudly rather than quietly comparing the
    // wrong moment.
    const uint64_t ourStart = std::strtoull(argv[3], nullptr, 10);
    const uint16_t landmarkPc = static_cast<uint16_t>(std::strtoul(argv[4], nullptr, 16));
    const uint64_t maxCycles = (argc > 5) ? std::strtoull(argv[5], nullptr, 10) : 400;
    constexpr uint64_t kCoarseMargin = 60000;

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
    auto GetConsoleState = reinterpret_cast<void (*)(BaseState&, int)>(dlsym(lib, "GetConsoleState"));

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
        {"GetConsoleState", reinterpret_cast<void*>(GetConsoleState)},
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

    Bus console;
    if (!console.load_cartridge(argv[2])) {
        std::fprintf(stderr, "we could not load %s\n", argv[2]);
        return 1;
    }
    console.cpu.reset();

    NesCpuState mesen{};
    auto mesenStep = [&](uint32_t n) {
        Step(kCpuTypeNes, n, kStepCpuCycle);
        for (int spin = 0; spin < 100000000 && !IsPaused(); ++spin) {
        }
    };
    auto mesenSample = [&]() -> Sample {
        GetCpuState(mesen, kCpuTypeNes);
        return {mesen.CycleCount, mesen.PC, mesen.A, mesen.X, mesen.Y, mesen.SP};
    };

    // One CPU cycle on our side. Bus::clock is a master tick; the CPU divider is
    // Bus::cpu_cycles, so a cycle is however many ticks move it by one.
    auto ourStep = [&]() {
        const uint64_t before = console.cpu_cycles;
        while (console.cpu_cycles == before) {
            console.clock();
        }
    };
    auto ourSample = [&]() -> Sample {
        return {console.cpu_cycles,      console.cpu.registers.PC, console.cpu.registers.A,
                console.cpu.registers.X, console.cpu.registers.Y,  console.cpu.registers.SP};
    };

    std::printf("start: ours pc $%04X cyc %llu | mesen pc $%04X cyc %llu\n", ourSample().pc,
                static_cast<unsigned long long>(ourSample().cycle), mesenSample().pc,
                static_cast<unsigned long long>(mesenSample().cycle));

    // ALIGNING ON A CYCLE NUMBER ASSUMES A SHARED ORIGIN, AND THERE IS NONE.
    // Mesen's counter starts wherever it starts - 27279, 27279 and 14914 across
    // three runs of one ROM - so seeding its seek from OUR cycle is only ever
    // approximately right, and the error is not even constant between ROMs:
    // measured +1098 on sprdma_and_dmc_dma and -3810 on the _512 variant. A
    // forward-only walk from a hint that is already PAST the target cannot find
    // it, which is how a control row came back "aligned" on the wrong moment,
    // reporting mesen resumes 0 - no 515-cycle freeze anywhere in the window,
    // because Mesen was not at a transfer at all.
    //
    // Passing OAM as the landmark aligns STRUCTURALLY instead: both sides seek to
    // a point comfortably before the transfer, then each advances to the next
    // long PC freeze BY THE SAME RULE. An OAM DMA is the only thing that holds PC
    // still for hundreds of cycles, transfers are ~178000 apart, and the backoff
    // below is 20000 - so both sides land on the same one for any origin error
    // under that, without either needing to know the other's clock.
    // THE BACKOFF IS PAID IN MESEN ROUND TRIPS, WHICH IS THE WHOLE COST HERE.
    // Our side runs 2M cycles in about a second; Mesen's Step()+IsPaused() is
    // ~2 ms whether it is asked for 1 cycle or 20000, because the cost is the
    // asynchronous call and not the emulation. So everything coarse is chunked
    // and only this window is single-stepped. Its size IS the call count, so it
    // is the whole cost of a row: 8000 calls, not the 20000 a coarse chunk uses.
    //
    // 8000 has to cover Mesen's origin error, measured at +1098 and -3810 on the
    // two ROMs here. It is not bounded in principle, so a miss is reported rather
    // than silently aligning on the wrong transfer - which is the failure this
    // whole change exists to remove.
    constexpr uint64_t kBackoff = 8000;
    constexpr uint64_t kEnterFreeze = 20;  // over any instruction, far under a DMA
    const bool structural = std::string(argv[4]) == "OAM";

    // Advances one side to kEnterFreeze cycles into its next long PC freeze.
    // Our side gets a generous bound because its steps are free.
    auto toNextFreeze = [&](bool mesen) -> bool {
        uint16_t prev = mesen ? mesenSample().pc : ourSample().pc;
        uint64_t held = 0;
        const uint64_t bound = mesen ? 2 * kBackoff : 40 * kBackoff;
        for (uint64_t i = 0; i < bound; ++i) {
            if (mesen) {
                mesenStep(1);
            } else {
                ourStep();
            }
            const uint16_t pc = mesen ? mesenSample().pc : ourSample().pc;
            if (pc == prev) {
                if (++held == kEnterFreeze) {
                    return true;
                }
            } else {
                held = 0;
                prev = pc;
            }
        }
        return false;
    };

    // Our side seeks by cycle, which is exact - our origin is zero.
    const uint64_t ourSeek = structural && ourStart > kBackoff ? ourStart - kBackoff : ourStart;
    while (console.cpu_cycles < ourSeek) {
        ourStep();
    }
    if (!structural && ourSample().pc != landmarkPc) {
        std::fprintf(stderr, "our cycle %llu holds pc $%04X, not the landmark $%04X\n",
                     static_cast<unsigned long long>(ourStart), ourSample().pc, landmarkPc);
        Stop();
        return 1;
    }

    // Mesen's side coarse-steps into the gap before the landmark, then walks to
    // it. Chunking is what makes this bearable: one round trip per 20000 cycles
    // instead of per cycle turns a forty-minute seek into a fraction of a
    // second, and only the final approach is single-stepped.
    constexpr uint32_t kChunk = 20000;

    // MESEN'S ORIGIN IS LARGE AND MUST BE SUBTRACTED. Its counter already reads
    // ~20005 when it has executed no program cycles, because it is running before
    // the debugger attaches. So OUR cycle X is Mesen's X + origin, and seeking
    // Mesen to X leaves it a whole origin too EARLY - 20005 cycles, which no
    // forward search sized for a few thousand recovers. A control row then aligns
    // on the wrong moment and reports no transfer at all.
    //
    // Subtracting it leaves only the boot jitter for the search to absorb, which
    // is the few thousand cycles the "+1098 / -3810" figures measure - the
    // residue after this term, not the whole error.
    const uint64_t mesenOrigin = mesenSample().cycle;
    const uint64_t base = structural ? ourSeek : (ourStart > kCoarseMargin ? ourStart - kCoarseMargin : 0);
    const uint64_t hint = base + mesenOrigin;
    while (mesenSample().cycle + kChunk < hint) {
        mesenStep(kChunk);
    }
    while (mesenSample().cycle < hint) {
        mesenStep(1);
    }

    // A MATCHING PC IS NOT AN ALIGNMENT. $E4FE occurs 32 times over the ROM's 16
    // sweep rows - twice per row, and the pair is only ~4300 cycles apart, not
    // the ~175000 between rows. So a coarse hint can easily land before the
    // FIRST of a pair while our side is on the second. Measured, by the version
    // of this check that only compared PC: ours at 2064787 with a $01 sp $F5,
    // Mesen at 2059959 with a $00 sp $F4 - one pair-gap early.
    //
    // So the landmark is the PC *and* the register file. Walk forward until all
    // of them agree, rather than stopping at the first PC match.
    if (structural) {
        if (!toNextFreeze(false)) {
            std::fprintf(stderr, "our side found no OAM DMA after cycle %llu\n",
                         static_cast<unsigned long long>(ourSeek));
            Stop();
            return 1;
        }
        if (!toNextFreeze(true)) {
            std::fprintf(stderr,
                         "Mesen found no OAM DMA within %llu cycles of the seek - its origin error\n"
                         "exceeds the backoff, so the two would be on different transfers. Raise\n"
                         "kBackoff rather than trusting anything measured from here.\n",
                         static_cast<unsigned long long>(2 * kBackoff));
            Stop();
            return 1;
        }
        std::printf(
            "aligned structurally on the next OAM DMA (%llu cycles into it):\n"
            "  ours cyc %llu pc $%04X | mesen cyc %llu pc $%04X\n",
            static_cast<unsigned long long>(kEnterFreeze), static_cast<unsigned long long>(ourSample().cycle),
            ourSample().pc, static_cast<unsigned long long>(mesenSample().cycle), mesenSample().pc);
    }

    const Sample o0 = ourSample();
    uint64_t walked = 0;
    int pcHits = 0;
    while (!structural) {
        const Sample m = mesenSample();
        if (m.pc == landmarkPc) {
            ++pcHits;
            if (m.a == o0.a && m.x == o0.x && m.y == o0.y && m.sp == o0.sp) {
                break;
            }
        }
        mesenStep(1);
        if (++walked > 8 * kCoarseMargin) {
            std::fprintf(stderr,
                         "no cycle within %llu of the hint has pc $%04X with a $%02X x $%02X y $%02X sp $%02X\n"
                         "(%d PC matches seen, none with the right registers) - the landmark is not identifying\n"
                         "a unique moment, so nothing compared from here would mean anything.\n",
                         static_cast<unsigned long long>(walked), landmarkPc, o0.a, o0.x, o0.y, o0.sp, pcHits);
            Stop();
            return 1;
        }
    }

    const Sample m0 = mesenSample();
    if (!structural) {
        std::printf("aligned on pc $%04X + registers: ours cyc %llu | mesen cyc %llu (walked %llu, %d PC hits)\n",
                    landmarkPc, static_cast<unsigned long long>(o0.cycle), static_cast<unsigned long long>(m0.cycle),
                    static_cast<unsigned long long>(walked), pcHits);
    }

    // DECOMPOSE BOTH FROZEN SPANS. Runs the compare window while recording, per
    // cycle, what each side is actually doing - our DMA state directly, Mesen's
    // through the DMC's BytesRemaining, which advances on its fetch cycle.
    {
        NesState mesenState{};
        GetConsoleState(reinterpret_cast<BaseState&>(mesenState), kConsoleTypeNes);
        uint16_t prevBytes = mesenState.Apu.Dmc.BytesRemaining;

        Bus probe;
        probe.load_cartridge(argv[2]);
        probe.cpu.reset();
        while (probe.cpu_cycles < ourStart) {
            const uint64_t b = probe.cpu_cycles;
            while (probe.cpu_cycles == b) {
                probe.clock();
            }
        }

        uint64_t ourDmcAt = 0, ourFetchAt = 0, ourOamFrom = 0, ourOamTo = 0, ourResume = 0;
        uint64_t mesenDmcAt = 0, mesenResume = 0;
        uint64_t ourFrozenFor = 0, mesenFrozenFor = 0;

        // WHAT MOVES DURING THE HALT. PC and the register file are frozen for the
        // whole transfer on both sides, so comparing them can only fire at the
        // resume - which reports the total difference and says nothing about where
        // it accrued. The OAM address is the one thing that advances cycle by
        // cycle: $2004 increments it once per byte the transfer writes. Ours is
        // registers.OAMADDR, Mesen's is Ppu.SpriteRamAddr, the same quantity in the
        // same units, so the first index where they part is the cycle one side
        // wrote a byte the other had not.
        uint64_t oamSplitAt = 0;
        uint8_t oamSplitOurs = 0, oamSplitMesen = 0;
        bool oamAgreedAtStart = false, oamStartChecked = false;
        bool prevOam = false;
        uint16_t frozenPc = 0;
        Bus::DmcDma prevDmc = probe.dmc_dma;

        // THE RESUME TEST IS THE SAME TEST ON BOTH SIDES, and it has to be, because
        // the number being compared is a difference between them. It previously
        // keyed ours off probe.dmc_dma and ourOamTo while testing MESEN's PC
        // against OURS - so Mesen's figure was only meaningful where the two
        // happened to freeze on the same instruction, which is rows 05 and 06 and
        // nowhere else. Each side now watches its own PC and nothing else: a PC
        // held for at least kFrozenCycles and then changing is the CPU coming back
        // from the DMA. The threshold is well under a 513-cycle OAM DMA and well
        // over the longest instruction, so nothing else can trip it.
        constexpr uint64_t kFrozenCycles = 100;
        uint16_t ourPrevPc = probe.cpu.registers.PC;
        uint16_t mesenPrevPc = mesenState.Cpu.PC;
        uint64_t ourPcHeldSince = probe.cpu_cycles;
        uint64_t mesenPcHeldSince = mesenState.Cpu.CycleCount;

        for (uint64_t i = 0; i < maxCycles; ++i) {
            const uint64_t b = probe.cpu_cycles;
            while (probe.cpu_cycles == b) {
                probe.clock();
            }
            mesenStep(1);
            GetConsoleState(reinterpret_cast<BaseState&>(mesenState), kConsoleTypeNes);

            if (probe.dmc_dma != Bus::DmcDma::Idle && ourDmcAt == 0) {
                ourDmcAt = probe.cpu_cycles;
                frozenPc = probe.cpu.registers.PC;
            }
            // OUR FETCH CYCLE, COMPARED AGAINST MESEN'S FETCH CYCLE. This used to
            // print where our DMA BEGAN next to where Mesen's DMC READ, which are
            // four cycles apart in the same run - so the two columns did not
            // describe the same event and their difference measured the halt
            // sequence's length as much as any disagreement. Get -> Idle is the
            // transition the read happens on.
            if (prevDmc == Bus::DmcDma::Get && probe.dmc_dma == Bus::DmcDma::Idle && ourFetchAt == 0) {
                ourFetchAt = probe.cpu_cycles;
            }
            prevDmc = probe.dmc_dma;
            const bool oam = probe.ppu.dma_in_progress();
            if (oam && !prevOam && ourOamFrom == 0) {
                ourOamFrom = probe.cpu_cycles;
            }
            if (!oam && prevOam && ourOamTo == 0) {
                ourOamTo = probe.cpu_cycles;
            }
            prevOam = oam;

            const uint16_t ourPc = probe.cpu.registers.PC;
            if (ourPc != ourPrevPc) {
                if (!ourResume && probe.cpu_cycles - ourPcHeldSince >= kFrozenCycles) {
                    ourResume = probe.cpu_cycles;
                    ourFrozenFor = probe.cpu_cycles - ourPcHeldSince;
                }
                ourPcHeldSince = probe.cpu_cycles;
                ourPrevPc = ourPc;
            }

            const uint16_t bytes = mesenState.Apu.Dmc.BytesRemaining;
            if (bytes != prevBytes && mesenDmcAt == 0) {
                mesenDmcAt = mesenState.Cpu.CycleCount;
            }
            prevBytes = bytes;

            // Checked once, before anything can have diverged: if the two do not
            // even START equal the comparison below is meaningless, and saying so
            // beats printing a first-divergence of zero as though it were a result.
            const uint8_t oamOurs = probe.ppu.registers.OAMADDR;
            const uint8_t oamMesen = mesenState.Ppu.SpriteRamAddr;
            if (!oamStartChecked) {
                oamAgreedAtStart = (oamOurs == oamMesen);
                oamStartChecked = true;
            }
            if (oamAgreedAtStart && !oamSplitAt && oamOurs != oamMesen) {
                oamSplitAt = i;
                oamSplitOurs = oamOurs;
                oamSplitMesen = oamMesen;
            }

            const uint16_t mesenPc = mesenState.Cpu.PC;
            if (mesenPc != mesenPrevPc) {
                if (!mesenResume && mesenState.Cpu.CycleCount - mesenPcHeldSince >= kFrozenCycles) {
                    mesenResume = mesenState.Cpu.CycleCount;
                    mesenFrozenFor = mesenState.Cpu.CycleCount - mesenPcHeldSince;
                }
                mesenPcHeldSince = mesenState.Cpu.CycleCount;
                mesenPrevPc = mesenPc;
            }
        }

        std::printf("\n=== decomposition of the halt at $%04X ===\n", frozenPc);
        std::printf("  ours : DMA begins %llu, fetch at %llu, OAM %llu..%llu (%llu cycles), resumes %llu\n",
                    (unsigned long long)ourDmcAt, (unsigned long long)ourFetchAt, (unsigned long long)ourOamFrom,
                    (unsigned long long)(ourOamTo ? ourOamTo - 1 : 0),
                    (unsigned long long)(ourOamTo > ourOamFrom ? ourOamTo - ourOamFrom : 0),
                    (unsigned long long)ourResume);
        std::printf("  mesen: DMC fetch at %llu, resumes %llu\n", (unsigned long long)mesenDmcAt,
                    (unsigned long long)mesenResume);
        // Both sides against the same event, the DMC read, so the difference is a
        // disagreement rather than an artefact of measuring from different places.
        if (mesenDmcAt && mesenResume) {
            std::printf("  mesen: fetch -> resumption = %llu\n", (unsigned long long)(mesenResume - mesenDmcAt));
        }
        if (ourFetchAt && ourResume) {
            std::printf("  ours : fetch -> resumption = %llu\n", (unsigned long long)(ourResume - ourFetchAt));
        }
        // The whole stolen span, measured identically on both sides: how long each
        // CPU's PC stood still. It does not depend on where either side thinks its
        // DMAs begin and end, so it survives the two structuring them differently -
        // which they do, Mesen running both DMAs in one loop.
        std::printf("  frozen span: ours %llu, mesen %llu\n", (unsigned long long)ourFrozenFor,
                    (unsigned long long)mesenFrozenFor);
        if (!oamAgreedAtStart) {
            std::printf(
                "  OAM address differed at the alignment cycle - transfer progress cannot\n"
                "  be compared from here, and no first-difference below would mean anything.\n");
        } else if (oamSplitAt) {
            std::printf("  OAM transfer progress first differs %llu cycles in: ours $%02X, mesen $%02X\n",
                        (unsigned long long)oamSplitAt, oamSplitOurs, oamSplitMesen);
            if (ourFetchAt) {
                std::printf("    (our fetch was %llu cycles in, our OAM DMA began %llu cycles in)\n",
                            (unsigned long long)(ourFetchAt - o0.cycle),
                            (unsigned long long)(ourOamFrom ? ourOamFrom - o0.cycle : 0));
            }
        } else {
            std::printf("  OAM transfer progress never differs in the compared window\n");
        }
        Stop();
        return 0;
    }
}
