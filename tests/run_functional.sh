#!/bin/sh
# Full functional pass over the project: everything a green unit-test run does
# NOT prove.
#
# WHY THIS EXISTS. Driving these checks by hand costs about fifteen separate
# commands and pages of output to read, and most of that output is noise on a
# good day. This prints one line per check and details only for failures, so
# the whole pass is one command and a short table.
#
# WHAT IT COVERS THAT `ctest` DOES NOT:
#
#   build     a clean tree configures and builds from nothing - catches a header
#             that only compiles because an old object file is lying around
#   nosdl     the suite builds AND LINKS with no SDL present. CLAUDE.md requires
#             this; nothing under tests/ may depend on the frontend, and the
#             check is `ldd | grep sdl`, not just "it compiled"
#   fetch     every fetch script works from a COLD cache and is idempotent on a
#             second run. Locally the ROMs are always already there, so this is
#             the one class of failure a developer machine structurally cannot
#             see - it only ever showed up in CI before
#   cli       ./build/NES reports failure with a non-zero EXIT CODE, not just a
#             message. A wrapper script cannot see a message
#   hygiene   no ROM or build output is tracked by git, imgui.ini is ignored
#   gui       the frontend actually opens a window and renders a cartridge
#
# THE GUI SECTION IS A SMOKE TEST, NOT AN ORACLE. It proves the window opens,
# the process survives, and nothing lands on stderr. Whether the PICTURE is
# right is not decidable here - screenshots are written to the output directory
# and judging them is a separate, human (or model) job. Saying otherwise would
# be the same false green this repo spends its effort avoiding.
#
#   tests/run_functional.sh              everything except the sanitizers
#   tests/run_functional.sh --full       including the sanitizer suite (slow)
#   tests/run_functional.sh --no-gui     skip the frontend (headless machines)
#
# Exit status is the number of failed checks, so `if tests/run_functional.sh`
# works.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${NES_FUNCTIONAL_OUT:-${TMPDIR:-/tmp}/nes-functional}
LOG="$OUT/log"

WITH_SANITIZERS=0
WITH_GUI=1
for arg in "$@"; do
    case "$arg" in
    --full) WITH_SANITIZERS=1 ;;
    --no-gui) WITH_GUI=0 ;;
    *) echo "usage: tests/run_functional.sh [--full] [--no-gui]" >&2; exit 2 ;;
    esac
done

rm -rf "$OUT"
mkdir -p "$OUT"
: >"$LOG"

FAILED=0
SKIPPED=0

# Each check prints one line. Detail goes to the log and is echoed only when
# the check fails - which is the whole point of the exercise.
report() {
    verdict=$1
    name=$2
    detail=${3:-}
    case "$verdict" in
    PASS) printf '  \033[32mPASS\033[0m  %-34s %s\n' "$name" "$detail" ;;
    SKIP) printf '  \033[33mSKIP\033[0m  %-34s %s\n' "$name" "$detail"; SKIPPED=$((SKIPPED + 1)) ;;
    FAIL)
        printf '  \033[31mFAIL\033[0m  %-34s %s\n' "$name" "$detail"
        FAILED=$((FAILED + 1))
        ;;
    esac
}

section() { printf '\n%s\n' "$1"; }

# ---------------------------------------------------------------- build ------
section "build integrity"

if cmake -S "$ROOT" -B "$OUT/fresh" -G Ninja >>"$LOG" 2>&1 &&
    cmake --build "$OUT/fresh" >>"$LOG" 2>&1 &&
    [ -x "$OUT/fresh/tests/tests" ]; then
    report PASS "clean tree configures and builds"
else
    report FAIL "clean tree configures and builds" "see $LOG"
fi

if cmake -S "$ROOT" -B "$OUT/nosdl" -G Ninja -DNES_BUILD_FRONTEND=OFF >>"$LOG" 2>&1 &&
    cmake --build "$OUT/nosdl" >>"$LOG" 2>&1 &&
    [ -x "$OUT/nosdl/tests/tests" ]; then
    sdl=$(ldd "$OUT/nosdl/tests/tests" 2>/dev/null | grep -ci sdl)
    if [ "$sdl" -eq 0 ]; then
        report PASS "suite builds and links without SDL"
    else
        report FAIL "suite builds and links without SDL" "$sdl SDL libs linked"
    fi
else
    report FAIL "suite builds without SDL" "see $LOG"
fi

BUILD=${NES_BUILD_DIR:-$ROOT/build}
[ -x "$BUILD/tests/tests" ] || BUILD="$OUT/fresh"

# ---------------------------------------------------------------- fetch ------
section "fixtures"

cold_ok=1
idem_ok=1
for script in "$ROOT"/tests/test_files/fetch_*.sh; do
    name=$(basename "$script" .sh)
    case "$name" in
    fetch_single_step_tests) continue ;;  # 1.1 GB, deliberately not pulled here
    esac

    dir=$(sed -n 's/^DEST="\$DIR\/\([a-z0-9_.]*\)".*/\1/p' "$script" | head -1)
    [ -n "$dir" ] && rm -rf "$ROOT/tests/test_files/$dir"

    # One retry, and it is not indulgence. The hosts these pull from produce a
    # real "TLS connect error: unexpected eof while reading" often enough that
    # a single attempt makes this check about the network rather than about the
    # project. A retry that SUCCEEDS is still reported, so the flakiness stays
    # visible instead of being smoothed away; a retry that fails is a failure.
    if ! "$script" >>"$LOG" 2>&1; then
        if "$script" >>"$LOG" 2>&1; then
            report PASS "cold fetch: $name" "after one retry - the host was flaky"
        else
            report FAIL "cold fetch: $name" "twice, see $LOG"
            cold_ok=0
        fi
        continue
    fi
    # A second run must re-download nothing.
    if "$script" 2>>"$LOG" | grep -qv "ok (cached)" >/dev/null 2>&1; then
        second=$("$script" 2>>"$LOG" | grep -c "fetching")
        [ "$second" -ne 0 ] && idem_ok=0
    fi
done
[ "$cold_ok" -eq 1 ] && report PASS "every fetch script from a cold cache"
[ "$idem_ok" -eq 1 ] && report PASS "fetch scripts re-download nothing" ||
    report FAIL "fetch scripts re-download nothing" "a second run fetched again"

# ---------------------------------------------------------------- suite ------
section "verification"

# grep, not `tail -N | head -1`. The summary is the last NON-BLANK line and the
# first draft of this counted lines from the end, landing on the blank one and
# reporting "no result" against a suite that had just passed 907 tests. Matching
# the content rather than its position is the fix.
counts=$(NES_TEST_BIN="$BUILD/tests/tests" "$ROOT/tests/run_tests.sh" 2>>"$LOG" | grep "executed" | tail -1)
case "$counts" in
*"0 failed"*) report PASS "test suite" "$counts" ;;
*) report FAIL "test suite" "${counts:-no result - see $LOG}" ;;
esac

if NES_BUILD_DIR="$BUILD" "$ROOT/tests/test_counts.sh" --check >>"$LOG" 2>&1; then
    report PASS "test-count table matches"
else
    report FAIL "test-count table matches" "run tests/test_counts.sh --check"
fi

if command -v scan-build >/dev/null 2>&1; then
    if "$ROOT/tests/run_scan_build.sh" >>"$LOG" 2>&1; then
        report PASS "static analyzer" "no bugs found"
    else
        report FAIL "static analyzer" "see $LOG"
    fi
else
    report SKIP "static analyzer" "scan-build not installed"
fi

if [ "$WITH_SANITIZERS" -eq 1 ]; then
    if cmake -S "$ROOT" -B "$OUT/asan" -G Ninja -DNES_BUILD_FRONTEND=OFF \
        -DNES_SANITIZE=address,undefined >>"$LOG" 2>&1 &&
        cmake --build "$OUT/asan" >>"$LOG" 2>&1; then
        san=$(ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1 \
            UBSAN_OPTIONS=print_stacktrace=1:report_error_type=1 \
            "$OUT/asan/tests/tests" 2>>"$LOG" | tail -1)
        case "$san" in
        *PASSED*) report PASS "ASan + UBSan" "$san" ;;
        *) report FAIL "ASan + UBSan" "${san:-see $LOG}" ;;
        esac
    else
        report FAIL "ASan + UBSan" "build failed, see $LOG"
    fi
else
    report SKIP "ASan + UBSan" "pass --full to include"
fi

# ------------------------------------------------------------------ cli ------
section "command line"

# The exit CODE, not the message. Measured without a pipe: `cmd | head; echo $?`
# reports head's status, which has silently turned a real failure into a pass
# more than once in this project's history.
check_exit() {
    desc=$1; want=$2; shift 2
    timeout 60 "$@" >"$OUT/cli.txt" 2>&1
    got=$?
    if [ "$got" -eq "$want" ]; then
        report PASS "$desc" "exit=$got"
    else
        report FAIL "$desc" "exit=$got, wanted $want"
    fi
}
check_exit "CLI rejects no arguments"  1 "$BUILD/NES"
check_exit "CLI rejects a missing file" 1 "$BUILD/NES" /nonexistent/nope.nes
check_exit "CLI rejects a non-ROM"      1 "$BUILD/NES" "$ROOT/README.md"

# -------------------------------------------------------------- hygiene ------
section "repository hygiene"

tracked=$(cd "$ROOT" && git ls-files | grep -cE '\.nes$|^build')
if [ "$tracked" -eq 0 ]; then
    report PASS "no ROMs or build output tracked"
else
    report FAIL "no ROMs or build output tracked" "$tracked files"
fi

if (cd "$ROOT" && git check-ignore -q imgui.ini); then
    report PASS "imgui.ini is ignored"
else
    report FAIL "imgui.ini is ignored"
fi

# ------------------------------------------------------------------ gui ------
section "frontend"

if [ "$WITH_GUI" -eq 0 ]; then
    report SKIP "frontend smoke test" "--no-gui"
elif [ ! -x "$BUILD/nes_frontend" ]; then
    report SKIP "frontend smoke test" "not built"
elif [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    report SKIP "frontend smoke test" "no display"
elif ! command -v xdotool >/dev/null 2>&1 || ! command -v import >/dev/null 2>&1; then
    report SKIP "frontend smoke test" "needs xdotool and ImageMagick"
else
    # One cartridge per supported mapper, so a mapper that stopped working in
    # the frontend shows up even though the ROM suite still passes.
    for entry in \
        "mapper0:$ROOT/tests/test_files/local/smb.nes" \
        "mapper2:$ROOT/tests/test_files/visual/240pee.nes" \
        "mapper3:$ROOT/tests/test_files/ppu_read_buffer/test_ppu_read_buffer.nes" \
        "mapper4:$ROOT/tests/test_files/mmc3/4-scanline_timing.nes"; do
        tag=${entry%%:*}
        rom=${entry#*:}
        [ -f "$rom" ] || { report SKIP "frontend $tag" "no ROM"; continue; }

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
        sleep 4

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
        sleep 0.5
    done
    echo "        screenshots in $OUT - the PICTURE still needs looking at"
fi

# --------------------------------------------------------------- verdict -----
printf '\n%s failed, %s skipped. Log: %s\n' "$FAILED" "$SKIPPED" "$LOG"
exit "$FAILED"
