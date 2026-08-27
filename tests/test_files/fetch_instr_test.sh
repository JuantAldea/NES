#!/bin/sh
# Fetches blargg's instr_test-v5 rom_singles - all 256 opcodes, official and
# unofficial, across every addressing mode.
#
# WHY THE SINGLES AND NOT all_instrs.nes: the combined ROMs (all_instrs.nes,
# official_only.nes) are 256KB and MAPPER 1 (MMC1), which this emulator does not
# support. The sixteen rom_singles are mapper 0 and cover the same ground. That
# was checked by reading their iNES headers, not assumed.
#
# These matter most for CI. Locally the 512 SingleStepTests verify every opcode
# per-cycle against hardware-captured bus traces, but they need 1.1 GB of
# vectors that CI does not fetch - so before this, CI's entire instruction-level
# coverage was nestest and Klaus.
#
# MEASURED on first contact: 15 of 16 report $6000 status $00. The exception is
# 03-immediate, and it is a genuine conflict between two oracles rather than a
# bug:
#
#   03-immediate reports $01 "AB ATX #n"
#
# $AB (LXA/ATX) computes A = X = (A | magic) & immediate, where `magic` is an
# analogue effect of the real chip - it varies with the die, its temperature and
# what is floating on the bus. There is no correct value. Measured directly:
#
#   magic = $FF   03-immediate PASSES, SingleStepTests op_ab fails 3 cases
#   magic = $EE   03-immediate fails on ATX, SingleStepTests op_ab PASSES
#   magic = $00   both fail
#
# This emulator uses $EE, which is what the SingleStepTests vectors encode.
# tests/instr_test_roms.cpp asserts that 03-immediate fails with EXACTLY this
# message, so the divergence is pinned rather than hidden: if ATX is ever
# changed, or if that ROM starts failing for some other reason, the test fails
# and says so.
#
# Not committed: redistributable-but-unlicensed dumps, SHA256-pinned.
#
# Usage: tests/test_files/fetch_instr_test.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/instr_test"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/instr_test-v5/rom_singles"

mkdir -p "$DEST"

ROMS="01-basics 4dd1cdd406bc3f747972e7da314ce8ca89321eb7a836c1ced569ee54ae44a384
02-implied 1c4d4fa130cf6feebc072543a5cd3627ae71063b56b08642bf43e9a6c6f44996
03-immediate 6f7ad8ff31c762c37deaee0f323df03eb94025cf1f3b0343ebe6fe567da0e943
04-zero_page 7a8feada4bb4460250c8f05401e5d728878bbe71956756d0b11d488e57eb12fd
05-zp_xy 767f422dc4e651e331456b207f7c6d60d19329fde0c0827e83591dbd91ae5e23
06-absolute 98df36dc4fcc4f37d9eb0539c71283020776b1e5dc6a6ce58671739a8d6534af
07-abs_xy 9ff58d77d8d384cc918fcd3ed877898c5e7330cd475ed2dafb11cbe80ff32eff
08-ind_x 2ec6f5d4a8caee5d8295cebe563f203c26ea9bc05f1dbc967feb88f5dc4f261f
09-ind_y 0fbc8b228d5daa83a4a083bf87ae3a61b5247ebdd91a6b91c8cf8c42784804ac
10-branches 63ab768e88931db6f7dfcfafe43d5e29ebc3dcb80da8fc7fcda8c930f34aef54
11-stack c534191fe3ea4c8940944fda98dd58eb42710268d453f97e8e2c4ae7f15f9cdb
12-jmp_jsr f5b4652690fc04e6b573a2b3b54a29407ad0615d3c264e7cb618b6694b50de55
13-rts b711d25bc55585c252046a1304a0bc64c13cacce7c96a1bac5c8e91f9fc2597f
14-rti f084b00605be1840946b53935032581e68abe1bb24479942751cfe46ddfcb280
15-brk da7ae9a191c4483b540771e15b1f6f18df68f1d1ecd717b59ea8b1ee3596ec3e
16-special 7d03410b61784e49920901e84b00a4f31a19078391f20005c6fac9036d2190f7"

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

count=$(ls -1 "$DEST"/*.nes 2>/dev/null | wc -l | tr -d ' ')
if [ "$count" -ne 16 ]; then
    echo "incomplete: $count/16 files present in $DEST" >&2
    exit 1
fi

echo "done: $count files in $DEST"
