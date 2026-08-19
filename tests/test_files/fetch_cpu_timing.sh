#!/bin/sh
# Fetches the three ROMs that time the CPU itself, from two suites.
#
# Fetched as a DIAGNOSTIC and kept as a guard. The DMC DMA investigation reached
# a point where sprdma_and_dmc_dma reported a uniform eleven-cycle excess that no
# part of the DMA model could account for, and the measurement it makes is of a
# timed code sequence - so the next suspect outside that model was the CPU's own
# instruction timing. These three answer that, and the answer is that it is
# correct. They now stay so the question does not have to be asked twice.
#
# All three are NROM (mapper 0). instr_timing's singles are 32KB PRG + 8KB CHR;
# cpu_timing_test is 16KB PRG with CHR-RAM.
#
# MEASURED, all passing on first contact:
#
#   1-instr_timing    status 0 "Passed"  - times every official instruction, the
#                     NOPs, alternate SBC and the unofficial instructions. Its
#                     own text notes it does not time the 8 branches or 12
#                     illegal instructions, which is what the second one is for.
#   2-branch_timing   status 0 "Passed"
#   cpu_timing_test   screen reads "6502 TIMING TEST (16 SECONDS) / OFFICIAL
#                     INSTRUCTIONS ONLY / PASSED"
#
# TWO REPORTING PROTOCOLS, so they are wired differently. The instr_timing
# singles use blargg's $6000 PRG-RAM protocol and go through
# blargg_rom_harness.h. cpu_timing_test6 predates it and prints to the screen,
# so it goes through nametable_screen.h like the other 2005-era ROMs - and it
# says PASSED in capitals rather than the "Passed" the protocol ROMs use.
#
# THE FRAME BUDGETS ARE LARGE ON PURPOSE. 1-instr_timing announces "takes about
# 25 seconds" and cpu_timing_test "16 SECONDS"; at ~60 frames a second that is
# 1500 and 960 frames of real work, not a hang. Measured: cpu_timing_test needs
# about 1200 frames before PASSED appears, and an early-exit-on-stable reader
# stops too soon because the screen sits unchanged for long stretches while it
# computes. Read it at a fixed frame count.
#
# (c) Shay Green (blargg) and Kevin Horton, from the nes-test-roms collection.
# Not committed: gitignored like every other fetched fixture.
#
# Usage: tests/test_files/fetch_cpu_timing.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/cpu_timing"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master"

mkdir -p "$DEST"

ROMS="instr_timing/rom_singles/1-instr_timing e260068839fe3d0402376e97e4ee15f5790ee77c701fd0700bba057527910222
instr_timing/rom_singles/2-branch_timing 0afaa393f375844ab98834c1ecba7fa6d8c44880c8b6e738936d0f04a84c8538
cpu_timing_test6/cpu_timing_test 6ab4fe8af23b12ca0dfccfc030de3d4069bf2498e3ef20ddcf1ca75555065b85"

echo "$ROMS" | while read -r path want; do
    name=$(basename "$path")
    dest="$DEST/$name.nes"

    if [ -f "$dest" ]; then
        have=$(sha256sum "$dest" | cut -d' ' -f1)
        if [ "$have" = "$want" ]; then
            echo "ok (cached): $name.nes"
            continue
        fi
        echo "checksum mismatch on cached $name.nes, refetching" >&2
    fi

    echo "fetching $name.nes..."
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --max-time 120 -o "$dest.tmp" "$BASE/$path.nes"

    have=$(sha256sum "$dest.tmp" | cut -d' ' -f1)
    if [ "$have" != "$want" ]; then
        rm -f "$dest.tmp"
        echo "SHA256 mismatch for $name.nes" >&2
        echo "  expected: $want" >&2
        echo "  actual:   $have" >&2
        exit 1
    fi

    mv "$dest.tmp" "$dest"
    echo "ok: $name.nes"
done

count=$(ls -1 "$DEST"/*.nes 2>/dev/null | wc -l)
if [ "$count" -ne 3 ]; then
    echo "expected 3 ROMs in $DEST, found $count" >&2
    exit 1
fi

echo "done: $count ROMs in $DEST"
