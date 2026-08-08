#!/bin/sh
# Runs Clang's static analyzer (scan-build) over the whole project and fails if
# it finds anything.
#
# This is the third correctness net, and it is the only one that needs no test
# to reach the code. The ROM oracles and the asserts both require execution:
# a bug on a path nothing exercises is invisible to them. The analyzer walks
# paths instead of running them, which is why it is worth having alongside a
# suite that already passes 875 tests.
#
# It is also the only job here that touches no test ROMs at all - nothing is
# executed, so nothing has to be fetched. A network failure cannot turn this
# red, and a missing fixture cannot either.
#
# MEASURED on 32 cores, from a clean tree:
#
#   normal build   10.0s wall, 119s user
#   scan-build     31.0s wall, 399s user     (~3x)
#
# and it currently reports ZERO bugs, which is what makes --status-bugs usable
# as a gate with no baseline file to maintain.
#
# VERIFIED TO ACTUALLY FAIL, because a gate that cannot go red is worse than no
# gate. A null dereference added to src/rom.cpp was reported as
# "warning: Dereference of null pointer (loaded from variable 'p')
# [core.NullDereference]" with the file and line, and the script exited 1.
# Re-do that check if you ever change the flags below.
#
# WHAT IT DOES NOT CATCH, measured the same way, because this matters for a
# codebase that is mostly indexing fixed-size arrays:
#
#   int buf[4]; return buf[n * 8];      NOT reported - n is unconstrained, so
#                                       the analyzer builds no concrete path
#   int i = 0; if (s > 2) i = 6;
#   return buf[i];                      reported, but as UndefReturn - the
#                                       garbage that came back, not the index
#
# So this is not static bounds checking. alpha.security.ArrayBound was tried
# and left OFF: enabling it changed neither result above, so there was no
# evidence it does anything here, and alpha checkers are free to change
# between Clang releases in a way a CI gate would feel. Runtime bounds
# checking is ASan's job, and remains the reason both exist.
#
# Usage:
#   tests/run_scan_build.sh                          # the gate
#   tests/run_scan_build.sh -DNES_BUILD_FRONTEND=ON  # extra configure args
#
# Environment:
#   NES_SCAN_BUILD_DIR   build tree to use          (default: build-scan)
#   NES_SCAN_GENERATOR   CMake generator            (default: Ninja)
#   NES_SCAN_INCREMENTAL non-empty: skip the wipe   (default: wipe)
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${NES_SCAN_BUILD_DIR:-$ROOT/build-scan}
GENERATOR=${NES_SCAN_GENERATOR:-Ninja}
REPORTS=$BUILD/report

if ! command -v scan-build >/dev/null 2>&1; then
    echo "scan-build not found. It ships with Clang:" >&2
    echo "  Debian/Ubuntu  apt-get install clang-tools" >&2
    echo "  Arch           pacman -S clang" >&2
    exit 127
fi

# The tree is wiped by default, and that is not tidiness. scan-build only sees
# what the build actually compiles, so an up-to-date tree analyses NOTHING and
# still prints "No bugs found" - a false green of exactly the kind this repo
# spends so much effort not having (see "Skipped is not passed" in CLAUDE.md).
# 30s is cheap enough that correctness wins; set NES_SCAN_INCREMENTAL to opt
# out while iterating on a fix.
if [ -z "${NES_SCAN_INCREMENTAL:-}" ]; then
    rm -rf "$BUILD"
fi

# The frontend is OFF by default here, and CMakeLists.txt makes that the
# default under the analyzer too - see the comment there. Short version: ImGui
# reports 16 findings of its own, including null dereferences and a division by
# zero, and they are not ours to fix. --exclude below keeps them out of the
# bug COUNT, but not out of the log, so leaving ImGui uncompiled is the only
# way the log stays all-ours. Pass -DNES_BUILD_FRONTEND=ON to analyse it
# anyway; measured clean on our side of that line.
#
# Both commands run under scan-build, and both are needed. The configure step
# is what records the analyzer wrappers as the compiler in the build files; the
# build step is what sets the environment ccc-analyzer reads to emit reports.
scan-build -o "$REPORTS" \
    cmake -S "$ROOT" -B "$BUILD" -G "$GENERATOR" -DNES_BUILD_FRONTEND=OFF "$@"

# --status-bugs is the gate: without it scan-build exits with the BUILD's
# status, so a clean compile with twenty findings would be a pass.
#
# --exclude drops reports whose file sits under _deps - googletest, and ImGui
# if it was turned on. It filters the generated reports rather than skipping
# the analysis (scan-build deletes the report files afterwards), so it changes
# the count and the exit status but not what gets printed during the build.
# Same principle as `target_compile_options(imgui PRIVATE -w)`: third-party
# code is not held to this project's standards, and must not bury our own
# findings.
scan-build -o "$REPORTS" --exclude "$BUILD/_deps" --status-bugs \
    cmake --build "$BUILD"
