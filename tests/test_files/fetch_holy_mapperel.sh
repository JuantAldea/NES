#!/bin/sh
# Fetches fourteen builds of Damian Yerrick's Holy Mapperel - an NES cartridge
# manufacturing test, and the only MMC1 oracle in reach that reports a verdict
# rather than asking a human to look at it. Nine mapper-1 images, and two
# mapper-4 ones covering ground no MMC3 ROM here reaches (see MAPPER 4 below).
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
#   M1_P512K_S32K        SXROM  512K   32K OK            8K RAM OK       0000
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
#   ALSO FIXED, later: M1_P512K_S32K reported 8K of work RAM where its header
#   says 32K, and called itself SUROM. That was the console rather than the
#   mapper - PrgRAM was an 8KB Bus device, sized to the $6000-$7FFF WINDOW
#   rather than to the board, so SXROM's four switchable 8KB banks had nowhere
#   to live. The device is 32KB now and MMC1 selects the bank from CHR register
#   bits 2-3, with ROM::prg_ram_offset folding every access into the size the
#   header declares so an 8KB board still cannot reach past its one chip.
#
#   The board name is the evidence, not the size line. Both 512K images are
#   SUROM as far as the PRG side can tell; SXROM is SUROM plus banked work RAM,
#   and the ROM can only tell them apart by writing tags to all four banks and
#   reading them back. M1_P512K_S8K stays SUROM with 8K, which is the fold
#   working.
#
# Settle frames were measured too and live next to kMaxFrames in that file.
#
#
# MAPPER 4. The two M4 images were added for a narrower reason than the SxROM
# above: to decide whether MMC3 $A000 bit 0 selects vertical mirroring or
# horizontal. The wiki gives the answer in ARRANGEMENT terms - "0: horizontal
# (A10); 1: vertical (A11)" - and arrangement is the inverse of mirroring, so
# reading that row as though it named a mirroring mode inverts the bit. This
# emulator had it inverted, and no test noticed.
#
#   image                header                       what it exercises
#   M4_P128K             128K PRG, 8K CHR-RAM, NES2.0  TGROM
#   M4_P256K_C256K       256K PRG, 256K CHR-ROM, NES2.0  TLROM
#
# MEASURED BEFORE THE FIX, with $A000 bit 0 still reading clear-means-horizontal:
#
#   image             board reported   detail  settled
#   M4_P128K          002 UNROM        0100    frame 85
#   M4_P256K_C256K    (blank screen)   -       never, hit the 400-frame cap
#
# "002 UNROM" is the finding, not a crash. Holy Mapperel works out the board by
# writing mirroring and observing where the nametables land, so an inverted bit
# does not corrupt the test - it makes an MMC3 answer the detection probe the
# way a discrete mapper would, and the ROM believes it. That is the failure mode
# a doc citation cannot produce and an oracle gets on the first run.
#
# MEASURED AFTER, and this is what the test pins:
#
#   image             board reported     PRG         work RAM          CHR             detail
#   M4_P128K          004 TGROM (MMC3)   128K PRG    MISSING           8K CHR RAM OK   0000
#   M4_P256K_C256K    004 TLROM (MMC3)   256K PRG    MISSING           256K CHR ROM OK 0000
#
# WHY blargg's mmc3_test_2 DID NOT CATCH THE MIRRORING BUG. All six of those
# ROMs test the IRQ counter. They write $A000 once during init and never read
# the nametables back, so mmc3_rom_tests.cpp was fully green with the polarity
# reversed.
#
#
# THE SECOND FINDING, and the better argument for these two images. Fixing the
# mirroring did NOT take M4_P128K to 0000 - it arrived at 0003, the CHR digit,
# which is bit-for-bit the signature the three MMC1 CHR-RAM boards showed before
# 58f1ce6 banked theirs. Reproduced here on a board sharing no mapper code with
# MMC1, and on the CHR-RAM image while the CHR-ROM one read 0000.
#
# 58f1ce6 had banked CHR-RAM for MMC1 but deliberately left MMC3's unbanked,
# with a comment saying real MMC3 does drive those lines so paging it was "very
# likely right" - but that nothing in the suite measured it, and naming THESE
# IMAGES as what would settle it. They did. Banking MMC3 CHR-RAM in 1KB units,
# the way it banks CHR-ROM, takes M4_P128K to 0000; M4_P256K_C256K is CHR-ROM,
# was already 0000, and stays there, so the change is confined to the RAM path.
#
# That is an open question closed by a ROM rather than by argument, and it is
# why the detail column above reads 0000 on both rows instead of pinning a known
# failure. It also means these two images now cover two distinct MMC3 subsystems
# - nametable mirroring and CHR-RAM addressing - neither of which any other MMC3
# oracle here reaches.
#
# THE THREE DISCRETE BOARDS - M0, M2, M3 - AND A CLEAN RESULT.
#
# Added last, and on a base rate rather than a suspicion: this oracle had been
# pointed at exactly two mappers and found three real faults in them (MMC1
# indexing CHR-RAM flat, MMC3 inverting $A000 bit 0, MMC3 leaving CHR-RAM
# unbanked). NROM, UNROM and CNROM predate the Mapper refactor and had been
# carried through two structural changes on the strength of suites largely
# written from the implementation, so they were the obvious next place to look.
#
# MEASURED, with nothing changed to accommodate them:
#
#   image             board reported  PRG    work RAM  CHR             detail
#   M0_P32K_C8K_V     066 NROM        32K    MISSING   8K ROM OK       0000
#   M2_P128K_V        002 UNROM       128K   MISSING   8K RAM OK       0000
#   M3_P32K_C32K_H    066 CNROM       32K    MISSING   32K ROM OK      0000
#
# All clean. Recorded because "we looked and there was nothing" is a result -
# it is the difference between three boards being correct and three boards
# being untested, and only one of those survives the next refactor.
#
# "066" ON TWO OF THEM IS NOT A MISREPORT. The Holy Mapperel README groups board
# 066 as "NROM, CNROM, GNROM" - mappers it cannot tell apart, because none of
# them changes mirroring and that is how the detection works. The number is the
# detection GROUP, the word after it the board within it. UNROM switches PRG, so
# it gets a line of its own.
#
# M2_P128K_V earns its place twice over: it is CHR-RAM, so it is the check that
# 58f1ce6 - which routed CHR-RAM through the mapper so MMC1 could page it - left
# alone the boards that must NOT page it. UNROM takes the flat default and reads
# 0000.
#
# Not committed: redistributable-but-unlicensed dumps, SHA256-pinned.
#
# Usage: tests/test_files/fetch_holy_mapperel.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/holy_mapperel"
BASE="https://raw.githubusercontent.com/koute/pinky/master/nes-testsuite/roms/holy_diver_batman"

mkdir -p "$DEST"

ROMS="M0_P32K_C8K_V 94ea295e37a94b055e9ee09d1de213e646d9ebe5919ff3b389f4f151a982f73f
M2_P128K_V 47c46bee4c56babda552c321256b9683c11edfe8904964ae4ae9f6af7158ad2c
M3_P32K_C32K_H e10c73aadda7c1a8d610bcaf09d07d253308fd3206efc5d9805a1c75f1d1874f
M1_P128K b3270d196e5dcaa3ed7026cf8ea583418b653055002c6d04bf4f637999badc36
M1_P128K_C32K 0f1e9329e2a9a4749de1f48c606a7b243fc5b8b1e50511de630015130e10d504
M1_P128K_C32K_S8K 0a35d243cf061366987aba7a91f58df61810ad0072128b28e6df5977a5cb8f1d
M1_P128K_C32K_W8K 3cba6e68f1f30dcba3c1e686ce81916bbf54384bfab47a6281e1b94ada875f6c
M1_P128K_C128K 56497c054cfddafc93120fb25d85a7659f9f31ebe35f3304e966cad6edfa053c
M1_P128K_C128K_S8K 1b90e96b17f373637114b59e78e379d6e7a776dc5477d60010df172c3c486687
M1_P128K_C128K_W8K 5065edcfaff5e0b4518f96c7b4e12889f6cc47f82bbedabd80cd3e07d33fe212
M1_P512K_S8K a68997cca022e1fdbd0773d847f3d49a3c18af2ee4eb187d31655e27a7cff34a
M1_P512K_S32K c5bfaa5c2f318295b167da8f9b5b4d32e47a8be080c92cc6d7bc2c63eaabadc9
M4_P128K acf96ad58b8fef57ae80b0d18f2dbb9aed3bb4447f6d6ee3e793c011f08b9481
M4_P256K_C256K c5dca47517d1cdb5cc252382802418253b8edf1a0cbff6c6b60b62ba2b6317ca"

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
if [ "$count" -ne 14 ]; then
    echo "incomplete: $count/14 files present in $DEST" >&2
    exit 1
fi

echo "done: $count files in $DEST"
