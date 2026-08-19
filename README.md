# NES Emulator
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)

A Nintendo Entertainment System emulator written in C++20, with an SDL2 +
Dear ImGui frontend that shows the picture and the machine drawing it side by
side.

## Current status

The CPU and the PPU render backgrounds and sprites, the frontend puts the
picture on screen, and the controllers work. A mapper 0, 2, 3 or 4 game should
draw and respond to input - Super Mario Bros plays, and the 240p Test Suite
runs. MMC3 brings the scanline IRQ counter that raster splits need. There is
still no audio.

| Area | State |
|---|---|
| 6502 CPU | Cycle-accurate, all 256 opcodes, verified per-cycle |
| PPU frame timing | Dot-accurate: vblank, NMI, suppression, odd-frame skip |
| PPU address space | Pattern tables, nametable and palette mirroring, `$2007` buffer, OAM, open-bus decay |
| PPU background | Loopy `v`/`t`/`x`/`w`, dot-exact tile pipeline, framebuffer of palette indices + emphasis |
| Sprites | Secondary OAM, per-dot evaluation, 8-per-line, the overflow search bug, priority, 8x16, flip. Passes blargg's 5 `sprite_overflow` and 11 `sprite_hit` ROMs |
| Cartridge | iNES, NROM (0), UNROM (2), CNROM (3) and MMC3 (4), CHR-ROM and CHR-RAM |
| MMC3 IRQ | A12-filtered scanline counter driving `/IRQ`, clocked on the right dot. Passes 5 of blargg's 6 `mmc3_test_2` ROMs; the sixth tests the other chip revision, see below |
| APU | Frame counter, `/IRQ`, length counters, the power-on/RESET state. Delta modulation channel, minus its CPU stall. Passes **all** of blargg's `apu_test`, `apu_reset` and `blargg_apu_2005` ROMs. No audio output yet. |
| Display | SDL2 + Dear ImGui: the screen and the debugger in one window |
| Controllers | Both ports at `$4016`/`$4017`, keyboard-driven. Passes blargg's `read_joy3` `test_buttons` |

Next: the rest of the APU - envelope, sweep, the channels themselves, the
mixer and the DMC. SDL is already a dependency and its audio callback is the
natural clock for it.

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
  Klaus. `cpu_exec_space` runs the CPU *through* I/O space, which is what pins
  CPU open bus.

One deliberate divergence per oracle pair, asserted rather than hidden.

`03-immediate` reports
`AB ATX #n`. Opcode `$AB` computes `A = X = (A | magic) & immediate`, where
`magic` is an analogue property of the physical chip. Measured, no value
satisfies both oracles - `$FF` passes blargg and fails 3 SingleStepTests cases,
`$EE` does the reverse. `$EE` is kept, and `tests/instr_test_roms.cpp` asserts
that ROM fails on *exactly* ATX, so any other regression in it still shows up.

`6-MMC3_alt` fails because it tests a different chip. Sharp and NEC MMC3 parts
disagree about what happens when the IRQ latch is 0 - one reloads and fires on
every clock, the other stops - and the two ROMs assert opposite things, so they
cannot both pass. This implements **Sharp**, because SMB3 and Mega Man 3 are
Sharp boards and that is what real games depend on.
`tests/mmc3_rom_tests.cpp` pins `6-MMC3_alt`'s exact status and message, so a
change in *how* it fails still surfaces.

`4-scanline_timing` used to be listed here as a third case - a real gap rather
than a divergence, pinned at "fails subtest 3". **It now passes**, and how it
was closed is worth recording because the theory in this README was wrong for
months. The suspected cause was the garbage nametable reads in each sprite
pattern fetch not reaching the PPU address bus. They were not reaching it, and
that was not the cause: the ROM passed once the real one was found. That was a
single dot - the background pipeline put its address on the bus at the first
dot of each two-dot access and the sprite pipeline at the second, so A12 rose
257 dots apart instead of the 256 the ROM hard-codes.

The garbage reads are on the bus now, fixed separately and for a different
reason. They hold A12 **low** for four dots between fetch groups, which is
under the MMC3's filter and so changes nothing on an ordinary line. It stops
being nothing for 8x16 sprites drawn from both pattern tables, where per NESdev
the gap between A12 edges grows "past the time that the MMC3 is able to filter
out, causing the timer to count more than once per scanline". No test ROM in
this suite sets that up, so `mmc3A12Filter.alternating_pattern_tables_clock_the_counter_more_than_once`
does, written from the wiki's wording rather than from this emulator's model of
itself. Five of the six MMC3 IRQ ROMs pass; `6-MMC3_alt` is the divergence
above.

Everything above answers "does it behave correctly when run". Clang's static
analyzer answers the other half - what happens on the branches no test takes -
and reports zero findings; see [Static analysis](#static-analysis).

Still unexercised by anything here: sustained play over minutes rather than a
few thousand frames, and every mapper beyond 0, 2, 3 and 4.

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
        selection, and a 256x240 framebuffer of 6-bit palette indices plus
        three emphasis bits. The dot
        each increment and `t`->`v` copy happens on is pinned by tests, not
        just the bits they move.
    *   Sprite 0 hit is evaluated in its real window, starting from `OAMADDR`,
        with `OAMADDR` held at 0 across the sprite-fetch dots as hardware does.
        Passes all eleven of blargg's `sprite_hit` ROMs, which measure the
        BACKGROUND pipeline as much as the sprite: pixel-exact alignment, the
        left-8 clip, and dot-exact flag timing.
    *   Passes blargg's `ppu_read_buffer` in full - the broadest single check
        here, covering CIRAM through `$2007` with both increment modes, PPU I/O
        mirroring, CHR-ROM reads, CNROM banking, sprite 0 hit, and OAM loaded
        from RAM, from ROM and from the PPU register file.
    *   Sprites are complete: secondary OAM, per-dot evaluation, the
        eight-per-line limit, the hardware overflow search bug, priority, 8x16
        and both flips. Passes blargg's 11 `sprite_hit` and 5 `sprite_overflow`
        ROMs.
    *   Eight sprite pattern fetches happen on every rendering line, however
        few sprites it has, because the empty ones still drive A12 and that is
        what clocks an MMC3 counter. Proven by mutation: removing them fails
        `2-details`.
    *   Colour emphasis and the "forced backdrop" case, both verified against
        blargg's `full_palette` suite. During forced blank with `v` pointing
        into `$3F00-$3FFF` the PPU draws the colour `v` addresses rather than
        the backdrop, which is what makes `$3F04/$3F08/$3F0C` reachable at all
        and what Micro Machines depends on. Emphasis is captured per pixel
        rather than read at display time, because the ROM rewrites `$2001`
        mid-frame; the framebuffer carries three emphasis bits above the index
        for that reason. Attenuation compounds per channel, so all three bits
        set darkens the whole picture.
*   **Cartridge:**
    *   iNES parsing with NROM (0), UNROM (2), CNROM (3) and MMC3 (4), trainer
        support, and `$6000-$7FFF` PRG-RAM. CNROM's switchable CHR window is
        what makes `ppu_read_buffer` reachable; UNROM's PRG window is what runs
        the 240p Test Suite.
    *   MMC3 adds the register file, both PRG modes, CHR A12 inversion, runtime
        mirroring and the scanline IRQ counter. Bus conflicts are deliberately
        not modelled on any of these boards - cartridges store the bank number
        at the address they write to, so the AND real hardware performs is a
        no-op for correct software.
    *   `$A001`'s PRG-RAM enable and write-protect bits are obeyed, not just
        stored. `Bus::decode` consults them for `$6000-$7FFF`: disabled decodes
        to no device, so reads are open bus and writes vanish; write-protected
        does that for writes only. The mapper owns the bits and `PrgRAM` is a
        separate Bus device, so the decode is the one place the two can meet.
        **No ROM here covers this** - the tests are written from the register
        description, which is weaker evidence than the rest of this list rests
        on.
*   **Bus:**
    *   A single address decode shared by reads and writes, so the two cannot
        drift apart. It takes the direction as a parameter for exactly one
        address: `$4017` is the APU frame counter on a write and controller
        port 2 on a read, which on hardware really are two different devices.
        `$4016` is one device in both directions, because the write is the
        strobe for *both* ports and one strobe line owns them.
*   **APU (Audio Processing Unit):**
    *   The frame counter is implemented, including the 4- and 5-step
        sequences and the frame interrupt - it is the machine's only maskable
        interrupt source, so the CPU's `/IRQ` path is untestable without it.
        Passes all five `cpu_interrupts_v2` ROMs.
    *   Length counters for the two pulses, the triangle and the noise, with
        the 32-entry length table, the halt bits, and a `$4015` that enables
        channels, clears a counter when it disables one, refuses a reload while
        disabled, and reports each counter's status on read. Passes six of
        blargg's eight `apu_test` ROMs.
    *   The state at power and at RESET, which is not "everything zero": at
        power it is as if `$00` were written to `$4017` followed by a 9-12
        cycle delay, and a RESET clears `$4015` and the frame interrupt then
        replays the last byte written to `$4017`. The delay is 10 rather than
        the 9 blargg calls typical, because it must be EVEN - an odd one
        inverts the CPU/APU phase and breaks `4-irq_and_dma`. Passes five of
        blargg's six `apu_reset` ROMs. `Bus::reset()` deliberately leaves the
        PPU alone; nothing measures a PPU reset yet.
    *   Audited against blargg's 2005 frame-counter suite, which reaches
        further than `apu_test` into length-counter timing: **all 11 pass**.
        Nine did on first contact, including `09.reset_timing`, an independent
        check of the power-on and RESET work above. Those ROMs predate the `$6000` protocol and report on
        screen, so they are read out of the nametable by
        `tests/nametable_screen.h` - and there `$01` means passed, not `0`.
    *   A write to the halt bit or to a length reload does not reach the
        counter on the cycle it is made: it lands after the *next* length
        clock. A reload arriving that way is dropped when the counter is
        non-zero and honoured when it is zero. Both come from
        `10.len_halt_timing` and `11.len_reload_timing`, which were pinned to
        their exact failure codes until the behaviour landed and then announced
        it by failing those pins. Moving the frame counter's own timing instead
        was tried and rejected - it fixes those two and breaks `05`/`06` with
        "first length is clocked too soon".
    *   The **delta modulation channel**: the 16-entry rate table, the sample
        address and length registers, the one-byte sample buffer, the output
        unit's shift register and 7-bit level, looping, and the DMC interrupt
        with its `$4015` bit 4 and bit 7. This closed the last three pinned
        ROMs - `7-dmc_basics`, `8-dmc_rates` and `works_immediately` - and
        **every APU ROM in the repository now passes**.
    *   **What the DMC does not do yet is stall the CPU.** Its memory reader
        fetches in zero cycles. Everything a program can observe by polling is
        implemented; the timing distortion the DMA imposes on the CPU is not,
        and it is deliberately absent rather than approximated - a wrong number
        of stolen cycles looks implemented while being wrong, which is worse
        than none. `dmc_dma_during_read4` and `sprdma_and_dmc_dma` are the
        oracles for that, and are the next step.
    *   Two DMC details are **spec-derived and unverified**, labelled as such in
        `src/apu.cpp`: the sample address wrapping to `$8000` rather than
        `$0000`, and the output level clamping at 127. Every ROM here passes
        with either behaviour, because their samples never reach `$FFFF` and
        nothing reads the level back.
    *   **Still no audio output.** No envelope, sweep or linear counter, no
        channel waveforms and no mixer.
    *   **There is no oracle for the envelope, sweep or linear counter, and
        that is not a gap in this repo's fixtures.** blargg's own `tests.txt`
        states his suite "does not test clocking of the envelope, sweep, or
        triangle's linear counter", and his readme adds that he never
        characterised that hardware either. `apu_mixer` and `volume_tests`
        verify by cancelling to silence and by comparing audio recordings,
        neither of which a CPU can check. So that work will need a different
        kind of verification, and the DMC - which has six ROMs waiting - is the
        better next step.

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
*   Both controller ports drawn as pads, lighting each button as it is pressed,
    with the button latch, the shift register and the strobe line beside them.
    The shift register is the half no other view can show - it is what the next
    eight reads of `$4016` will return, so a game clocking the pad out is
    visible. Port 2 is drawn and stays dark, because nothing writes it yet.
*   Run, pause, single-step, step-one-frame, reset, and a CPU trace to stdout.

The keyboard drives port 1: arrows for the D-pad, `X` for A, `Z` for B, shift
for Select, enter for Start. Keys reach the game only while no ImGui widget
holds the keyboard, so clicking into the ROM path field takes the pad away
until you click back out - the alternative being that typing a path also walks
the player left.

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
tests/test_files/fetch_instr_test.sh          #  ~708 KB  instr_test-v5, all 256 opcodes
tests/test_files/fetch_cpu_interrupts.sh      #  ~200 KB  cpu_interrupts_v2 ROMs
tests/test_files/fetch_cpu_behaviour.sh       #  ~224 KB  reset and dummy-access ROMs
tests/test_files/fetch_cpu_exec_space.sh      #   ~92 KB  CPU executing through I/O space
tests/test_files/fetch_blargg_ppu.sh          #  ~400 KB  ppu_vbl_nmi ROMs
tests/test_files/fetch_blargg_ppu_2005.sh     #   ~64 KB  palette/VRAM/OAM ROMs
tests/test_files/fetch_ppu_address_space.sh   #  ~100 KB  OAM and open-bus ROMs
tests/test_files/fetch_ppu_read_buffer.sh     #   ~40 KB  $2007 read buffer pack
tests/test_files/fetch_sprite_hit.sh          #  ~224 KB  sprite 0 hit ROMs
tests/test_files/fetch_sprite_overflow.sh     #  ~104 KB  sprite overflow ROMs
tests/test_files/fetch_mmc3.sh                #  ~268 KB  MMC3 scanline IRQ ROMs
tests/test_files/fetch_dmc_dma.sh             #  ~272 KB  DMC DMA versus the CPU
tests/test_files/fetch_cpu_timing.sh          #  ~112 KB  instruction and branch timing
tests/test_files/fetch_read_joy3.sh           #   ~48 KB  controller ROM
tests/test_files/fetch_visual_roms.sh         #  ~100 KB  homebrew visual checks
tests/test_files/fetch_single_step_tests.sh   #   1.1 GB  SingleStepTests vectors
```

Each verifies a pinned SHA256 (the vectors are validated structurally instead,
since upstream regenerates them wholesale) and skips anything already present,
so re-running is cheap.

That is all fifteen, and the list has to stay complete to be useful: everything
except the last one hard-fails when its ROMs are absent, so a partial list reads
like a working setup and then fails in several places at once.

**A green run does not mean the suite verified everything**, and the headline
count actively hides this. The two per-opcode suites call `GTEST_SKIP` when the
1.1 GB of vectors is absent, and a skipped test exits 0, so `ctest` counts it as
a pass. With no vectors fetched the suite still reports "100% tests passed out
of N" while having executed far fewer. The per-suite breakdown is in
[tests/TEST_COUNTS.md](tests/TEST_COUNTS.md).

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

Two things decide how long that takes, and the second mattered more than the
first:

* Parallelism, up to the point where the total is just the length of the single
  slowest test. Past that, more cores stop helping - which is why the runner
  defaults to `nproc` shards and there is no reason to ask for more.
* The default build type is `Checked` (`-O3 -g`, asserts left on). The suite
  spends nearly all its time emulating - the test ROMs run hundreds of millions
  of bus cycles each - so an unoptimised build costs roughly 5x for no added
  coverage. `-O3` measured ~10% faster than `-O2`; `-march=native` was slower
  than plain `-O3` and not portable.

`Debug`, `Release` and the other stock types are untouched and still available
via `-DCMAKE_BUILD_TYPE=`.

Plain `ctest` still works and is still serial:

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build -j8 --output-on-failure   # or pick your own level
```

The suite is dominated by the two per-opcode suites - 256 opcodes checked
for their bus trace and 256 for their final state, 10,000 cases apiece.

### Sanitizers

`NES_SANITIZE` builds everything with the given `-fsanitize=` list. It is
off by default, and orthogonal to the build type - the sanitizer build is the
normal `Checked` build (`-O3 -g`, asserts live) plus instrumentation:

```sh
cmake -S . -B build-asan -G Ninja -DNES_SANITIZE=address,undefined
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1 \
UBSAN_OPTIONS=print_stacktrace=1 ctest --test-dir build-asan -j8
```

CI runs exactly this on every push, as the **Test under ASan + UBSan** job.
The suite currently reports **no** ASan errors, no UBSan diagnostics and no
leaks.

Two details worth knowing if you change this:

* The build adds `-fno-sanitize-recover=all`. UBSan's default is to print a
  diagnostic and continue, which exits 0 - so without this the job would go
  green while stepping on undefined behaviour on every run.
* Leak detection is deliberately left **on**. The argument for disabling it is
  that a short-lived test binary leaking at exit is harmless noise, but that
  argument assumes a dirty baseline; this suite has none, so detection is free
  and the next leak becomes a build failure rather than something nobody sees.

Instrumentation is substantially slower than the normal build, which is why it
is a separate CI job rather than a step inside the fast one: the suite that
gates everyday work should not wait behind it.

### Static analysis

```sh
tests/run_scan_build.sh          # or: ninja -C build analyze
```

Clang's analyzer over a separate `build-scan/` tree, run in CI as the **Clang
static analyzer** job. It reports **zero** findings, so the job gates on
`--status-bugs` with no baseline to maintain, and uploads the HTML reports as
an artifact when it does fail.

It is worth having next to a suite that already passes because it
answers a different question. The ROM oracles and the asserts both require the
code to *run*: a bug on a branch no test enters is invisible to them however
green they are. The analyzer walks those branches instead of executing them.

It is **not** a static bounds checker, which is worth saying out loud in a
codebase that is mostly indexing fixed-size arrays. Measured: `buf[n * 8]` with
an unconstrained `n` is not reported at all, and an index the analyzer *can*
pin down gets reported as a garbage return value rather than as the
out-of-bounds access. `alpha.security.ArrayBound` was tried and left off - it
changed neither result, so there was no evidence it was doing anything here.
Bounds are ASan's job at runtime, which is why both jobs exist.

Two consequences that are easy to miss:

* **It fetches nothing.** Nothing is executed, so no test ROM is needed - the
  one check here that a network failure or a moved download cannot turn red.
* **The tree is wiped on every run.** scan-build sees only what the build
  actually compiles, so analysing an up-to-date tree analyses *nothing* and
  still prints "No bugs found" - the same false green as a skipped test scored
  as a pass. `NES_SCAN_INCREMENTAL=1` opts out while iterating on a fix.

It costs noticeably more than a normal build, which is why it is a separate
tree and a separate CI job.

The frontend is excluded by default, and `CMakeLists.txt` makes that the
default whenever it detects an analyzer tree. ImGui produces 16 findings of its
own - null dereferences, a division by zero, dead stores - and scan-build's
`--exclude` keeps them out of the bug count but not out of the build log, so
compiling it at all would bury ours. This is the same call as building ImGui
with `-w`. Our own frontend sources analyse clean and
`-DNES_BUILD_FRONTEND=ON` is supported; `frontend/debugger_state.cpp` is
covered either way, because the test binary compiles it directly.

## License
This project is licensed under the GNU General Public License v2.0. See the [LICENSE](LICENSE) file for details.
