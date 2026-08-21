// Dumps Mesen2's per-frame debug events, to compare its DMA timing against
// ours cycle by cycle.
//
// SUPERSEDED, AND LOSSY BY CONSTRUCTION - use tools/mesen_cycle_trace.cpp for
// any new measurement. Mesen's events are scoped to a frame and cleared on
// StartFrame, and this tool polls for snapshots on a WALL CLOCK, so every frame
// the core finishes between two polls is dropped and its row is simply absent.
// Measured on sprdma_and_dmc_dma: 20s caught 12 of 16 rows, 60s caught 13.
// Tripling the wall time bought one row, because the limit is sampling rate,
// not duration. The dropped rows were not random - they re-aligned the table
// and produced a confident refutation of a finding that was TRUE. It is kept
// for the API notes below, which cost real debugging rounds and still apply.
// Read the rest of this header as history, not as a recommendation.
//
// NOT PART OF ANY BUILD. It works in the narrow sense: it reaches Mesen's event
// API, separates DMC DMA reads from every other DMA read, and reports both
// against the sprite DMA's first read in CPU cycles. Its companion, tools/mesen_reference_dump.cpp,
// produced the reference table in tests/test_files/fetch_dmc_dma.sh.
//
// Verified against the ROM's own arithmetic: consecutive DMC fetches come 432
// CPU cycles apart, which is sync_dmc's rate-$0F sample period (54 * 8), and a
// lone DMC DMA shows as 3 halt/dummy/align reads re-reading the CPU's own
// address followed by the fetch - the documented 4 cycles, seen directly.
//
// WHAT IT MEASURED, at Mesen2 b9fa69d. `span` is first sprite read to last, so
// an undisturbed transfer is 255 gaps * 2 = 510 cycles:
//
//   DMC at OAM-6, -4, -2   span 510   outside
//   DMC at OAM+2 ... +10   span 512   inside
//
// A DMC DMA landing inside a sprite DMA therefore costs exactly TWO cycles, and
// that is a direct measurement of the collision rather than a subtraction from a
// total - which is what every failed hypothesis on this bug did instead.
//
// The offsets step by 2 and never reach OAM+0, which is the parity gate
// quantizing the request. Any theory that needs the DMC to shift by one cycle is
// dead on this evidence alone.
//
// CAVEAT, not yet resolved: the last ten observations come in equal PAIRS
// (+2,+2,+4,+4,...). They are distinct transfers - different scanline and dot -
// so either the ROM runs two sprite DMAs per sweep row, or the sweep advances
// every second iteration. Which one decides how a line here maps to a printed
// row, so it has to be settled before any row is named.
//
// TWO API FACTS, both of which cost a debugging round. GetEvents reads a
// SNAPSHOT buffer, so TakeEventSnapshot has to be called first or the count is 0;
// and GetEventCount FILTERS on a config that defaults to all-invisible, so
// SetEventViewerConfig has to run or everything is recorded and then discarded.
//
// EVERYTHING ELSE ABOUT THE PIPELINE IS FINE, and was audited at length to no
// benefit - do not re-walk it. CpuType::Nes = 8; BaseEventViewerConfig is empty
// so SetConfiguration's downcast cannot mis-land a field; NesEventManager::
// AddEvent pushes unconditionally; nothing in Core overwrites _config;
// Debugger.cpp:252 has no gate and is instantiated for Nes at :1217;
// DebuggerRequest's destructor only decrements a counter and never frees the
// Debugger.
//
// THE REAL LESSON, three times over: every failure here was a WINDOW or a FILTER
// of this tool's own, and each one produced clean, plausible, wrong output
// rather than an error.
//
//   sleep(4) then snapshot   sampled a FINISHED ROM idling in a jmp-to-self
//                            loop. Events are per-frame (ClearFrameEvents), so
//                            every sample held a frame where nothing happened.
//   break on "any event"     tripped instantly on $2002 vblank polling, which
//                            is present from frame 1 and means nothing.
//   burst = longest run of   OAM DMA alternates read and $2004 write, and the
//   consecutive addresses    writes are Register events, so every run capped at
//                            1 and it reported NO sprite DMA on frames full of
//                            one. It would ALSO have split on a colliding DMC -
//                            the case under study - and put the burst start in
//                            the middle of the transfer.
//   suppress burst by index  256 reads span ~512 indices once the writes are
//                            counted, so half the transfer printed as unrelated
//                            DMA at OAM+256, +258, +260.
//
// So: when this disagrees with expectation, suspect the sampling window and the
// filters BEFORE suspecting Mesen or the emulator. The guard that prints
// "NOT A SPRITE DMA" exists because of the third one and is worth more than the
// feature it protects.
//
// WHY. sprdma_and_dmc_dma disagrees with us on exactly one row - row 05, the
// iteration where the DMC DMA crosses into the OAM DMA. Our sweep runs OAM-10,
// -9 ... -5 and then jumps to OAM+2; Mesen crosses one iteration earlier. The
// open question is whether Mesen's DMC halt lands LATER than ours (so it falls
// inside the OAM DMA) or in the same place but ABSORBED by the sprite DMA's
// halt and dummy cycles. Those imply different fixes, and four hypotheses have
// now failed by guessing between them instead of measuring.
//
// MemoryOperationType distinguishes DmcDmaRead from DmaRead, which is exactly
// the separation needed.
//
// THE STRUCT IS INCLUDED, NOT MARSHALLED. DebugEventInfo (Core/Debugger/
// BaseEventManager.h) nests MemoryOperationInfo and DmaChannelConfig and mixes
// widths; hand-marshalling it across dlopen would produce plausible garbage
// rather than an error. Including Mesen's own header lets the compiler compute
// the layout from the same source the library was built from.
//
//   g++ -O2 -std=c++17 -I/home/juen/projects/Mesen2 \
//       -I/home/juen/projects/Mesen2/Core \
//       -o mesen_event_dump tools/mesen_event_dump.cpp -ldl
//
//   cd /home/juen/projects/Mesen2/bin/linux-x64/Release
//   LD_LIBRARY_PATH=.:./Dependencies /path/to/mesen_event_dump \
//       ./MesenCore.so /path/to/sprdma_and_dmc_dma.nes 4
//
// If the include paths fight back, that is the expected first failure - add
// -I dirs until it resolves rather than falling back to marshalling by hand.
//
// WHAT IS STILL MISSING: which sweep ROW each observed sprite DMA belongs to.
// The sweep prints one line per distinct transfer, in order, but nothing yet
// ties a line to row 05.
//
// Do NOT tie it by counting frames or cycles. Read it off the nametable - and
// note that rows EXIST BEFORE THEY HOLD RESULTS: the non-blank row count was
// observed at 1, 3, 19 and 30 at different points in the same run, because the
// ROM prints its layout first and fills the values in as it goes. So the test
// is "row 05 has its value filled in", not "row 05 is present". Keying on
// presence would select a frame from before the measurement existed.
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Debugger/BaseEventManager.h"
#include "NES/Debugger/NesEventManager.h"

namespace
{
// CpuType::Nes - check Core/Shared/CpuType.h if the events come back empty;
// this is the first thing to suspect.
constexpr int kCpuTypeNes = 8;

// MemoryType::NesPpuMemory, and the nametable geometry - same values and same
// tile-is-the-character decode as tools/mesen_reference_dump.cpp, which is
// verified. Used here only to answer "did the ROM run", not to read a result.
constexpr int kNesPpuMemory = 9;
constexpr int kRows = 30;
constexpr int kColumns = 32;

// A sprite DMA is 256 reads. The threshold is well below that because a DMC DMA
// landing inside one steals cycles from it, and because a snapshot taken
// mid-transfer holds only the part that has happened so far - both of which are
// states worth catching rather than rejecting.
constexpr size_t kMinSpriteReads = 100;

struct DmaBurst {
    size_t start = 0;
    size_t length = 0;
    int page = -1;  // the RAM page $4014 selected, or -1 if no burst was found
};

// The sprite DMA reads 256 bytes of ONE page, so it is found by counting reads
// per page rather than by looking for a consecutive run.
//
// A run-based version of this failed twice over, and both failures produced
// confident wrong output rather than an error:
//
//  * OAM DMA alternates read and write, and NesDebugger::ProcessWrite files each
//    $2004 write as a Register event. Anything that resets the run on a non-
//    DmaRead event therefore caps every run at 1 and reports NO sprite DMA at
//    all, on a frame that plainly contains one.
//  * A DMC DMA landing inside the sprite DMA - the entire case under study -
//    splits the march in two. "Longest run" then returns the larger half and
//    puts the burst START IN THE MIDDLE of the transfer, which silently biases
//    every offset measured from it.
//
// Counting per page is immune to both: interleaved events do not matter, and
// neither does the split. The page is not hardcoded, because $4014 chooses it;
// halt, dummy and align reads re-read the CPU's own address, which is in ROM and
// so never in the RAM page the sprite DMA walks.
DmaBurst find_oam_burst(const std::vector<DebugEventInfo>& events)
{
    size_t perPage[256] = {};
    for (const DebugEventInfo& e : events) {
        if (e.Type == DebugEventType::DmaRead) {
            ++perPage[(e.Operation.Address >> 8) & 0xFF];
        }
    }

    int page = -1;
    for (int p = 0; p < 256; ++p) {
        if (perPage[p] >= kMinSpriteReads) {
            page = p;
            break;
        }
    }

    DmaBurst best;
    best.page = page;
    if (page < 0) {
        return best;
    }
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i].Type == DebugEventType::DmaRead && ((events[i].Operation.Address >> 8) & 0xFF) == page) {
            if (best.length == 0) {
                best.start = i;
            }
            ++best.length;
        }
    }
    return best;
}

// PPU dot to a monotonic position within the frame. Scanline -1 is the
// pre-render line, so the +1 keeps this non-negative; 341 dots per scanline.
long abs_dot(const DebugEventInfo& e) { return static_cast<long>(e.Scanline + 1) * 341 + e.Cycle; }
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <MesenCore.so> <rom.nes> [seconds]\n", argv[0]);
        return 2;
    }
    const int seconds = (argc > 3) ? std::atoi(argv[3]) : 4;

    void* lib = dlopen(argv[1], RTLD_NOW);
    if (lib == nullptr) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    auto InitDll = reinterpret_cast<void (*)()>(dlsym(lib, "InitDll"));
    auto InitializeEmu =
        reinterpret_cast<void (*)(const char*, void*, void*, bool, bool, bool, bool)>(dlsym(lib, "InitializeEmu"));
    auto LoadRom = reinterpret_cast<bool (*)(char*, char*)>(dlsym(lib, "LoadRom"));
    auto InitializeDebugger = reinterpret_cast<void (*)()>(dlsym(lib, "InitializeDebugger"));
    auto SetEventViewerConfig =
        reinterpret_cast<void (*)(int, NesEventViewerConfig&)>(dlsym(lib, "SetEventViewerConfig"));
    auto TakeEventSnapshot = reinterpret_cast<uint32_t (*)(int, bool)>(dlsym(lib, "TakeEventSnapshot"));
    auto GetDebugEventCount = reinterpret_cast<uint32_t (*)(int)>(dlsym(lib, "GetDebugEventCount"));
    auto GetDebugEvents = reinterpret_cast<void (*)(int, DebugEventInfo*, uint32_t&)>(dlsym(lib, "GetDebugEvents"));
    auto Stop = reinterpret_cast<void (*)()>(dlsym(lib, "Stop"));

    // Three probes, because the deduction has run out. The recording path is
    // unconditional and the config is proven to land, yet _debugEvents is empty -
    // so a PREMISE is wrong, not a step, and each of these kills a different one.
    //   IsDebuggerRunning   is `if(_debugger)` in Emulator::ProcessMemoryRead
    //                       actually true while the ROM runs?
    //   GetMemorySize/State is the ROM running AT ALL in this process? Inferring
    //                       that from mesen_reference_dump is an assumption, not
    //                       a measurement - it is a different program.
    auto IsDebuggerRunning = reinterpret_cast<bool (*)()>(dlsym(lib, "IsDebuggerRunning"));
    auto GetMemorySize = reinterpret_cast<uint32_t (*)(int)>(dlsym(lib, "GetMemorySize"));
    auto GetMemoryState = reinterpret_cast<void (*)(int, uint8_t*)>(dlsym(lib, "GetMemoryState"));

    // Every symbol is REQUIRED, and named on failure. An earlier version guarded
    // the two event-viewer ones with `if (p != nullptr)` and skipped them
    // silently, which is indistinguishable from "the ROM performed no DMA" -
    // the exact failure this tool exists to avoid producing.
    const struct {
        const char* name;
        void* p;
    } required[] = {
        {"InitDll", reinterpret_cast<void*>(InitDll)},
        {"InitializeEmu", reinterpret_cast<void*>(InitializeEmu)},
        {"LoadRom", reinterpret_cast<void*>(LoadRom)},
        {"InitializeDebugger", reinterpret_cast<void*>(InitializeDebugger)},
        {"SetEventViewerConfig", reinterpret_cast<void*>(SetEventViewerConfig)},
        {"TakeEventSnapshot", reinterpret_cast<void*>(TakeEventSnapshot)},
        {"GetDebugEventCount", reinterpret_cast<void*>(GetDebugEventCount)},
        {"GetDebugEvents", reinterpret_cast<void*>(GetDebugEvents)},
        {"Stop", reinterpret_cast<void*>(Stop)},
        {"IsDebuggerRunning", reinterpret_cast<void*>(IsDebuggerRunning)},
        {"GetMemorySize", reinterpret_cast<void*>(GetMemorySize)},
        {"GetMemoryState", reinterpret_cast<void*>(GetMemoryState)},
    };
    bool missing = false;
    for (const auto& sym : required) {
        if (sym.p == nullptr) {
            std::fprintf(stderr, "dlsym failed: %s\n", sym.name);
            missing = true;
        }
    }
    if (missing) {
        return 1;
    }

    InitDll();
    InitializeEmu("./MesenHome", nullptr, nullptr, true, true, true, true);

    std::string rom = argv[2];
    std::string patch;
    if (!LoadRom(rom.data(), patch.data())) {
        std::fprintf(stderr, "Mesen could not load %s\n", rom.c_str());
        return 1;
    }
    InitializeDebugger();  // required: GetDebugEvents goes through the debugger

    // GetEventCount runs FilterEvents() and returns only what the config says is
    // VISIBLE (Core/Debugger/BaseEventManager.cpp:24). The manager's _config is
    // zero-initialised and nothing in Core ever sets it - the UI is the only
    // caller of SetEventViewerConfig - so every event is recorded and then
    // filtered away, which reads as "the ROM performed no DMA".
    //
    // EVERY category is enabled, not just the two DMA ones. Enabling only
    // DmcDmaReads/OtherDmaReads gives a count of 0 two ways - config not landing,
    // or no events recorded - and cannot tell them apart. With all of them on,
    // the ROM's constant $4014/$4010/$4015 writes must show up as Register
    // events, so 0 now means the events genuinely are not there.
    //
    // 0xFF fills Visible=true and Color=0xFFFFFFFF across every
    // EventViewerCategoryCfg; the two trailing bools are set back by hand.
    // ShowPreviousFrameEvents in particular would fold in the previous frame and
    // make the dump span two frames, which is the opposite of what step two needs.
    NesEventViewerConfig cfg;
    std::memset(&cfg, 0xFF, sizeof(cfg));
    cfg.ShowPreviousFrameEvents = false;
    cfg.ShowNtscBorders = false;
    SetEventViewerConfig(kCpuTypeNes, cfg);

    // POLL FROM t=0, rather than sleeping first and sampling afterwards.
    //
    // Sleeping 4s and then snapshotting is what reported "0 events" for three
    // rounds of debugging. sprdma_and_dmc_dma settles by frame 157 - about 2.6s -
    // and a FINISHED blargg ROM sits in a jmp-to-self loop: no OAM DMA, no DMC
    // DMA, no register reads. Events are per-frame (ClearFrameEvents, Debugger.cpp
    // :596), so every snapshot after the ROM went idle catches a frame in which
    // genuinely nothing happened. The only event still pushed each frame is
    // BgColorChange, and GetEventConfig has no case for it, so it returns {} and
    // filters out as invisible. Count: 0 - from a perfectly working pipeline.
    //
    // The whole recording chain had been verified correct by then. The wrong
    // premise was that the ROM was still doing DMA at the moment of sampling.
    std::fprintf(stderr, "polling for up to %d seconds...\n", seconds);
    // Break on a frame containing DMA, not on a frame containing ANY event. The
    // ROM polls $2002 in a vblank wait loop from the moment it starts, so "some
    // events" is true almost immediately and says nothing about whether the
    // sampled frame is one of the ones under test.
    uint32_t count = 0;
    size_t dmaCount = 0;
    std::vector<DebugEventInfo> events;
    for (int tick = 0; tick < seconds * 20; ++tick) {
        usleep(50000);
        TakeEventSnapshot(kCpuTypeNes, false);
        count = GetDebugEventCount(kCpuTypeNes);
        if (count == 0) {
            continue;
        }
        events.assign(count, DebugEventInfo{});
        uint32_t returned = count;
        GetDebugEvents(kCpuTypeNes, events.data(), returned);
        events.resize(returned);

        // Break on a frame containing a SPRITE DMA, not one containing any DMA.
        // The ROM's dmc_timer calibration runs lone 4-cycle DMC DMAs for most of
        // a second before the sweep starts, and those satisfy "some DMA" while
        // containing no collision at all - the thing under test.
        dmaCount = find_oam_burst(events).length;
        if (dmaCount >= kMinSpriteReads) {
            std::fprintf(stderr, "%u events, OAM burst of %zu reads, at t=%.2fs\n", returned, dmaCount, tick * 0.05);
            break;
        }
    }
    count = dmaCount > 0 ? static_cast<uint32_t>(events.size()) : 0;

    // PROBE 1: is the ROM executing in THIS process? mesen_reference_dump proves
    // only that it runs in a different program. A blank nametable here means the
    // event question was never the question.
    //
    // It is ALSO how "the ROM finished" is read: a full table of non-blank rows
    // next to a zero event count means the sampling window closed too late, not
    // that the DMA is missing.
    const uint32_t ppuSize = GetMemorySize(kNesPpuMemory);
    int nonBlankRows = 0;
    if (ppuSize >= 0x2000 + kRows * kColumns) {
        std::string vram(ppuSize, '\0');
        GetMemoryState(kNesPpuMemory, reinterpret_cast<uint8_t*>(vram.data()));
        for (int row = 0; row < kRows; ++row) {
            for (int col = 0; col < kColumns; ++col) {
                const uint8_t tile = static_cast<uint8_t>(vram[0x2000 + row * kColumns + col]);
                if (tile > 0x20 && tile < 0x7F) {
                    ++nonBlankRows;
                    break;
                }
            }
        }
    }
    std::fprintf(stderr, "ppu memory: %u bytes, %d non-blank nametable rows%s\n", ppuSize, nonBlankRows,
                 nonBlankRows == 0 ? "  <- THE ROM IS NOT RUNNING" : "  <- the ROM is running");

    // PROBE 2: is the debugger alive when asked from THIS thread? Emulator::
    // ProcessMemoryRead records only under `if(_debugger)`, so a false here
    // explains an empty _debugEvents completely.
    std::fprintf(stderr, "debugger running: %s\n", IsDebuggerRunning() ? "yes" : "no");

    if (count == 0) {
        std::fprintf(stderr,
                     "no events, with EVERY category visible, polling from t=0.\n"
                     "Read the two probe lines above FIRST - they say which premise broke:\n"
                     "  ROM not running        the event API was never the problem\n"
                     "  debugger not running   Emulator::ProcessMemoryRead records only\n"
                     "                         under `if(_debugger)`, so nothing was logged\n"
                     "  ROM running AND a full   the ROM finished and is idling in a\n"
                     "  table of non-blank rows  jmp-to-self loop, so the frames being\n"
                     "                           sampled contain no DMA at all. Poll\n"
                     "                           earlier; do not debug the event API.\n"
                     "Ruled out, with sources, at Mesen2 b9fa69d - do not re-walk these:\n"
                     "  CpuType::Nes = 8            Core/Shared/CpuType.h\n"
                     "  event manager reachable     TakeEventSnapshot returned 262\n"
                     "  config layout               BaseEventViewerConfig is EMPTY, so the\n"
                     "                              downcast in SetConfiguration is a no-op\n"
                     "  a gate on recording         NesEventManager::AddEvent pushes\n"
                     "                              unconditionally; nothing enables it\n"
                     "  a gate on the read path     Debugger.cpp:252 has none, and is\n"
                     "                              instantiated for Nes at :1217\n"
                     "  config lost between calls   DebuggerRequest's dtor only decrements\n"
                     "                              a counter; it never frees the Debugger\n"
                     "  a single unlucky snapshot   10 of them now, spread over ~1s\n");
        Stop();    // the emulator thread is still running - returning without this
        return 1;  // is what segfaulted on exit
    }

    // DMA only. The $2002 vblank-poll traffic is thousands of events per frame and
    // buries the handful that matter. Compared against the enum rather than the
    // numbers it happens to have (DmcDmaRead=6, DmaRead=7) so that a Mesen bump
    // that reorders DebugEventType is a compile error, not a silently wrong dump.
    //
    // DMC vs everything-else is the split NesDebugger.cpp:198 makes, on
    // NesCpu::IsDmcDma(). It is NOT a DMC-vs-OAM split, and labelling it that way
    // is wrong: _isDmcDmaRead is set only around the DMC fetch (NesCpu.cpp:406),
    // so DmaRead covers BOTH the sprite reads (:415) AND the halt/dummy/align
    // reads (:363, :424, :442) of either DMA. An earlier version of this loop
    // printed "OAM" for that bucket, which made a lone 4-cycle DMC DMA - 3 halt
    // cycles re-reading the CPU's own address, then the fetch - look like an OAM
    // DMA colliding with a DMC one. Exactly the plausible-wrong artifact that
    // sends this investigation into another dead end.
    //
    // So: "oth", and read the address. Halt/dummy/align re-read the CPU's next
    // address, so addr tracks pc. Sprite reads march through a $xx00 page and come
    // 256 at a time. The two are unmistakable in the dump; a label guessing
    // between them would not be.
    //
    // Everything is reported RELATIVE TO THE SPRITE DMA'S FIRST READ, in CPU
    // cycles, because that is what our own OAM-5 is measured from. Reporting
    // absolute scanline/dot would need a formula to compare against ours, and
    // subtracting against a baseline taken under different conditions is exactly
    // what made the "11-cycle phase error" wrong enough to retract.
    //
    // Dots are converted at 3 per CPU cycle (NTSC). A DMA read always lands on a
    // CPU cycle boundary, so a remainder means the conversion or the timebase is
    // wrong - it is flagged rather than rounded away.
    const DmaBurst burst = find_oam_burst(events);
    const long oamStart = abs_dot(events[burst.start]);
    if (burst.length < kMinSpriteReads) {
        std::printf(
            "*** NOT A SPRITE DMA: the longest marching run is %zu reads, not ~256.\n"
            "*** The poll timed out before the sweep started, so every offset below\n"
            "*** is measured from a halt read and means nothing. Raise the seconds\n"
            "*** argument; do not read the numbers.\n",
            burst.length);
    }
    std::printf("OAM DMA: %zu reads from $%04X, scanline %d dot %u\n", burst.length,
                static_cast<unsigned>(events[burst.start].Operation.Address), events[burst.start].Scanline,
                events[burst.start].Cycle);

    // Only the events OUTSIDE the sprite burst: the 256 marching reads are noise
    // once their start is known, and the halt/dummy/align reads plus the DMC
    // fetches are the entire question.
    for (size_t i = 0; i < events.size(); ++i) {
        const DebugEventInfo& e = events[i];
        const bool isDmc = e.Type == DebugEventType::DmcDmaRead;
        if (!isDmc && e.Type != DebugEventType::DmaRead) {
            continue;
        }
        // Suppress the sprite reads BY PAGE, not by index range. Reads alternate
        // with the $2004 writes that NesDebugger::ProcessWrite files as Register
        // events, so 256 reads span roughly 512 indices - an index range covers
        // barely half the transfer and lets its tail print as though it were
        // unrelated DMA. That looked like reads at OAM+256, +258, +260 sitting
        // suspiciously outside the burst, which is a fictitious finding.
        if (!isDmc && burst.page >= 0 && ((e.Operation.Address >> 8) & 0xFF) == burst.page) {
            continue;
        }
        const long dots = abs_dot(e) - oamStart;
        const long cycles = dots / 3;
        std::printf("  %-3s  OAM%+5ld cyc  (%+6ld dots%s)  addr $%04X  pc $%04X\n", isDmc ? "DMC" : "oth", cycles, dots,
                    (dots % 3 != 0) ? " NOT ON A CPU BOUNDARY" : "", static_cast<unsigned>(e.Operation.Address),
                    static_cast<unsigned>(e.ProgramCounter));
    }

    // THE SWEEP. One frame is one iteration of the ROM's test, and a single
    // iteration cannot show a crossing - our sweep and Mesen's differ in WHERE
    // the DMC stops falling outside the sprite DMA and starts falling inside it,
    // which is only visible across a series.
    //
    // Reported per observation: the DMC fetch nearest the sprite DMA's first
    // read. "Inside" is decided against the transfer's own last read rather than
    // a 513-cycle constant, because a colliding DMC lengthens the very transfer
    // being measured - assuming the textbook length is how a collision would be
    // made to look like no collision.
    std::printf("\nsweep - one line per distinct sprite DMA:\n");
    // 4ms, not 10ms. A frame is 16.7ms and events are per-frame, so a snapshot
    // landing mid-frame BEFORE that row's sprite DMA sees nothing, and at 10ms
    // the next poll is already into the following row - which is how a 16-row
    // sweep came back as 13 rows with no indication that three were missing.
    // Oversampling is free here: identical consecutive transfers are deduped on
    // the burst's start dot.
    long lastKey = -1;
    int observed = 0;
    for (int tick = 0; tick < seconds * 250 && observed < 40; ++tick) {
        usleep(4000);
        TakeEventSnapshot(kCpuTypeNes, false);
        const uint32_t n = GetDebugEventCount(kCpuTypeNes);
        if (n == 0) {
            continue;
        }
        events.assign(n, DebugEventInfo{});
        uint32_t got = n;
        GetDebugEvents(kCpuTypeNes, events.data(), got);
        events.resize(got);

        const DmaBurst b = find_oam_burst(events);
        if (b.length < kMinSpriteReads) {
            continue;
        }
        const long start = abs_dot(events[b.start]);
        if (start == lastKey) {
            continue;  // same transfer seen twice; snapshots outpace the ROM
        }
        lastKey = start;

        long end = start;
        for (const DebugEventInfo& e : events) {
            if (e.Type == DebugEventType::DmaRead && ((e.Operation.Address >> 8) & 0xFF) == b.page) {
                end = std::max(end, abs_dot(e));
            }
        }

        // PRE: how many DMA cycles run BEFORE the first sprite read - the OAM
        // DMA's halt, plus its alignment cycle when one is needed. Mesen files
        // those as DmaRead events too, at the CPU's own read address rather than
        // on the sprite page (_isDmcDmaRead is set only around NesCpu.cpp:406),
        // so they sit contiguously just below the burst and are found by walking
        // back one CPU cycle - three dots - at a time.
        //
        // This is the number the diagnostic's finding 10 asks for. Our side
        // alternates pre 2,3 and puts the crossing pair at start-2 and start-1,
        // both before the DMA begins, so neither collides. Mesen's `near -2` is
        // dmcAbs == start when pre is 2 - a collision - but start-1 when pre is
        // 3, which is not one. The two answers are different bugs in different
        // files, so this decides which, and guessing between them is what has
        // to be avoided.
        long pre = 0;
        for (long dot = start - 3;; dot -= 3) {
            bool found = false;
            for (const DebugEventInfo& e : events) {
                if (e.Type == DebugEventType::DmaRead && abs_dot(e) == dot &&
                    ((e.Operation.Address >> 8) & 0xFF) != b.page) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                break;
            }
            ++pre;
        }

        bool haveDmc = false;
        long nearest = 0;
        int inside = 0;
        for (const DebugEventInfo& e : events) {
            if (e.Type != DebugEventType::DmcDmaRead) {
                continue;
            }
            const long d = abs_dot(e) - start;
            if (!haveDmc || std::labs(d) < std::labs(nearest)) {
                nearest = d;
                haveDmc = true;
            }
            if (abs_dot(e) >= start && abs_dot(e) <= end) {
                ++inside;
            }
        }

        std::printf("  sl %3d dot %3u  reads %3zu  span %4ld cyc  pre %ld  ", events[b.start].Scanline,
                    events[b.start].Cycle, b.length, (end - start) / 3, pre);
        if (haveDmc) {
            std::printf("nearest DMC OAM%+5ld  inside %d\n", nearest / 3, inside);
        } else {
            std::printf("no DMC fetch this frame\n");
        }
        ++observed;
    }
    std::printf("(%d sprite DMAs observed)\n", observed);
    if (observed < 16) {
        std::printf(
            "*** INCOMPLETE: the ROM's sweep is 16 rows and only %d were caught.\n"
            "*** Missing rows are invisible here - the ones that matter are at the\n"
            "*** crossing, and a short table looks exactly like a complete one.\n"
            "***\n"
            "*** RAISING THE SECONDS DOES NOT FIX THIS. Measured: 20s caught 12\n"
            "*** rows, 60s caught 13. Tripling the run bought one row, so the\n"
            "*** limit is not duration. What is missing is structural - the\n"
            "*** rows that survive are the ones with a DMC INSIDE the transfer,\n"
            "*** which come in pairs (+2,+2 +4,+4 ...), while the outside rows\n"
            "*** arrive as singletons (-6, -4, -2) where ours are paired. The\n"
            "*** three absent rows are the second of each outside pair.\n"
            "***\n"
            "*** Suspect the sampler, not the ROM: events are per-frame and this\n"
            "*** polls every 4ms, so any frame the core finishes faster than that\n"
            "*** is never snapshotted. Fix the capture before reading this table.\n",
            observed);
    }

    Stop();  // the emulator thread outlives main() otherwise, and segfaults on exit
    return 0;
}
