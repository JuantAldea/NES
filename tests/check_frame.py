#!/usr/bin/env python3
"""Fails if a PPM framebuffer dump is blank, or near enough to it.

WHY THIS EXISTS. run_functional.sh's frontend check opened a window per mapper,
waited three wall-clock seconds and screenshotted whatever was there, reporting
"window up, screenshot captured". That sentence is true of a completely black
screen, and it HAS been true of one: the check used to point at
4-scanline_timing, which draws its verdict around frame 310, so every run
photographed black and passed. The ROM was swapped for a faster one and nothing
was added that would notice next time.

WHAT IT CHECKS, and deliberately not more. A rendered NES frame is not a
photograph - Holy Mapperel draws two colours, text on a flat background - so
counting colours would reject correct output. What separates "drew something"
from "drew nothing" is whether ONE colour covers the whole screen.

MEASURED at the frame count the check actually uses, which is what sets the
threshold:

    an unpainted frame              1 colour    100.0% dominant
    M66_P64K_C16K_V   frame 1300    2 colours    92.9%
    test_ppu_read_buffer, 1266      4 colours    87.6%
    Super Mario Bros  frame 1300   11 colours    77.4%
    240pee            frame 1300   10 colours    44.3%

So 99% sits six points clear of the tightest real output and one below a blank
one. Holy Mapperel's flat text is the worst case and a game is nowhere near it.

It is a FLOOR, not a quality bar: it says a frame was drawn, not that it was
right. Judging the picture is still a person's job, and the screenshots are
still written for that.
"""

import collections
import sys

DOMINANT_LIMIT = 0.99


def read_ppm(path):
    """(pixels as bytes, width, height) from a binary P6 file."""
    data = open(path, "rb").read()
    if not data.startswith(b"P6"):
        raise ValueError(f"{path} is not a binary PPM (P6)")
    offset = 0
    for _ in range(3):  # magic, dimensions, maximum value
        offset = data.index(b"\n", offset) + 1
    header = data[:offset].split()
    return data[offset:], int(header[1]), int(header[2])


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_frame.py <dump.ppm>", file=sys.stderr)
        return 2

    path = sys.argv[1]
    try:
        pixels, width, height = read_ppm(path)
    except (OSError, ValueError, IndexError) as error:
        print(f"{path}: {error}", file=sys.stderr)
        return 1

    total = len(pixels) // 3
    if total != width * height:
        print(f"{path}: header says {width}x{height} but the body holds {total} pixels", file=sys.stderr)
        return 1

    counts = collections.Counter(pixels[i : i + 3] for i in range(0, len(pixels), 3))
    colour, count = counts.most_common(1)[0]
    dominant = count / total

    if dominant > DOMINANT_LIMIT:
        print(
            f"{path}: blank - one colour #{colour.hex()} covers {dominant:.1%} of the frame "
            f"({len(counts)} distinct). The emulator drew nothing worth looking at.",
            file=sys.stderr,
        )
        return 1

    print(f"{len(counts)} colours, most common {dominant:.1%}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
