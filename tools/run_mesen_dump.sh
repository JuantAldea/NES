#!/bin/sh
# Builds and runs the Mesen2 reference tools against a test ROM.
#
# WHY A SCRIPT. Both tools link against Mesen rather than against us, so they
# need a -I pair, an LD_LIBRARY_PATH, and a working directory that is not the
# repo - four things that are easy to get subtly wrong and that fail in ways
# which look like results. Running from the wrong directory, for instance, gets
# a Mesen that loads no ROM and reports an empty screen, which reads exactly
# like "the ROM printed nothing".
#
# THE REVISION IS PINNED, and this checks it. The reference table in
# tests/test_files/fetch_dmc_dma.sh says "Mesen2 b9fa69d prints these sixteen
# numbers". A table taken from one revision and compared against another is not
# a measurement, so a mismatch is reported loudly rather than left to be
# discovered when the numbers disagree for reasons nobody can reconstruct.
#
# Mesen is NOT committed, for the same reason the test ROMs are not - it is a
# large third-party artifact and what belongs in the repo is the recipe:
#
#   git clone https://github.com/SourMesen/Mesen2.git
#   cd Mesen2 && git checkout b9fa69d
#   make core -j"$(nproc)"          # InteropDLL only; no .NET, no UI
#
# Usage:
#   tools/run_mesen_dump.sh [--screen] [ROM] [SECONDS]
#
#   default    tools/mesen_event_dump.cpp     - per-frame DMA event sweep
#              LOSSY: it samples frames on a wall clock and drops rows. Prefer
#              --cycles for anything being compared against our trace.
#   --screen   tools/mesen_reference_dump.cpp - the printed nametable screen
#   --cycles   tools/mesen_cycle_trace.cpp    - cycle-stepped CPU trace
#              tools/run_mesen_dump.sh --cycles ROM FROM_CYCLE [COUNT]
#
#   NES_MESEN_DIR=  points at another Mesen2 tree (default ../Mesen2)
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MESEN=${NES_MESEN_DIR:-$(dirname -- "$REPO")/Mesen2}
PINNED=b9fa69d

TOOL=mesen_event_dump
CYCLES=0
if [ "${1:-}" = "--screen" ]; then
    TOOL=mesen_reference_dump
    shift
elif [ "${1:-}" = "--cycles" ]; then
    # Cycle stepping, the one mode that loses nothing. Takes a START CYCLE and a
    # COUNT rather than a duration, because no duration is involved - see the
    # header of tools/mesen_cycle_trace.cpp for why the event dump's wall-clock
    # sampling had to be replaced rather than tuned.
    TOOL=mesen_cycle_trace
    CYCLES=1
    shift
fi

ROM=${1:-$REPO/tests/test_files/dmc_dma/sprdma_and_dmc_dma.nes}

# 20s, because the sweep is 16 rows and a SHORT TABLE LOOKS LIKE A COMPLETE ONE.
# A 10s run returned 13 rows with no indication that three were missing, and
# those three were compared against our full 16 as though they lined up. The
# tool now prints *** INCOMPLETE below 16; do not read a table that says so.
SECONDS_TO_RUN=${2:-20}

# --cycles reuses the same two positional slots for a start cycle and a count.
FROM_CYCLE=${2:-0}
TRACE_COUNT=${3:-64}

# Every precondition names what to do about it. A missing ROM here is NOT the
# same failure as a missing fetched ROM in the test suite - nothing in CI runs
# this - but the message still has to name the fetch script, because "no such
# file" sends people looking for a bug in the tool.
if [ ! -d "$MESEN" ]; then
    echo "no Mesen2 tree at $MESEN" >&2
    echo "clone it, or set NES_MESEN_DIR - see the header of this script" >&2
    exit 1
fi

SO=$MESEN/bin/linux-x64/Release/MesenCore.so
if [ ! -f "$SO" ]; then
    echo "no MesenCore.so at $SO" >&2
    echo "build it:  cd $MESEN && make core -j\"\$(nproc)\"" >&2
    exit 1
fi

if [ ! -f "$ROM" ]; then
    echo "no ROM at $ROM" >&2
    echo "run tests/test_files/fetch_dmc_dma.sh" >&2
    exit 1
fi

REV=$(git -C "$MESEN" rev-parse --short HEAD 2>/dev/null || echo unknown)
if [ "$REV" != "$PINNED" ]; then
    echo "WARNING: Mesen2 is at $REV, but the reference table in" >&2
    echo "         tests/test_files/fetch_dmc_dma.sh was taken at $PINNED." >&2
    echo "         Numbers from this run are not comparable to it." >&2
fi

OUT=${TMPDIR:-/tmp}/$TOOL
cc=${CXX:-g++}
"$cc" -O2 -std=c++17 -I"$MESEN" -I"$MESEN/Core" -o "$OUT" "$REPO/tools/$TOOL.cpp" -ldl

# cd into the Release directory: Mesen resolves its Dependencies relatively, and
# InitializeEmu is given a relative home directory too.
cd "$MESEN/bin/linux-x64/Release"
if [ "$CYCLES" = "1" ]; then
    # $2 is a start cycle here and $3 a count, not seconds.
    LD_LIBRARY_PATH=.:./Dependencies "$OUT" ./MesenCore.so "$ROM" "${FROM_CYCLE}" "${TRACE_COUNT}"
else
    LD_LIBRARY_PATH=.:./Dependencies "$OUT" ./MesenCore.so "$ROM" "$SECONDS_TO_RUN"
fi
