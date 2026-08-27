#!/bin/sh
# Fetches blargg's cpu_reset and cpu_dummy_reads/writes ROMs.
#
# These cover CPU behaviour that nothing else here reaches:
#
#   registers.nes / ram_after_reset.nes  (cpu_reset)
#       What a RESET does, as opposed to a power-on. This project called
#       CPU::reset() as setup in almost every test and asserted on it in none,
#       and registers.nes found a real bug on first contact: reset was
#       implemented as power-on, zeroing A/X/Y and slamming S to $FD. The ROM
#       states the rule in a line - "Reset should set I flag, subtract 3 from S,
#       nothing more" - and reported Failed #3 until it did exactly that.
#
#   cpu_dummy_reads.nes
#       The extra read an indexed or read-modify-write access performs before
#       the real one. Reported Passed on first contact.
#
#   cpu_dummy_writes_oam.nes / cpu_dummy_writes_ppumem.nes
#       "Any read-modify-write opcode should first write the original value,
#       then the calculated value." Both reported $6000 status $00 on first
#       contact - the schedule table already had this right.
#
# Mappers: cpu_dummy_reads is mapper 3 (CNROM), the rest mapper 0. All
# supported.
#
# MEASURED. Two reporting protocols are in play and the harness needs both:
# cpu_dummy_reads reports on screen, the other four use the $6000 protocol.
#
# THE RESET ROMS NEED A RESET DRIVEN. They park at $6000 status $81 ("ROM wants
# a soft reset") and wait. No harness here drove one before - blargg_ppu_tests
# explicitly fails that case with "harness does not drive one". Measured: the
# delay matters. blargg's readme says wait ~100ms; at 7 frames registers.nes
# still reported Failed #3, and it passes from 12 frames onward. The harness
# uses 15.
#
# Not committed: redistributable-but-unlicensed dumps, SHA256-pinned.
#
# Usage: tests/test_files/fetch_cpu_behaviour.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/cpu_behaviour"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms"

mkdir -p "$DEST"

ROMS="registers cpu_reset a30f33fb6c9f56012fba38dc85ddc3dccc06bfc0b25fef7711b63f8207279715
ram_after_reset cpu_reset f1802a5618aaaa0c4d592caa45b0b13c54082af93fc311bda0c27bceacbc7c7f
cpu_dummy_reads cpu_dummy_reads db4f91b80c5fbc123e7dcb420fb7fea9b8a18613edf4de7f3d1e3ed95e3117c9
cpu_dummy_writes_oam cpu_dummy_writes 7c1d71a38b2e873d0874add8b823ff39b99151bb29f50096d8021787020c566c
cpu_dummy_writes_ppumem cpu_dummy_writes f59ac329f4872277ccbeff9dd595b901d861af8d53e8a43dcca93bb86752a6b3"

echo "$ROMS" | while read -r name dir want; do
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
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --max-time 120 -o "$dest.tmp" "$BASE/master/$dir/$name.nes"

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
if [ "$count" -ne 5 ]; then
    echo "incomplete: $count/5 files present in $DEST" >&2
    exit 1
fi

echo "done: $count files in $DEST"
