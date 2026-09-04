// Reports, for each of Mesen's OAM transfers, where the DMC's read lands
// relative to the END of that transfer - the same quantity NES_DMA_TRACE prints
// for this emulator, so the two can be compared directly.
//
// WHY NOT LOCKSTEP. Aligning the two emulators needs a correspondence between
// their cycle counters, and there is none to be had. Mesen's counter starts
// wherever the scheduler left it - 14914, 20005 and 27279 across runs of one ROM
// - and on top of that the two DRIFT, because sprdma_and_dmc_dma is a timing
// test and they do not agree about its timing. Modelling that offset - either by
// ignoring the origin or by subtracting it - aligns a CONTROL row onto the wrong
// moment, which is a wrong answer that looks like a measurement.
//
// Nothing here needs the correspondence. "Where does the fetch land inside the
// transfer" is measured against that side's OWN transfer, on both sides, and the
// derived offsets are what get compared. The Nth transfer is the Nth row of the
// sweep on either emulator without any clock agreeing with any other.
//
// COST IS PER CALL, NOT PER CYCLE. Step()+IsPaused() is ~2 ms whether it is asked
// for 1 cycle or 200, so the scan for transfers is chunked and only the ~600
// cycles inside each one are single-stepped. A chunk has to be under half a
// transfer for two consecutive samples to be guaranteed to land inside it, hence
// 200 against 513.
//
//   M=/home/juen/projects/Mesen2
//   g++ -O2 -std=c++20 -Wall -Wextra -I "$M" -I "$M/Core"
//       -o /tmp/dmamap tools/mesen_dma_map.cpp -ldl
//   cd "$M/bin/linux-x64/Release"
//   LD_LIBRARY_PATH=.:./Dependencies /tmp/dmamap ./MesenCore.so <rom> [max-cycles]
#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "Core/NES/NesTypes.h"

namespace
{
constexpr int kCpuTypeNes = 8;
constexpr int kStepCpuCycle = 3;
constexpr int kConsoleTypeNes = 2;

// Under half a transfer, so two consecutive samples must both land inside one.
constexpr uint32_t kScanChunk = 200;
// A PC held this long is a DMA rather than an instruction or a wait loop.
constexpr int kFreezeConfirm = 40;
// How far past the transfer to keep watching for a fetch that just missed it.
constexpr int kTailCycles = 40;
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <MesenCore.so> <rom.nes> [max-cycles]\n", argv[0]);
        return 2;
    }
    const uint64_t maxCycles = (argc > 3) ? std::strtoull(argv[3], nullptr, 10) : 4200000;

    // ANCHORED MODE, for comparing a distance INSIDE the ROM's timed section.
    // The printed value is measured by code_timer between time_code_begin and
    // time_code_end, and everything outside that diverges between emulators by
    // tens of thousands of cycles - so only a distance anchored within the
    // section can carry a one-cycle signal. `sta $4015` at $E376 runs exactly
    // once a row, ~3000 cycles before the transfer ends, which makes it the
    // anchor. Finding a specific PC needs single-stepping, so the scan chunks to
    // just before it and single-steps only the last few thousand cycles.
    const uint64_t anchorFrom = (argc > 4) ? std::strtoull(argv[4], nullptr, 10) : 0;
    const uint16_t anchorPc = (argc > 5) ? static_cast<uint16_t>(std::strtoul(argv[5], nullptr, 16)) : 0;
    const int anchorRows = (argc > 6) ? std::atoi(argv[6]) : 6;
    const uint16_t tailPc = (argc > 7) ? static_cast<uint16_t>(std::strtoul(argv[7], nullptr, 16)) : 0;

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
    auto GetConsoleState = reinterpret_cast<void (*)(BaseState&, int)>(dlsym(lib, "GetConsoleState"));
    auto IsPaused = reinterpret_cast<bool (*)()>(dlsym(lib, "IsPaused"));
    auto IsDebuggerRunning = reinterpret_cast<bool (*)()>(dlsym(lib, "IsDebuggerRunning"));
    auto Stop = reinterpret_cast<void (*)()>(dlsym(lib, "Stop"));
    if (!InitDll || !InitializeEmu || !LoadRom || !InitializeDebugger || !Step || !GetCpuState || !GetConsoleState ||
        !IsPaused || !IsDebuggerRunning || !Stop) {
        std::fprintf(stderr, "a required symbol is missing from the interop DLL\n");
        return 1;
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
    NesState full{};
    auto step = [&](uint32_t n) {
        Step(kCpuTypeNes, n, kStepCpuCycle);
        for (int spin = 0; spin < 100000000 && !IsPaused(); ++spin) {
        }
    };
    auto pc = [&]() -> uint16_t {
        GetCpuState(reinterpret_cast<BaseState&>(cpu), kCpuTypeNes);
        return cpu.PC;
    };
    auto cyc = [&]() -> uint64_t {
        GetCpuState(reinterpret_cast<BaseState&>(cpu), kCpuTypeNes);
        return cpu.CycleCount;
    };
    auto bytesRemaining = [&]() -> uint16_t {
        GetConsoleState(reinterpret_cast<BaseState&>(full), kConsoleTypeNes);
        return full.Apu.Dmc.BytesRemaining;
    };
    // The transfer's own end, as distinct from the CPU's resume: $2004 bumps the
    // OAM address once per byte written, so the last bump is the last write. Our
    // side puts that exactly on the freeze's last cycle on all sixteen rows, which
    // is what makes "fetch against the freeze end" mean "fetch against the
    // transfer end" there. If Mesen does not agree, the two columns are not
    // measuring the same thing and must not be subtracted.
    auto oamAddr = [&]() -> uint8_t {
        GetConsoleState(reinterpret_cast<BaseState&>(full), kConsoleTypeNes);
        return full.Ppu.SpriteRamAddr;
    };

    if (anchorPc) {
        // Chunk to the hint, then single-step: each row's anchor is found from the
        // PREVIOUS row's anchor plus a row interval, which is self-correcting -
        // the drift that matters is then only one row's worth, not the run's.
        constexpr uint64_t kRowInterval = 174000;  // deliberately short of ~178000
        constexpr int kAnchorSearch = 90000;       // first row only; later ones need ~8000
        std::printf("row   anchor      transfer end   end-anchor\n");
        uint64_t prevAnchor = 0;
        for (int r = 0; r < anchorRows; ++r) {
            const uint64_t target = prevAnchor ? prevAnchor + kRowInterval : anchorFrom;
            while (cyc() + kScanChunk < target) {
                step(kScanChunk);
            }
            uint64_t anchor = 0;
            for (int i = 0; i < kAnchorSearch; ++i) {
                step(1);
                if (pc() == anchorPc) {
                    anchor = cyc();
                    break;
                }
            }
            if (!anchor) {
                std::fprintf(stderr, "row %d: anchor $%04X not found - nothing below is comparable\n", r, anchorPc);
                Stop();
                return 1;
            }
            // Now single-step into the transfer and to its last cycle.
            uint16_t held = 0, prevP = pc();
            uint64_t end = 0;
            for (int i = 0; i < 12000 && !end; ++i) {
                step(1);
                const uint16_t p = pc();
                if (p == prevP) {
                    if (++held >= 40) {
                        for (int j = 0; j < 900; ++j) {
                            step(1);
                            if (pc() != p) {
                                end = cyc() - 1;
                                break;
                            }
                        }
                    }
                } else {
                    held = 0;
                    prevP = p;
                }
            }
            // A SECOND LANDMARK PAST THE TRANSFER, so the span measured contains
            // the code after it. The span ending at the transfer's last cycle
            // cannot hold a defect in the `sta $100` that follows, which is how a
            // prediction about that store came to be tested against a window
            // structurally unable to show it.
            // THE POST-TRANSFER DMA'S LENGTH, measured as the CPU stall around the
            // fetch. Once the transfer is over the CPU is running again, so the
            // only thing holding PC still for several cycles is the DMC's own
            // stall - which makes its length directly comparable to this
            // emulator's, where the same span is 5 cycles at rows 08, 0A and 0C
            // alike.
            uint64_t tail = 0, stallFrom = 0, stallTo = 0, fetchAt = 0;
            uint16_t prevB = bytesRemaining();
            uint16_t lastPc = pc();
            uint64_t heldSince = cyc();
            for (int i = 0; i < 400 && !tail; ++i) {
                step(1);
                if (bytesRemaining() != prevB) {
                    fetchAt = cyc();
                    prevB = bytesRemaining();
                }
                const uint16_t p = pc();
                if (p != lastPc) {
                    if (fetchAt && !stallTo && heldSince < fetchAt) {
                        stallFrom = heldSince;
                        stallTo = cyc() - 1;
                    }
                    lastPc = p;
                    heldSince = cyc();
                }
                if (tailPc && p == tailPc) {
                    tail = cyc();
                }
            }
            std::printf(" %02X  end-anchor %lld   tail %+lld   fetch %+lld   stall %lld cycles\n", r,
                        static_cast<long long>(end) - static_cast<long long>(anchor),
                        tail ? static_cast<long long>(tail) - static_cast<long long>(end) : 0,
                        fetchAt ? static_cast<long long>(fetchAt) - static_cast<long long>(end) : 0,
                        stallTo ? static_cast<long long>(stallTo - stallFrom + 1) : 0);
            std::printf("      stall spans %+lld .. %+lld\n",
                        static_cast<long long>(stallFrom) - static_cast<long long>(end),
                        static_cast<long long>(stallTo) - static_cast<long long>(end));
            std::fflush(stdout);
            prevAnchor = anchor;
        }
        Stop();
        return 0;
    }

    std::printf("row  transfer  fetch relative to the transfer's last cycle\n");

    const uint64_t start = cyc();
    uint16_t prevPc = pc();
    int row = 0;

    while (cyc() - start < maxCycles && row < 16) {
        step(kScanChunk);
        const uint16_t nowPc = pc();
        if (nowPc != prevPc) {
            prevPc = nowPc;
            continue;
        }

        // A REPEATED PC IS NOT YET A TRANSFER: this ROM spends most of its time
        // in delay loops, which can show the same PC two samples apart. Confirm
        // by single-stepping - only a DMA holds it for kFreezeConfirm.
        bool frozen = true;
        for (int i = 0; i < kFreezeConfirm; ++i) {
            step(1);
            if (pc() != nowPc) {
                frozen = false;
                break;
            }
        }
        prevPc = pc();
        if (!frozen) {
            continue;
        }

        // Inside transfer `row`. Single-step to its last cycle, then a little
        // past, recording every cycle BytesRemaining moves - that is the fetch.
        uint16_t prevBytes = bytesRemaining();
        uint8_t prevOam = oamAddr();
        int oamWrites = 0;
        uint64_t lastWrite = 0;
        std::vector<uint64_t> fetches;
        uint64_t endCycle = 0;
        for (int i = 0; i < 900; ++i) {
            step(1);
            const uint16_t b = bytesRemaining();
            if (b != prevBytes) {
                fetches.push_back(cyc());
            }
            prevBytes = b;
            const uint8_t o = oamAddr();
            if (o != prevOam) {
                ++oamWrites;
                lastWrite = cyc();
                prevOam = o;
            }
            if (pc() != nowPc) {
                endCycle = cyc() - 1;  // the last cycle PC was still held
                break;
            }
        }
        for (int i = 0; i < kTailCycles; ++i) {
            step(1);
            const uint16_t b = bytesRemaining();
            if (b != prevBytes) {
                fetches.push_back(cyc());
            }
            prevBytes = b;
        }
        prevPc = pc();

        std::printf(" %02X  ends %llu  oamwrites %3d  lastwrite %+lld  fetch ", row,
                    static_cast<unsigned long long>(endCycle), oamWrites,
                    static_cast<long long>(lastWrite) - static_cast<long long>(endCycle));
        if (fetches.empty()) {
            std::printf("no fetch within %d cycles of the end\n", kTailCycles);
        } else {
            for (const uint64_t f : fetches) {
                const long long rel = static_cast<long long>(f) - static_cast<long long>(endCycle);
                std::printf("%+lld ", rel);
            }
            std::printf("\n");
        }
        std::fflush(stdout);
        ++row;
    }

    if (row < 16) {
        std::fprintf(stderr, "only %d transfers found in %llu cycles - the table above is incomplete\n", row,
                     static_cast<unsigned long long>(maxCycles));
    }
    Stop();
    return 0;
}
