#!/bin/sh
# Mutation testing: change the code mechanically, and report what the tests do
# not notice.
#
# WHY THIS EXISTS, and it is not "more coverage". Three adversarial reviews of
# the APU found, between them, about 29 single-token changes that the whole
# suite accepted - including one real bug, an inverted CPU/APU clock phase. The
# author had run a mutation sweep before each of those reviews and killed almost
# everything in it. The difference was never diligence:
#
#   A MUTATION SET CHOSEN BY THE PERSON WHO WROTE THE TESTS IS AIMED WHERE THE
#   TESTS ALREADY POINT.
#
# Writing the set before the tests was tried, in APU phase 2, and did not help -
# it was still the same aim. What works is generating the mutants MECHANICALLY,
# from the text, with no judgement about which ones are interesting. This script
# is that, and its whole value is that it does not know what the code means.
#
# Usage:
#   tests/run_mutants.sh -f 'testAPUMixer.*' src/apu.cpp
#   tests/run_mutants.sh -f 'testAPU*' --since HEAD~1 src/apu.cpp include/apu.h
#   tests/run_mutants.sh -f 'testCPU.*' -n 40 -l 100-260 src/cpu.cpp
#
#   -f, --filter <glob>   gtest filter the mutants must fail. Required: running
#                         the whole suite per mutant is minutes each.
#   -n, --max <N>         stop after N mutants.
#   -l, --lines <A-B>     only mutate this line range.
#       --since <rev>     only mutate lines this working tree changed since
#                         <rev>. This is the one to use after writing a phase:
#                         it attacks what you just wrote and nothing else.
#       --list            print the mutants and exit without building anything.
#
# SURVIVORS NEED TRIAGE, NOT PANIC. A survivor is one of three things, and only
# the first is a defect:
#
#   a hole          the behaviour is real and nothing checks it
#   equivalent      the mutation cannot change behaviour. Two real examples from
#                   this codebase: removing the mixer's divide-by-zero guards,
#                   because IEEE 754 already gives 0 there; and reordering the
#                   linear counter's reload and decrement, because the reload
#                   overwrites the decrement
#   unreachable     the line cannot execute in any state the tests can construct
#
# Deciding which is a human's job. Recording the answer next to the code is what
# stops the next person re-deriving it.
#
# WHAT IT DOES NOT MUTATE, so nobody reads a clean run as more than it is:
# comments - whole-line AND trailing, the latter only since the bus.cpp run that
# reported `// wrong phase - wait for the right one` becoming `+ wait` as a
# survivor - any line containing a string literal (which would only change a
# failure message), and anything the operator list below does not cover -
# notably it cannot swap two arguments at a call site or reorder statements.
# Both of those have found real defects here and neither is expressible as a
# line-local substitution. A clean run means "these operators found nothing",
# not "the tests are complete".
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${NES_BUILD_DIR:-$ROOT/build}
BIN=$BUILD/tests/tests
OUT=${TMPDIR:-/tmp}/nes-mutants
FILTER=""
MAX=0
LINES=""
SINCE=""
LIST_ONLY=0
FILES=""

while [ $# -gt 0 ]; do
    case "$1" in
    -f | --filter)
        FILTER=$2
        shift 2
        ;;
    -n | --max)
        MAX=$2
        shift 2
        ;;
    -l | --lines)
        LINES=$2
        shift 2
        ;;
    --since)
        SINCE=$2
        shift 2
        ;;
    --list)
        LIST_ONLY=1
        shift
        ;;
    -*)
        printf 'unknown option: %s\n' "$1" >&2
        exit 2
        ;;
    *)
        FILES="$FILES $1"
        shift
        ;;
    esac
done

[ -n "$FILES" ] || {
    printf 'usage: tests/run_mutants.sh -f <gtest-filter> <source-file>...\n' >&2
    exit 2
}
[ -n "$FILTER" ] || {
    printf 'a --filter is required: the whole suite per mutant is minutes each\n' >&2
    exit 2
}

rm -rf "$OUT"
mkdir -p "$OUT/orig"

# Restore on ANY exit, including a Ctrl-C mid-build. A run that died leaving a
# mutated source behind would be worse than not running it: the next build would
# be silently wrong and nothing would say so.
restore_all() {
    for f in $FILES; do
        [ -f "$OUT/orig/$(printf '%s' "$f" | tr / _)" ] &&
            cp "$OUT/orig/$(printf '%s' "$f" | tr / _)" "$ROOT/$f"
    done
    return 0
}
trap 'restore_all' EXIT INT TERM

# A PREVIOUS RUN MAY HAVE DIED WITHOUT RESTORING. The trap below handles every
# signal it can, and SIGKILL is not one of them - an OOM kill, or the harness
# being killed from outside, leaves whatever mutant was in flight sitting in the
# working tree. The next build is then silently wrong and nothing says so.
#
# This is the only thing that can catch that after the fact, and it is cheap:
# git already knows what these files should look like. Refusing is deliberate -
# a mutated file and a legitimately edited one are indistinguishable from here,
# so this cannot safely restore, only stop.
if git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    for f in $FILES; do
        if ! git -C "$ROOT" diff --quiet -- "$f" 2>/dev/null; then
            printf '%s differs from HEAD.\n' "$f" >&2
            printf 'If that is your own work, commit or stash it first. If a previous run of\n' >&2
            printf 'this script was killed - SIGKILL cannot be trapped - it may be a leftover\n' >&2
            printf 'mutant, and building on it would be silently wrong.\n' >&2
            exit 2
        fi
    done
fi

for f in $FILES; do
    [ -f "$ROOT/$f" ] || {
        printf 'no such file: %s\n' "$f" >&2
        exit 2
    }
    cp "$ROOT/$f" "$OUT/orig/$(printf '%s' "$f" | tr / _)"
done

# --- which lines are eligible ------------------------------------------------

eligible_lines() {
    # $1 = file. Prints the line numbers this run is allowed to mutate.
    if [ -n "$SINCE" ]; then
        # Lines the working tree added or changed relative to <rev>. The +N,M
        # hunk headers give the new-file ranges.
        git -C "$ROOT" diff -U0 "$SINCE" -- "$1" |
            awk '/^@@/ { split($3, a, ","); s = substr(a[1], 2); n = (a[2] == "" ? 1 : a[2]);
                         for (i = 0; i < n; i++) print s + i }'
    elif [ -n "$LINES" ]; then
        awk -v r="$LINES" 'BEGIN { split(r, a, "-"); for (i = a[1]; i <= a[2]; i++) print i }'
    else
        awk 'END { for (i = 1; i <= NR; i++) print i }' "$ROOT/$1"
    fi
}

# --- generating the mutants --------------------------------------------------
#
# Delegated to tests/mutants_generate.py, which emits TSV of
# file, line, payload-path, label - the payload being the whole replacement
# line, so no sed escaping is involved anywhere.
#
# Python rather than more awk because the two most productive mutation classes
# here need balanced-paren parsing: dropping one clause from a compound
# condition, and swapping two arguments at a call site. Both have found real
# defects in this project and neither is a regex.
generate() {
    file=$1
    eligible_lines "$file" >"$OUT/eligible"
    python3 "$ROOT/tests/mutants_generate.py" "$ROOT/$file" "$OUT/eligible" "$OUT" |
        sed "s|^$ROOT/||"
}

: >"$OUT/mutants.tsv"
for f in $FILES; do
    generate "$f" >>"$OUT/mutants.tsv"
done

TOTAL=$(wc -l <"$OUT/mutants.tsv" | tr -d ' ')
if [ "$LIST_ONLY" -eq 1 ]; then
    awk -F'\t' '{ printf "%s:%s  %s\n", $1, $2, $4 }' "$OUT/mutants.tsv"
    printf '\n%s mutants\n' "$TOTAL"
    exit 0
fi

# --- the baseline, which is the part most worth getting right ----------------
#
# This repository has two KNOWN failing tests - the parked DMC/OAM collision
# rows. A harness that called a mutant "killed" whenever the suite reported a
# failure would score every single one as killed, and report a perfect run while
# testing nothing. So the baseline's failing test NAMES are recorded, and a
# mutant counts as killed only when a test fails that was not already failing.
run_filter() {
    # Prints the names of failing tests, one per line. printf, never echo: gtest
    # prints \0 escapes in some assertion messages and a shell echo will chew
    # them, which has already corrupted one analysis in this project.
    #
    # THE EXIT STATUS IS RECORDED SEPARATELY, and that is not tidiness. A mutant
    # that CRASHES the binary prints no "[  FAILED  ]" line for the test that
    # died, so this function returns an empty list - indistinguishable from a
    # clean run. And the status cannot be read from the pipeline, because a
    # pipeline's status is its LAST command's, which here is sort.
    #
    # Measured: `storage(0.0f, max + 1)` on SampleRing builds an empty vector, so
    # `% storage.size()` divides by zero. The binary dies with SIGFPE, exit 136,
    # on the third test - and this harness scored it SURVIVED. The most severe
    # mutants there are, the ones that crash rather than merely compute wrong
    # answers, were the ones it could not see.
    #
    # Wrapped in `if`, because `set -e` is on and a killing mutant makes the
    # binary exit 1 by design. Taking it out of the pipeline removed the very
    # thing that had been masking its status, which then aborted the whole run
    # on the first mutant that worked.
    if "$BIN" --gtest_filter="$FILTER" >"$OUT/raw" 2>&1; then
        printf '0\n' >"$OUT/status"
    else
        printf '%s\n' "$?" >"$OUT/status"
    fi
    tr -d '\000' <"$OUT/raw" |
        awk '/^\[  FAILED  \] [A-Za-z]/ && !/ms\)$/ { print $3 }' | sed 's/,$//' | sort -u
}

# gtest exits 0 when everything passed and 1 when something failed. Anything
# else - 134 for abort, 136 for SIGFPE, 139 for SIGSEGV - is the binary dying,
# which no amount of parsing its output will reveal.
#
# WHAT THE OLD BLINDNESS COST, checked rather than assumed, because every number
# this harness had produced up to that point came out of it. Only LIVE can become
# KILL under the fix, so re-scoring the RECORDED SURVIVORS is sufficient and the
# kills need no re-run:
#
#   blip.cpp     8 survivors  all still survivors
#   audio.cpp   13 survivors  all still survivors
#   apu.cpp      the two rate-table index masks, both killed either way
#   apu.h        kPulseDuty and kTriangleSequence generate NO mutants - pure
#                variable indexing, nothing the operator set touches - so the
#                out-of-bounds read that would crash cannot be expressed
#
# So the blindness cost exactly the two mutants it was found by, both in
# audio.cpp, and no conclusion recorded anywhere else was wrong. That is a
# negative result and is written down because the alternative is leaving the
# question open in every comment the old harness contributed to.
#
# Note the sweeps run against the Checked build, not the ASan one. An
# out-of-bounds read that stays inside .rodata returns garbage rather than
# faulting, so it is caught here only if some test notices the value. Running
# mutants under ASan would close that, at a cost nobody has measured yet.
crashed() {
    [ "$1" != "0" ] && [ "$1" != "1" ]
}

printf 'building baseline...\n'
cmake --build "$BUILD" >"$OUT/baseline-build.log" 2>&1 || {
    printf 'the baseline does not build; fix that before mutating it\n' >&2
    exit 1
}
run_filter >"$OUT/baseline-failures"
if crashed "$(cat "$OUT/status")"; then
    printf 'the baseline CRASHES (exit %s) - every mutant would be scored against\n' "$(cat "$OUT/status")" >&2
    printf 'a broken reference. Fix the tree first; see %s/raw\n' "$OUT" >&2
    exit 1
fi
BASE_FAILURES=$(wc -l <"$OUT/baseline-failures" | tr -d ' ')

# $2, not $1: the line is "[==========] 20 tests from 1 test suite ran." and
# $1 is the banner of equals signs.
EXECUTED=$("$BIN" --gtest_filter="$FILTER" 2>&1 | tr -d '\000' |
    awk '/tests? from .* ran/ { print $2; exit }')
[ -n "$EXECUTED" ] || EXECUTED=0
if [ "$EXECUTED" = "0" ]; then
    printf 'the filter %s matches no tests; nothing could kill anything\n' "$FILTER" >&2
    exit 1
fi

printf 'baseline: %s tests match %s, %s already failing\n' "$EXECUTED" "$FILTER" "$BASE_FAILURES"
printf 'mutating: %s candidates\n\n' "$TOTAL"

# --- the run -----------------------------------------------------------------

KILLED=0
SURVIVED=0
BROKEN=0
DONE=0
: >"$OUT/survivors"

while IFS="$(printf '\t')" read -r file line payload label; do
    [ "$MAX" -gt 0 ] && [ "$DONE" -ge "$MAX" ] && break
    DONE=$((DONE + 1))

    awk -v n="$line" -v p="$payload" 'NR == n { while ((getline l < p) > 0) print l; next } { print }' \
        "$OUT/orig/$(printf '%s' "$file" | tr / _)" >"$OUT/mutated" && cp "$OUT/mutated" "$ROOT/$file"

    if ! cmake --build "$BUILD" >"$OUT/build.log" 2>&1; then
        BROKEN=$((BROKEN + 1))
        printf '  ....  %s:%s  %s\n' "$file" "$line" "$label"
    else
        run_filter >"$OUT/failures"
        if crashed "$(cat "$OUT/status")"; then
            # Reported distinctly from an assertion failure: a mutant that takes
            # the process down is a different finding, and one worth looking at
            # even though it counts the same.
            KILLED=$((KILLED + 1))
            printf '  KILL  %s:%s  %s  (exit %s, crashed)\n' "$file" "$line" "$label" "$(cat "$OUT/status")"
        elif [ "$(comm -13 "$OUT/baseline-failures" "$OUT/failures" | wc -l | tr -d ' ')" -gt 0 ]; then
            KILLED=$((KILLED + 1))
            printf '  kill  %s:%s  %s\n' "$file" "$line" "$label"
        else
            SURVIVED=$((SURVIVED + 1))
            printf '  LIVE  %s:%s  %s\n' "$file" "$line" "$label"
            printf '%s:%s  %s\n' "$file" "$line" "$label" >>"$OUT/survivors"
        fi
    fi

    restore_all
done <"$OUT/mutants.tsv"

cmake --build "$BUILD" >/dev/null 2>&1 || true

printf '\n%s killed, %s SURVIVED, %s did not compile (of %s run)\n' \
    "$KILLED" "$SURVIVED" "$BROKEN" "$DONE"

if [ "$SURVIVED" -gt 0 ]; then
    printf '\nsurvivors - each is a hole, an equivalent mutant, or unreachable:\n'
    sed 's/^/  /' "$OUT/survivors"
fi

printf '\nlogs: %s\n' "$OUT"

# The exit status is the survivor count, so this composes with the other
# scripts here. It is deliberately NOT an error: survivors are a question, and
# a build that failed to compile a nonsense mutant is not a finding at all.
exit "$SURVIVED"
