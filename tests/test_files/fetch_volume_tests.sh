#!/bin/sh
# Damian Yerrick's volume_tests - THE ONLY ORACLE THIS PROJECT'S AUDIO HAS.
#
# Everything else in the APU is verified against register-level ROMs or against
# cited documentation. Nothing in the suite could say whether the emulator's
# CHANNEL BALANCE - how loud the triangle is against the pulses, how loud the
# DMC is against the noise - resembles a real NES. That is what this measures,
# and it is the one thing seven adversarial reviews of the audio could not.
#
# WHY IT WORKS AS AN ORACLE when apu_mixer and volume_tests were both written
# off as "listening tests". The distinction is not the ROM, it is what ships
# beside it: volume_tests includes RECORDINGS, among them nes-001.ogg, made on
# a Nintendo Entertainment System (NTSC U/C) with a PowerPak. Real hardware.
# So the comparison is numeric after all - render the same ROM, measure the same
# quantity, compare. The ROM's own README prescribes the quantity:
#
#   "you can't just measure the maximum voltage; you have to measure the
#    difference between the high and low values"      (DC filters differ)
#   "you have to compare relative volumes, not absolute volumes"  (headroom)
#
# So: peak-to-peak per tone, normalised. Twelve tones, each near 1000 Hz:
# pulse 1 at four duties, pulses 1+2 at four duties, triangle, noise long,
# noise short, and the DMC at amplitude 30.
#
# ONLY THE ROM IS FETCHED HERE. The recordings are not, because the test does
# not need them: the twelve hardware levels derived from nes-001.ogg are
# committed as constants in tests/audio_tests.cpp, with their provenance. That
# keeps the test free of an Ogg decoder and of 600 KB of audio per suite, and
# the numbers are 12 floats.
#
# MEASURED, against nes-001.ogg, relative peak-to-peak normalised to tone 3:
#
#   source           max |dB|   rms dB
#   fceux 2.0.4          2.49     1.05
#   THIS EMULATOR        2.99     1.66
#   Nestopia 1.40        3.51     1.66
#   Nintendulator        4.01     1.88
#
# Mid-pack among three mature emulators, which is the result and also the
# calibration: those three span 1.05 to 1.88 dB rms measured identically, so the
# metric cannot resolve better than a couple of dB. A first reading of this
# emulator's triangle at -3.0 dB looked like a mixer bug until Nintendulator's
# was measured at -4.0.
#
# THE ROM NEEDS INPUT. Nothing plays until A is pressed on controller 1 - the
# README says so and a first render was seventeen seconds of silence.
#
# Not committed: redistributable but not ours. volume_tests is Copyright (c)
# 2009 Damian Yerrick under a zlib-style licence.
#
# Usage: tests/test_files/fetch_volume_tests.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/volume_tests"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master/volume_tests"

mkdir -p "$DEST"

name="volumes"
want="36c4628d400212adb61bc08f2f7b32c522e4c2d3ae949d35304ea4b12fed0df8"
dest="$DEST/$name.nes"

if [ -f "$dest" ]; then
    have=$(sha256sum "$dest" | cut -d' ' -f1)
    if [ "$have" = "$want" ]; then
        echo "ok (cached): $name.nes"
        exit 0
    fi
    echo "checksum mismatch on cached $name.nes, refetching" >&2
fi

if ! curl -fsSL --retry 5 --retry-all-errors -o "$dest.tmp" "$BASE/$name.nes"; then
    rm -f "$dest.tmp"
    echo "could not download $name.nes" >&2
    exit 1
fi

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

count=$(ls -1 "$DEST"/*.nes 2>/dev/null | wc -l | tr -d ' ')
if [ "$count" -ne 1 ]; then
    echo "incomplete: $count/1 files present in $DEST" >&2
    exit 1
fi

echo "done: $count file in $DEST"
