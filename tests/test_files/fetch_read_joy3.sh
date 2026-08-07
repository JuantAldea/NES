#!/bin/sh
# Fetches blargg's read_joy3 controller test - test_buttons.nes only.
#
# This is the external oracle for the standard controller at $4016. It names a
# button on screen, waits for it to be pressed AND released, and moves on; after
# all eight it reports "Passed". Driving it from a test therefore exercises the
# whole path at once - controller strobe and shift register, the CPU reading
# $4016, the PPU rendering the prompt, and the nametable text reader.
#
# It is mapper 3 (CNROM) with 32KB PRG and 8KB CHR, which is already supported.
#
# ONLY test_buttons is fetched, and that is a considered decision rather than
# laziness. The suite also ships thorough_test.nes and count_errors.nes, and
# both were measured before being rejected:
#
#   thorough_test reports "Passed" with NO controller implemented at all - and
#   still reports "Passed" with the shift register mutated so it never advances.
#   It tests read RELIABILITY, and a port that always answers "nothing pressed"
#   is perfectly reliable. Adding it would be a green light that means nothing,
#   which is worse than no light at all.
#
#   count_errors displays a counter that needs input and time to become
#   meaningful; it says nothing useful in a headless run.
#
# test_buttons, by contrast, is provably non-vacuous. Measured against three
# deliberate defects:
#
#   controller always reports "not pressed"  -> stalls forever on prompt "A"
#   shift register never advances            -> "Failed"
#   strobe ignored (always reloading)        -> "Failed"
#
# Not committed: redistributable-but-unlicensed dump. SHA256-pinned so a
# truncated or substituted download fails here rather than as an inexplicable
# emulation bug.
#
# Usage: tests/test_files/fetch_read_joy3.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/read_joy3"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/read_joy3"

mkdir -p "$DEST"

name="test_buttons"
want="15f53317fd2adf8454256fdafb4ea6c5fb27940166adcdadd17e9e6fb94fdfac"
dest="$DEST/$name.nes"

if [ -f "$dest" ]; then
    have=$(sha256sum "$dest" | cut -d' ' -f1)
    if [ "$have" = "$want" ]; then
        echo "ok (cached): $name.nes"
        exit 0
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
