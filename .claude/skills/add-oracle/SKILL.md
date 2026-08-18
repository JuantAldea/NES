---
name: add-oracle
description: Add a new hardware test ROM suite as a verification oracle - fetch script with SHA256 pins, gitignore, both CI jobs, an actionable failure when the ROM is absent, and a baseline measurement of what the ROMs report before the feature exists. Use when bringing in a new blargg suite, a nesdev test ROM, or any external ROM/vector set the tests will run against.
---

# Adding a test ROM oracle

Five things must all happen. Missing any one of them produces a suite that
looks wired up and silently does nothing — or worse, a commit containing an
unlicensed ROM dump.

## 1. Find out what the ROMs actually are

Before writing anything, download them to the scratchpad and check:

- The iNES header of each: mapper number, PRG size, CHR size, mirroring. State
  these in the script header — if a suite needs a mapper that is not
  implemented yet, that is the finding, and it changes the plan.
- The suite's `readme.txt` if it has one. Blargg's readmes routinely state
  *how* the ROM tests something, which constrains the implementation. The MMC3
  readme's "by writing to `$2006`" line is the reason the mapper has to see A12
  on any PPU bus activity rather than only during rendering — a design that
  hooked rendering alone would pass none of them.
- Whether any ROMs in the set test mutually exclusive hardware revisions.

## 2. Measure the baseline, before implementing anything

Run each ROM against the *current* emulator and record, per ROM:

    name    status-code @ frame-of-first-failure   "the ASCII message"

Put that table in the fetch script's header, marked `MEASURED with <what was
and was not implemented at the time>`. This is the single most valuable part of
the whole exercise:

- It proves each ROM boots and reports rather than hanging or blanking, which
  is what makes it usable as an oracle at all.
- It converts a wall of red into an ordered work queue.
- The frame numbers are a *floor*, not a budget — a ROM that gets further runs
  longer, so the timeouts need re-measuring once any of them pass. Say so.

[fetch_mmc3.sh](tests/test_files/fetch_mmc3.sh) is the reference for the shape
and tone of this header.

## 3. Write the fetch script

Copy an existing `tests/test_files/fetch_*.sh` — they share one structure:
`set -eu`, a `DIR`/`DEST`/`BASE` preamble, a `name sha256` list, per-file
cached-checksum check, `curl -fsSL --retry 5 --retry-all-errors`, download to
`.tmp`, verify, `mv`, and a final count assertion.

Get the SHA256s by downloading and hashing; never invent them. Most suites live
under `https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/`.

The header must say the ROMs are not committed and why.

## 4. Wire it in — all four places

- `.gitignore`: a comment naming the fetch script, then the directory path
  **with no trailing slash**. Follow the existing block's format.
- `.github/workflows/main.yml`: the fetch list is duplicated in the `test` job
  *and* the `sanitizers` job. Add it to both; they drift otherwise.
- The test file: a message naming the fetch script when the ROMs are absent —
  `could not load <path> - run tests/test_files/fetch_x.sh`. **Fail, do not
  skip.** CI fetches these, so a missing one means the *fetch step failed*, and
  `GTEST_SKIP` exits 0: skipping there turns a broken CI run green, which is
  the outcome this repo is built to prevent. The message is what stops a
  missing fixture looking like an emulation defect.

  `GTEST_SKIP` is only for fixtures that **cannot** be fetched at all —
  `tests/test_files/local/` cartridge dumps and the 1.1 GB SingleStepTests
  vectors. Those are absent on every clean checkout, so failing on them would
  mean a permanently red suite. `SKIP_IF_ROM_ABSENT` in `tests/rom_fixture.h`
  is for exactly that case and no other.

  This section used to say every oracle needed a `GTEST_SKIP`. Twenty suites
  did not have one and were right not to; see "Test ROMs are never committed"
  in CLAUDE.md.
- The CI "Report what was actually verified" step, **only if** the new tests
  cannot run in CI (large fixtures, user-supplied ROMs). Its `skipped` grep
  needs the new suite's test-name pattern, or the executed count goes wrong.

## 5. Write the test

For blargg suites, go through `tests/blargg_rom_harness.h` — it already
implements the PRG-RAM reporting protocol. Do not reimplement it.

Prefer `TEST_P` with a parameterised list of ROM names over one test per ROM;
that is what `tests/mmc3_rom_tests.cpp` does.

`tests/rom_fixture.h` holds the shared helpers — `rom_present`,
`distinct_indices`, `distinct_pixels`, and `SKIP_IF_ROM_ABSENT`. Use them
rather than growing a per-file copy; three suites once had three incompatible
`SKIP_IF_ABSENT` macros, one of which quietly emulated 300 frames behind a name
that promised a file check.

If two ROMs in the suite test opposite hardware revisions, do not drop one.
Pick a revision, say which and why in a comment, and assert the other's failure
*precisely* — the pattern from opcode `$AB` in `tests/instr_test_roms.cpp`,
where an exact-failure assertion keeps a real disagreement visible instead of
burying it.

**Pin the failures you are not fixing yet**, with their exact status and
message, as `tests/apu_rom_tests.cpp` does. A skipped failure says nothing when
it changes; a pinned one fails in both directions, and its message tells the
next person to promote the ROM into the passing list. That is how completing a
feature announces itself — three ROMs did exactly that when the APU length
counters landed.

**Not every non-zero status is a failure.** Blargg's `$81` means "needs reset":
the ROM finished one half and is waiting for a soft RESET to run the other.
`blargg_rom_harness.h` now drives that reset, so `$81` should not survive to
the end of a run — `RomResult::needs_reset` is set only when a ROM asked for
more than `kMaxResets`, which means a reset loop rather than a verdict.

This paragraph used to say the harness did *not* drive one, and that a `$81`
ROM was "blocked on a harness feature". That was true, and writing it down as a
distinct category from a real failure is what made it obvious the feature was
worth building — five `apu_reset` ROMs went straight to passing once it
existed. Keep that habit: a baseline that records *why* something is stuck, not
just that it is, tells you what to build next.

## Finally

Run `tests/test_files/fetch_<name>.sh` twice — the second run must report
`ok (cached)` for every file and not re-download. Then run the suite and check
that the results match the baseline table you recorded in step 2.
