#!/bin/sh
# Fetches blargg's nes_cpu_test5 - every instruction, checked by a different
# method from every other CPU oracle here.
#
# WHY IT IS WORTH HAVING WHEN instr_test-v5 ALREADY PASSES. The two suites are
# built the opposite way round. instr_test-v5 codes each instruction's behaviour
# as its own test; this one, per its readme, sets "many combinations of input
# values for registers, flags, and memory, running the instruction under test,
# then updating a running checksum with the resulting values", and compares that
# checksum against "what a NES gives". So a wrong result that both suites' authors
# would have had to make the same mistake to miss is not shared between them - the
# checksum sees every flag and register after every combination, not the cases
# someone thought to write down.
#
# Both are mapper 1 (MMC1), 256KB PRG + 8KB CHR. They report ON SCREEN, listing
# failing instructions by opcode and name, so they go through
# tests/nametable_screen.h rather than the $6000 protocol - blargg::run_rom sees
# no signature from them at all.
#
# MEASURED, against this emulator:
#
#   official.nes   ALL ELEVEN GROUPS PASS. "All tests complete", no opcode
#                  listed. Settles by frame 639.
#   cpu.nes        ONE failure, "AB ATX #n", then "Errors: 1 / Failed". Settles
#                  by frame 981.
#
# THE ONE FAILURE IS THE DIVERGENCE THIS REPO ALREADY CHOSE, and finding it here
# independently is the point. Opcode $AB (ATX/LAX #imm) is unstable on hardware:
# it ORs the accumulator with a constant that differs between consoles and with
# temperature. tests/instr_test_roms.cpp picks $EE and pins blargg's
# 03-immediate to fail on exactly ATX and nothing else. This suite, written
# around a different method, disagrees about exactly that opcode and agrees about
# every other one - which is a much stronger statement about the rest of the CPU
# than either suite makes alone.
#
# So cpu.nes is asserted to fail on exactly "AB ATX" with "Errors: 1", the same
# shape as the existing $AB pin. A second opcode appearing there, or the error
# count moving, is a regression that surfaces rather than a ROM anyone has
# learned to ignore.
#
# (c) Shay Green (blargg). Not committed: gitignored like every other fetched
# fixture.
#
# Usage: tests/test_files/fetch_cpu_test5.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/cpu_test5"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/blargg_nes_cpu_test5"

mkdir -p "$DEST"

ROMS="official 5b412b3940abe3f9ed562b86b93cf660688bee95eba927c4dacd53c9da89fe9a
cpu 782b97d45ade98f642893341277bf161d6ff0f973fd0de0de3753d844e931940"

echo "$ROMS" | while read -r name want; do
    [ -n "$name" ] || continue
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
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --max-time 120 -o "$dest.tmp" "$BASE/$name.nes"

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

count=$(find "$DEST" -name '*.nes' | wc -l)
if [ "$count" -ne 2 ]; then
    echo "expected 2 ROMs in $DEST, found $count" >&2
    exit 1
fi
echo "done: $count ROMs in $DEST"
