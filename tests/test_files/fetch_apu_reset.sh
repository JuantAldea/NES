#!/bin/sh
# Fetches blargg's apu_reset suite - what the APU looks like at power, and what
# a RESET does to it.
#
# Fetched now because it tests exactly what APU Phase 1 just built: $4015's
# enables and status, the length counters, and the $4017 handling that was
# already there. It audits that work rather than extending it.
#
# All six are NROM (mapper 0), 32KB PRG + 8KB CHR - no mapper work needed.
#
# MEASURED with the frame counter, its /IRQ and the length counters
# implemented, and nothing else - no envelope, sweep, linear counter, channels
# or DMC:
#
#   4015_cleared        $81 @ frame 10   "Press RESET"
#   irq_flag_cleared    $81 @ frame 11   "Press RESET"
#   len_ctrs_enabled    $81 @ frame 12   "Press RESET"
#   4017_written          2 @ frame 20   "At power, $4017 should be written with $00"
#   works_immediately     2 @ frame 20   "At power, writes should work immediately"
#   4017_timing           3 @ frame 30   "Frame IRQ flag should be set sooner after
#                                         power/reset" (prints: delay 4)
#
# THE SPLIT IS THE FINDING, and the two halves need different work.
#
# THREE ARE BLOCKED ON THE HARNESS, NOT THE EMULATOR. Status $81 is blargg's
# "needs reset": the ROM finished its power-on checks, and is waiting for a soft
# RESET so it can test the reset half. blargg_rom_harness.h records that in
# RomResult::needs_reset and does not act on it - "we do not drive one". So
# those three are not failing; they are passing the half we can reach and
# stopping at the half we cannot. Driving a reset is a HARNESS feature, and
# until it exists these ROMs test power-on only.
#
# THREE REPORT REAL FAILURES, and they are one subject: the machine's state at
# power-on.
#
#   - "$4017 should be written with $00" - at power the APU must behave as if
#     $00 had been written to $4017, which it currently does not.
#   - "writes should work immediately" - $4017, $4015 and the length counters
#     must be usable from the first instruction.
#   - The delay before the frame IRQ appears. The ROM PRINTS the measured
#     figure, which is the most useful number in this suite: ours is 4, and
#     blargg's readme says hardware after a minute powered off is "usually 9",
#     with 9-12 the accepted range. That is a concrete target, not a verdict.
#
# All six BOOT AND REPORT rather than hanging, which is what makes them usable.
# The frame numbers are a FLOOR and will rise as ROMs get further.
#
# NOT COVERED HERE: everything about the channels. This suite is about power and
# reset state only.
#
# (c) Shay Green (blargg), from the nes-test-roms collection. Not committed:
# gitignored like every other fetched fixture.
#
# Usage: tests/test_files/fetch_apu_reset.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/apu_reset"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/apu_reset"

mkdir -p "$DEST"

ROMS="4015_cleared ef83bc2831f0ddb9e563ac5cbcfa21b129f092911ef42adabae7d47b3e990d95
4017_timing 0e6072c6dcee98fb73dc7f3af2e48face78be300e8510515ed48dc75d15c1f13
4017_written 022bd3b45a733179d9a0a9bf0311d09ca81419d7e7434e6f559e42650b39616b
irq_flag_cleared e2435b213bf21065b7c9c645359500c1c860a7395d12d051232ab14dad1b0bb5
len_ctrs_enabled e05546cbfaa1414d9193b0212084b324ea6b13130af5f171a34c9e574f5ac373
works_immediately c750113762ee375319b1bfbf65c457875dbb194649d7e7b3594fba38eb8eefa1"

echo "$ROMS" | while read -r name want; do
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

count=$(ls -1 "$DEST"/*.nes 2>/dev/null | wc -l)
if [ "$count" -ne 6 ]; then
    echo "expected 6 ROMs in $DEST, found $count" >&2
    exit 1
fi

echo "done: $count ROMs in $DEST"
