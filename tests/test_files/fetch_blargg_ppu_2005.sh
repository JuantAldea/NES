#!/bin/sh
# Fetches Blargg's 2005 PPU tests, which cover palette RAM, VRAM access through
# $2006/$2007, and OAM.
#
# These predate his $6000 reporting protocol: they report "on screen and by
# beeping". That looked like it made them unusable headlessly - and they were
# excluded on those grounds - but it turns out the on-screen report IS readable
# without any rendering. The ROMs write their result into the NAMETABLE as tile
# indices, and the font is ASCII-mapped, so reading $2000 onwards and treating
# each byte as a character recovers the text directly. A result of "$01" is a
# pass, per blargg's readme: "A result code of 1 always indicates that all tests
# were passed."
#
# power_up_palette is deliberately NOT here. It reports $02, "Palette differs
# from table", and its own documentation says the values it expects "are
# probably unique to my NES" - so it cannot pass on anything else, and a test
# that cannot pass is worse than no test.
#
# Not committed: redistributable-but-unlicensed dumps. SHA256-pinned.
#
# Usage: tests/test_files/fetch_blargg_ppu_2005.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/blargg_ppu_2005"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/blargg_ppu_tests_2005.09.15b"

mkdir -p "$DEST"

ROMS="palette_ram 11c1de16c67795b247169819a34b9d4dd46165478d6de19d7cd6e7311e906a80
vram_access a0cde2a3c98d9f62afcba62d4e8f4f4228574ff96ce96fbfdc777e1393c16ee6
sprite_ram 81275795ea64dfe22f05e35ef3316e6cf84eba40fc9538fc637a08bf263a3110"

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
if [ "$count" -ne 3 ]; then
    echo "incomplete: $count/3 files present in $DEST" >&2
    exit 1
fi

echo "done: $count files in $DEST"
