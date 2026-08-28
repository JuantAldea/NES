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

### The evidence is where the bugs are

Six adversarial reviews of the APU found essentially one class of defect. Not
wrong algorithms - those came out right nearly every time - but wrong *evidence
about* the algorithms, in three recurring shapes:

* **A floored instrument read as a result.** A sweep of the BLEP kernel's width
  returned five identical numbers, recorded as "the kernel was never the
  constraint". The harness could not see below about -40 dB; the real answer was
  8.7 dB. The same shape as reading `Bus::clock()` as a CPU cycle when it is a
  master cycle, or as its return value when that is an instruction boundary.
* **A calculation labelled "measured".** A -3 dB corner written up as
  "measured 16287 Hz" was the analytic chain times a theoretical factor; the
  code actually produced 15730. An alias table quoted at -56 dB was another
  review's estimate of an ideal implementation, copied across.
* **A test whose signal choice makes it structurally blind.** An alias test used
  a 50% duty square - the one duty with no even harmonics, so the one waveform
  that cannot see transition-band leakage. A window test ran at equal input and
  output rates, where sum, mean and last-sample are the same number. A response
  assertion sat at normalised 0.70, which mirrors 0.30 for any real signal.

So, before trusting a number:

**Validate the instrument against a known-bad reference.** Feed it something
that *should* score badly and check it says so. The alias harness read -17.3 dB
for point-sampled audio, which immediately explained the flat sweep - and that
check cost one minute after several hours of wrong conclusions.

**Do not write "measured" unless it was run.** Calculations are labelled as
calculations. This one recurred inside the very commit that confessed to it.

**Ask what input would make each test blind.** This is the highest-value
question and the easiest to skip. "What signal would this pass regardless of the
answer?" would have caught the duty cycle, the equal rates and the mirrored
frequency before any of them hid a defect.

### Mutation testing, for code no oracle can reach

```sh
tests/run_mutants.sh -f 'testAPUMixer.*' --since HEAD~1 src/apu.cpp include/apu.h
# 19 killed, 1 SURVIVED, 0 did not compile
```

It changes the code mechanically - one operator, literal, condition clause or
call argument at a time - rebuilds, and reports what the tests do not notice.
`--since <rev>` restricts it to lines you just wrote, which is the mode to use
after finishing a piece of work. `--list` shows the candidates without building.

**Use it where there is no oracle.** The APU's envelope, sweep, waveform
generators and mixer are invisible to the CPU, so no test ROM can reach them;
mutation is the only mechanical check available. Where an oracle *does* exist,
it is worth more.

**Why it is not just "another sweep I could run by hand".** Three adversarial
reviews of the APU found ~29 single-token changes the whole suite accepted,
including a real bug - an inverted CPU/APU clock phase. A hand-written sweep had
run before each review and killed nearly everything in it. The difference was
never diligence: *a mutation set chosen by the person who wrote the tests is
aimed where the tests already point.* Writing the set before the tests was tried
and did not help, because it was still the same aim. Generating them mechanically
is what removes the aim.

**A survivor is a question, not a verdict.** It is a hole, an equivalent mutant,
or unreachable, and only the first is a defect. Two real equivalents here: the
mixer's divide-by-zero guards, because IEEE 754 already yields 0 there; and
swapping `mix_levels`' first two arguments, because the formula sums them.
Record the answer next to the code so nobody re-derives it.

**It counts kills against a baseline, not against zero.** Two tests fail in this
repository by design (the parked DMC rows). A harness scoring any failure as a
kill would report a perfect run while testing nothing - so it records the
baseline's failing test *names* and only counts newly-failing ones.

It does **not** replace the review. Mutation tests code against itself, so it
cannot catch a comment that cites a source accurately over code doing something
else, or a value asserted as a hardware fact that no source states. Both have
been real findings here.

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
a soft RESET via `Bus::reset()` when a ROM reports `$81`. A new suite must go
through it. **Eleven do, and none forks it any more.**

The drift that made the folding worth doing is worth remembering, because it
was never cosmetic. `blargg_ppu_tests.cpp` drove no resets at all.
`cpu_behaviour_tests.cpp` lacked the stale-`$6000` guard below and paid for it
with a 15-frame reset delay that masked a spurious extra reset rather than
fixing one. `cpu_exec_space_tests.cpp`, `instr_test_roms.cpp` and
`mmc3_rom_tests.cpp` handled every status *except* `$81`, and so reported a ROM
asking for a reset as "failed with code 129" — a request misread as a verdict. A suite needing `CPU::power_on()` instead of `reset()` passes
`blargg::Start::PowerOn`, which is what removed the last reason to fork.

Three claims about the count have now been wrong here, each a different way:
"there is no longer a second copy" (written after folding two of five); a table
naming three suites as still private (left standing after they were folded, so
it advertised a fixed bug as latent); and "eight", from grepping
`blargg::run_rom` — which misses the suites that sit *inside* `namespace
blargg` and call it unqualified. There are three of those, so the qualified
grep undercounts by exactly them.

Enumerate users this way instead, and count the lines:

```sh
grep -rln "run_rom(" tests/          # 12 hits: the harness + 11 suites
```

A private copy is a suite that runs blargg ROMs and does *not* appear in that
list. Do not grep for the `$DE $B0 $61` signature bytes to find one — `0xDE`
and `0xB0` are also an opcode table, a `0xDEADBEEF` payload and a palette
fixture, and the one real hit is `blargg_ppu_tests.cpp` *writing* the signature
on purpose to prove the PRG-RAM window round-trips. Count the copies before
claiming a number, and check the grep can see all of them first.

Per-suite *diagnostics* still belong in the suite: what to suspect when a ROM
times out is different for each, and those messages are the reason a red run is
a work queue rather than a wall. Only the protocol is shared.

**Blargg's 2005-era suites predate `$6000` and report on screen.** They are not
unusable headlessly: `tests/nametable_screen.h` reads the result straight out of
the nametable, because the ROMs write ASCII-mapped tile indices. Three suites go
through it — the 2005 PPU ROMs, `sprite_hit`, and `blargg_apu_2005`. **In those,
`$01` means passed, not `0`** — a reader that treated `0` as success would score
an unwritten nametable, i.e. a ROM that never ran, as a pass. Let the screen
settle too: intermediate codes are printed while the ROM works.

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

**A worktree also gets none of the test ROMs**, because they are gitignored, and
linking them in is easy to get wrong in a way nothing reports. An unresolved
fixture *skips*, and a skip exits 0 — so the suite still says "0 failed" while
quietly executing fewer cases. A review agent run this way reported that all
seven commits on a branch overclaimed their executed count by exactly 5:
constant offset, reproducible, evidence attached, and entirely its own rig. The
setup had created a `local` directory and symlinked the real one *inside* it, so
the ROM sat at `test_files/local/local/smb.nes` while the test asked for
`test_files/local/smb.nes`, and the five `commercialRom` cases skipped in every
worktree and in none of the main-tree runs. Acting on it would have rewritten
seven correct commit messages into wrong ones.

So make any count from a worktree carry its *skipped* number next to the
executed one — `tests/run_tests.sh` prints both, and the pair is what makes the
artifact visible — then cross-check the total against a main-tree run before
believing it. A constant offset is evidence about the instrument until proven
otherwise.

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
