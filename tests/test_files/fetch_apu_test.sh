#!/bin/sh
# Fetches blargg's apu_test suite - the oracle for building the APU.
#
# The APU is the last subsystem that is mostly absent: the frame counter and
# its /IRQ are real, and nothing else is. clock_quarter_frame() and
# clock_half_frame() are empty bodies, $4000-$4013 are unhandled, and the $4015
# write is a stub. So unlike every other suite added here, these ROMs are not
# guarding finished work - they are the work queue for writing it.
#
# blargg's readme.txt gives a failure code per subtest, which is what makes the
# queue ordered rather than a wall of red. Worth reading before starting any of
# it; 1-len_ctr alone enumerates seven distinct ways length counters go wrong.
#
# THE COMBINED apu_test.nes IS DELIBERATELY NOT FETCHED. It is 128KB of PRG on
# MMC1, which this emulator does not support - the same reason
# fetch_blargg_ppu.sh takes ppu_vbl_nmi's singles. The eight singles are NROM
# (mapper 0), 32KB PRG + 8KB CHR, and need no mapper work.
#
# MEASURED with the frame counter and its IRQ implemented and NOTHING else -
# no length counters, no length table, no envelope, sweep or linear counter,
# no channels and no DMC:
#
#   1-len_ctr           2 @ frame 18  "Problem with length counter load or $4015"
#   2-len_table         1 @ frame 15  (fails without a subtest number)
#   3-irq_flag          0 @ frame 21  PASSED
#   4-jitter            0 @ frame 20  PASSED
#   5-len_timing        2 @ frame 19  "First length of mode 0 is too soon"
#   6-irq_flag_timing   0 @ frame 22  PASSED
#   7-dmc_basics        2 @ frame 20  "DMC isn't working well enough to test further"
#   8-dmc_rates         2 @ frame 17  "Rate 0's period is too short"
#
# THREE ALREADY PASS, and which three is the useful part: 3-irq_flag, 4-jitter
# and 6-irq_flag_timing are exactly the frame-counter and frame-IRQ ROMs. The
# existing implementation - including the write-parity detail in APU::write -
# is confirmed by an oracle it was never run against. The other five split
# cleanly by subsystem: three want length counters and $4015, two want the DMC.
#
# All eight COMPLETE rather than hang, which is what makes them usable at all.
# The frame numbers are a FLOOR, not a budget: a ROM that gets further runs
# longer, so they must be re-measured once any of these start passing.
#
# NOT COVERED BY THIS SUITE, so do not read "8/8" as "the APU is done": the
# envelope, the sweep unit, the triangle's linear counter, the frequency timers
# and the non-linear mixer all need their own oracles (test_apu_env,
# test_apu_sweep, test_tri_lin_ctr, test_apu_timers, apu_mixer).
#
# x0000's test_apu_2 was considered and left out: it is not in the
# christopherpow mirror and has no source that can be SHA256-pinned
# reproducibly. An unpinnable fixture is worse than none here.
#
# (c) Shay Green (blargg), from the nes-test-roms collection. Not committed:
# gitignored like every other fetched fixture.
#
# Usage: tests/test_files/fetch_apu_test.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/apu_test"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/apu_test/rom_singles"

mkdir -p "$DEST"

ROMS="1-len_ctr aacf86c1d773badd11392e54506a43a06a3dd0b67a4c255909d1daf770a4a1e2
2-len_table c002ff1483b4dfb36a6eb004d49739cd58a2dff16e0bac167d5a7c12235caeeb
3-irq_flag dd888551665937391a2d691b1f96d1858316dbfce4951306146e9e396367f079
4-jitter bff573d72d0f134fe307f0bb8b968b8d2ffdb85e8aadad9c152839068d6db32a
5-len_timing 4d88f8cc0b21303dc151af4d0f4169d79284634a73082d7ea1ae5cfafedd1e46
6-irq_flag_timing fc1daff82dd1a49c7c1242392ffbf1c6f44fb70156868582117f2a844cc4dffd
7-dmc_basics 547324867ee0ba2aa11401001d8d1288530aa4e0ecaaac1667ce79980a388ec1
8-dmc_rates 5d9a79a505b37fa277cacc95a362f7e2a56e59ace7a698213d78432cc06a8867"

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
if [ "$count" -ne 8 ]; then
    echo "expected 8 ROMs in $DEST, found $count" >&2
    exit 1
fi

echo "done: $count ROMs in $DEST"
