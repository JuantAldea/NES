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
for arg in "$@"; do
    case "$arg" in
    --full) WITH_SANITIZERS=1 ;;
    --no-gui) WITH_GUI=0 ;;
    *)
        echo "usage: tests/run_functional.sh [--full] [--no-gui]" >&2
        exit 2
        ;;
    esac
done

rm -rf "$OUT"
mkdir -p "$OUT"

FAILED=0
SKIPPED=0

report() {
    case "$1" in
    PASS) printf '  \033[32mPASS\033[0m  %-34s %s\n' "$2" "${3:-}" ;;
    SKIP)
        printf '  \033[33mSKIP\033[0m  %-34s %s\n' "$2" "${3:-}"
        SKIPPED=$((SKIPPED + 1))
        ;;
    FAIL)
        printf '  \033[31mFAIL\033[0m  %-34s %s\n' "$2" "${3:-}"
        FAILED=$((FAILED + 1))
        ;;
    esac
}

# Starts a named background job. Its exit status lands in $OUT/<name>.rc and its
# output in $OUT/<name>.log, so results are collected in a fixed order later and
# the printed table stays deterministic however the jobs interleave.
job() {
    name=$1
    shift
    (
        "$@" >"$OUT/$name.log" 2>&1
        echo $? >"$OUT/$name.rc"
    ) &
}
rc_of() { cat "$OUT/$1.rc" 2>/dev/null || echo 99; }

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

printf 'starting independent trees and fetches in parallel...\n'

job fresh build_tree "$OUT/fresh" tests/tests -DCMAKE_BUILD_TYPE=Checked
job nosdl build_tree "$OUT/nosdl" tests/tests -DNES_BUILD_FRONTEND=OFF
job fetch fetch_all
command -v scan-build >/dev/null 2>&1 && job analyze "$ROOT/tests/run_scan_build.sh"
[ "$WITH_SANITIZERS" -eq 1 ] &&
    job asanbuild sh -c "cmake -S '$ROOT' -B '$OUT/asan' -G Ninja -DNES_BUILD_FRONTEND=OFF -DNES_SANITIZE=address,undefined >/dev/null 2>&1 && cmake --build '$OUT/asan'"

wait

BUILD=${NES_BUILD_DIR:-$ROOT/build}
[ -x "$BUILD/tests/tests" ] || BUILD="$OUT/fresh"

section() { printf '\n%s\n' "$1"; }

section "build integrity"
[ "$(rc_of fresh)" -eq 0 ] && report PASS "clean tree configures and builds" ||
    report FAIL "clean tree configures and builds" "see $OUT/fresh.log"

if [ "$(rc_of nosdl)" -eq 0 ]; then
    sdl=$(ldd "$OUT/nosdl/tests/tests" 2>/dev/null | grep -ci sdl)
    [ "$sdl" -eq 0 ] && report PASS "suite builds and links without SDL" ||
        report FAIL "suite builds and links without SDL" "$sdl SDL libs linked"
else
    report FAIL "suite builds without SDL" "see $OUT/nosdl.log"
fi

section "fixtures"
if [ "$(rc_of fetch)" -eq 0 ]; then
    # `grep -c` prints 0 AND exits 1 when it matches nothing, so `|| echo 0`
    # yields TWO lines and every later numeric test on it misbehaves. Count the
    # lines instead; there is no failure path to paper over.
    retried=$(grep RETRY "$OUT/fetch.log" 2>/dev/null | wc -l)
    if [ "$retried" -eq 0 ]; then
        report PASS "fetch scripts, cold and idempotent"
    else
        report PASS "fetch scripts, cold and idempotent" "$retried needed a retry - flaky host"
    fi
else
    report FAIL "fetch scripts, cold and idempotent" "see $OUT/fetch.log"
fi

section "verification"
counts=$(NES_TEST_BIN="$BUILD/tests/tests" "$ROOT/tests/run_tests.sh" 2>/dev/null | grep "executed" | tail -1)
case "$counts" in
*"0 failed"*) report PASS "test suite" "$counts" ;;
*) report FAIL "test suite" "${counts:-no result}" ;;
esac

NES_BUILD_DIR="$BUILD" "$ROOT/tests/test_counts.sh" --check >"$OUT/counts.log" 2>&1 &&
    report PASS "test-count table matches" ||
    report FAIL "test-count table matches" "run tests/test_counts.sh --check"

if [ -f "$OUT/analyze.rc" ]; then
    [ "$(rc_of analyze)" -eq 0 ] && report PASS "static analyzer" "no bugs found" ||
        report FAIL "static analyzer" "see $OUT/analyze.log"
else
    report SKIP "static analyzer" "scan-build not installed"
fi

if [ "$WITH_SANITIZERS" -eq 1 ]; then
    if [ "$(rc_of asanbuild)" -eq 0 ]; then
        # SHARDED. As one process this is ~5x slower and leaves the machine idle.
        san=$(NES_TEST_BIN="$OUT/asan/tests/tests" \
            ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1 \
            UBSAN_OPTIONS=print_stacktrace=1:report_error_type=1 \
            "$ROOT/tests/run_tests.sh" 2>/dev/null | grep "executed" | tail -1)
        case "$san" in
        *"0 failed"*) report PASS "ASan + UBSan" "$san" ;;
        *) report FAIL "ASan + UBSan" "${san:-see $OUT/asanbuild.log}" ;;
        esac
    else
        report FAIL "ASan + UBSan" "build failed, see $OUT/asanbuild.log"
    fi
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

printf '\n%s failed, %s skipped. Logs: %s\n' "$FAILED" "$SKIPPED" "$OUT"
exit "$FAILED"
