---
name: status
description: Reconcile the README's status table and feature claims against an actual test run, so the documented state matches the code. Use when finishing a milestone, before a release or PR, or when the README's claims are suspected of being stale.
---

# Reconciling the README with reality

The README makes specific, checkable claims: which suites pass, which mappers
exist, what is deliberately absent. Those claims drift as features land — the
status table gets updated and the prose below it does not. This skill closes
that gap with evidence rather than recollection.

## 1. Get the real numbers

Fetch the fixtures (skip `fetch_single_step_tests.sh` unless the 1.1 GB of
vectors is already present), build, and run the suite:

```sh
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build -j"$(nproc)" --output-on-failure
```

Then compute the honest figures the way CI does — registered, skipped,
executed. `ctest --test-dir build -N` lists without running. **Never report the
`ctest` pass total as the headline**: `GTEST_SKIP` exits 0, so skips are
counted as passes.

Note which suites skipped and why (missing fixtures, user-supplied ROMs) — that
belongs in the write-up, not just the total.

## 2. Check every claim against the source

Go through the README claim by claim. For each, find the code or test that
backs it. The recurring failure modes:

- **Negative claims that are no longer true.** "No sprite rendering", "no
  audio", "Controller 1 is open bus for now" — these were accurate when
  written. Grep for the feature; if `include/controller.h` and
  `tests/controller_tests.cpp` exist, the claim is stale.
- **Mapper lists.** Check `src/rom.cpp` and the mapper tests for what is
  actually implemented, not what the prose remembers.
- **"Next:" and roadmap lines** pointing at work that has since merged. Check
  `git log`.
- **Named test counts** ("all eleven `sprite_hit` ROMs"). Verify against the
  fetch script's file list and the actual run.
- **Bare totals in prose** ("854 tests", "executed 359 of them"). These do not
  belong in the README at all any more — `tests/TEST_COUNTS.md` is the one
  place a count is written down, and it is generated. If you find one, replace
  it with a link to that table rather than updating it; see the `test-counts`
  skill. A total maintained by hand goes stale between the paragraph and the
  run it describes, which this README has already done once.

## 3. Rewrite, keeping the voice

The README's register is precise, measured, and willing to state what is *not*
covered — the "Verification: what has and has not been exercised" section is
the model. Preserve that. Specifically:

- Do not simply delete negative statements. Some gaps are real and stating them
  is the README's most valuable property. Confirm each one before removing it.
- Keep the honesty caveats about skipped-vs-executed counts and about
  `tests/test_files/local/` ROMs never being fetched.
- Keep documented divergences (the `$AB` opcode, the MMC3 chip-revision split).
  They are findings, not embarrassments.

## 4. Report

Tell the user what changed and what evidence backed each edit. If a claim could
not be verified either way, say so rather than guessing — an unverified claim
in this README is worse than an absent one.
