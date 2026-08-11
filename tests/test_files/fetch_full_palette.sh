#!/bin/sh
# Fetches blargg's full_palette suite: the oracle for the two behaviours
# include/ppu.h has recorded as deliberate gaps since the PPU was written.
#
#   - PPUMASK colour emphasis, bits 5-7, is not applied.
#   - The "forced backdrop" case is not modelled: with rendering disabled and
#     the VRAM address v pointing into $3F00-$3FFF, hardware outputs the colour
#     v addresses rather than the backdrop.
#
# These three ROMs are the reason both are testable at all. They are built ON
# the forced-backdrop trick rather than merely touching it - blargg's own
# header for full_palette.s says it outright:
#
#   "Displays entire 400+ color NTSC NES palette on screen. Disables PPU
#    rendering so that current scanline color can be set directly by VRAM
#    address, then uses cycle-timed code to cycle through all colors in a
#    clean grid."
#
# and the emphasis bits are driven from the same loop, `tya / and #$E0 /
# sta $2001`. One suite, both gaps.
#
# THEY REPORT NOTHING. There is no $6000 status byte and no PRG-RAM protocol -
# the reset handler is `irq: nmi: rti` and the program just draws. So
# blargg_rom_harness.h does not apply; these go through the framebuffer, like
# tests/visual_rom_tests.cpp. What can be asserted is what the picture CONTAINS
# rather than that a ROM said "Passed".
#
# BE WARNED WHAT THIS SUITE ACTUALLY TESTS. It is not a palette lookup table
# check. The grid only comes out if the CPU and PPU agree to the dot: the ROM
# synchronises to VBL by exploiting that its loop takes 27 cycles against a
# 29780.67-cycle frame, and it deliberately leans on the odd-frame clock skip -
# "Enable BG so that PPU will make every other frame shorter by one PPU clock.
# This allows our code to synchronize better and reduce horizontal shaking."
# A timing regression shows up here as a smeared or shifted grid, not as a
# wrong colour.
#
# MEASURED BEFORE ANY OF IT WAS IMPLEMENTED - emphasis absent, forced backdrop
# absent - counting distinct palette indices in the finished framebuffer:
#
#   full_palette.nes          5 distinct indices  (01 0F 11 21 31)
#   full_palette_smooth.nes   5 distinct indices  (01 0F 11 21 31)
#   flowing_palette.nes       5 at frame 300, 4 at frame 900 (animated)
#
# Stable at 60, 300 and 900 frames, so the ROMs run rather than hang, and the
# number is not a snapshot of something still settling. $0F is the blackened
# backdrop the ROM fills the palette with, covering the screen because a
# disabled renderer currently emits $3F00 instead of the colour v points at;
# the other four are bands where the background was still on while the ROM
# rewrote palette RAM underneath it.
#
# The target is the whole palette: 64 colours, and 8 emphasis states over them.
# Distinct-index count is therefore the honest progress metric, and 5 is the
# floor to beat.
#
# All three are NROM (mapper 0), 32KB PRG + 8KB CHR, already supported - the
# suite needs no mapper work, only PPU work.
#
#   full_palette.nes         the grid, one colour per cell.
#   full_palette_smooth.nes  the same with finer cycle timing.
#   flowing_palette.nes      animated; the frame-to-frame variant.
#
# (c) Shay Green (blargg), from the nes-test-roms collection. Not committed:
# gitignored like every other fetched fixture.
#
# Usage: tests/test_files/fetch_full_palette.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/full_palette"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/full_palette"

mkdir -p "$DEST"

fetch() {
    name=$1
    want=$2
    dest="$DEST/$name"

    if [ -f "$dest" ]; then
        have=$(sha256sum "$dest" | cut -d' ' -f1)
        if [ "$have" = "$want" ]; then
            echo "ok (cached): $name"
            return 0
        fi
        echo "checksum mismatch on cached $name, refetching" >&2
    fi

    echo "fetching $name..."
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --max-time 120 -o "$dest.tmp" "$BASE/$name"

    have=$(sha256sum "$dest.tmp" | cut -d' ' -f1)
    if [ "$have" != "$want" ]; then
        rm -f "$dest.tmp"
        echo "SHA256 mismatch for $name" >&2
        echo "  expected: $want" >&2
        echo "  actual:   $have" >&2
        exit 1
    fi

    mv "$dest.tmp" "$dest"
    echo "ok: $name"
}

fetch full_palette.nes \
    7924ba0a808fb1b7029fab5b3fcfe08c8a318a9bdae2948258ffdcbb8d99b657
fetch full_palette_smooth.nes \
    cf3dccb3e149d6edfd53d1d020bce988973c732f51e5cfbedac1c69fedec495f
fetch flowing_palette.nes \
    bcac93cba22243da0625e82126c4c12bf529b20b57dcc626d8cb0776df5156f0

count=$(ls -1 "$DEST"/*.nes 2>/dev/null | wc -l)
if [ "$count" -ne 3 ]; then
    echo "expected 3 ROMs in $DEST, found $count" >&2
    exit 1
fi

echo "done: $count ROMs in $DEST"
