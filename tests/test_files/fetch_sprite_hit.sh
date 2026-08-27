#!/bin/sh
# Fetches Blargg's sprite 0 hit tests (sprite_hit_tests_2005.10.05).
#
# These are the ONLY external oracle the background rendering pipeline has.
# Sprite 0 hit fires when an opaque sprite-0 pixel overlaps an opaque
# BACKGROUND pixel, so passing these requires the background pipeline to put
# the right pixel at the right place on the right dot - they test far more of
# the background than of sprites. 02.alignment checks pixel-exact tile
# placement, 05.left_clip the PPUMASK left-8 window, and 09-11 the dot-exact
# timing of the flag.
#
# Like the rest of the 2005 series they predate the $6000 protocol and report
# "on screen and by beeping", which is readable without any rendering: the ROMs
# write their result into the nametable as ASCII-mapped tile indices.
#
# All eleven are mapper 0 (NROM) with CHR-RAM - verified from their iNES
# headers - so they upload their own font through $2007 and need no mapper
# support beyond what is already here.
#
# MEASURED before any rendering existed, which is how these facts are known
# rather than assumed:
#
#   * They report "PASSED" or "FAILED #N" - NOT the "$01" of the 2005 PPU
#     ROMs. A reader that looks for "$01" reads these as a permanent failure.
#   * They print a TITLE line above the verdict, so "first non-blank row" - the
#     rule the 2005 PPU reader uses - returns "SPRITE HIT BASICS" and never
#     sees the result. The whole screen has to be read.
#   * Ten of the eleven reported a specific failure code with the PPU unable to
#     draw anything at all: 01-04 gave #2, 06-10 gave #3, 05 gave #4. That is
#     what makes them a usable oracle - they fail LOUDLY and specifically.
#   * 11.edge_timing reported "PASSED" with rendering entirely unimplemented.
#     Its three checks are all negative ("hit time shouldn't be based on pixels
#     under left clip / at X=255 / off right edge"), and a hit that never
#     happens is never too early. It is VACUOUS until sprite 0 hit works, so
#     "11/11" is ten ROMs of evidence plus one that has to be shown capable of
#     failing by mutation before it counts for anything.
#
# Not committed: redistributable-but-unlicensed dumps. SHA256-pinned so a
# truncated download fails here rather than as an inexplicable emulation bug.
#
# Usage: tests/test_files/fetch_sprite_hit.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/sprite_hit"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/sprite_hit_tests_2005.10.05"

mkdir -p "$DEST"

ROMS="01.basics 51819e8e502bd88fe3b7244198a074dbeef2e848f66c587be04b04f1f0d4bb52
02.alignment 125bbb3ce1e67370f1f4559c2ad3221e52a3e98880b9789400292b5f3a8b39e6
03.corners 9dd57776bc6267fe6183c5521d67cbe3fccc6662ae545eb2c419949bf39644d3
04.flip 5f7142bddb51b7577f93fa22f9f668efebbeea00346d7255089e1863acb9d46a
05.left_clip 69b329658c17b953f149c2f0de77eb272089df22c815bd2fd3d6f43206791c13
06.right_edge 8e6653fcb869e06873e29e5e4423122ea72ba0bf38f3ba9e39f471420db759a4
07.screen_bottom 05849956f80267838c5b6556310266b794078a4300841cbb36339fd141905a0b
08.double_height 127fd966b6b32d6d88a53c5f59d7e938827783c9ad056091f119be1c4ab21c71
09.timing_basics 311698c717e50150edd0b5fd0016c41de686463205c20efb5630d6adb90859fd
10.timing_order 0f36bc07bfe51c416e3cc1a5231053572aa6b15aa60e6d2fd0568be49b6dc2e9
11.edge_timing 5a7c121f6e76617be88a0a7035c0e402293be5c685c95b97190a8d70835736ab"

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
if [ "$count" -ne 11 ]; then
    echo "incomplete: $count/11 files present in $DEST" >&2
    exit 1
fi

echo "done: $count files in $DEST"
