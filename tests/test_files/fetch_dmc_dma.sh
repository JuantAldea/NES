#!/bin/sh
# Fetches the two suites that measure what DMC DMA does to the CPU.
#
# The DMC's memory reader halts the CPU: per the NESdev DMA page, "DMC DMA
# normally takes 3 or 4 cycles, depending on whether alignment is needed", built
# from a halt cycle, an always-present dummy cycle, an optional alignment cycle,
# and the get cycle that performs the read. The halt only lands on a READ cycle - "if the CPU is
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
#   dma_2007_read           prints, from frame 8
#   dma_2007_write          prints, from frame 9
#   dma_4016_read           prints, from frame 11
#
# RUN THEM WITH cpu.reset() AFTER load_cartridge, OR THEY LOOK HUNG AND ARE NOT.
# PC comes from $FFFC, which is open bus until a cartridge is present, and Bus's
# constructor resets before one is - so without a second reset the CPU starts at
# $0000. It then BRK-slides through zeroed RAM and through the $00 padding ahead
# of blargg's `.align 64` routines, two bytes at a time, into the IRQ handler at
# $E742 (`bit $4015` / `rti`) and back out. A PC oscillating over a few addresses
# is indistinguishable from a wait loop from outside, so this reads as "blank,
# and alive" rather than as a harness fault.
#
# It also swallows a whole instruction, which is what makes it look DMC-shaped:
# the last BRK before sync_dmc consumes its own padding byte and $E040 with it,
# so execution resumes at $E041 - one byte into `lda #$80` - and `sta $4010`
# never runs. The DMC timer period stays 0, clock_dmc returns early forever, and
# $4015 bit 4 never clears, so the ROM sits in sync_dmc waiting on a channel that
# cannot finish.
#
# MEASURED with the CPU stall implemented and phantom reads not, which is what
# each ROM printed before the repeats followed the CPU's attempted access:
#
#   dma_2007_write          PASSED. Prints 11 11 AA 33 44 55 66 77 five times
#                           and its own name. Already correct then and now.
#   dma_4016_read           FAILED, printing 08 08 08 08 08 and CRC FBF7C7B1.
#                           NOW PASSES on 08 08 07 08 08 and check_crc $F0AB808C.
#   dma_2007_read           printed 11 22 on all five rows, CRC 498C5C5F. NOW
#                           prints 44 55 on the third and CRC 5E3DF9C4, one of
#                           the two AprNes accepts - the 4-cycle alignment, where
#                           Mesen lands on the 3-cycle one and gives 33 44 with
#                           159A7A8F. Both are correct; which one an emulator
#                           sees depends on CPU/PPU sync at reset.
#   double_2007_read        unchanged at D84F6815, and still a PPU problem
#                           rather than a DMC one - see below.
#
# WHAT THEY ARE WAITING FOR: PHANTOM READS. NESdev's DMA page, verbatim - "When
# RDY is deasserted, the 6502 core repeats the last read cycle indefinitely,
# making no forward progress nor handling interrupts. On 2A03 CPUs, these
# repeated reads are externally visible on any no-operation DMA cycle, causing
# data loss if reading a register with side effects." The no-operation cycles
# are the halt, the DMC's dummy and the optional alignment.
#
# It is the LAST READ CYCLE that repeats - the address the CPU was reading when
# it was halted - and NOT simply whatever sits on the bus, which gets the
# ordering right by accident and the address wrong. The controllers are the
# exception and the reason dma_4016_read is its
# own ROM: "Joypads are clocked via direct lines from the CPU, called joypad 1
# /OE and joypad 2 /OE, rather than going over the address bus", giving 0-4
# extra reads rather than one per no-op cycle. erspicu/AprNes names "$4016
# phantom read not implemented" as the cause of dma_4016_read specifically.
#
# double_2007_read is a PPU problem rather than a DMC one, and its source says
# what it tests: `lda $20F7,x` with x=$10 crosses a page, so the CPU reads $2007
# TWICE in succession - the dummy read at the unfixed address and then the real
# one. With x=$00 it reads once. Both mirror to $2007.
#
# We print 22 33 44 55 66 for the single read, which is right, and 33 44 55 66 77
# for the double, which is not: we apply BOTH reads in full. Every one of
# blargg's four accepted second lines starts 22, 02 or 32, so hardware ignores
# the extra read or applies it partially. Its expected CRCs are 85CFD627,
# F018C287, 440EF923 or E52F41A5 - four, because CPU/PPU sync varies.
#
# THE MECHANISM IS A LATCH PIPELINE, NOT A COOLDOWN, which AprNes is often cited
# for and does not implement. Its ppu_r_2007 returns the buffer, advances 7
# master clocks, and then SETS AN SR
# LATCH, leaving a per-dot state machine - PD_RB, ReadALE, PPU_READ, TStep - to
# refill the buffer and increment the address afterwards. It is a port of
# TriCNES. A second read arriving mid-flight sees a pipeline that has not
# updated, which is why the result depends on alignment and has four answers.
#
# IMPLEMENTED as the one consequence that pipeline has here: the REFILL lands a
# few dots after the read returns, and the address increment does not. That gives
# 22 44 55 66 77 and CRC 85CFD627, the first of the four. Delaying the increment
# too would give 22 33 44 55 66 instead - also accepted, and a different ROM
# alignment - so the split between the two is what the answer turns on.
#
# THE DELAY IS BRACKETED BY TWO HARDWARE-CRC ORACLES rather than chosen, and the
# whole admissible window is 4, 5 and 6 dots. Below 4 this ROM's two reads are 3
# dots apart and the second sees a refreshed buffer, so its CRC never moves off
# D84F6815. At 7 and above dma_2007_read breaks, its DMA repeats being a cycle
# apart. Swept at 2,3,4,5,6,7,8,9,12,15,20,40,80,200: past 15 the damage is broad
# - ppuReadBuffer, vram_access, cpu_dummy_writes_ppumem and every HolyMapperel
# board - and past 39 the CRC leaves the accepted set. See
# PPU::kReadBufferRefillDots.
#
# Five PPU unit tests read $2007 two or three times with ZERO dots between them,
# which no CPU can issue - even the page-crossing double read is 3 dots apart and
# an ordinary pair is 12. They now advance the PPU between reads.
#
# THE FULL SET OF ACCEPTED CRCs is catalogued in erspicu/AprNes'
# unittest/run_tests.py, per ROM:
#
#   dma_2007_read      159A7A8F or 5E3DF9C4
#   double_2007_read   85CFD627, F018C287, 440EF923 or E52F41A5
#
# Ours prints D84F6815, in neither set - the value that emulator reported before
# fixing the $2007 cooldown. christopherpow/nes-test-roms' status.txt carries
# the same 5E3DF9C4 and confirms dma_2007_read and double_2007_read never print
# "Passed" - they are compare-by-eye ROMs. dma_2007_write and dma_4016_read do.
#
# dma_2007_write, dma_4016_read and read_write_2007 carry NO crc list there,
# because they do print a verdict. sprdma_and_dmc_dma carries none either: it
# self-checks with check_crc $FBADA48D internally and prints Passed on its own.
# So no expected table for it is published anywhere. Do not try to recover one
# from the checksum - see the trap in tests/dmc_dma_tests.cpp, where ~657 tables
# satisfy the 32-bit constraint.
#
# The four dmc_tests ROMs are absent from that catalogue entirely, and
# nes-test-roms' status.txt marks them "???? Not sure yet" - so an emulator
# sitting at 174/174 does not run them either. Leaving them unfetched here turns
# out to be the same call, independently made.
#
# buffer_retained, latency, status AND status_irq CANNOT BE ORACLES HERE, and
# that is measured rather than inherited. All four are NROM, 16KB PRG + CHR-RAM,
# and there is no source or readme for them upstream.
#
# Run with cpu.reset(), they RUN TO COMPLETION and park - they are not waiting on
# anything. Each ends on a deliberate exit that disables NMI and rendering, then
# configures pulse 1 and plays a continuous tone:
#
#   sei / lda #$00 / sta $2000 / sta $2001
#   lda #$82 / sta $4000 / lda #$01 / sta $4002 / sta $4015
#   lda #$09 / sta $4003 / jmp *
#
# The tone is the whole report. Nothing is written to either nametable page - all
# 2048 bytes stay $00 - no $6000 signature is set, and the background palette
# stays 0. A=9 at the halt is that `lda #$09` for $4003, not a result code.
#
# SILENCE DOES NOT MEAN PASS, checked against a known-bad reference rather than
# assumed. Forcing $4015 bit 4 to 0 - a break severe enough to fail 7-dmc_basics
# and 8-dmc_rates - leaves all four of these byte-identical: same blank
# nametable, same terminal loop, same registers. They cannot separate a correct
# DMC from a badly broken one through any channel this emulator reads, so
# fetching them would add four tests that pass unconditionally.
#
# That matches the catalogue: nes-test-roms' status.txt marks them "???? Not sure
# yet" and AprNes does not list them. buffer_retained is the tempting one,
# because the load-versus-reload split is exactly the open question, and it
# answers nothing here.
#
# SO THE USABLE ORACLES ARE THE TWO sprdma_and_dmc_dma ROMS. Each self-checks by
# CRC over its own printed output and prints Passed or Failed, and the numbers it
# prints say why when it fails. read_write_2007 passes and is asserted to keep
# passing, which guards against a stall implementation that breaks what already
# works.
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
# BOTH TABLES NOW MATCH, ROW FOR ROW, AND BOTH ROMS PASS THEIR OWN CRC. Two
# independent oracles agreeing: Mesen's dumped values and the hardware constants
# blargg compiled into the ROMs ($FBADA48D and $F1A58F55). The pins live in
# tests/dmc_dma_tests.cpp.
#
# The _512 ROM was the harder case and was where the last of the work was - not
# in the single row 05 of the first. It diverged on six rows: 04-07 to the
# collision's boundary costs, and 0A-0B to the write refusal and the parity gate
# being served as two delays for one cause. The DMC stall work was described as
# "15 of 16 rows exact" while that was only ever true of the FIRST ROM; the
# second was failing a not-yet-implemented pin, so nobody had looked past its
# verdict.
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
#   ELIMINATED ALONG THE WAY, so nobody spends another day on them:
#
#   * Halt-entry timing. Removing the get/put gate in Bus::advance_dmc_dma moves
#     every row of BOTH ROMs by +1 - all 32, uniformly - and leaves row 05's
#     crossing exactly where it was. The gate is load-bearing three ways: it
#     produces the 3-cycle load and 4-cycle reload, it decides which cycles the
#     write refusal is reached on, and it puts acceptance on the odd
#     remaining_dma_cycles values the collision costs are keyed to.
#   * Any +-1 cycle theory about when the DMC transfer is REQUESTED. The parity
#     gate quantises it: a reload waits for a put cycle, so shifting the request
#     by one cycle just shortens the idle wait and the halt still lands on the
#     same cycle. Deferring only the reload produced a byte-identical table and
#     an identical CRC. This class of hypothesis is unfalsifiable here - three of
#     four wrong guesses in one session were in it, and four more followed.
#   * Supplying the gate's effect as a delay elsewhere. A flat one-cycle wait
#     before accepting, and a deferred reload in apu.cpp, BOTH HANG BOTH ROMS.
#     The gate is not a delay, so nothing that only delays can replace it.
#   * Moving clock_dmc() relative to the CPU access. It shifts the DMC's timer
#     and its request together, so their offset never changes. A void test.
#   * The timer arithmetic, the sample-buffer occupancy and the load/reload
#     split. All measured correct. The "+11 constant" that implicated them was
#     an artefact of subtracting the 512/513 baseline, which is invalid.
#
#   Each ROM self-checks by CRC, so the whole table has to be right; there is no
#   partial credit and no need to guess which row is wrong.
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
