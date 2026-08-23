#!/bin/sh
# Fetches blargg's instr_misc rom_singles - four CPU behaviours that no other
# oracle here reaches.
#
# All four are mapper 0, 32KB PRG + 8KB CHR, iNES 1.0, vertical mirroring,
# verified from their headers. No mapper work needed.
#
# WHAT EACH ONE IS FOR, from the readme:
#
#   01-abs_x_wrap       $FFFF wraps to 0 for STA abs,X and LDA abs,X
#   02-branch_wrap      branching past either end of RAM wraps around
#   03-dummy_reads      the dummy read before the real access, for LDA/STA
#                       with (ZP,X), (ZP),Y and ABS,X, plus ROL ABS,X
#   04-dummy_reads_apu  the same for (hopefully) EVERY instruction that does
#                       one, unofficial opcodes included, printing the opcode
#                       of each failure. Needs $4015 IRQ flag reads to work.
#
# 04 is the one worth having. cpu_dummy_reads (fetch_cpu_behaviour.sh) already
# covers the documented cases; this one sweeps the whole opcode table including
# the unofficial half, and names offenders by opcode rather than by subtest.
#
# MEASURED, before this script existed, with nothing changed to accommodate it:
#
#   01-abs_x_wrap       $00 Passed @ frame  11
#   02-branch_wrap      $00 Passed @ frame  12
#   03-dummy_reads      $00 Passed @ frame  56
#   04-dummy_reads_apu  $00 Passed @ frame 140
#
# A clean sweep on first contact, which is the expected result rather than a
# surprise: nestest, Klaus, SingleStepTests and instr_test-v5 all pass, so the
# instruction core was already well covered and this was a completeness check.
# The rows stay because "correct" and "untested" are indistinguishable until
# something moves, and 04 is the only thing here that would notice a dummy read
# going missing from an UNOFFICIAL opcode.
#
# THE WORD "Passed" IS THE POINT, and it is why these are oracles where
# apu_mixer is not. Both use the $6000 protocol and both report status $00 on
# this emulator. The difference is in the message: apu_mixer prints "1. Should
# play short tone. / 2. Should be nearly silent." - instructions to a listener,
# status $00 meaning only that it ran - while these print the literal string
# "Passed". blargg's readme draws that line explicitly ("Only a test which
# prints 'done' at the end requires that you watch/listen"), and the status byte
# alone does not distinguish the two. See tests/apu_rom_tests.cpp.
#
# Those frame numbers are completion figures, not floors - all four already
# finish - so the cap in the test only has to absorb the emulator getting
# slower per frame, not a ROM getting further.
#
# Not committed: redistributable-but-unlicensed dumps, SHA256-pinned.
#
# Usage: tests/test_files/fetch_instr_misc.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/instr_misc"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/instr_misc/rom_singles"

mkdir -p "$DEST"

ROMS="01-abs_x_wrap 892e892b3b5d3526913b5491252f628d987baf44509dd28a7d109efc8a16bacc
02-branch_wrap 2e535f572a16ab0e27bf6e076de6aaa2a9885c9ae1b02acda7853810529b24c5
03-dummy_reads 9210b8a274c060e0abf9cf7a9f4a140598a8fa01ce852b0a9aef500c9728e822
04-dummy_reads_apu 3e4e974a57b82247861576bc855b589fa6d00291ec159f79795c9e3f50c39fef"

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

count=$(ls "$DEST" | wc -l | tr -d ' ')
if [ "$count" -ne 4 ]; then
    echo "incomplete: $count/4 files present in $DEST" >&2
    exit 1
fi

echo "done: $count files in $DEST"
