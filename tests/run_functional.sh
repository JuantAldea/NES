#!/bin/sh
# Full functional pass over the project: everything a green unit-test run does
# NOT prove.
#
# WHY THIS EXISTS. Driving these checks by hand costs about fifteen separate
# commands and pages of output to read, most of it noise on a good day. This
# prints one line per check, detail only on failure, and exits with the number
# of failures.
#
# WHAT IT COVERS THAT `ctest` DOES NOT:
#
#   build     a clean tree configures and builds from nothing - catches a header
#             that only compiles because an old object file is lying around
#   nosdl     the suite builds AND LINKS with no SDL present. CLAUDE.md requires
#             it; the check is `ldd | grep sdl`, not just "it compiled"
#   fetch     every fetch script works from a COLD cache and is idempotent.
#             Locally the ROMs are always already there, so this is the one
#             class of failure a developer machine structurally cannot see
#   cli       ./build/NES reports failure with a non-zero EXIT CODE, not just a
#             message a wrapper script cannot see
#   hygiene   no ROM or build output tracked by git, imgui.ini ignored
#   gui       the frontend opens a window and renders one cartridge per mapper
#
# THE GUI SECTION IS A SMOKE TEST, NOT AN ORACLE. It proves the window opens,
# the process survives, and stderr is empty. Whether the PICTURE is right is not
# decidable here - screenshots are written out and judging them is a separate
# job. Saying otherwise would be the false green this repo exists to avoid.
#
# ---------------------------------------------------------------------------
# PARALLELISM, and the two measurements that shaped it.
#
# The independent work is the four TREES - a clean build, a no-SDL build, a
# sanitizer build, and scan-build - plus the fetches, which are network-bound
# and share no state with any of them. Those all start at once. Everything that
# consumes a build waits for its own tree and nothing else.
#
# The sanitizer suite is run SHARDED, through run_tests.sh, not as one process.
# Measured: 297s as a single process against 57s sharded, a 5.2x difference
# that is entirely "one core versus all of them". A single-process sanitizer run
# looks like a hung machine - almost no CPU, for five minutes - which is how
# this was noticed.
#
# The ordinary suite is NOT worth parallelising further: 41s of CPU across 907
# cases, but the slowest single case is 3.5s, and that is the floor no amount of
# sharding beats.
#
#   tests/run_functional.sh              everything except the sanitizers
#   tests/run_functional.sh --full       including the sanitizer suite
#   tests/run_functional.sh --no-gui     skip the frontend (headless machines)
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${NES_FUNCTIONAL_OUT:-${TMPDIR:-/tmp}/nes-functional}

WITH_SANITIZERS=0
WITH_GUI=1
WITH_TREES=1 # the clean build, the no-SDL build, the cold fetch and the analyzer
for arg in "$@"; do
    case "$arg" in
    --full) WITH_SANITIZERS=1 ;;
    --quick) WITH_TREES=0 ;;
    --no-gui) WITH_GUI=0 ;;
    *)
        echo "usage: tests/run_functional.sh [--quick|--full] [--no-gui]" >&2
        exit 2
        ;;
    esac
done

rm -rf "$OUT"
mkdir -p "$OUT"

FAILED=0
SKIPPED=0
WALL_START=$(date +%s)

# Milliseconds. Timing exists to answer one question: is a slow pass slow
# because the machine is working, or because it is waiting on the network? A
# step that takes 90s at 3% CPU and one that takes 90s at 3000% look identical
# in a total.
now_ms() { date +%s%3N; }
secs() { awk -v ms="$1" 'BEGIN { printf "%6.1fs", ms / 1000 }'; }

# $4, when given, is the step's duration in ms.
report() {
    dur=""
    [ -n "${4:-}" ] && dur="[$(secs "$4")]"
    case "$1" in
    PASS) printf '  \033[32mPASS\033[0m %9s  %-34s %s\n' "$dur" "$2" "${3:-}" ;;
    SKIP)
        printf '  \033[33mSKIP\033[0m %9s  %-34s %s\n' "$dur" "$2" "${3:-}"
        SKIPPED=$((SKIPPED + 1))
        ;;
    FAIL)
        printf '  \033[31mFAIL\033[0m %9s  %-34s %s\n' "$dur" "$2" "${3:-}"
        FAILED=$((FAILED + 1))
        ;;
    esac
}

# Times a serial check and leaves the result in $STEP_MS.
STEP_MS=0
timed() {
    _t0=$(now_ms)
    "$@"
    _rc=$?
    STEP_MS=$(( $(now_ms) - _t0 ))
    return $_rc
}

# Starts a named background job. Its exit status lands in $OUT/<name>.rc and its
# output in $OUT/<name>.log, so results are collected in a fixed order later and
# the printed table stays deterministic however the jobs interleave.
job() {
    name=$1
    shift
    (
        _j0=$(now_ms)
        "$@" >"$OUT/$name.log" 2>&1
        _rc=$?
        echo $(( $(now_ms) - _j0 )) >"$OUT/$name.ms"
        echo $_rc >"$OUT/$name.rc"
    ) &
}
rc_of() { cat "$OUT/$1.rc" 2>/dev/null || echo 99; }
ms_of() { cat "$OUT/$1.ms" 2>/dev/null || echo 0; }

build_tree() { cmake -S "$ROOT" -B "$1" -G Ninja "$3" >/dev/null 2>&1 && cmake --build "$1" && [ -x "$1/$2" ]; }

# Runs every fetch script IN A SANDBOX, leaving tests/test_files untouched.
#
# Each script derives its destination from its own location, so copying the
# scripts into a scratch directory and running them there fetches into the
# scratch directory. The real fixtures are never moved, deleted or risked.
#
# That is the second design. The first deleted the real directories to force a
# cold fetch, and it cost exactly what it looks like it would: a run killed
# part-way left tests/test_files/sprite_hit holding 7 of its 11 ROMs, and the
# next suite run reported four failures in an emulator that was working
# perfectly. A backup-and-restore trap was the third design and still had a
# window. Not touching the tree has no window.
fetch_all() {
    sandbox="$OUT/fetch-sandbox"
    rm -rf "$sandbox"
    mkdir -p "$sandbox"
    cp "$ROOT"/tests/test_files/fetch_*.sh "$sandbox/"

    status=0
    for script in "$sandbox"/fetch_*.sh; do
        case "$(basename "$script")" in
        fetch_single_step_tests.sh) continue ;; # 1.1 GB, deliberately not pulled
        esac

        # One retry: these hosts really do produce "TLS connect error:
        # unexpected eof". A retry that SUCCEEDS is still reported, so the
        # flakiness stays visible rather than being smoothed away.
        if ! "$script"; then
            echo "RETRY $(basename "$script")"
            "$script" || {
                echo "COLD-FETCH-FAILED $(basename "$script")"
                status=1
                continue
            }
        fi
        "$script" | grep -q "fetching" && {
            echo "NOT-IDEMPOTENT $(basename "$script")"
            status=1
        }
    done
    rm -rf "$sandbox"
    return $status
}

# The suite runs against the repository's OWN build directory, so it never
# depended on the clean-tree build and can start immediately.
BUILD=${NES_BUILD_DIR:-$ROOT/build}
[ -x "$BUILD/tests/tests" ] || BUILD="$OUT/fresh"

# BUILDS FIRST, and that is not a convenience.
#
# This ran the binary already sitting in build/ without rebuilding it. Given an
# edited-but-not-compiled tree it reported "907 executed, 0 skipped, 0 failed"
# against a deliberately broken test - measured, not theoretical. A tool whose
# whole job is "are my changes good" was reporting on somebody else's changes,
# which is the exact false green this repository exists to prevent.
#
# The incremental build is ~1s against a ~9s suite, so there was never a cost
# worth trading for it.
suite_job() {
    if ! cmake --build "$BUILD" >"$OUT/suite-build.log" 2>&1; then
        echo "BUILD FAILED - see $OUT/suite-build.log"
        return 1
    fi
    NES_TEST_BIN="$BUILD/tests/tests" "$ROOT/tests/run_tests.sh" 2>/dev/null | grep "executed" | tail -1
}

# Build AND run chained inside ONE job, so the suite starts the instant its
# build is ready instead of at a barrier. This chain is the critical path: it
# was 105s of a 106s wall, every other job finishing inside it.
#
# THE TREE IS PERSISTENT AND INCREMENTAL, which is where the time went. Building
# a fresh sanitizer tree into $OUT cost ~47s of the chain on every run, and
# bought nothing: clean-tree integrity is what the `fresh` job checks, and the
# sanitizer build has no reason to be clean. scan-build is the opposite case and
# genuinely must be wiped - it only analyses what it compiles, so an up-to-date
# tree would analyse nothing and still report success. ASan has no such
# property; a stale object file cannot hide a sanitizer finding, it just is not
# rebuilt.
#
# build-asan is also the tree CLAUDE.md documents, so this shares the developer's
# existing one rather than keeping a second copy warm.
ASAN_TREE=${NES_ASAN_DIR:-$ROOT/build-asan}
asan_job() {
    cmake -S "$ROOT" -B "$ASAN_TREE" -G Ninja -DNES_BUILD_FRONTEND=OFF \
        -DNES_SANITIZE=address,undefined >/dev/null 2>&1 || return 1
    cmake --build "$ASAN_TREE" >/dev/null 2>&1 || return 1
    NES_TEST_BIN="$ASAN_TREE/tests/tests" \
        ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1 \
        UBSAN_OPTIONS=print_stacktrace=1:report_error_type=1 \
        "$ROOT/tests/run_tests.sh" 2>/dev/null | grep "executed" | tail -1
}

# EVERY unit of work is a job. There is still one `wait`, but it no longer
# blocks anything - it only delays PRINTING. Before this the test suite sat idle
# from 41s to 56.4s waiting for an analyzer it does not depend on, and the
# sanitizer suite waited from 46.7s for the same reason.
printf 'launching every independent job at once...\n'

job suite suite_job
if [ "$WITH_TREES" -eq 1 ]; then
    job fresh build_tree "$OUT/fresh" tests/tests -DCMAKE_BUILD_TYPE=Checked
    job nosdl build_tree "$OUT/nosdl" tests/tests -DNES_BUILD_FRONTEND=OFF
    job fetch fetch_all
    command -v scan-build >/dev/null 2>&1 && job analyze "$ROOT/tests/run_scan_build.sh"
fi
[ "$WITH_SANITIZERS" -eq 1 ] && job asan asan_job

wait

section() { printf '\n%s\n' "$1"; }

section "build integrity"
if [ "$WITH_TREES" -eq 0 ]; then
    report SKIP "clean tree configures and builds" "--quick"
    report SKIP "suite builds and links without SDL" "--quick"
elif [ "$(rc_of fresh)" -eq 0 ]; then
    report PASS "clean tree configures and builds" "" "$(ms_of fresh)"
else
    report FAIL "clean tree configures and builds" "see $OUT/fresh.log" "$(ms_of fresh)"
fi

if [ "$WITH_TREES" -eq 1 ]; then

    if [ "$(rc_of nosdl)" -eq 0 ]; then
        sdl=$(ldd "$OUT/nosdl/tests/tests" 2>/dev/null | grep -ci sdl)
        [ "$sdl" -eq 0 ] && report PASS "suite builds and links without SDL" "" "$(ms_of nosdl)" ||
            report FAIL "suite builds and links without SDL" "$sdl SDL libs linked" "$(ms_of nosdl)"
    else
        report FAIL "suite builds without SDL" "see $OUT/nosdl.log" "$(ms_of nosdl)"
    fi
fi

section "fixtures"
if [ "$WITH_TREES" -eq 0 ]; then
    report SKIP "fetch scripts (NETWORK-BOUND)" "--quick"
elif [ "$(rc_of fetch)" -eq 0 ]; then
    # `grep -c` prints 0 AND exits 1 when it matches nothing, so `|| echo 0`
    # yields TWO lines and every later numeric test on it misbehaves. Count the
    # lines instead; there is no failure path to paper over.
    retried=$(grep RETRY "$OUT/fetch.log" 2>/dev/null | wc -l)
    if [ "$retried" -eq 0 ]; then
        report PASS "fetch scripts (NETWORK-BOUND)" "" "$(ms_of fetch)"
    else
        report PASS "fetch scripts (NETWORK-BOUND)" "$retried retried - flaky host" "$(ms_of fetch)"
    fi
else
    report FAIL "fetch scripts (NETWORK-BOUND)" "see $OUT/fetch.log" "$(ms_of fetch)"
fi

section "verification"
counts=$(grep -E "executed|BUILD FAILED" "$OUT/suite.log" 2>/dev/null | tail -1)
case "$counts" in
*"0 failed"*) report PASS "test suite (built first)" "$counts" "$(ms_of suite)" ;;
*) report FAIL "test suite (built first)" "${counts:-no result}" "$(ms_of suite)" ;;
esac

timed env NES_BUILD_DIR="$BUILD" "$ROOT/tests/test_counts.sh" --check >"$OUT/counts.log" 2>&1 &&
    report PASS "test-count table matches" "" "$STEP_MS" ||
    report FAIL "test-count table matches" "run tests/test_counts.sh --check" "$STEP_MS"

if [ "$WITH_TREES" -eq 0 ]; then
    report SKIP "static analyzer" "--quick"
elif [ -f "$OUT/analyze.rc" ]; then
    [ "$(rc_of analyze)" -eq 0 ] && report PASS "static analyzer" "no bugs found" "$(ms_of analyze)" ||
        report FAIL "static analyzer" "see $OUT/analyze.log" "$(ms_of analyze)"
else
    report SKIP "static analyzer" "scan-build not installed"
fi

if [ "$WITH_SANITIZERS" -eq 1 ]; then
    san=$(grep "executed" "$OUT/asan.log" 2>/dev/null | tail -1)
    case "$san" in
    *"0 failed"*) report PASS "ASan + UBSan (build + suite)" "$san" "$(ms_of asan)" ;;
    *) report FAIL "ASan + UBSan (build + suite)" "${san:-see $OUT/asan.log}" "$(ms_of asan)" ;;
    esac
else
    report SKIP "ASan + UBSan" "pass --full to include"
fi

section "command line"
check_exit() {
    desc=$1
    want=$2
    shift 2
    timeout 60 "$@" >"$OUT/cli.txt" 2>&1
    got=$?
    [ "$got" -eq "$want" ] && report PASS "$desc" "exit=$got" ||
        report FAIL "$desc" "exit=$got, wanted $want"
}
check_exit "CLI rejects no arguments" 1 "$BUILD/NES"
check_exit "CLI rejects a missing file" 1 "$BUILD/NES" /nonexistent/nope.nes
check_exit "CLI rejects a non-ROM" 1 "$BUILD/NES" "$ROOT/README.md"

section "repository hygiene"
tracked=$(cd "$ROOT" && git ls-files | grep -cE '\.nes$|^build')
[ "$tracked" -eq 0 ] && report PASS "no ROMs or build output tracked" ||
    report FAIL "no ROMs or build output tracked" "$tracked files"
(cd "$ROOT" && git check-ignore -q imgui.ini) && report PASS "imgui.ini is ignored" ||
    report FAIL "imgui.ini is ignored"

section "frontend"
# Serial by necessity: every instance opens a window called NES, so two at once
# cannot be told apart by xdotool.
if [ "$WITH_GUI" -eq 0 ]; then
    report SKIP "frontend smoke test" "--no-gui"
elif [ ! -x "$BUILD/nes_frontend" ]; then
    report SKIP "frontend smoke test" "not built"
elif [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    report SKIP "frontend smoke test" "no display"
elif ! command -v xdotool >/dev/null 2>&1 || ! command -v import >/dev/null 2>&1; then
    report SKIP "frontend smoke test" "needs xdotool and ImageMagick"
else
    for entry in \
        "mapper0:$ROOT/tests/test_files/local/smb.nes" \
        "mapper2:$ROOT/tests/test_files/visual/240pee.nes" \
        "mapper3:$ROOT/tests/test_files/ppu_read_buffer/test_ppu_read_buffer.nes" \
        "mapper4:$ROOT/tests/test_files/mmc3/4-scanline_timing.nes"; do
        tag=${entry%%:*}
        rom=${entry#*:}
        [ -f "$rom" ] || {
            report SKIP "frontend $tag" "no ROM"
            continue
        }

        SDL_VIDEODRIVER=x11 "$BUILD/nes_frontend" "$rom" >"$OUT/$tag.err" 2>&1 &
        pid=$!
        wid=""
        i=0
        while [ $i -lt 20 ]; do
            wid=$(xdotool search --name '^NES$' 2>/dev/null | head -1)
            [ -n "$wid" ] && break
            i=$((i + 1))
            sleep 0.5
        done
        sleep 3

        if [ -z "$wid" ]; then
            report FAIL "frontend $tag" "no window appeared"
        elif ! kill -0 "$pid" 2>/dev/null; then
            report FAIL "frontend $tag" "process died: $(head -1 "$OUT/$tag.err")"
        else
            import -window "$wid" "$OUT/$tag.png" 2>/dev/null
            report PASS "frontend $tag" "window up, screenshot captured"
        fi
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    done
    echo "        screenshots in $OUT - the PICTURE still needs looking at"
fi

wall=$(( $(date +%s) - WALL_START ))
printf '\n%s failed, %s skipped. Wall %ss. Logs: %s\n' "$FAILED" "$SKIPPED" "$wall" "$OUT"

# The parallel phase is bounded by its SLOWEST job, not by their sum, so a
# breakdown that added up would be misleading. What matters is which job is the
# long pole and whether it is working or waiting.
printf '\nparallel phase, per job:\n'
for j in suite fresh nosdl fetch analyze asan; do
    [ -f "$OUT/$j.ms" ] || continue
    printf '  %-12s %s\n' "$j" "$(secs "$(ms_of "$j")")"
done
printf '  wall is the LONGEST of these, not their sum - they overlap.\n'
printf '  fetch is network-bound: time there is idle CPU, not slow code.\n'
exit "$FAILED"
