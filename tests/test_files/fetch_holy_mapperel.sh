#!/bin/sh
# Fetches the nine mapper-1 builds of Damian Yerrick's Holy Mapperel - an NES
# cartridge manufacturing test, and the only MMC1 oracle in reach that reports
# a verdict rather than asking a human to look at it.
#
# WHY NOT MMC1_A12. nes-test-roms carries MMC1_A12/mmc1_a12.nes, which is
# mapper 1 and looks like the obvious choice. It is not an oracle: its screen
# reads "USE U/D L/R TO ADJUST DELAY=" and the answer is the WIDTH OF A
# GRAYSCALE BAR you eyeball while walking a delay counter up and down. No
# PASS/FAIL text, no status byte. Its inner loop is commented "Wait until PRG
# RAM is disabled (emulators will freeze here)" and does exactly that unless
# WRAM enable is modelled within a scanline, tracking sprite fetches. Driving
# the pad headlessly - which this emulator can do, Controllers::press - would
# yield a threshold number that WE would then have to judge. That makes us the
# oracle, which is the thing an oracle is for avoiding.
#
# Mirror, not upstream: pinobatch/holy-mapperel ships built ROMs only inside a
# .7z release asset, so these come from koute/pinky's checked-in copies. The
# SHA256s below are what make that safe - the mirror is a transport, not an
# authority.
#
# HOW IT REPORTS, because it decides how the test reads it. Holy Mapperel
# prints to the nametable, and src/main.s's puts is:
#
#     lda (0),y
#     beq done
#     and #$3F        ; tile index = ASCII & 0x3F
#     sta PPUDATA
#
# So digits land at $30-$39 unchanged and A-Z at $01-$1A. tests/nametable_screen.h
# reads the tiles; the $3F folding has to be undone on the way out, which is why
# this suite passes its own decoder rather than reusing the identity one the
# blargg 2005 ROMs need.
#
# The headline is a four-digit detail code - WRAM, PRG ROM, IRQ, CHR - where
# zero is normal. The README documents two of the mapper-1 non-zero values:
#
#   1xxx   $E000 bit 4 does not disable WRAM
#   4xxx   $A000 bit 4 does not disable WRAM (SNROM), or does when it should not
#
# THE HEADERS ARE NES 2.0, WHICH IS A PREREQUISITE AND NOT A DETAIL. Byte 7 is
# $08 in all nine (bits 2-3 = 10b). The sizes these ROMs exist to check live in
# bytes 10 and 11, which rom.h used to describe as "unused by this loader":
#
#   ROM                  PRG   CHR        byte 10          byte 11
#   M1_P128K             128K  8K RAM     -                $07  8K CHR-RAM
#   M1_P128K_C32K        128K  32K ROM    -                -
#   M1_P128K_C32K_S8K    128K  32K ROM    $70  8K battery  -
#   M1_P128K_C32K_W8K    128K  32K ROM    $07  8K work     -
#   M1_P128K_C128K       128K  128K ROM   -                -
#   M1_P128K_C128K_S8K   128K  128K ROM   $70  8K battery  -
#   M1_P128K_C128K_W8K   128K  128K ROM   $07  8K work     -
#   M1_P512K_S8K         512K  8K RAM     $70  8K battery  $07  8K CHR-RAM
#   M1_P512K_S32K        512K  8K RAM     $90  32K battery $07  8K CHR-RAM
#
# The two 512K images are SUROM: MMC1 has five PRG bank bits and only four
# reach a 256K part, so the CHR register's bit 4 doubles as PRG A18. They are
# deliberately the last tier of this suite, not the first.
#
# MEASURED, twice, because the first picture is uninformative on its own.
#
# BEFORE, with no MMC1 at all: all nine rejected by the loader, identically -
# "ROM: mapper 1 is not supported". True, and worth recording, but it proves
# nothing about whether the ROMs boot and report rather than hanging, which is
# the property that makes a suite usable as an oracle.
#
# AFTER, and this is the CURRENT state rather than the work queue the first cut
# produced - see the FIXED note below for what the first cut got wrong and how
# the failing SET named it:
#
#   image                board  PRG    work RAM          CHR             detail
#   M1_P128K             SGROM  128K   MISSING           8K RAM OK       0000
#   M1_P128K_C32K        SFROM  128K   MISSING           32K ROM OK      0000
#   M1_P128K_C32K_S8K    SJROM  128K   8K OK             32K ROM OK      0000
#   M1_P128K_C32K_W8K    SJROM  128K   8K OK             32K ROM OK      0000
#   M1_P128K_C128K       SLROM  128K   MISSING           128K ROM OK     0000
#   M1_P128K_C128K_S8K   SKROM  128K   8K OK             128K ROM OK     0000
#   M1_P128K_C128K_W8K   SKROM  128K   8K OK             128K ROM OK     0000
#   M1_P512K_S8K         SUROM  512K   8K OK             8K RAM OK       0000
#   M1_P512K_S32K        SUROM  512K   8K OK  (want 32K) 8K RAM OK       0000
#
# Every board is identified correctly, which is the load-bearing line: the ROM
# works out which SxROM it is on purely from how the mapper answers, so a right
# name means mirroring control, PRG banking and size detection all behaved.
#
# TWO THINGS WERE LEFT AT THAT POINT. One is now fixed, one is not, and the
# fixed one is the argument for taking all nine images rather than a favourite.
#
#   FIXED, and the detail column above now reads 0000 on every row: the three
#   CHR-RAM boards read 0003 where all six CHR-ROM boards read 0000. Digits are
#   WRAM, PRG, IRQ, CHR, so both 4K CHR windows failed their bank-tag readback
#   (mmcdrivers.s runs mmc1_test_one_chr_window at $0000 and again at $1000,
#   ORing the second in shifted left). Identical on a 128K board and on a 512K
#   one, which ruled out the PRG side - only the 512K boards route a CHR
#   register bit to PRG A18 - and left the CHR-RAM path.
#
#   The cause was that CHR-RAM was indexed FLAT, `chr_ram[addr]`, because the
#   array lives in the PPU and the comment there called it RAM "the console
#   supplies". It is not the console's. CHR-RAM sits on the cartridge behind the
#   same mapper CHR address lines as CHR-ROM, so MMC1 in 4KB mode pages an 8KB
#   chip as two halves. It goes through Mapper::chr_offset now.
#
#   NOT FIXED: M1_P512K_S32K reports 8K of work RAM where its header says 32K.
#   That is the console rather than the mapper - PrgRAM is a fixed 8KB Bus
#   device, so SXROM's four switchable banks have nowhere to live. Pinned
#   exactly in tests/holy_mapperel_tests.cpp, the way opcode $AB is pinned in
#   tests/instr_test_roms.cpp, so closing it surfaces as a distinct message
#   instead of a row everyone has learned to expect red.
#
# Settle frames were measured too and live next to kMaxFrames in that file.
#
# Not committed: redistributable-but-unlicensed dumps, SHA256-pinned.
#
# Usage: tests/test_files/fetch_holy_mapperel.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/holy_mapperel"
BASE="https://raw.githubusercontent.com/koute/pinky/master/nes-testsuite/roms/holy_diver_batman"

mkdir -p "$DEST"

ROMS="M1_P128K b3270d196e5dcaa3ed7026cf8ea583418b653055002c6d04bf4f637999badc36
M1_P128K_C32K 0f1e9329e2a9a4749de1f48c606a7b243fc5b8b1e50511de630015130e10d504
M1_P128K_C32K_S8K 0a35d243cf061366987aba7a91f58df61810ad0072128b28e6df5977a5cb8f1d
M1_P128K_C32K_W8K 3cba6e68f1f30dcba3c1e686ce81916bbf54384bfab47a6281e1b94ada875f6c
M1_P128K_C128K 56497c054cfddafc93120fb25d85a7659f9f31ebe35f3304e966cad6edfa053c
M1_P128K_C128K_S8K 1b90e96b17f373637114b59e78e379d6e7a776dc5477d60010df172c3c486687
M1_P128K_C128K_W8K 5065edcfaff5e0b4518f96c7b4e12889f6cc47f82bbedabd80cd3e07d33fe212
M1_P512K_S8K a68997cca022e1fdbd0773d847f3d49a3c18af2ee4eb187d31655e27a7cff34a
M1_P512K_S32K c5bfaa5c2f318295b167da8f9b5b4d32e47a8be080c92cc6d7bc2c63eaabadc9"

echo "$ROMS" | while read -r name want; do
    [ -n "$name" ] || continue
    dest="$DEST/$name.nes"

    if [ -f "$dest" ]; then
        have=$(sha256sum "$dest" | cut -d' ' -f1)
        if [ "$have" = "$want" ]; then
            echo "ok (cached): $name.nes"
            continue
        fi
        echo "checksum mismatch on cached $name.nes, refetching" >&2
    fi

    echo "fetching $name.nes..."
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --max-time 120 -o "$dest.tmp" "$BASE/$name.nes"

    have=$(sha256sum "$dest.tmp" | cut -d' ' -f1)
    if [ "$have" != "$want" ]; then
        rm -f "$dest.tmp"
        echo "SHA256 mismatch for $name.nes" >&2
        echo "  expected: $want" >&2
        echo "  actual:   $have" >&2
        exit 1
    fi

    mv "$dest.tmp" "$dest"
    echo "ok: $name.nes"
done

count=$(ls "$DEST" | wc -l | tr -d ' ')
if [ "$count" -ne 9 ]; then
    echo "incomplete: $count/9 files present in $DEST" >&2
    exit 1
fi

echo "done: $count files in $DEST"
