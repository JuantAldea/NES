#!/bin/sh
# Fetches blargg's cpu_exec_space ROMs.
#
# From its readme: "These tests verify that the CPU can execute code from any
# possible memory location, even if that is mapped as I/O space." They walk the
# program counter THROUGH $4000-$40FF and $2000-$2007, so every byte fetched
# there is whatever the hardware puts on the bus for a write-only register.
#
# That makes them the only thing here that measures CPU open bus. Both mapper 0.
#
# MEASURED, and they found two real defects:
#
#   ppuio  passed on first contact.
#
#   apu    failed at $4000 immediately. Bus::read returned a constant 0 for
#          write-only registers and unmapped space, where hardware leaves the
#          last value that was on the data bus. Adding a real CPU open-bus
#          latch got it as far as $4014, then it failed there:
#
#              0022 4000 4001 ... 4013 4014 ERROR
#              Mysteriously Landed at $1734
#
#          $4014 is OAM DMA, which is NOT a PPU register - it lives in the 2A03
#          beside the CPU, and only Bus::decode routes it to the PPU device. It
#          was returning the PPU's internal latch instead of the CPU bus. Those
#          are different wires. The write half of that distinction was already
#          modelled; the read half was not.
#
# The fix also let the controller drop its own approximation: the undriven top
# bits of $4016/$4017 were hardcoded to $40, and now come from the real latch.
#
# Not committed: redistributable-but-unlicensed dumps, SHA256-pinned.
#
# Usage: tests/test_files/fetch_cpu_exec_space.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/cpu_exec_space"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/cpu_exec_space"

mkdir -p "$DEST"

ROMS="test_cpu_exec_space_apu 9036b3d64b7f4e0ac7d44ed85eafb4b2f93ffd2ca2ddecc19f1e0d2bec3574fe
test_cpu_exec_space_ppuio 9f7f24d420033e032f5da0e1eb721cce1d843e032b31973acddb0255ef479251"

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
if [ "$count" -ne 2 ]; then
    echo "incomplete: $count/2 files present in $DEST" >&2
    exit 1
fi

echo "done: $count files in $DEST"
