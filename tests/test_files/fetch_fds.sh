#!/bin/sh
# Fetches fdsirqtests - the Famicom Disk System timer IRQ suite, and the only
# FDS test material in nes-test-roms.
#
# THE CONTAINER IS .fds, NOT .nes. These are fwNES disk images: the 16-byte
# header "FDS\x1a" plus a side count, then 65500 bytes of raw disk data per
# side. Both files here are one side, 65516 bytes. ROM::load accepts iNES only,
# so nothing in this emulator can open them yet.
#
# THE BIOS IS REQUIRED AND IS NOT FETCHED BY THIS SCRIPT. An FDS disk carries no
# PRG-ROM and no vectors; the 8KB BIOS at $E000-$FFFF loads the disk's files and
# owns the reset vector. That BIOS is Nintendo firmware and is not
# redistributable - a rung below even the unlicensed dumps the other fetch
# scripts pull - so it lives at tests/test_files/local/disksys.rom, which
# nothing fetches, ever. Do not add a fetch script for it.
#
# MEASURED against this emulator, with no FDS support written:
#
#   both images   "ROM: missing iNES magic number", exit 1. Nothing runs.
#
# That is the entire baseline. Unlike the mmc3 or cpu_test5 tables there are no
# partial results to order the work by, because the container is rejected before
# any emulation happens - the first milestone is a disk that boots at all.
#
# MEASURED against Mesen2 b9fa69d with that BIOS, via
# tools/run_mesen_dump.sh --screen, both images print the SAME screen:
#
#     .....O....No Irq At Start.......
#     .....O....Trigger 1 IRQ.........
#     .....O....IRQ when rvalue = 0...
#     .....O....Reload val not reset..
#     .....O....Disable DiskReg Test..
#     .....O....No Disk Reg = No IRQ..
#     .....O....Can't Ack w/ $4020....
#     .....O....Can't Ack w/ $4021....
#     .....O....Can Ack w/ W:$4022:0..
#     .....O....Cant w/ W:$4022:2.....
#     .....O....Can Ack w/ W:$4023:0..
#     .....O....Cant w/ W:$4023:1.....
#     .....O....2x W:4022 Delays IRQ..
#     .....O....Enbl DiskR after 4022.
#     .....O....Set RelVal DskRg Off..
#     .....O....4022:0 stops irq ctr..
#     .....O....4022:0 not reset rval.
#     .....O....RVal=0 4x W:$4022:2...
#     .....O....Irq w/ Repeat test....
#     .....O....Irq repeat stop test..
#     .....O....RVal=0 w/ Repeat......
#     ....V7 2017-09-22...............
#
# So a correct emulator passes all 21 with nothing pinned as a known failure -
# there is no $AB-shaped divergence to preserve here.
#
# O IS PASS, X IS FAIL, ? IS NOT RUN. The ROM loads all three as immediates -
# 'O' at $61F6 and $654E, 'X' at $61C9, '?' at $67D2. So the assertion has to
# COUNT 21 O's: "no X on the screen" is also true of an unwritten nametable,
# which is a ROM that never ran. Same trap as the 2005 suites, where $01 means
# passed and 0 means the screen was never touched.
#
# The font is ASCII-mapped - tile $41 draws 'A', $30 draws '0' - so
# tests/nametable_screen.h reads the result directly. These ROMs predate and
# ignore the $6001/$6000 protocol, and could not use it anyway: $6000 is where
# the disk loads the test's own code.
#
# WHY BOTH IMAGES. They differ at byte 1924, inside the first file's payload,
# and both carry the banner "V7 2017-09-22" - so what the patch changes is not
# known. On a CORRECT emulator they agree, which is exactly why keeping both
# costs nothing and might pay: a broken one may well separate them.
#
# REACHING THAT SCREEN IS ALSO A DISK-DRIVE TEST, not only a timer test. The
# disk holds 5 files and 10252 bytes, and the BIOS must spin the motor, match
# the *NINTENDO-HVC* block, CRC-check each block and place every byte before a
# single character appears - including one file at $DFF6, the BIOS's own vector
# dispatch, and a one-byte file at $2000, which is PPUCTRL. The test program
# itself makes no BIOS calls at all (no JSR or JMP into $E000-$FFFF) and touches
# only $4020, $4021, $4022, $4023, $4025 once and $4030. So the suite splits
# cleanly: the boot exercises the drive, the 21 rows exercise the timer.
#
# The settle time is NOT measured - the runs above were given 25 seconds and the
# screen was stable well before that. Detect settling rather than guessing a
# frame count, as tests/cpu_test5_roms.cpp does.
#
# Not committed: gitignored like every other fetched fixture.
#
# Usage: tests/test_files/fetch_fds.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/fds"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/fdsirqtests"

mkdir -p "$DEST"

DISKS="fdsirqtests 38c3ed71d73c71bd09c54e687173bcc2ceea9aa78c78a2813f6c56106814e86b
fdsirqtestsV7_patched 3ae386ecdf9137c6ba866eb1f2ecbb3d61954f3a2de9441d2b9e60708579a067"

echo "$DISKS" | while read -r name want; do
    [ -n "$name" ] || continue
    dest="$DEST/$name.fds"

    if [ -f "$dest" ]; then
        have=$(sha256sum "$dest" | cut -d' ' -f1)
        if [ "$have" = "$want" ]; then
            echo "ok (cached): $name.fds"
            continue
        fi
        echo "checksum mismatch on cached $name.fds, refetching" >&2
    fi

    echo "fetching $name.fds..."
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --max-time 120 -o "$dest.tmp" "$BASE/$name.fds"

    have=$(sha256sum "$dest.tmp" | cut -d' ' -f1)
    if [ "$have" != "$want" ]; then
        rm -f "$dest.tmp"
        echo "SHA256 mismatch for $name.fds" >&2
        echo "  expected: $want" >&2
        echo "  actual:   $have" >&2
        exit 1
    fi

    mv "$dest.tmp" "$dest"
    echo "ok: $name.fds"
done

count=$(find "$DEST" -name '*.fds' | wc -l)
if [ "$count" -ne 2 ]; then
    echo "expected 2 disk images in $DEST, found $count" >&2
    exit 1
fi

# The BIOS is the other half of the fixture and this script cannot supply it, so
# say so here rather than letting the suite fail later with a message about a
# disk image that is present and fine.
if [ ! -f "$DIR/local/disksys.rom" ]; then
    echo "note: $DIR/local/disksys.rom is absent, so these disks cannot boot." >&2
    echo "      It is Nintendo firmware and is deliberately not fetched - see the" >&2
    echo "      header of this script and tests/test_files/local/README.md." >&2
fi

echo "done: $count disk images in $DEST"
