#!/bin/sh
# Fetches the SingleStepTests 65x02 vectors used by tests/cpu_cycle_tests.cpp.
#
# These are an *independent* oracle: 10,000 randomized cases per opcode, each
# with the full before/after CPU state, the memory it touched, and the bus
# activity of every cycle. Checking our CPU against them proves rather more than
# checking "SLO does ASL then ORA" against an assertion that SLO does ASL then
# ORA.
#
# Only the opcodes the suite actually exercises are fetched (~5 MB each, ~360 MB
# total) rather than cloning the whole ~1.4 GB repository. Everything lands in
# tests/test_files/single_step_tests/ and is gitignored.
#
# The upstream files are not checksum-pinned the way the nestest assets are:
# there are 90 of them and upstream regenerates them wholesale on occasion. The
# tests validate the shape of what they parse instead, so a truncated or
# malformed download fails as a parse error rather than as a silent pass.
#
# Usage: tests/test_files/fetch_single_step_tests.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/single_step_tests"
BASE="https://raw.githubusercontent.com/SingleStepTests/65x02/main/nes6502/v1"

# The opcodes under test: every illegal opcode implemented in src/cpu.cpp, all
# eight conditional branches, the stack/flow instructions whose flag handling
# changed, and the indexed read/store/read-modify-write forms that the page
# cross ("oops") cycle classification hinges on.
OPCODES="03 07 0f 13 17 1b 1f 23 27 2f 33 37 3b 3f 43 47 4f 53 57 5b 5f
63 67 6f 73 77 7b 7f 83 87 8f 97 a3 a7 af b3 b7 bf c3 c7 cf d3 d7 db df
e3 e7 ef f3 f7 fb ff 0b 2b 4b 10 30 50 70 90 b0 d0 f0 40 28 08 68 48
bd b9 b1 9d 99 91 1e 3e 5e 7e de fe bc be a9 69 e9 c9 2a 6a 4a 0a"

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

for op in $OPCODES; do
    if [ -s "$DEST/$op.json" ]; then
        continue
    fi

    if ! curl -fsSL --retry 3 --max-time 300 -o "$DEST/$op.json.tmp" "$BASE/$op.json"; then
        rm -f "$DEST/$op.json.tmp"
        echo "failed to fetch $op.json from $BASE" >&2
        exit 1
    fi

    mv "$DEST/$op.json.tmp" "$DEST/$op.json"
    echo "  ok: $op.json"
done

echo "done: $(ls "$DEST" | wc -l) files in $DEST"
