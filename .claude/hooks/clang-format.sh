#!/bin/sh
# PostToolUse hook: run clang-format on any C/C++ source this project owns
# after an edit.
#
# .clang-format has been in this repo since 2020, so formatting drift produces
# noise in every subsequent diff. Applying it at edit time keeps that out of
# review rather than fixing it in a later sweep commit.
#
# Deliberately silent and always exit 0: this is a convenience, not a gate, and
# a hook that fails an edit because clang-format is not installed would be
# worse than no hook.
set -u

command -v jq >/dev/null 2>&1 || exit 0
command -v clang-format >/dev/null 2>&1 || exit 0

file=$(jq -r '.tool_input.file_path // empty')
[ -n "$file" ] || exit 0
[ -f "$file" ] || exit 0

case "$file" in
    # Only this project's own sources. build/ holds fetched third-party code
    # (ImGui, googletest), which is built with -w precisely so it is not held
    # to our standards - reformatting it would be worse than pointless.
    */build/*|*/build-*/*) exit 0 ;;
    *.cpp|*.h|*.hpp|*.cc|*.c) clang-format -i "$file" ;;
esac

exit 0
