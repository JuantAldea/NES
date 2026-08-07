#!/bin/sh
# Fetches the two ROMs used for LOOKING at the picture.
#
# Nothing in ctest runs these. They exist because the automated suite cannot
# answer "does it look right" - the ROM oracles report pass/fail through RAM or
# a nametable, and a renderer can satisfy every one of them while producing a
# picture nobody would recognise. These are run by hand, through
# `build/nes_frontend` or a PPM dump via include/frame_dump.h, and the output is
# looked at.
#
# They are pinned here so those checks are reproducible. Claiming "I looked at
# it and it was fine" is worth very little if the next person cannot obtain the
# same bytes.
#
#   spritecans.nes   NROM (mapper 0), 16KB PRG + 8KB CHR-ROM.
#                    64 sprites bouncing, with deliberate OAM cycling so that no
#                    sprite disappears for more than a frame. Exercises the
#                    eight-per-line dropout and the flicker that follows it,
#                    which no pass/fail ROM covers.
#                    (c) Damian Yerrick, from the nes-test-roms collection.
#
#   240pee.nes       UNROM (mapper 2), 64KB PRG, CHR-RAM.
#                    The 240p Test Suite: scrolling grids, sprite-0 hit splits,
#                    overscan and monoscope patterns. GPL-2.0, source at
#                    https://github.com/pinobatch/240p-test-mini - it is free
#                    software, not a redistributed commercial game.
#                    This is the closest thing to a real game the project runs,
#                    and it is what exercises mid-frame scrolling and split
#                    screens - the paths the test ROMs leave almost untouched.
#
# NOTE: no commercial game is fetched by anything in this repository, and none
# should be. If you own a cartridge and have dumped it, point the frontend at
# your own file.
#
# Not committed: gitignored like every other fetched fixture.
#
# Usage: tests/test_files/fetch_visual_roms.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/visual"

mkdir -p "$DEST"

fetch() {
    name=$1
    want=$2
    url=$3
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
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --max-time 120 -o "$dest.tmp" "$url"

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

fetch spritecans.nes \
    87ccbff575df8679b7474688090ba758c50334bcf3a794ec64867fddded2c61d \
    "https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/spritecans-2011/spritecans.nes"

fetch 240pee.nes \
    04f01d7372f66ea2befe325b9bd655a9fcc395a31fa46d9466286bb8f9d2e62e \
    "https://github.com/pinobatch/240p-test-mini/releases/download/v0.23/240pee.nes"

echo "done. Run one with:  ./build/nes_frontend $DEST/240pee.nes"
