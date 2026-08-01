#!/bin/sh
# Fetches the nestest ROM and its canonical execution log.
#
# These are not committed: the ROM is a redistributable-but-unlicensed dump and
# the log is ~850 KB of generated text. Both are verified by SHA256 so a silent
# upstream change or a truncated download fails here rather than showing up as a
# mysterious CPU divergence.
#
# Usage: tests/test_files/fetch_nestest.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

ROM_URL="https://www.qmtpro.com/~nes/misc/nestest.nes"
LOG_URL="https://www.qmtpro.com/~nes/misc/nestest.log"

ROM_SHA="f67d55fd6b3cf0bad1cc85f1df0d739c65b53e79cecb7fea8f77ec0eadab0004"
LOG_SHA="627c8e180b1a924dfa705c5dc6958fad7ab75a62de556173caf880ccc1337540"

fetch() {
    url=$1
    dest=$2
    want=$3

    if [ -f "$dest" ]; then
        have=$(sha256sum "$dest" | cut -d' ' -f1)
        if [ "$have" = "$want" ]; then
            echo "ok (cached): $(basename "$dest")"
            return 0
        fi
        echo "checksum mismatch on cached $(basename "$dest"), refetching" >&2
    fi

    echo "fetching $(basename "$dest")..."
    curl -fsSL --max-time 120 -o "$dest.tmp" "$url"

    have=$(sha256sum "$dest.tmp" | cut -d' ' -f1)
    if [ "$have" != "$want" ]; then
        rm -f "$dest.tmp"
        echo "SHA256 mismatch for $url" >&2
        echo "  expected: $want" >&2
        echo "  actual:   $have" >&2
        exit 1
    fi

    mv "$dest.tmp" "$dest"
    echo "ok: $(basename "$dest")"
}

fetch "$ROM_URL" "$DIR/nestest.nes" "$ROM_SHA"
fetch "$LOG_URL" "$DIR/nestest.log" "$LOG_SHA"
