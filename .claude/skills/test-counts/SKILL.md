---
name: test-counts
description: Regenerate tests/TEST_COUNTS.md and check it against a real run, so a suite that quietly loses cases is caught. Use after adding or removing tests, when a count in a commit message or the README needs a source, or when the suite total moved and you need to know which suite moved it.
---

# The test-count table

`tests/TEST_COUNTS.md` is the only place a test count is written down. It is
generated; do not hand-edit it, and do not copy figures out of it into prose
that will then rot.

```sh
tests/test_counts.sh --check    # compare committed vs actual (default)
tests/test_counts.sh --write    # regenerate after an explained change
```

## Why a table and not a total

The repo previously carried a running total in eight places across four files.
Nothing read them, every added test invalidated all eight, and they went stale
for four commits at a time — inside the very paragraphs warning that
`"100% tests passed out of N"` is a misleading headline. The table exists so
the number stops being something a human maintains.

**Per suite, because a total only moves.** A total cannot distinguish "someone
added six tests and deleted five" from "nothing happened". A per-suite table
names the suite and the direction, which is what makes it a regression check
rather than a report.

## Reading a `--check` failure

Two categories, and they are not equally serious.

**GROWTH** — a suite gained cases, or a new suite appeared. Normal after doing
work. It exits 0 on its own. Regenerate with `--write` and commit the table
alongside the change that caused it.

**REGRESSION** — a suite lost cases, or vanished. Exits 1. This is never
automatically fine, and it has three causes worth separating before you touch
the table:

1. **A test was deliberately deleted.** Legitimate — e.g. promoting a pinned
   failure into a passing list removes the pin. Say so in the commit message.
2. **A file stopped compiling into the binary.** `tests/CMakeLists.txt` globs
   `*.cpp`, so a renamed or moved file silently drops every case in it. The
   suite vanishes entirely rather than shrinking.
3. **A fixture stopped being found.** Cases that `GTEST_SKIP` still *register*,
   so this does **not** shrink the table — which is the point of the CI column.
   If a suite shrank, the cases are gone, not skipped.

Never run `--write` to make a REGRESSION go away. Explain it first; the table
is the memory of what used to exist.

## Registered vs executed

The table counts **registered** cases. `GTEST_SKIP` exits 0 and still
registers, so registered is always ≥ executed — see "Skipped is not passed" in
CLAUDE.md. The `Runs in CI` column and the summary block carry the difference:

- the 512 per-opcode SingleStepTests need 1.1 GB of vectors CI does not fetch
- `commercialRom` needs a cartridge dump nothing here will ever download

Locally, with both present, every registered case executes — which is why
`tests/run_tests.sh` reports `0 skipped` while CI reports several hundred.

## When quoting a number

Commit messages and reports should give the **executed** figure from
`tests/run_tests.sh`, which runs the suite rather than listing it. Use the
table for *which suites exist and how big they are*; use `run_tests.sh` for
*what actually ran and passed*. They answer different questions, and a
commit message claiming a pass rate from a listing has not run anything.

## The trap this script itself fell into

The first draft anchored its skip pattern with `^`, so it never matched
`AllOpcodes/SingleStepBusTrace` and reported 902 executed in CI instead of 390.
It looked like a clean run. **A pattern that cannot match is indistinguishable
from one that found nothing wrong** — the same failure mode as a `grep` for
`FAILED\]` against gtest's `[  FAILED  ]`.

If you change `CI_SKIPPED_PATTERN` or the parsing, check the result against a
figure derived another way before believing it. The CI workflow computes the
same split independently in its "Report what was actually verified" step; if
those two disagree, one of them is broken.
