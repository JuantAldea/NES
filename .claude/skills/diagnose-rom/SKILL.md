---
name: diagnose-rom
description: Diagnose a failing or hanging test ROM - read its reported status and message, find the frame of first failure, separate a hung CPU from a waiting one, and narrow to the responsible subsystem with CPU traces, nestest comparison and frame dumps. Use when a blargg or nesdev ROM test fails, times out, or reports a code you need to interpret.
---

# Diagnosing a failing ROM

Work down this list in order. Each step is cheaper than the one after it, and
most failures are identified in the first two.

## 1. Read what the ROM said

Blargg ROMs report through PRG-RAM, and `tests/blargg_rom_harness.h` already
captures it in `RomResult`:

- `saw_signature` — did `$6001-$6003` ever read back `$DE $B0 $61`? If not, the
  ROM never reached its own init code. That is a load/mapper/reset problem, not
  a problem with whatever the ROM tests.
- `status` — `$80` still running, `$81` wants a soft reset, anything else is
  the final code with `0` meaning pass. The harness drives the reset after
  `kResetDelayFrames`, so a returned `$81` means the ROM blew past `kMaxResets`
  — a reset loop, not a verdict. `resets_driven` says how many it took.
- `message` — the NUL-terminated ASCII at `$6004`. Blargg's messages are
  specific and usually name the exact behaviour. Quote it verbatim; do not
  paraphrase it into the commit message.

The status code is meaningful per suite — check the suite's `readme.txt` for
the code table before guessing.

## 2. Separate "hung" from "waiting"

On a timeout, the harness samples `cpu_cycles` and `final_pc`.

- `cpu_cycles` barely advanced → the CPU is stuck: a jam opcode, or an
  interrupt loop.
- `cpu_cycles` large, `final_pc` in a tight range → the ROM is alive and
  spinning on something that never arrives. Nearly always a flag or interrupt:
  vblank never sets, sprite 0 never hits, the IRQ never fires. Identify which
  address the spin loop reads, and that names the subsystem.
- `frames_run` hit the cap but the ROM was progressing → the budget may just be
  too low. Frame counts recorded in the fetch scripts are floors measured
  against a *less* complete emulator; a ROM that now gets further legitimately
  runs longer. Raise it and re-measure before concluding anything.

## 3. Is it the CPU, or the thing the ROM is testing?

If there is any doubt the CPU itself is sound, run the cheap oracles first —
they are fast and unambiguous:

```sh
./build/tests/tests --gtest_filter='*nestest*:*Klaus*:*instr_test*'
```

`nestest` compares against a canonical log for all 8991 instructions, so a
mismatch reports the first diverging instruction directly. That is a far better
starting point than a PPU-level symptom. If those pass, the CPU is not the
problem and the fault is in the subsystem under test.

## 4. Narrow with a trace

The frontend has a CPU trace to stdout, and there is a step-one-frame control —
useful when the failure is at a known frame from the baseline table.

For anything visual, use `include/frame_dump.h` and `tests/nametable_screen.h`
rather than screenshots: they give the framebuffer as palette indices and the
nametable as text, both diffable and both assertable in a regression test
afterwards.

## 5. Check the obvious hardware-shaped causes

Failures in this emulator concentrate in a few places. Before deep debugging,
rule out:

- A bus access at the wrong *cycle* rather than the wrong address — dummy reads
  on un-carried indexed addresses, and the RMW dummy write of the old value.
  Both are invisible in final state and both break PPU timing.
- `/NMI` treated as an edge rather than a level the CPU samples. Suppression
  cases are unexpressible if it is an edge.
- OAMADDR not held at 0 across the sprite-fetch dots.
- Palette mirroring aliases `$3F10/$14/$18/$1C`.
- The `$2007` read buffer.
- A mapper watching only rendering rather than all PPU address-bus activity.

## 6. Before calling it fixed

Re-run the whole suite, not just the ROM you were chasing — timing fixes in
this codebase routinely move other ROMs in both directions. Report the
*executed* count, not the total: `GTEST_SKIP` exits 0 and `ctest` counts skips
as passes.

If the ROM turns out to test hardware behaviour this emulator deliberately does
not match, do not delete it. Assert the failure precisely, the way opcode `$AB`
is handled in `tests/instr_test_roms.cpp`.
