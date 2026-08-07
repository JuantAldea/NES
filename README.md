# NES Emulator
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)

A Nintendo Entertainment System emulator written in C++20, with an SDL2 +
Dear ImGui frontend that shows the picture and the machine drawing it side by
side.

## Current status

The CPU and the PPU render backgrounds and sprites, the frontend puts the
picture on screen, and the controllers work. A mapper 0, 2 or 3 game should draw
and respond to input - the 240p Test Suite runs, which is the most demanding
thing here. There is no audio, and no mapper beyond those three, so MMC3's
several hundred titles are still out of reach.

| Area | State |
|---|---|
| 6502 CPU | Cycle-accurate, all 256 opcodes, verified per-cycle |
| PPU frame timing | Dot-accurate: vblank, NMI, suppression, odd-frame skip |
| PPU address space | Pattern tables, nametable and palette mirroring, `$2007` buffer, OAM, open-bus decay |
| PPU background | Loopy `v`/`t`/`x`/`w`, dot-exact tile pipeline, framebuffer of palette indices |
| Sprites | Secondary OAM, per-dot evaluation, 8-per-line, the overflow search bug, priority, 8x16, flip. Passes blargg's 5 `sprite_overflow` and 11 `sprite_hit` ROMs |
| Cartridge | iNES, NROM (0), UNROM (2) and CNROM (3), CHR-ROM and CHR-RAM |
| APU | Frame counter and `/IRQ`. No audio. |
| Display | SDL2 + Dear ImGui: the screen and the debugger in one window |
| Controllers | Both ports at `$4016`/`$4017`, keyboard-driven. Passes blargg's `read_joy3` `test_buttons` |

Next: MMC3 (mapper 4), which unlocks several hundred games and brings the
scanline-counter IRQ - and with it the one deliberate PPU gap, the sprite
pattern fetches skipped for empty slots, which only becomes observable once a
mapper watches the PPU address bus.

### Verification: what has and has not been exercised

Most oracles here are *test* ROMs - written to isolate one behaviour and report
a verdict - plus two freely-licensed homebrew programs. A retail game is a
different kind of load, so `tests/local_rom_tests.cpp` runs one end to end when
it is available: it boots to the title screen, presses Start, holds Right, and
checks that the playfield scrolls and the sprite-0 status bar split fires.
Measured on Super Mario Bros: coarse X takes 26 of a possible 32 values over 300
frames, and the split fires on 249 of them.

**Those tests skip unless you supply the ROM yourself.** Nothing here fetches a
commercial game - they are copyrighted, unlike the redistributable test dumps
the fetch scripts pull. Drop a dump of a cartridge you own at
`tests/test_files/local/smb.nes` and they switch from SKIPPED to executed; see
that directory's README.

Two things about test counts that are easy to misread:

* **`ctest` counts a skipped test as a pass.** `GTEST_SKIP` exits 0. The honest
  figure for any run is *executed*, not *passed*.
* **CI executes far fewer tests than it reports.** The 512 per-opcode
  SingleStepTests need 1.1 GB of vectors that CI does not fetch, so they skip
  there and run only locally. The workflow prints the breakdown on every run.
  blargg's `instr_test-v5` singles cover all 256 opcodes in a few hundred KB and
  DO run in CI, so instruction coverage there is no longer just nestest and
  Klaus.

One deliberate divergence, asserted rather than hidden: `03-immediate` reports
`AB ATX #n`. Opcode `$AB` computes `A = X = (A | magic) & immediate`, where
`magic` is an analogue property of the physical chip. Measured, no value
satisfies both oracles - `$FF` passes blargg and fails 3 SingleStepTests cases,
`$EE` does the reverse. `$EE` is kept, and `tests/instr_test_roms.cpp` asserts
that ROM fails on *exactly* ATX, so any other regression in it still shows up.

Still unexercised by anything here: sustained play over minutes rather than a
few thousand frames, and every mapper beyond 0, 2 and 3.

## Features

### Core Components
*   **CPU (Ricoh 2A03 / 6502):**
    *   All 256 opcodes, official and undocumented, including the unstable
        `SHA`/`SHX`/`SHY`/`TAS`/`LXA`/`XAA` family.
    *   **Cycle-stepped**: exactly one bus access per cycle, at the cycle
        hardware performs it. That includes the accesses that are easy to miss
        because they change no register - the dummy read at the un-carried
        address when an indexed access crosses a page, and the dummy write of
        the old value that every read-modify-write performs. Those are
        observable on the bus, and getting them wrong breaks PPU timing.
    *   Verified against the [SingleStepTests 65x02
        vectors](https://github.com/SingleStepTests/65x02): 256/256 opcodes on
        the per-cycle bus trace **and** 256/256 on final CPU/memory state,
        10,000 randomized cases each.
    *   Matches the canonical [`nestest`](https://www.qmtpro.com/~nes/misc/)
        log for all 8991 instructions, both on flat memory and through the real
        bus.
    *   Passes the [Klaus2m5 functional and interrupt test
        suites](https://github.com/Klaus2m5/6502_65C02_functional_tests).
*   **PPU (Picture Processing Unit):**
    *   Frame timing is dot-accurate: vblank set/clear, NMI enable and
        suppression, and the odd-frame clock skip. Passes all ten of Blargg's
        `ppu_vbl_nmi` ROMs.
    *   `/NMI` is modelled as a level the CPU samples, not an edge it is
        handed, which is what makes NMI suppression expressible.
    *   OAM DMA takes the 513 or 514 CPU cycles it should, stealing them from
        a halted CPU.
    *   The address space is real: CHR-ROM or CHR-RAM behind the pattern
        tables, nametable mirroring driven by the iNES flag, palette mirroring
        with the `$3F10/$14/$18/$1C` aliases, the `$2007` read buffer, and an
        open bus whose bits decay independently. Passes `oam_read`,
        `oam_stress` and `ppu_open_bus`.
    *   The background renders: the loopy `v`/`t`/`x`/`w` scroll registers, the
        8-cycle nametable/attribute/pattern fetch, shift registers with fine-X
        selection, and a 256x240 framebuffer of 6-bit palette indices. The dot
        each increment and `t`->`v` copy happens on is pinned by tests, not
        just the bits they move.
    *   Sprite 0 hit is implemented and **only** sprite 0 hit - evaluated in
        its real window, starting from `OAMADDR`, with `OAMADDR` held at 0
        across the sprite-fetch dots as hardware does. Passes all eleven of
        blargg's `sprite_hit` ROMs, which measure the BACKGROUND pipeline as
        much as the sprite: pixel-exact alignment, the left-8 clip, and
        dot-exact flag timing.
    *   Passes blargg's `ppu_read_buffer` in full - the broadest single check
        here, covering CIRAM through `$2007` with both increment modes, PPU I/O
        mirroring, CHR-ROM reads, CNROM banking, sprite 0 hit, and OAM loaded
        from RAM, from ROM and from the PPU register file.
    *   **No sprite rendering, no display.** No priority, no 8-per-line
        evaluation, no overflow flag, no colour emphasis, and nothing draws the
        framebuffer to a screen.
*   **Cartridge:**
    *   iNES parsing with NROM (mapper 0) and CNROM (mapper 3), 16KB and 32KB
        images, trainer support, and `$6000-$7FFF` PRG-RAM. CNROM's switchable
        CHR window is what makes `ppu_read_buffer` reachable.
*   **Bus:**
    *   A single address decode shared by reads and writes, so the two cannot
        disagree. Controller 1 at `$4016` is open bus for now.
*   **APU (Audio Processing Unit):**
    *   The frame counter is implemented, including the 4- and 5-step
        sequences and the frame interrupt - it is the machine's only maskable
        interrupt source, so the CPU's `/IRQ` path is untestable without it.
        Passes all five `cpu_interrupts_v2` ROMs.
    *   **No audio.** No channels, no mixer, no output.

### Frontend and debugger (`nes_frontend`)
An SDL2 window hosting Dear ImGui panels:
*   The 256x240 screen, scaled by an integer factor with nearest-neighbour
    filtering, updated every frame.
*   CPU registers, the decoded opcode at `PC`, and writable status flags.
*   PPU state: scanline and dot, loopy `v`/`t`/`x`/`w` raw and decoded, and the
    three status flags.
*   A clipped hex view of the 2KB internal RAM, with `PC`, the stack pointer and
    the stack base highlighted.
*   Palette RAM as colour swatches, with the `$3F10`/`$3F14`/`$3F18`/`$3F1C`
    aliases resolved so what is shown is what the PPU renders.
*   Run, pause, single-step, step-one-frame, reset, and a CPU trace to stdout.

No panel reads emulator state through `Bus::read` or `PPU::read`. Those are
hardware ports where a read clears the vblank flag, advances the VRAM address or
acknowledges an IRQ - a debugger that displayed them by reading them would
change the run it is supposed to be observing.

It replaced a Qt/QHexView debugger. Immediate mode suits a machine being
single-stepped far better than a widget tree does, ImGui is vendored source
rather than a system package, and SDL is what the APU's audio callback will need
when there is audio to clock.

## Building the Project

### Prerequisites
*   A C++20 compatible compiler (e.g., GCC, Clang)
*   CMake (version 3.15 or later)
*   Ninja (optional, for faster builds)
*   SDL2 - **optional**, only for the `nes_frontend` target. Configure with
    `-DNES_BUILD_FRONTEND=OFF` to skip it; CMake also skips it automatically if
    SDL2 is not installed. The emulator libraries and the test suite build
    without SDL, and nothing under `tests/` links the frontend.

Google Test and Dear ImGui are fetched automatically by CMake, each pinned to a
release tag.

### Build Steps

```sh
# Clone the repository
git clone https://github.com/JuantAldea/NES.git
cd NES

# Configure the build using CMake
mkdir build && cd build
cmake ..
# Or, if you want to use Ninja
cmake .. -G Ninja

# Build the project
cmake --build .
```

## Usage

The build process generates two executables in the `build/` directory.

### Command-Line (`NES`)
Not production ready.

### Frontend and debugger (`nes_frontend`)
```sh
./build/nes_frontend path/to/rom.nes    # loads and starts running
./build/nes_frontend                    # then type a path, or drag a .nes in
```

Panel positions are saved to `imgui.ini` in the working directory.

## Running Tests

The project uses Google Test. Tests build automatically with the project.

### Test fixtures

Most of the suite runs against external test ROMs and hardware-derived vectors.
These are **not committed** - they are unlicensed ROM dumps and, in one case,
1.1 GB of generated JSON. Fetch them with:

```sh
tests/test_files/fetch_nestest.sh             #  ~900 KB  nestest ROM + log
tests/test_files/fetch_blargg_ppu.sh          #  ~400 KB  ppu_vbl_nmi ROMs
tests/test_files/fetch_cpu_interrupts.sh      #  ~200 KB  cpu_interrupts_v2 ROMs
tests/test_files/fetch_ppu_address_space.sh   #  ~100 KB  OAM and open-bus ROMs
tests/test_files/fetch_blargg_ppu_2005.sh     #   ~64 KB  palette/VRAM/OAM ROMs
tests/test_files/fetch_sprite_hit.sh          #  ~224 KB  sprite 0 hit ROMs
tests/test_files/fetch_ppu_read_buffer.sh     #   ~40 KB  $2007 read buffer pack
tests/test_files/fetch_single_step_tests.sh   #   1.1 GB  SingleStepTests vectors
```

Each verifies a pinned SHA256 (the vectors are validated structurally instead,
since upstream regenerates them wholesale) and skips anything already present,
so re-running is cheap.

**A green run does not mean the suite verified everything**, and the headline
count actively hides this. The two per-opcode suites call `GTEST_SKIP` when the
1.1 GB of vectors is absent, and a skipped test exits 0, so `ctest` counts it as
a pass. With no vectors fetched the suite still reports "100% tests passed out
of 749" while having executed 237 of them.

The ROM suites behave the other way round: a missing ROM is a loud failure
naming the fetch script to run, not a skip. So the failure modes are:

| Fixture | Missing behaviour |
|---|---|
| SingleStepTests vectors | 512 tests skip, counted as passing |
| Everything else | hard failure naming the fetch script |

CI deliberately does not fetch the vectors and prints the executed count on
every run for this reason.

### Running

```sh
ninja -C build check     # or: make -C build check
```

`check` runs the suite across every core. The tests are independent - each one
that writes a fixture writes a uniquely named one - and the suite is dominated
by 512 per-opcode cases that parallelise perfectly.

On 32 cores the full 749-test suite takes about **3 seconds**. Two things got
it there, and the second mattered more than the first:

* Parallelism took it from 129s to 16s. Past that point the total was simply
  the length of the single slowest test, so more cores stopped helping.
* The default build type is `Checked` (`-O3 -g`, asserts left on), which took
  16s to 3s. The suite spends nearly all its time emulating - the test ROMs run
  hundreds of millions of bus cycles each - so an unoptimised build costs about
  5x. `-O3` measured ~10% faster than `-O2`; `-march=native` was slower than
  plain `-O3` and not portable.

`Debug`, `Release` and the other stock types are untouched and still available
via `-DCMAKE_BUILD_TYPE=`.

Plain `ctest` still works and is still serial:

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build -j8 --output-on-failure   # or pick your own level
```

The 749 tests are dominated by the two per-opcode suites - 256 opcodes checked
for their bus trace and 256 for their final state, 10,000 cases apiece.

## License
This project is licensed under the GNU General Public License v2.0. See the [LICENSE](LICENSE) file for details.
