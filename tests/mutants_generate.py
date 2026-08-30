#!/usr/bin/env python3
"""Generate mutations of a C++ source file, mechanically.

Called by tests/run_mutants.sh; see that script for why mutation testing is
here at all. This half exists separately, and in Python rather than awk, for
one reason: the two most productive mutation classes in this codebase are not
expressible as line-local text substitutions.

  DROPPING ONE CLAUSE of a compound condition. APU::pulse_level gates on four
  conditions ORed together. Deleting any one of them is a real, plausible bug
  and each needs its own mutant.

  SWAPPING TWO ARGUMENTS at a call site. mixer_output() passes five same-typed
  levels positionally; a transposition there is invisible to every test that
  exercises one channel at a time, and an adversarial review found exactly that
  hole.

Both need balanced-parenthesis parsing. Neither is a regex.

Output is TSV on stdout: file, line, payload-path, label. The payload file holds
the replacement text for that whole line, which sidesteps every sed-escaping
question - the driver swaps the line wholesale.

WHAT THIS DELIBERATELY DOES NOT DO. It has no type information, so the argument
swapper over-generates: it will happily swap two arguments of different types
and produce something that does not compile. That is fine and is the cheaper
error - the driver reports non-compiling mutants separately and they cost a
build each. Being wrong in the direction of "too many candidates" keeps the
generator from having opinions about what is interesting, which is the whole
point of generating them mechanically.
"""

import re
import sys
from pathlib import Path

# Space-surrounded binary operators. .clang-format is authoritative in this
# repository and puts spaces around every binary operator, while
# `static_cast<uint16_t>` and `foo->bar` have none - so requiring the spaces
# keeps templates and member access out without parsing C++.
BINARY_OPS = {
    " < ": " <= ",
    " <= ": " < ",
    " > ": " >= ",
    " >= ": " > ",
    " == ": " != ",
    " != ": " == ",
    " && ": " || ",
    " || ": " && ",
    " + ": " - ",
    " - ": " + ",
    " >> ": " << ",
    " << ": " >> ",
}

SKIP_LINE = re.compile(r"^\s*(//|\*|#)")

# `const std::string& path`, `float corner_hz`, `int tap` - a type and a name,
# possibly qualified, referenced or templated. See argument_swaps.
DECLARED_PARAMETER = re.compile(r"^\s*(const\s+)?[A-Za-z_][\w:]*(\s*<[^<>]*>)?\s*[&*]?\s+[A-Za-z_]\w*\s*$")
LITERAL = re.compile(r"[0-9]+\.[0-9]+f|0[xX][0-9a-fA-F]+|\b[0-9]+\b")


def perturb(literal: str) -> str | None:
    """A meaningfully different literal, or None if there isn't one."""
    if literal.endswith("f"):
        # PROPORTIONAL, not a fixed step. A divisor of 8227 nudged by a
        # hundredth moves the mixer's output by ~1e-7, below the tolerance the
        # tests compare at - so it would survive, mean nothing, and be
        # indistinguishable in the report from a real hole. Zero is the
        # exception: 0 * 1.01 is 0, an equivalent mutant by construction.
        base = float(literal[:-1])
        value = 1.0 if base == 0.0 else base * 1.01
        text = repr(value)
        return text + "f" if ("." in text or "e" in text) else text + ".0f"
    if literal.lower().startswith("0x"):
        return f"0x{int(literal, 16) + 1:X}"
    return str(int(literal) + 1)


def split_top_level(text: str, separator: str) -> list[str] | None:
    """Split on `separator` at paren/bracket depth zero. None if it never occurs."""
    parts, depth, current, i, found = [], 0, [], 0, False
    while i < len(text):
        char = text[i]
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        if depth == 0 and text.startswith(separator, i):
            parts.append("".join(current))
            current = []
            i += len(separator)
            found = True
            continue
        current.append(char)
        i += 1
    parts.append("".join(current))
    return parts if found else None


def condition_clause_drops(line: str):
    """`if (A || B || C)` -> one mutant per clause removed."""
    match = re.search(r"\bif\s*\(", line)
    if not match:
        return
    start = match.end() - 1
    depth = 0
    for i in range(start, len(line)):
        if line[i] == "(":
            depth += 1
        elif line[i] == ")":
            depth -= 1
            if depth == 0:
                end = i
                break
    else:
        return

    inner = line[start + 1 : end]
    for separator in (" || ", " && "):
        parts = split_top_level(inner, separator)
        if not parts or len(parts) < 2:
            continue
        for drop in range(len(parts)):
            kept = parts[:drop] + parts[drop + 1 :]
            new_inner = separator.join(kept)
            yield (
                line[: start + 1] + new_inner + line[end:],
                f"drop clause {drop + 1} of {len(parts)} ({separator.strip()})",
            )


def argument_swaps(line: str):
    """`f(a, b, c)` -> swap each adjacent pair. Type-blind on purpose."""
    for match in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_:]*)\s*\(", line):
        start = match.end() - 1
        depth = 0
        for i in range(start, len(line)):
            if line[i] == "(":
                depth += 1
            elif line[i] == ")":
                depth -= 1
                if depth == 0:
                    end = i
                    break
        else:
            continue

        inner = line[start + 1 : end]
        if not inner.strip():
            continue
        args = split_top_level(inner, ", ")
        if not args or len(args) < 2:
            continue
        # A DECLARATION'S PARAMETER LIST IS NOT A CALL, and swapping it only
        # renames the parameters - the definition lives elsewhere and keeps its
        # own names, so the mutant is a no-op that can never be killed and is
        # reported as a survivor forever. Two of the nine mutants of blip.h were
        # this, and two more of audio.h's eighteen.
        #
        # Detected by shape: every argument of a declaration is `type name`,
        # which no call site's argument looks like. Deliberately narrow - it
        # requires EVERY argument to match, so `f(count, 0.0f)` is untouched, and
        # the operator keeps working where it has found real defects.
        if all(DECLARED_PARAMETER.match(arg) for arg in args):
            continue
        for i in range(len(args) - 1):
            swapped = list(args)
            swapped[i], swapped[i + 1] = swapped[i + 1], swapped[i]
            yield (
                line[: start + 1] + ", ".join(swapped) + line[end:],
                f"{match.group(1)}(): swap args {i + 1} and {i + 2}",
            )


def mutate_line(line: str):
    """Every mutant of one line, as (new_line, label)."""
    for pattern, replacement in BINARY_OPS.items():
        for occurrence in range(line.count(pattern)):
            before, sep, after = "", "", line
            for _ in range(occurrence + 1):
                head, sep, after = after.partition(pattern)
                before += head + sep
            new = before[: -len(pattern)] + replacement + after
            yield new, f"{pattern.strip()} -> {replacement.strip()} (#{occurrence + 1})"

    seen: dict[str, int] = {}
    for match in LITERAL.finditer(line):
        literal = match.group(0)
        # Not part of an identifier: uint16_t, a name ending in digits.
        if match.start() > 0 and (line[match.start() - 1].isalnum() or line[match.start() - 1] == "_"):
            continue
        if match.end() < len(line) and (line[match.end()].isalpha() or line[match.end()] == "_"):
            continue
        replacement = perturb(literal)
        if replacement is None:
            continue
        seen[literal] = seen.get(literal, 0) + 1
        new = line[: match.start()] + replacement + line[match.end() :]
        yield new, f"{literal} -> {replacement} (#{seen[literal]})"

    yield from condition_clause_drops(line)
    yield from argument_swaps(line)


def main() -> int:
    source, eligible_path, outdir = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    eligible = {int(n) for n in Path(eligible_path).read_text().split()}
    lines = Path(source).read_text().splitlines()

    index = 0
    for number, line in enumerate(lines, start=1):
        if number not in eligible or SKIP_LINE.match(line):
            continue
        # Any line carrying a string literal: mutating a failure message proves
        # nothing about the code.
        if '"' in line:
            continue
        # Mutate the CODE only, and put the trailing comment back untouched. A
        # comment mutant cannot be killed by any test, so it is reported as a
        # survivor forever and reads exactly like a real hole: the first run of
        # this harness against src/bus.cpp scored 7/8 because `// wrong phase -
        # wait for the right one` became `+ wait`. Splitting on the first `//`
        # is safe here precisely because the check above dropped every line
        # carrying a `"`, so this one cannot be inside a string.
        code, separator, comment = line.partition("//")
        if not code.strip():
            continue
        for new_code, label in mutate_line(code):
            if new_code == code:
                continue
            new_line = new_code + separator + comment
            index += 1
            payload = outdir / f"payload.{index}"
            payload.write_text(new_line + "\n")
            print(f"{source}\t{number}\t{payload}\t{label}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
