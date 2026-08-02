#!/bin/sh
# Fetches the SingleStepTests 65x02 vectors used by tests/cpu_cycle_tests.cpp.
#
# These are an *independent* oracle: 10,000 randomized cases per opcode, each
# with the full before/after CPU state, the memory it touched, and the bus
# activity of every cycle. Checking our CPU against them proves rather more than
# checking "SLO does ASL then ORA" against an assertion that SLO does ASL then
# ORA.
#
# All 256 opcodes are fetched (~4 MB each, ~1.1 GB total) by direct download
# rather than by cloning the repository. Everything lands in
# tests/test_files/single_step_tests/ and is gitignored.
#
# The full set is needed because the per-cycle bus trace is checked, not only
# the final CPU state. A cycle schedule is a property of the addressing mode and
# access class, and the trickiest sequences -- JSR, RTS, RTI, BRK, and the
# read-modify-write forms with their dummy write of the old value -- are exactly
# the ones the earlier 90-opcode subset omitted.
#
# The upstream files are not checksum-pinned the way the nestest assets are:
# there are 256 of them and upstream regenerates them wholesale on occasion. The
# tests validate the shape of what they parse instead, so a truncated or
# malformed download fails as a parse error rather than as a silent pass.
#
# Usage: tests/test_files/fetch_single_step_tests.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/single_step_tests"
BASE="https://raw.githubusercontent.com/SingleStepTests/65x02/main/nes6502/v1"

# Every opcode $00-$ff, including the twelve JAM/STP opcodes (upstream provides
# vectors for those too).
OPCODES=$(awk 'BEGIN { for (i = 0; i < 256; i++) printf "%02x\n", i }')

mkdir -p "$DEST"

missing=0
for op in $OPCODES; do
    [ -s "$DEST/$op.json" ] || missing=$((missing + 1))
done

if [ "$missing" -eq 0 ]; then
    echo "ok (cached): all vectors already present in $DEST"
    exit 0
fi

echo "fetching $missing opcode vector file(s) into $DEST ..."

failed=""
for op in $OPCODES; do
    if [ -s "$DEST/$op.json" ]; then
        continue
    fi

    # --retry-all-errors matters here: a mid-transfer TLS teardown is reported
    # as a non-HTTP error, which plain --retry will not retry, and one such
    # blip previously aborted the whole run 115 files in.
    if ! curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --max-time 300 \
              -o "$DEST/$op.json.tmp" "$BASE/$op.json"; then
        rm -f "$DEST/$op.json.tmp"
        echo "  FAILED: $op.json" >&2
        failed="$failed $op"
        continue
    fi

    mv "$DEST/$op.json.tmp" "$DEST/$op.json"
    echo "  ok: $op.json"
done

count=$(ls "$DEST" | wc -l | tr -d ' ')

# Fail loudly on an incomplete set. A partial fetch previously looked like a
# success and left the suite quietly checking a subset of the opcodes.
if [ -n "$failed" ]; then
    echo "incomplete:$failed could not be fetched ($count/256 present)" >&2
    echo "re-run this script to retry only the missing files" >&2
    exit 1
fi

if [ "$count" -ne 256 ]; then
    echo "incomplete: $count/256 files present in $DEST" >&2
    exit 1
fi

echo "done: $count files in $DEST"
