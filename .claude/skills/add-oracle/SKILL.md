---
name: add-oracle
description: Add a new hardware test ROM suite as a verification oracle - fetch script with SHA256 pins, gitignore, both CI jobs, GTEST_SKIP wiring, and a baseline measurement of what the ROMs report before the feature exists. Use when bringing in a new blargg suite, a nesdev test ROM, or any external ROM/vector set the tests will run against.
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
- The test file: `GTEST_SKIP` with a message naming the fetch script when the
  ROMs are absent, so a missing fixture reports as skipped rather than as a
  mysterious emulation failure.
- The CI "Report what was actually verified" step, **only if** the new tests
  cannot run in CI (large fixtures, user-supplied ROMs). Its `skipped` grep
  needs the new suite's test-name pattern, or the executed count goes wrong.

## 5. Write the test

For blargg suites, go through `tests/blargg_rom_harness.h` — it already
implements the PRG-RAM reporting protocol. Do not reimplement it.

Prefer `TEST_P` with a parameterised list of ROM names over one test per ROM;
that is what `tests/mmc3_rom_tests.cpp` does.

If two ROMs in the suite test opposite hardware revisions, do not drop one.
Pick a revision, say which and why in a comment, and assert the other's failure
*precisely* — the pattern from opcode `$AB` in `tests/instr_test_roms.cpp`,
where an exact-failure assertion keeps a real disagreement visible instead of
burying it.

## Finally

Run `tests/test_files/fetch_<name>.sh` twice — the second run must report
`ok (cached)` for every file and not re-download. Then run the suite and check
that the results match the baseline table you recorded in step 2.
