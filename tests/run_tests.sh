#!/bin/sh
# Runs the test binary sharded across N processes, and reports the honest
# executed/skipped/failed breakdown.
#
# WHY THIS EXISTS, given `ctest -j` already parallelises: ctest and this script
# solve different halves of the problem.
#
#   - ctest -j runs one process per test case and is the right tool for "did
#     the suite pass". But gtest_discover_tests hands each case to a separate
#     process, so gtest's own end-of-run summary never appears - and that
#     summary is where the SKIPPED count lives. ctest reports a skip as a pass
#     (see "Skipped is not passed" in CLAUDE.md), which is exactly the number
#     this repo must not lose.
#   - The binary run directly keeps gtest's output and the skip count, but is
#     one process: MEASURED 35.3s for all 872 tests on 32 cores.
#
# Sharding the binary gets both. GTEST_TOTAL_SHARDS/GTEST_SHARD_INDEX are
# gtest's own partitioning, applied AFTER --gtest_filter, so a filtered subset
# shards too. MEASURED on 32 cores: 35.3s -> 4.3s wall (8.2x), 872 tests, same
# result. That floor is the single slowest test, so more shards will not help.
#
# Process-parallel is safe here for the same reason `ctest -j` is: every test
# that writes a fixture writes a uniquely named one (see tests/CMakeLists.txt).
#
# Usage:
#   tests/run_tests.sh                             # everything, nproc shards
#   tests/run_tests.sh -j8                         # eight shards
#   tests/run_tests.sh --gtest_filter='testCPU.*'  # extra args go to the binary
#
# Override the binary with NES_TEST_BIN= for a non-default build tree
# (build-asan, say).
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${NES_TEST_BIN:-$DIR/build/tests/tests}

JOBS=$(nproc 2>/dev/null || echo 4)

# -j8 and -j 8 both, then stop: everything after belongs to the binary.
while [ $# -gt 0 ]; do
    case $1 in
        -j)  JOBS=$2; shift 2 ;;
        -j*) JOBS=${1#-j}; shift ;;
        *)   break ;;
    esac
done

if ! [ -x "$BIN" ]; then
    echo "no test binary at $BIN - build first, or set NES_TEST_BIN" >&2
    exit 1
fi

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT INT TERM

i=0
while [ "$i" -lt "$JOBS" ]; do
    GTEST_TOTAL_SHARDS=$JOBS GTEST_SHARD_INDEX=$i \
        "$BIN" "$@" > "$OUT/shard.$i" 2>&1 &
    i=$((i + 1))
done

# Not `wait -n`-style early exit: a shard that fails should not cut short the
# others, or the counts below would be a partial run reported as a whole one.
wait

# gtest's summary lines, verified against real runs of this binary:
#   [==========] N tests from M test suites ran.   <- N includes skips
#   [  PASSED  ] N tests.                          <- excludes skips
#   [  SKIPPED ] N tests, listed below:            <- absent when N is 0
#   [  FAILED  ] N tests, listed below:            <- absent when N is 0
# so executed = ran - skipped = passed + failed. The `listed below:` suffix is
# what separates the summary lines from the per-test ones, which repeat the
# same tag with a test name where the count would be.
#
# The count is taken as the first field AFTER the bracket tag is stripped, not
# as a fixed column: awk splits `[  PASSED  ]` on its internal spaces into
# three fields but `[==========]` into one, so a column number that is right
# for one tag is wrong for the other. The first version of this script had that
# bug and the MISCOUNT check below is what caught it.
sum() { sed 's/^\[[^]]*\]//' < "$1" | awk '{ s += $1 } END { print s + 0 }'; }

grep -h '^\[==========\].*ran\.'            "$OUT"/shard.* > "$OUT/ran"     || true
grep -h '^\[  PASSED  \]'                   "$OUT"/shard.* > "$OUT/passed"  || true
grep -h '^\[  SKIPPED \].*listed below:'    "$OUT"/shard.* > "$OUT/skipped" || true
grep -h '^\[  FAILED  \].*listed below:'    "$OUT"/shard.* > "$OUT/failed"  || true

RAN=$(sum "$OUT/ran")
PASSED=$(sum "$OUT/passed")
SKIPPED=$(sum "$OUT/skipped")
FAILED=$(sum "$OUT/failed")
EXECUTED=$((RAN - SKIPPED))

# gtest names each failed or skipped test TWICE - inline as it happens, then
# again in the end-of-run listing - so a naive grep doubles every count below.
# Only the inline copy carries a ` (N ms)` suffix; dropping those leaves exactly
# one line per test.
listing() { grep -h "^\[  $1 \] [A-Za-z]" "$OUT"/shard.* | grep -v '([0-9]* ms)$' || true; }

if [ "$FAILED" -gt 0 ]; then
    echo
    listing 'FAILED '
fi

if [ "$SKIPPED" -gt 0 ]; then
    echo
    echo "skipped, by suite:"
    listing 'SKIPPED' | sed 's/^\[[^]]*\] //; s/[.\/].*//' | sort | uniq -c | sort -rn
fi

echo
echo "$EXECUTED executed, $SKIPPED skipped, $FAILED failed  ($JOBS shards)"

# A skipped test exits 0, so the totals above are the only thing standing
# between a green run and an unverified one. If they stop adding up the parse
# has drifted from gtest's output format and every number here is suspect.
if [ "$((PASSED + FAILED + SKIPPED))" -ne "$RAN" ]; then
    echo "MISCOUNT: $PASSED passed + $FAILED failed + $SKIPPED skipped != $RAN ran" >&2
    exit 2
fi

[ "$FAILED" -eq 0 ]
