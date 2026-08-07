#!/bin/sh
# Fetches blargg's mmc3_test_2 rom_singles - the MMC3 scanline IRQ counter.
#
# All six are mapper 4, 32KB PRG + 8KB CHR, verified from their headers.
#
# From the readme, and it shapes the implementation: "The ROMs mainly test
# behavior by manually clocking the MMC3's IRQ counter by writing to $2006 to
# change the current VRAM address." So the mapper must see A12 on ANY PPU
# address-bus activity, not only on pattern fetches during rendering - a design
# that hooks rendering alone passes none of these.
#
# MEASURED with PRG/CHR banking and mirroring implemented but NO IRQ counter.
# All six load, run, and report a specific IRQ failure - no hangs, no blank
# screens - which is what makes them usable:
#
#   1-clocking         $03 @30  "Should decrement when A12 is toggled via PPUADDR"
#   2-details          $02 @31  "Counter isn't working when reloaded with 255"
#   3-A12_clocking     $04 @29  "Should be clocked when A12 changes to 1 via PPUADDR write"
#   4-scanline_timing  $0E @47  "IRQ never occurred"
#   5-MMC3             $02 @33  "Should reload and set IRQ every clock when reload is 0"
#   6-MMC3_alt         $02 @32  "IRQ shouldn't be set when reloading to 0 ..."
#
# THE LAST TWO TEST DIFFERENT CHIPS AND MAY NOT BOTH BE PASSABLE. The readme
# says "The last two ROMs test behavior that differs among MMC3 chips", and
# their failure messages above are direct opposites: 5 wants an IRQ on every
# clock when the reload value is 0 (Sharp), 6 wants none in that case (NEC).
# Pick a revision, say which and why, and pin the other's failure precisely -
# the pattern already used for the unstable $AB opcode in
# tests/instr_test_roms.cpp, where dropping the ROM would have hidden a real
# disagreement.
#
# Those frame numbers are time-to-FIRST-failure and so a floor, not a budget: a
# ROM that gets further runs longer. Re-measure once any of them pass.
#
# Not committed: redistributable-but-unlicensed dumps, SHA256-pinned.
#
# Usage: tests/test_files/fetch_mmc3.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/mmc3"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/mmc3_test_2/rom_singles"

mkdir -p "$DEST"

ROMS="1-clocking b06d8a97f0ca672be92c841d6af7d1e650696e86e9cc0cf6eeb90d67a6ab499b
2-details e7af16c764b119e60effb7b1cfeec3dd8e2e657041283693cdbbeedb4081f1e3
3-A12_clocking b375f15b9f9d372c8084b9c50928be9e41a3ac48be831ce82d203c18891433ad
4-scanline_timing 14a220b9d1272acc7a820ab38e9762a7cdf2d54c65e753be87f23dfcaf1bb845
5-MMC3 e0824123d60b83868dac1189b28250f8e10376a01be468a5a74aa59937cb32ca
6-MMC3_alt 56698b6918453d161a8d4e51f66e363d6966b054939c8176c53c401a6b55269b"

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
if [ "$count" -ne 6 ]; then
    echo "incomplete: $count/6 files present in $DEST" >&2
    exit 1
fi

echo "done: $count files in $DEST"
