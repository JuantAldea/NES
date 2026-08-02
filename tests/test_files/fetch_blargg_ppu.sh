#!/bin/sh
# Fetches Blargg's ppu_vbl_nmi test ROMs (the individual, NROM-mapper builds).
#
# The combined ppu_vbl_nmi.nes uses MMC1, which this emulator does not support;
# the rom_singles builds are all mapper 0, 32KB PRG + 8KB CHR.
#
# Not committed: redistributable-but-unlicensed ROM dumps. SHA256-pinned so a
# truncated download or an upstream change fails here rather than surfacing as
# an inexplicable emulation bug.
#
# Usage: tests/test_files/fetch_blargg_ppu.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/blargg_ppu"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/ppu_vbl_nmi/rom_singles"

mkdir -p "$DEST"

# name<TAB>sha256
ROMS="01-vbl_basics 06aea5af4edab4e3141c939cd5ac9936f8758203b25dcaf84ae1a09db49e024a
02-vbl_set_time dd98856130078844e3aa4bd95a9be8ab501ea84c089f1d8ad49a1b20af4b3a80
03-vbl_clear_time 787fdaa4dd6c5b6df5f4308fb6d55b57e2c2f69bd5ecdf8ad5c69735db4fcc72
04-nmi_control 84722c75b896c47c8642f83220230fe14f0a31e55e26ecb83c400e6a26d91b32
05-nmi_timing 72e515d689d7404ae5779b8c9c4c7b3563a755a94bd44864516f1b03df044482
06-suppression 811dd5997bbf48c2e5687ab06845f17ea76b2be472786596c334137582cc72aa
07-nmi_on_timing 1ed154363660b5775b112ae63ce9bb4e400ebde2afef4d0ac12fc433efda3702
08-nmi_off_timing 1d2a4093091c8e58a7f99d6a3531bbc6346b52cfc59bcb17ca04c1f2376cf2fc
09-even_odd_frames 1ac04283021ddd9294cc74ee709c55e20a350dc4815c15a8a93b3654837e858d
10-even_odd_timing 7217d2d172ce11ad45c4da40c2f22201cf0eb758bc2cd8dd39d2cf0a7d4ca83e"

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
    curl -fsSL --max-time 120 -o "$dest.tmp" "$BASE/$name.nes"

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
