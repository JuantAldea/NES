// What Mesen's PPU read buffer does across a DMC DMA that collides with a
// $2007 read - the one question the printed table can only be reasoned back to.
//
// dma_2007_read prints five rows and its third depends on how many repeated
// reads land before the CPU's own. Walking the buffer backwards from a printed
// row gives a count but not WHICH cycle each read happened on, so the ordering
// cannot be settled from it. This reads the buffer directly, once per CPU
// cycle.
//
// NesPpuState carries MemoryReadBuffer, VideoRamAddr and BusAddress, so a read
// of $2007 is visible as the buffer changing and the address stepping, on the
// exact cycle it happens. ApuDmcState::BytesRemaining marks the DMC's own fetch,
// which is what separates the DMA's get cycle from the no-op cycles around it.
//
// NO CROSS-EMULATOR CLOCK IS USED OR NEEDED. Everything printed is Mesen against
// Mesen, relative to the landmark PC below, for the same reason
// tools/mesen_dma_map.cpp measures each side against its own transfer: the two
// counters have no fixed correspondence and inventing one has produced a wrong
// answer that looked like a measurement twice.
//
// Usage, from tools/run_mesen_dump.sh's Release directory:
//   mesen_2007_trace ./MesenCore.so <rom> <seekCycle> <landmarkPc> <rows> <window>
//
// <landmarkPc> is single-stepped to, <rows> times; each time, <window> CPU
// cycles are traced from it. $E1xx-range PCs inside the timed section are the
// useful anchors - see the ROM's source in nes-test-roms.
#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Core/NES/NesTypes.h"
#include "Core/Shared/BaseState.h"

namespace
{
constexpr int kCpuTypeNes = 8;
constexpr int kStepCpuCycle = 3;
constexpr int kConsoleTypeNes = 2;

// Seeking is chunked because the cost is the interop CALL, about 2 ms, whether
// it asks for one cycle or twenty thousand. Only the traced window is stepped
// one cycle at a time.
constexpr uint32_t kChunk = 20000;
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <MesenCore.so> <rom> <seekCycle> [landmarkPc] [rows] [window]\n", argv[0]);
        return 1;
    }
    const uint64_t seekTo = std::strtoull(argv[3], nullptr, 10);
    const uint16_t landmarkPc = (argc > 4) ? static_cast<uint16_t>(std::strtoul(argv[4], nullptr, 16)) : 0xE1AF;
    const int rows = (argc > 5) ? std::atoi(argv[5]) : 5;
    const int window = (argc > 6) ? std::atoi(argv[6]) : 24;

    // The landmark recurs about every 20580 cycles in this ROM, and hunting it
    // by single-stepping costs an interop call each - so most of the gap is
    // chunked and only the last stretch is stepped.
    const uint32_t skipBetweenRows = (argc > 7) ? std::strtoul(argv[7], nullptr, 10) : 19000;
    const int huntBudget = (argc > 8) ? std::atoi(argv[8]) : 6000;

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
    auto readCpu = [&]() { GetCpuState(reinterpret_cast<BaseState&>(cpu), kCpuTypeNes); };
    auto readAll = [&]() { GetConsoleState(reinterpret_cast<BaseState&>(full), kConsoleTypeNes); };

    readCpu();
    const uint64_t origin = cpu.CycleCount;
    std::printf("mesen origin %llu (no fixed relation to ours - see the header)\n",
                static_cast<unsigned long long>(origin));

    // Mesen is already running when the debugger attaches, so its counter starts
    // wherever the scheduler left it. Seeking to OUR cycle number without
    // subtracting that lands a whole origin early.
    const uint64_t target = origin + seekTo;
    while (true) {
        readCpu();
        if (cpu.CycleCount >= target) {
            break;
        }
        const uint64_t left = target - cpu.CycleCount;
        step(left > kChunk ? kChunk : static_cast<uint32_t>(left));
    }

    for (int row = 0; row < rows; ++row) {
        // Single-step to the landmark. A miss is reported rather than silently
        // tracing the wrong place.
        bool found = false;
        for (int i = 0; i < huntBudget; ++i) {
            readCpu();
            if (cpu.PC == landmarkPc) {
                found = true;
                break;
            }
            step(1);
        }
        if (!found) {
            readCpu();
            std::printf("row %d: landmark $%04X not reached in %d steps; PC is $%04X at cycle %llu\n", row, landmarkPc,
                        huntBudget, cpu.PC, static_cast<unsigned long long>(cpu.CycleCount));
            break;
        }

        readCpu();
        const uint64_t base = cpu.CycleCount;
        std::printf("\n--- row %d, from $%04X ---\n", row, landmarkPc);
        std::printf("  off  PC     v      buf  bus   dmcBytes\n");

        uint8_t prevBuf = 0;
        uint16_t prevV = 0;
        bool first = true;
        for (int i = 0; i < window; ++i) {
            readCpu();
            readAll();
            const uint8_t buf = full.Ppu.MemoryReadBuffer;
            const uint16_t v = full.Ppu.VideoRamAddr;
            const char* mark = "";
            if (!first && (buf != prevBuf || v != prevV)) {
                mark = "   <-- $2007 read";
            }
            std::printf("  %3lld  $%04X  $%04X  $%02X  $%04X  %u%s\n", static_cast<long long>(cpu.CycleCount - base),
                        cpu.PC, v, buf, full.Ppu.BusAddress, full.Apu.Dmc.BytesRemaining, mark);
            prevBuf = buf;
            prevV = v;
            first = false;
            step(1);
        }
        step(1);
        if (row + 1 < rows) {
            step(skipBetweenRows);
        }
    }

    Stop();
    return 0;
}
