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
#   the OAM DMA: the first five happen before it, the rest inside it. A DMC DMA
#   costs 4 cycles normally and only 2 when it collides, because "DMA units
#   don't interfere with each other unless they're both trying to access on the
#   same cycle, in which case DMC DMA wins". So the expected table is around
#   515-517, not the 783 a wrong implementation produced.
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
