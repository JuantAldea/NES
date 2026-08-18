#!/bin/sh
# Fetches blargg's 2005 APU frame-counter suite - eleven ROMs that go deeper
# into the frame counter and length counters than apu_test does.
#
# WHY THIS SUITE AND NOT MORE OF apu_test: it is the only remaining oracle for
# what is already built. The envelope, sweep and triangle linear counter - the
# obvious next APU features - have NO test ROM anywhere, and that is not an
# accident of this mirror. blargg says so twice, in tests.txt:
#
#   "The tests do not test clocking of the envelope, sweep, or triangle's
#    linear counter."
#
# and in readme.txt, where he explains that he never characterised the hardware
# either: "Also not documented is the exact operation of the envelope, sweep,
# and triangle's linear counter when register writes occur close to clocking."
# apu_mixer and volume_tests do touch channel volume, but they verify by
# cancelling to silence and by comparing audio recordings - neither is
# CPU-checkable. So this suite is the last thing that can audit the frame
# counter, and the envelope will need a different kind of verification.
#
# All eleven are NROM (mapper 0), 16KB PRG, CHR-RAM (CHR size 0), horizontal
# mirroring - no mapper work needed.
#
# THEY REPORT ON SCREEN, NOT THROUGH $6000. These predate blargg's PRG-RAM
# protocol, so blargg_rom_harness.h cannot read them; tests/nametable_screen.h
# can, because the ROMs write their result into the nametable as ASCII-mapped
# tile indices. Note the codes are NOT the $6000 convention: here $01 means
# PASSED and everything else is a failure. Zero is not a pass.
#
# MEASURED with the frame counter, its /IRQ, the length counters and the
# power-on/RESET state implemented, and nothing else - no envelope, sweep,
# linear counter, channels or DMC:
#
#   01.len_ctr             $01 @ frame 24   PASSED
#   02.len_table           $01 @ frame 11   PASSED
#   03.irq_flag            $01 @ frame 19   PASSED
#   04.clock_jitter        $01 @ frame 15   PASSED
#   05.len_timing_mode0    $01 @ frame 21   PASSED
#   06.len_timing_mode1    $01 @ frame 23   PASSED
#   07.irq_flag_timing     $01 @ frame 17   PASSED
#   08.irq_timing          $01 @ frame 15   PASSED
#   09.reset_timing        $01 @ frame 12   PASSED
#   10.len_halt_timing     $03 @ frame 13   "Length should be clocked when
#                                            halted at 14915"
#   11.len_reload_timing   $04 @ frame 14   "Reload during length clock when
#                                            ctr = 0 should work normally"
#
# NINE OF ELEVEN PASS ON FIRST CONTACT, and that is the finding rather than a
# disappointment - this suite was fetched to audit work already done, and it
# largely confirms it against ROMs blargg states pass on real hardware.
#
# 09.reset_timing is the one worth naming: "After reset or power-up, APU acts as
# if $4017 were written with $00 from 9 to 12 clocks before first instruction
# begins." That is APU::power_on() and APU::reset(), checked by a suite that had
# no part in building them.
#
# THE TWO FAILURES ARE ONE SUBJECT: write-versus-clock ordering inside the
# length counter, at single-cycle resolution.
#
#   - 10 wants a halt-bit write landing ON the length clock to take effect
#     AFTER that clock, not before. APU::write sets lengths[].halt immediately.
#   - 11 wants a $4003-style reload landing ON the length clock to be ignored
#     when the counter is non-zero, but to work when it is zero. APU::write
#     reloads unconditionally.
#
# Both are in code that already exists, both are one cycle wide, and neither is
# reachable by apu_test - which is exactly why this suite was worth adding.
#
# The frame numbers are a FLOOR and will rise as ROMs get further.
#
# (c) Shay Green (blargg), from the nes-test-roms collection. Not committed:
# gitignored like every other fetched fixture.
#
# Usage: tests/test_files/fetch_blargg_apu_2005.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/blargg_apu_2005"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/blargg_apu_2005.07.30"

mkdir -p "$DEST"

ROMS="01.len_ctr e1e3a29ab5369ab84a6f5f2f426c64bde86b9a1d26a906739d43fbf624bb8829
02.len_table 63cc6a57fae3da5e30df9520d02b723c5b93789e8a6eef6793f876345b245b51
03.irq_flag 6f71c7e3de4b6c00da92c20a86c1c2095196a55dc201ee3286004ecf33a08c2f
04.clock_jitter 46fa69b26fe8c24dc1d0b5908f90ab0141972eeb607bd563d28f53d6f4543fe6
05.len_timing_mode0 606802d6849ccfcf74e907a8512c03a50d443752d1f616e62a242a1fa7eca0ff
06.len_timing_mode1 0f34e26d56ad235d8d6d63565ed4728fbd0b9b8590a4fd4048eaf64283b429d4
07.irq_flag_timing 851c9698941d51da34b4bfbc9644aa08ee41b39c6c814cc9b8412c204ea68085
08.irq_timing 0a20a2b9ca9a8e78d65b500b161294c889399f0ff048edd104b07b15255946ca
09.reset_timing bb04f8328a51abb2d17e6e5362b8375f3cbfbf0641733d068b22f23e7dc588e6
10.len_halt_timing cbdaa9a5cf9c19ba2360d3349a47922eec25a3e610374d963c422f2b67c57ac9
11.len_reload_timing 40e633285a4a8710780bfd80d346dee62406f4be161eb75615f469cd9e84e132"

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
if [ "$count" -ne 11 ]; then
    echo "expected 11 ROMs in $DEST, found $count" >&2
    exit 1
fi

echo "done: $count ROMs in $DEST"
