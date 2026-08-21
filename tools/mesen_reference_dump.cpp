// Dumps a test ROM's printed screen out of Mesen2, to get reference values for
// ROMs that publish none.
//
// NOT PART OF ANY BUILD. tests/CMakeLists.txt globs tests/*.cpp only, so this
// file is deliberately outside it - it links against Mesen, not against us.
//
// WHY THIS EXISTS. sprdma_and_dmc_dma self-checks by CRC ($FBADA48D) and prints
// no expected values, and none are published anywhere - not in
// christopherpow/nes-test-roms' status.txt, not in erspicu/AprNes' per-ROM CRC
// catalogue. So an implementation that fails it gets a verdict and no target.
// Deriving the target by hand was attempted at length and produced four
// confident wrong answers; reading it off an emulator that passes took twelve
// seconds. See the header of tests/test_files/fetch_dmc_dma.sh for the table
// this produced and for what it corrected.
//
// MESEN IS NOT COMMITTED, for the same reason the test ROMs are not: it is a
// large third-party artifact, and what belongs in the repo is the recipe.
//
//   git clone https://github.com/SourMesen/Mesen2.git
//   cd Mesen2 && git checkout b9fa69d      # the revision the table came from
//   make core -j"$(nproc)"                 # InteropDLL only; no .NET, no UI
//
//   g++ -O2 -o mesen_reference_dump tools/mesen_reference_dump.cpp -ldl
//   cd Mesen2/bin/linux-x64/Release
//   LD_LIBRARY_PATH=.:./Dependencies /path/to/mesen_reference_dump \
//       ./MesenCore.so /path/to/rom.nes 12
//
// Pin the revision when recording a result. "Mesen passes it" is not a citable
// measurement; "Mesen2 b9fa69d prints these sixteen numbers" is.
//
// HOW IT READS THE SCREEN. It pulls MemoryType::NesPpuMemory (the $0000-$3FFF
// PPU address space) through the debugger's memory dumper and decodes
// $2000-$23FF with the same rule tests/nametable_screen.h uses: the 2005-era
// blargg ROMs write ASCII-mapped tile indices, so the tile index IS the
// character. Decoding both sides identically is the point - a reference that
// went through a different reader would not be comparable.
//
// It runs in real time and then samples, rather than stepping frames, because
// the screens it reads are static once the ROM has settled. Twelve seconds is
// ~720 frames; sprdma_and_dmc_dma settles by frame 157.
#include <dlfcn.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
constexpr int kNesPpuMemory = 9;  // MemoryType::NesPpuMemory, from Core/Shared/MemoryType.h
constexpr uint16_t kNametableBase = 0x2000;
constexpr int kRows = 30;
constexpr int kColumns = 32;
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <MesenCore.so> <rom.nes> [seconds]\n", argv[0]);
        return 2;
    }
    const int seconds = (argc > 3) ? std::atoi(argv[3]) : 12;

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
    auto GetMemorySize = reinterpret_cast<uint32_t (*)(int)>(dlsym(lib, "GetMemorySize"));
    auto GetMemoryState = reinterpret_cast<void (*)(int, uint8_t*)>(dlsym(lib, "GetMemoryState"));
    auto Stop = reinterpret_cast<void (*)()>(dlsym(lib, "Stop"));

    if (!InitDll || !InitializeEmu || !LoadRom || !GetMemorySize || !GetMemoryState) {
        std::fprintf(stderr, "a required symbol is missing - is this MesenCore.so?\n");
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
    if (InitializeDebugger != nullptr) {
        InitializeDebugger();
    }

    std::fprintf(stderr, "running for %d seconds...\n", seconds);
    sleep(seconds);

    const uint32_t size = GetMemorySize(kNesPpuMemory);
    if (size < kNametableBase + kRows * kColumns) {
        std::fprintf(stderr, "NesPpuMemory is %u bytes - wrong MemoryType, or nothing loaded\n", size);
        return 1;
    }

    std::string buffer(size, '\0');
    GetMemoryState(kNesPpuMemory, reinterpret_cast<uint8_t*>(buffer.data()));

    for (int row = 0; row < kRows; ++row) {
        std::string line;
        bool any = false;
        for (int col = 0; col < kColumns; ++col) {
            const uint8_t tile = static_cast<uint8_t>(buffer[kNametableBase + row * kColumns + col]);
            line.push_back((tile >= 0x20 && tile < 0x7F) ? static_cast<char>(tile) : '.');
            if (tile > 0x20 && tile < 0x7F) {
                any = true;
            }
        }
        if (any) {
            std::printf("%s\n", line.c_str());
        }
    }

    if (Stop != nullptr) {
        Stop();
    }
    return 0;
}
