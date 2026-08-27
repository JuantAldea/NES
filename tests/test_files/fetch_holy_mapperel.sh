#!/bin/sh
# Fetches twenty-one builds of Damian Yerrick's Holy Mapperel - an NES cartridge
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
# MAPPERS 9 AND 10 - the first boards the CPU does not fully drive.
#
# MMC2 and MMC4 change CHR bank when the PPU FETCHES tile $FD or $FE out of a
# window, so each 4KB window has two bank registers and a latch saying which is
# live. Punch-Out!!'s animated faces are what the part exists for. These three
# images are the only ones here that exercise PPU::ppu_bus_read notifying the
# mapper after a pattern read.
#
#   image                header                            what it exercises
#   M9_P128K_C64K        128K PRG, 64K CHR-ROM, NES2.0     PNROM (MMC2)
#   M10_P128K_C64K_S8K   + 8K battery work RAM             FJROM/FKROM (MMC4)
#   M10_P128K_C64K_W8K   + 8K work RAM                     FJROM/FKROM (MMC4)
#
# MEASURED BEFORE, with the boards unregistered from kBoards:
#
#   M9_P128K_C64K        ROM: mapper 9 (MMC2 / PxROM) is not supported
#   M10_P128K_C64K_S8K   ROM: mapper 10 (MMC4 / FxROM) is not supported
#   M10_P128K_C64K_W8K   ROM: mapper 10 (MMC4 / FxROM) is not supported
#
# MEASURED AFTER, first run, nothing adjusted:
#
#   image                board reported      PRG    work RAM  CHR          detail
#   M9_P128K_C64K        009 PNROM (MMC2)    128K   MISSING   64K ROM OK   0000
#   M10_P128K_C64K_S8K   010 F*ROM (MMC4)    128K   8K OK     64K ROM OK   0000
#   M10_P128K_C64K_W8K   010 F*ROM (MMC4)    128K   8K OK     64K ROM OK   0000
#
# "F*ROM" with a literal asterisk is the ROM's own output: FJROM and FKROM
# differ only in work-RAM size and it declines to guess between them.
#
# THE MIRRORING POLARITY WAS DECIDED HERE, and measured both ways rather than
# read off a table - the MMC3 register above was inverted for exactly as long as
# it was taken from a wiki row written in arrangement terms. With $F000 bit 0
# clear meaning VERTICAL these identify correctly. With it flipped, neither
# image renders a report at all inside 400 frames: detection runs from RAM and
# HANGS on a wrong probe answer rather than printing a wrong board. Louder than
# the MMC3 case, and it means a regression in that one bit shows up as a timeout
# rather than as a diff.
#
# MAPPER 7. M7_P128K is the oracle AxROM was written against, and unlike every
# other image here it was fetched BEFORE the board existed rather than to check
# one that already did.
#
#   image        header                          what it exercises
#   M7_P128K     128K PRG, 8K CHR-RAM, NES2.0    ANROM
#
# MEASURED BEFORE, with no mapper 7 at all:
#
#   ROM: mapper 7 (AxROM) is not supported
#
# That message is worth recording as the baseline even though it is only a
# rejection: it is the naming table in mapper.cpp doing its job, and the reason
# the enum carries forty entries when five have boards.
#
# MEASURED AFTER, first run, nothing adjusted:
#
#   image        board reported  PRG    work RAM  CHR           detail  settled
#   M7_P128K     007 ANROM       128K   MISSING   8K RAM OK     0000    frame 85
#
# THE BOARD LINE IS THE TEST, more here than anywhere else in this suite.
# Detection works by writing mirroring and observing where the nametables land,
# and AxROM is the board where both answers are ONE SCREEN with a register bit
# choosing which. So "007" is not a label read off the header - it is bit 4 of
# the latch demonstrably moving all four nametable slots together. The same
# probe is what reported "002 UNROM" for an MMC3 whose mirroring bit was
# inverted.
#
# ANROM rather than AOROM is the size detection: AOROM is the 256KB variant, and
# getting 128KB right means the ROM walked the bank tags through a 32KB window
# with no fixed half to stand on. AxROM is the only board here with no fixed PRG
# bank at all - the vectors are switchable, so the cartridge has to replicate
# its handlers in every bank, and reproducing the board means reproducing that.
#
# MAPPER 69 - the first board that counts CPU cycles, and the only rows in this
# suite pinned to a non-zero detail code.
#
# Sunsoft's FME-7 uses a command/parameter pair ($8000 selects one of fourteen
# registers, $A000 supplies the value) and carries a 16-bit down-counter clocked
# by M2. That is a different animal from the MMC3's counter, which watches PPU
# A12 and therefore counts scanlines by accident - this one measures time, and
# fires with rendering off. It is why Mapper::wants_cpu_clock exists.
#
# MEASURED BEFORE, with the board unregistered:
#
#   M69_P128K_C64K_S8K   ROM: mapper 69 (Sunsoft FME-7) is not supported
#   M69_P128K_C64K_W8K   ROM: mapper 69 (Sunsoft FME-7) is not supported
#
# MEASURED AFTER:
#
#   image                board reported      PRG    work RAM  CHR          detail
#   M69_P128K_C64K_S8K   069 J*ROM (FME-7)   128K   8K OK     64K ROM OK   0010
#   M69_P128K_C64K_W8K   069 J*ROM (FME-7)   128K   8K OK     64K ROM OK   0010
#
# The digits are [WRAM][PRG][IRQ][CHR] and the bit meanings are in global.inc;
# the one left is IRQ 1 = MAPTEST_IRQ ($10).
#
# IT WAS 2110 WHEN THE BOARD FIRST LANDED, and the two digits that closed did so
# together, because they were one cause: PRG window 1 is $6000-$7FFF and FME-7
# can map PRG-ROM there, which Bus::decode could not do. It now asks the
# cartridge (ROM::prg_rom_at_6000) before assuming work RAM. Finding that also
# turned up a real bug this file's own constants settle - global.inc names
# FME7_PRGBANK_ROM $00, FME7_PRGBANK_OFF $40 and FME7_PRGBANK_RAM $C0, which
# only decode if BIT 6 SELECTS and BIT 7 ENABLES. This board had them the other
# way round.
#
# THE IRQ DIGIT IS THE ROM BEING WRONG, NOT THE BOARD. fme7_test_irq writes 256
# to the counter and comments "Schedule an IRQ 256 cycles from now"; the wiki's
# "decremented from $0000 to $FFFF" is 257. tepples, who wrote this ROM, says
# the subtest "was made based on outdated information" and that Holy Diver
# Batman tests "whether things are connected at all rather than the precise
# behavior of the CPLD" - https://forums.nesdev.org/viewtopic.php?p=177596
#
# Measured here too, by offsetting the counter and reading the digit back: the
# ROM accepts an assertion 237 to 255 cycles after the arming write, and our
# correct 257 misses that window by exactly the 2 cycles Sour measured in Mesen.
# The counter is otherwise verified in the same run - asserts at 257, the
# handler acknowledges 32 cycles later, next assertion 65504 cycles after that.
#
# Pinned exactly in tests/holy_mapperel_tests.cpp rather than left loose, so
# closing the digit - or losing another one - fails there and reports which.
#
# TxSROM (118), THE FIRST BOARD HERE WITH BANKED NAMETABLES.
#
# MEASURED BEFORE:
#
#   M118_P128K_C64K   ROM: mapper 118 (TxSROM) is not supported
#
# MEASURED AFTER:
#
#   image             board reported      PRG    work RAM  CHR          detail
#   M118_P128K_C64K   118 T*SROM (MMC3)   128K   MISSING   64K ROM OK   0000
#
# Clean on the first run, which is worth being suspicious of rather than glad
# about - so the row was checked by breaking it. Two plausible wrong versions of
# TxSRom::ciram_page, "never bank the nametables" and "flip which register set
# the CHR inversion selects", BOTH HANG the ROM for the full 400 frames instead
# of mis-reporting: it stops in the mapper detection phase, which runs from RAM.
# The image is therefore a real discriminator for the one function this board
# adds, not a row that would pass regardless.
#
# The board line is the load-bearing assertion. Holy Mapperel identifies a board
# by writing mirroring and watching where the nametables land, so "118 T*SROM"
# is a direct readout of the only thing TxSROM does differently from an MMC3.
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
M4_P256K_C256K c5dca47517d1cdb5cc252382802418253b8edf1a0cbff6c6b60b62ba2b6317ca
M7_P128K 6161a17001ba7d0345d6650d799994832d62990fd545c3c3a054015604de6f83
M9_P128K_C64K 6605a2a6a2b14bc8cfdfcf1444f902b8a7024332fa0ca31994ab3953b559c1c7
M10_P128K_C64K_S8K 5181996ce6fbf0dedd6b253815ef2809d938a296d172160d5e463e64cf07f757
M10_P128K_C64K_W8K 54047d0cf47847e363a4c5932e81f22127862581782a6f66d79be77feee50779
M69_P128K_C64K_S8K 6a3f84f29f5f18a09237af0ef2c73f1b6f199be908009bf9b568e765be86f7b1
M69_P128K_C64K_W8K 61bb0cdd871c76a607364b092cdad8c709020e3bdf365994613b7a69ee847d7e
M118_P128K_C64K 5e1ab21ff5a2b4a3cdb026cdf4d8800660c210a40bc6705e656c353e030de173"

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
if [ "$count" -ne 21 ]; then
    echo "incomplete: $count/21 files present in $DEST" >&2
    exit 1
fi

echo "done: $count files in $DEST"
