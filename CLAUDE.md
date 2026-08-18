# NES emulator — working notes

A cycle-accurate NES emulator in C++20, verified against hardware test ROMs.
The whole value of this project is *exactness*, so the rules below are not
style preferences — most of them exist because breaking them makes the test
suite lie about correctness while still reporting green.

## Build and test

```sh
cmake -S . -B build -G Ninja      # defaults to the Checked build type
cmake --build build
ctest --test-dir build -j"$(nproc)" --output-on-failure
```

Single suite: `./build/tests/tests --gtest_filter='testCPU.*'`

Reporting a result to anyone — commit message, README, the user — needs the
*executed* count, which `ctest` cannot give (see below):

```sh
tests/run_tests.sh                # or `ninja -C build check-counts`
# <N> executed, 0 skipped, 0 failed  (32 shards)
```

It shards the binary across every core rather than running one process, takes
`-jN` and passes everything else through, so
`tests/run_tests.sh --gtest_filter='testCPU.*'` works. `NES_TEST_BIN=` points
it at another tree, e.g. `build-asan`.

Everything a green suite does *not* prove — a clean tree building from nothing,
the suite linking with no SDL, every fetch script from a cold cache, the CLI's
exit codes, and the frontend opening a window per mapper:

```sh
tests/run_functional.sh            # add --full for the sanitizers, --no-gui for headless
# 0 failed, 2 skipped
```

One line per check, detail only on failure. It exits with the number of
failures. Prefer it to driving those checks by hand — it was written by
automating a pass that took fifteen separate commands and produced pages of
output to read.

Sanitizers (a separate tree, never the default one):

```sh
cmake -S . -B build-asan -G Ninja -DNES_BUILD_FRONTEND=OFF -DNES_SANITIZE=address,undefined
ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1 ctest --test-dir build-asan -j"$(nproc)"
```

`-DNES_BUILD_FRONTEND=OFF` skips SDL2. Nothing under `tests/` links the
frontend, so the suite must always configure and build without SDL present.

Static analysis (a separate tree again, `build-scan/`):

```sh
tests/run_scan_build.sh           # or `ninja -C build analyze`
# scan-build: No bugs found.
```

It reports **zero** findings today, so `--status-bugs` gates CI with no
baseline file to keep. Two things about it that are not obvious:

* **It needs no test ROMs**, because nothing runs — it is the only check here
  that a network failure cannot turn red. It is also the only one that reaches
  code no test exercises, which is exactly where the oracles and the asserts
  are blind.
* **The tree is wiped on every run, deliberately.** scan-build only sees what
  the build actually compiles, so an up-to-date tree analyses nothing and still
  prints "No bugs found". That is the same false green as a skipped test
  counted as a pass. `NES_SCAN_INCREMENTAL=1` opts out while iterating.

The frontend is off by default there too: ImGui reports 16 findings of its own
and `--exclude` can keep them out of the count but not out of the log. Our own
frontend sources analyse clean — `-DNES_BUILD_FRONTEND=ON` is supported.

### Never build the tests as Release or RelWithDebInfo

Both define `NDEBUG`, which silently removes every `assert`. Asserts are one of
this project's three correctness nets (the ROM oracles and the static analyzer
are the others), so an `NDEBUG` build covers *less* while reporting the same
passes — and it costs twice, because a live `assert` is also a path constraint
the analyzer reasons with, which is why scan-build ships a
`--force-analyze-debug-code` flag for projects that lost theirs. The default
`Checked` type exists precisely for this: `-O3 -g` with asserts live. `Debug`
is ~5x slower for no added coverage — the suite is emulation-bound. See the
rationale and measurements at the top of [CMakeLists.txt](CMakeLists.txt).

## Test ROMs are never committed

They are redistributable-but-unlicensed dumps. Every suite is pulled by a
`tests/test_files/fetch_*.sh` script with **SHA256 pins per file**, and the
target directory has a `.gitignore` entry. Write `tests/test_files/mmc3`, not
`tests/test_files/mmc3/` — a trailing slash there has bitten this repo before.

Adding a new oracle means all four of: the fetch script, the `.gitignore`
entry, the wiring into **both** CI jobs (`test` and `sanitizers` each carry
their own copy of the fetch list), and a clear message when the ROM is absent.

**A missing fetched ROM must FAIL, not skip**, and the message must name the
fetch script — `could not load <path> - run tests/test_files/fetch_x.sh`. CI
fetches these, so a missing one means the *fetch step failed*, and `GTEST_SKIP`
exits 0: skipping there would turn a broken CI run green. That is the one
outcome this repo is built to prevent, and it outranks "a missing fixture
shouldn't look like an emulation bug" — which the message already handles.

`GTEST_SKIP` is for fixtures that **cannot** be fetched at all:
`tests/test_files/local/` cartridge dumps and the 1.1 GB SingleStepTests
vectors. Those are absent on every clean checkout and always will be, so
failing on them would mean a permanently red suite.

This paragraph used to say every oracle needed a `GTEST_SKIP`. Twenty suites
did not have one, and were right not to.

`tests/test_files/local/` is for ROMs of cartridges the user owns. Nothing
fetches those, ever, and they must stay ignored.

### Measure the failure before implementing the feature

The header of [fetch_mmc3.sh](tests/test_files/fetch_mmc3.sh) is the model to
copy: it records what each of the six ROMs reports *with the feature not yet
written* — status code, message, frame of first failure. That turns a wall of
red into a work queue, proves each ROM actually runs rather than hanging, and
gives a floor for the frame budget. Do this before writing the implementation,
not after.

## Skipped is not passed

`GTEST_SKIP` exits 0, so `ctest` counts a skipped test as a passing one.
"100% tests passed out of N" is a misleading headline in this repo and the CI
workflow has a step whose entire job is to say so on every run. When reporting
results — in a commit message, the README, or to the user — give the *executed*
count, and say what was skipped and why.

`ctest` structurally cannot tell you that count: `gtest_discover_tests` gives
each case its own process, so gtest's end-of-run summary — the only place the
SKIPPED total appears — is never printed. `tests/run_tests.sh` runs the binary
itself to keep that summary, and prints executed/skipped/failed with the skips
broken down by suite. Use it rather than eyeballing `ctest` output.

The 512 SingleStepTests cases need 1.1 GB of vectors that CI does not fetch, so
they only run locally. `tests/test_files/local/` ROMs never run in CI either.
Hiding both reproduces what CI sees. The per-suite breakdown, and which suites
CI cannot run, live in [tests/TEST_COUNTS.md](tests/TEST_COUNTS.md) - generated
by `tests/test_counts.sh`, never maintained by hand. `--check` fails when a
suite loses cases, which a bare total cannot detect.

## Deliberate divergences are asserted, not hidden

Where two oracles genuinely disagree about hardware, pick a side, document why,
and pin the other side's failure with a test. The pattern is opcode `$AB` in
[instr_test_roms.cpp](tests/instr_test_roms.cpp): `$EE` is chosen, blargg's
`03-immediate` is asserted to fail on *exactly* `ATX` and nothing else, so any
further regression in that ROM still surfaces. Deleting the disagreeing ROM
would have hidden a real finding.

MMC3 is the same situation, and is already resolved the same way in
[mmc3_rom_tests.cpp](tests/mmc3_rom_tests.cpp): `5-MMC3` (Sharp revision B/C)
and `6-MMC3_alt` (NEC revision A) test opposite reload-to-zero behaviours and
cannot both pass. Sharp is implemented, `5-MMC3` is asserted to pass, and
`6-MMC3_alt` is asserted to fail on *exactly* that subtest — so a failure
anywhere else in it, or an unexpected pass, still surfaces as a distinct
message rather than as a ROM everyone has learned to ignore.

## Code conventions

- **Comments explain why, and cite the measurement.** The codebase is full of
  comments carrying numbers, hardware citations, and rejected alternatives.
  Match that. A comment restating what the line does is worse than none.
- `.clang-format` is authoritative; run it on files you touch.
- Warnings are `-Wall -Wextra -pedantic`. Third-party code (ImGui) is built
  with `-w` so it cannot bury our own warnings — keep it that way.
- **The debugger never reads emulator state through `Bus::read` or
  `PPU::read`.** Those are hardware ports: a read clears the vblank flag,
  advances the VRAM address, or acknowledges an IRQ. A panel that displayed
  state by reading it would change the run it is meant to be observing. Use the
  peek/inspection paths in `frontend/debugger_state.*`.
- Dependencies are pinned to release tags, never branches (googletest
  `v1.15.2`, Dear ImGui `v1.92.9b`).

## Layout

| Path | What |
|---|---|
| `src/`, `include/` | The emulator: `cpu`, `ppu`, `bus`, `rom`, `apu`, `device` |
| `frontend/` | SDL2 + Dear ImGui screen and debugger (optional target) |
| `tests/` | GoogleTest suites; `blargg_rom_harness.h` runs any blargg ROM |
| `tests/test_files/` | Fetch scripts and their (ignored) output directories |

`blargg_rom_harness.h` implements the shared PRG-RAM reporting protocol —
signature at `$6001`, status at `$6000`, ASCII message at `$6004` — and drives
a soft RESET via `Bus::reset()` when a ROM reports `$81`. Any new blargg suite
should go through it rather than reimplementing the protocol.

Two suites still carry their own copy from before that was true:
`blargg_ppu_tests.cpp` (does not drive resets at all) and
`cpu_behaviour_tests.cpp` (drives them, but starts with `CPU::power_on()`
rather than `reset()`, which its ROMs genuinely require). Do not add a third.

**`$6000` survives a reset, because PRG-RAM does.** So the status read straight
after one is the value from *before* it, and trusting it schedules a second
reset the ROM never asked for. The harness waits for the ROM to republish `$80`
first. This cost real time to find, because raising the reset delay made the
symptom disappear without fixing anything — if `kResetDelayFrames` ever starts
mattering again, that guard is where the bug is.

## Working style

Emulation correctness is sequential and context-heavy. Do not delegate "is this
cycle timing right" to a subagent that arrives cold — it will produce a
plausible, wrong answer, which is the worst outcome here. Subagents are for
*searching* (hardware references, nesdev) and *waiting* (long sanitizer or
SingleStepTests runs), not for deciding.

When an agent worktree is involved: it branches from `origin`, not `HEAD`, so
fast-forward it before starting or unpushed commits go missing.

### Which model does what

The emulation work itself runs on the largest available model, and is not
downgraded to save tokens. "Is this dummy read on cycle 4 or 5" is the entire
value of the project; a cheaper answer there is not a cheaper answer, it is a
wrong one that still compiles and still reports green.

Subagents are cheaper, and the rule for when that is safe is: **a smaller model
is acceptable exactly where the agent is required to show its evidence, so its
output is checkable rather than trusted.**

| Agent | Model | What makes it checkable |
|---|---|---|
| `hw-reference` | Sonnet | Must return source, URL and an explicit confidence level for every claim |
| `rom-sweep` | Haiku | Must print the raw `ctest` output and the arithmetic behind its executed/skipped count |

Strip those requirements and the model choice stops being defensible — so if
either agent's instructions are ever loosened, raise its model in the same
edit.
