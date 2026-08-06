#!/bin/sh
# Fetches Blargg's sprite overflow flag tests (sprite_overflow_tests).
#
# These are the external oracle for sprite EVALUATION: the eight-sprites-per-line
# limit, the order sprites are scanned in, and the overflow flag in $2002 bit 5.
# Sprite 0 hit already has an oracle (sprite_hit_tests_2005.10.05); this suite
# covers the part of sprite handling that one does not touch.
#
# All five are mapper 0 (NROM), 16KB PRG, no CHR-ROM (so CHR-RAM) and horizontal
# mirroring - verified from their iNES headers, not assumed. They need no mapper
# support beyond what is already here.
#
# Like the 2005-era suites they report "on screen and by beeping", which is
# readable headlessly: the ROMs write their result into the nametable as
# ASCII-mapped tile indices, so tests/nametable_screen.h reads them without any
# rendering. They print a TITLE line above the verdict, so the "first non-blank
# row" rule used by the 2005 PPU reader returns "SPRITE OVERFLOW BASICS" and
# never sees the result; the whole screen has to be read.
#
# THE VERDICT FORMAT IS NOT THE SAME AS sprite_hit_tests, and assuming it was
# cost a wasted measurement run. These print
#
#     FAILED: #2      (with a colon)
#
# where sprite_hit_tests prints "FAILED #2". A reader carrying the sprite-hit
# predicate over unchanged never matches, so all five run to the frame cap and
# report "timed out" - which reads as a dead emulator when in fact every one of
# them is reporting a specific, useful failure code.
#
# From blargg's readme, and it matters for how these are used:
#
#   "THE TESTS MUST BE RUN (*AND* *PASS*) IN ORDER, because some earlier ROMs
#    test things that later ones assume will work properly."
#
# So a failure in 1.Basics makes the verdicts of 3/4/5 meaningless rather than
# merely additional. 5.Emulator in particular exists to catch "an emulator with
# predictive overflow flag handling" - it checks that disabling rendering,
# rewriting OAM, or changing sprite height RECALCULATES the flag time, which
# rules out computing it once per scanline.
#
# MEASURED against this emulator before any sprite evaluation existed. With the
# overflow flag never set at all, every one of the five reports a specific code,
# and none of them passes vacuously:
#
#   1.Basics     FAILED: #2  at frame  7   9 sprites on a scanline sets nothing
#   2.Details    FAILED: #2  at frame  8   nor do sprites under the left clip
#   3.Timing     FAILED: #3  at frame 31   reads as "cleared too early at end of VBL"
#   4.Obscure    FAILED: #2  at frame  8   byte 2 of sprite #10 not read as its Y
#   5.Emulator   FAILED: #2  at frame  8   no overflow calculated without a $2002 read
#
# All five are consistent with "$2002 bit 5 is never set", which is exactly the
# state of the emulator. That they fail LOUDLY and SPECIFICALLY is what makes
# them usable as an oracle, and that none passes vacuously means there is no
# 11.edge_timing situation here - every future pass is a real one.
#
# Those frame numbers are how long a ROM takes to reach its FIRST failure, so
# they are a floor, not the budget. A ROM that gets further runs longer: 3.Timing
# needed 31 frames to reach its third check, so its full fourteen plausibly need
# ~200. tests/sprite_overflow_tests.cpp therefore caps generously and the cap is
# to be RE-MEASURED once any of these pass. Guessed caps have produced a false
# "hang" three times on this project (60 for ppu_vbl_nmi, 300 for
# cpu_interrupts_v2, 400 for oam_stress which needs 1703).
#
# Not committed: redistributable-but-unlicensed dumps. SHA256-pinned so a
# truncated or substituted download fails here, loudly, rather than surfacing as
# an inexplicable emulation bug.
#
# Usage: tests/test_files/fetch_sprite_overflow.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/sprite_overflow"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/sprite_overflow_tests"

mkdir -p "$DEST"

ROMS="1.Basics 1a6782f63ccb3a3dd1aa6a24272036c9c3aa232c2d1ff0b21e872741a3ee4fe2
2.Details 6405a7ff1042fe7a50d9bfe521e43460a3251799425bd3e1863b29b67e7cd587
3.Timing 2252ec8fc35932b408f409ef9b6863edf084aa871fc10ad180b0ac4c2468ef8c
4.Obscure aebf2199344321465ae0d8dcd81f6c528c7f661f31f1814711365b4e573a8263
5.Emulator cf994454219696de82794f0b84f2bd63458444d12a1171c85bba8697ab94acb4"

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
if [ "$count" -ne 5 ]; then
    echo "incomplete: $count/5 files present in $DEST" >&2
    exit 1
fi

echo "done: $count files in $DEST"
