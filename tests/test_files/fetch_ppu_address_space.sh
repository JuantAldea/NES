#!/bin/sh
# Fetches the PPU test ROMs that can be read HEADLESSLY - i.e. that report
# through the $6000 PRG-RAM protocol rather than by drawing their result.
#
# Blargg's blargg_ppu_tests_2005.09.15b suite (palette_ram, vram_access,
# sprite_ram, power_up_palette) is deliberately NOT here. Those predate the
# $6000 protocol and display their results on screen: their images contain no
# $DE $B0 $61 signature and no "Passed"/"Failed" strings, so a $6000 harness
# reads them as "timed out without initialising" no matter how correct the
# emulator is. They are now read off the nametable instead - see
# fetch_blargg_ppu_2005.sh.
#
# ppu_read_buffer used to be excluded here for two reasons, one of which was
# simply wrong. It IS mapper 3 (CNROM) - correct, and that is why it sat
# unused - but "it does not use the protocol either" was not: it reports
# through $6000 with the standard signature, which a probe confirmed as soon
# as CNROM made it loadable. It has its own script now,
# fetch_ppu_read_buffer.sh.
#
# All three below are mapper 0.
#
# Not committed: redistributable-but-unlicensed dumps. SHA256-pinned so a
# truncated download fails here rather than as an inexplicable emulation bug.
#
# Usage: tests/test_files/fetch_ppu_address_space.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/ppu_address_space"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master"

mkdir -p "$DEST"

# name<space>upstream-path<space>sha256
ROMS="oam_read oam_read/oam_read.nes f298973dabeb61ca35007445f7a615f77e87703c958c870986af83b1aabde926
oam_stress oam_stress/oam_stress.nes 95882d72a7acabe928fd277e3b3e0372f21ef3d41e36d7d8fb17fc017a356f70
ppu_open_bus ppu_open_bus/ppu_open_bus.nes d4208a3ff6340532dd0fced7f9d408d5b6585853a0ddc9c1f64ee1722ef08e67"

echo "$ROMS" | while read -r name path want; do
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
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --max-time 120 -o "$dest.tmp" "$BASE/$path"

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
if [ "$count" -ne 3 ]; then
    echo "incomplete: $count/3 files present in $DEST" >&2
    exit 1
fi

echo "done: $count files in $DEST"
