#!/bin/sh
# Fetches the two suites that measure what DMC DMA does to the CPU - the one
# part of the delta modulation channel that is deliberately not implemented.
#
# The DMC's memory reader currently fetches in zero cycles. On hardware it halts
# the CPU: per the NESdev DMA page, "DMC DMA normally takes 3 or 4 cycles,
# depending on whether alignment is needed", built from a halt cycle, an always
# -present dummy cycle, an optional alignment cycle, and the get cycle that
# performs the read. The halt only lands on a READ cycle - "if the CPU is
# writing, it ignores the halt...repeating until successful" - so a
# read-modify-write can delay it 2 cycles and an interrupt 3.
#
# All seven are NROM (mapper 0). dmc_dma_during_read4 is 32KB PRG with CHR-RAM;
# sprdma_and_dmc_dma is 32KB PRG + 8KB CHR.
#
# THEY REPORT ON SCREEN, not through $6000, so they go through
# tests/nametable_screen.h like the other 2005-era suites.
#
# MEASURED with the DMC implemented except for the CPU stall:
#
#   sprdma_and_dmc_dma      FAILED @ frame 157   prints a 16-row table of DMA
#   sprdma_and_dmc_dma_512  FAILED @ frame 157   lengths and a CRC, then a
#                                                verdict. Every row reads 512 or
#                                                513 - the pure OAM DMA lengths,
#                                                with no DMC interference at all,
#                                                which is exactly the missing
#                                                behaviour showing up as data.
#   read_write_2007         PASSED @ frame 14
#   double_2007_read        no verdict @ frame 12 - prints a table and the CRC
#                           D84F6815 and stops. It is a compare-by-eye ROM, not
#                           self-checking, so it cannot be an automatic oracle
#                           without a known-good CRC from hardware.
#   dma_2007_read           BLANK, and alive
#   dma_2007_write          BLANK, and alive
#   dma_4016_read           BLANK, and alive
#
# THE BLANK ONES ARE NOT HUNG, which is worth recording because "blank screen"
# and "crashed" look identical from outside. Measured over 300 frames each: the
# CPU runs 8.9M cycles and the PC oscillates over two or three addresses around
# $E066, i.e. a tight wait loop. Do not treat them as fixtures that failed to
# load.
#
# WHAT THEY ARE WAITING FOR IS NOW KNOWN, and it is not the stall: they test
# PHANTOM READS. While the CPU is halted for a DMA it repeatedly reads whatever
# address is on the bus - with $4016/$4017 triggering only on the halt cycle and
# other registers on every no-op cycle. erspicu/AprNes names "$4016 phantom read
# not implemented" as the cause of dma_4016_read specifically.
#
# double_2007_read is a PPU bug rather than a DMC one, and the evidence is
# exact: the D84F6815 above is the same CRC AprNes reported before fixing a
# missing ~6-dot cooldown after a $2007 read, during which a second read returns
# open bus without swapping the buffer or incrementing the VRAM address. Its
# expected CRCs are 85CFD627, F018C287, 440EF923 or E52F41A5 - four, because
# CPU/PPU sync varies.
#
# THE FULL SET OF ACCEPTED CRCs is catalogued in erspicu/AprNes'
# unittest/run_tests.py, per ROM:
#
#   dma_2007_read      159A7A8F or 5E3DF9C4
#   double_2007_read   85CFD627, F018C287, 440EF923 or E52F41A5
#
# Ours prints D84F6815, in neither set - the value that emulator reported before
# fixing the $2007 cooldown. christopherpow/nes-test-roms' status.txt carries
# the same 5E3DF9C4 and confirms these ROMs never print "Passed".
#
# dma_2007_write, dma_4016_read and read_write_2007 carry NO crc list there,
# because they do print a verdict. sprdma_and_dmc_dma carries none either: it
# self-checks with check_crc $FBADA48D internally and prints Passed on its own.
# So no expected table for it is published anywhere, which is exactly why
# recovering one from the checksum was worth attempting.
#
# The four dmc_tests ROMs are absent from that catalogue entirely, and
# nes-test-roms' status.txt marks them "???? Not sure yet" - so an emulator
# sitting at 174/174 does not run them either. Leaving them unfetched here turns
# out to be the same call, independently made.
#
# THAT IS NOW MEASURED RATHER THAN INHERITED. buffer_retained, latency, status
# and status_irq were fetched and run for 600 frames each: all four report
# NOTHING by any of the three mechanisms - no nametable text, no $6000, no
# background colour - while being demonstrably alive, 17.8M cycles with the PC
# parked in the same $E14x-$E16x wait loop the blank ROMs above sit in. They are
# waiting on phantom reads, not on anything about DMC fetch timing.
#
# buffer_retained is the tempting one, because the load-versus-reload split is
# exactly the open question. It does not answer it.
#
# SO THE USABLE ORACLE IS sprdma_and_dmc_dma. It self-checks by CRC over its own
# printed output and prints Passed or Failed, it fails today, and the numbers it
# prints say why. read_write_2007 passes and is asserted to keep passing, which
# guards against a stall implementation that breaks what already works.
#
# WHAT sprdma_and_dmc_dma ACTUALLY DOES, and what it expects. Its source is NOT
# in this mirror; it is in koute/pinky at
# nes-testsuite/roms/sprdma_and_dmc_dma/source/, together with the common/
# dma_timing.inc that drives it. Worth knowing before attempting the stall,
# because the printed column is easy to misread:
#
#   The timed code is  lda #$07 / sta $4014 / sta $100 / rts  - so the number is
#   how long that SEQUENCE took, including any cycles a DMA stole from it, and
#   not the length of the OAM DMA on its own. The baseline 512/513 is that
#   sequence with no DMC DMA in it.
#
#   Each of the 16 iterations moves the DMC DMA one cycle closer to the start of
#   the OAM DMA: the first ones happen before it, the rest inside it. A DMC DMA
#   costs 4 cycles normally and only 2 when it collides, because "DMA units
#   don't interfere with each other unless they're both trying to access on the
#   same cycle, in which case DMC DMA wins".
#
#   THE REFERENCE TABLE, read out of Mesen2 (b9fa69d), which passes this ROM.
#   No expected values are published anywhere - the ROM self-checks by CRC - so
#   this was dumped from a passing implementation rather than derived:
#
#     00 527   04 527   08 525   0C 525
#     01 528   05 526   09 526   0D 526
#     02 527   06 525   0A 525   0E 525
#     03 528   07 526   0B 526   0F 526
#
# sprdma_and_dmc_dma_512, SAME Mesen2 revision, captured 2026-08-21 with
# tools/run_mesen_dump.sh --screen. Mesen reports Passed:
#
#     00 525   04 524   08 527   0C 527
#     01 526   05 525   09 528   0D 528
#     02 525   06 526   0A 526   0E 527
#     03 526   07 527   0B 527   0F 528
#
# WE DIVERGE ON SIX ROWS HERE, not one: 04, 05, 06, 07, 0A and 0B. Ours reads
# 525 526 525 526 for 04-07 and 527 528 for 0A-0B. Note Mesen emits 524 at row
# 04, a value our implementation never produces at all.
#
# This matters because the DMC stall work was described as "15 of 16 rows
# exact" while that was only ever true of the FIRST ROM. The second was failing
# the same not-yet-implemented pin, so nobody had looked past its verdict. The
# two ROMs differ in OAM DMA alignment, so the second is the harder case and is
# where the remaining work actually is - not in the single row 05 of the first.
#
#   This header used to say "the expected table is around 515-517". That was an
#   estimate, written here where it then read as measured fact, and it is wrong:
#   527 and 525 are correct. It cost most of an investigation, because the
#   512/513 baseline below was taken with no stall at all - so the DMC also stole
#   nothing inside the ROM's own sync loops, which then ran 429 and 3419 instead
#   of 433 and 3423 and locked at a different count. That baseline is not
#   "hardware minus the swept DMA" and must not be subtracted from.
#
#   READ THE ROW COUNT CAREFULLY BEFORE BELIEVING A NEAR-MISS. Rows 00-04 are all
#   "outside" and 06-0F all "inside", so within each regime the printed value
#   does not depend on the sweep offset at all. Only row 05, the transition, is
#   sensitive to it. Fifteen matching rows are therefore much weaker evidence
#   than they look: an implementation can be wrong throughout and still match
#   everywhere except the boundary.
#
#   WHERE THIS IMPLEMENTATION STANDS: row 05 only, reading 528 where the
#   reference reads 526. Our outside->inside crossing happens one iteration
#   late. Everything else measured correct - OAM DMA 513/514, collision cost
#   exactly 2, standalone 3/4, the ROM's sync loops running at blargg's designed
#   433 and 3423, load/reload classification matching the NESdev DMA page.
#
#   ALREADY ELIMINATED, so nobody spends another day on them:
#
#   * Halt-entry timing. Removing the get/put gate in Bus::advance_dmc_dma moved
#     ALL SIXTEEN rows by +1 and left row 05's crossing exactly where it was.
#     The gate is what produces the 3-cycle load and 4-cycle reload; it is right.
#   * Any +-1 cycle theory about when the DMC transfer is REQUESTED. The parity
#     gate quantises it: a reload waits for a put cycle, so shifting the request
#     by one cycle just shortens the idle wait and the halt still lands on the
#     same cycle. Deferring only the reload produced a byte-identical table and
#     an identical CRC. This class of hypothesis is unfalsifiable here - three of
#     four wrong guesses in one session were in it.
#   * Moving clock_dmc() relative to the CPU access. It shifts the DMC's timer
#     and its request together, so their offset never changes. A void test.
#   * The timer arithmetic, the sample-buffer occupancy and the load/reload
#     split. All measured correct. The "+11 constant" that implicated them was
#     an artefact of subtracting the 512/513 baseline, which is invalid.
#
#   WHAT IS LEFT is an interaction, not either DMA alone, which is why every
#   single-mechanism hypothesis failed. The sweep runs OAM-10, -9 ... -5 and then
#   jumps straight to OAM+2, skipping seven cycles: a DMC DMA landing just before
#   the store delays the store by its own 4 cycles and drags the OAM DMA start
#   along with it. Row 05 sits exactly in that discontinuity. Seeing it needs a
#   cycle-level trace of row 05 from BOTH emulators - Mesen's via the event
#   viewer's DmcDmaRead/DmaRead types - rather than more reasoning from the
#   printed totals. See tools/mesen_reference_dump.cpp.
#
#   It self-checks with check_crc $FBADA48D, so the whole table has to be right;
#   there is no partial credit and no need to guess which row is wrong.
#
#   dma_timing.inc delays 3424 cycles between events, which is 8 x 428 - one
#   byte at rate index 0, the slowest. That is the intended DMA frequency and a
#   useful sanity check on any implementation: roughly one DMA per 3424 cycles
#   through the timed section, not one per few hundred.
#
# The frame numbers are a FLOOR and will rise as ROMs get further.
#
# (c) Shay Green (blargg) and contributors, from the nes-test-roms collection.
# Not committed: gitignored like every other fetched fixture.
#
# Usage: tests/test_files/fetch_dmc_dma.sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$DIR/dmc_dma"
BASE="https://raw.githubusercontent.com/christopherpow/nes-test-roms/master"

mkdir -p "$DEST"

ROMS="dmc_dma_during_read4/dma_2007_read a2e0fa3f6f155cbe0b8c9517b2f6a57f1fd68f13711c11d6d2fe5676c522d7b2
dmc_dma_during_read4/dma_2007_write 54c75d491c685fb4cfff281bcf3e199a41e95f6c523e2b0607d67ba039f19f84
dmc_dma_during_read4/dma_4016_read c6af72e11c197b449129921a9992db2351d9121bb593b3d0ab71895b646b0ebe
dmc_dma_during_read4/double_2007_read 779e6e7db863a7405a3dda8723b8517a23d271e78ce4802970fb0a7d3039ce6b
dmc_dma_during_read4/read_write_2007 bc5281ca3f12a6d0ac9fe1a5e727ecc3cac5fc6a47f45ac130d644f0dbd522cf
sprdma_and_dmc_dma/sprdma_and_dmc_dma db3199bc1b0bdc07a316b3ab999d8fd8bb361456d2154e364c132cb06a26a10f
sprdma_and_dmc_dma/sprdma_and_dmc_dma_512 3789f5134b0561b4344e3f4ce08b4d2a416f67435e083917a80d87fdb9d3583c"

echo "$ROMS" | while read -r path want; do
    name=$(basename "$path")
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
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --max-time 120 -o "$dest.tmp" "$BASE/$path.nes"

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

count=$(ls -1 "$DEST"/*.nes 2>/dev/null | wc -l)
if [ "$count" -ne 7 ]; then
    echo "expected 7 ROMs in $DEST, found $count" >&2
    exit 1
fi

echo "done: $count ROMs in $DEST"
