#!/bin/sh
# Fetches blargg's ppu_read_buffer test, which his readme calls a "mammoth test
# pack ... mostly centering around the PPU $2007 read buffer".
#
# It was excluded until now for one reason: it is mapper 3 (CNROM), and only
# NROM was supported. CNROM is NROM plus a switchable 8KB CHR window, and this
# ROM genuinely needs it - its header advertises four CHR banks (32KB).
#
# It reports textually on screen rather than through the $6000 protocol, so it
# is read with the nametable-ASCII reader in tests/nametable_screen.h, the same
# one the 2005-era suites use.
#
# Not committed: redistributable-but-unlicensed dumps. SHA256-pinned.
#
# Usage: tests/test_files/fetch_ppu_read_buffer.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/ppu_read_buffer"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/ppu_read_buffer"

mkdir -p "$DEST"

NAME="test_ppu_read_buffer"
WANT="230a52fc557c098eba163801d1b6bbf9f57fe8c5ff79a3f968c804dedb1290ba"
dest="$DEST/$NAME.nes"

if [ -f "$dest" ]; then
    have=$(sha256sum "$dest" | cut -d' ' -f1)
    if [ "$have" = "$WANT" ]; then
        echo "ok (cached): $NAME.nes"
        exit 0
    fi
    echo "checksum mismatch on cached $NAME.nes, refetching" >&2
fi

echo "fetching $NAME.nes..."
curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --max-time 120 -o "$dest.tmp" "$BASE/$NAME.nes"

have=$(sha256sum "$dest.tmp" | cut -d' ' -f1)
if [ "$have" != "$WANT" ]; then
    rm -f "$dest.tmp"
    echo "SHA256 mismatch for $NAME.nes" >&2
    echo "  expected: $WANT" >&2
    echo "  actual:   $have" >&2
    exit 1
fi

mv "$dest.tmp" "$dest"
echo "ok: $NAME.nes"
