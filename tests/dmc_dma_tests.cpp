// What DMC DMA does to the CPU, and the residual this file has not closed.
//
// The DMC's memory reader halts the CPU: per NESdev's DMA page, "DMC DMA
// normally takes 3 or 4 cycles, depending on whether alignment is needed", made
// of a halt cycle, an always-present dummy cycle, an optional alignment cycle,
// and the get cycle that performs the read. The halt only lands on a READ cycle
// - "if the CPU is writing, it ignores the halt...repeating until successful" -
// so a read-modify-write can delay it by two cycles and an interrupt by three.
// That sequence is implemented; see Bus::advance_dmc_dma.
//
// THE POINT OF THIS FILE IS THAT THE GAP IS MEASURED RATHER THAN ASSERTED.
// sprdma_and_dmc_dma prints a 16-row table of DMA lengths, CRCs its own output
// and prints a verdict. It still fails, but no longer for the reason this file
// opened with: the table did show a flat 512/513, the pure OAM DMA lengths with
// no DMC interference at all, and now steps 527/528 down to 525/526 - the right
// shape, about eleven cycles high, on rows with no collision as much as on rows
// with one. Everything below is the record of narrowing that, in order.
//
// PARKED. Read the AUDIT section before trusting any figure here.
//
// read_write_2007 already passes and is asserted to keep passing, which is the
// other half: a stall implementation that breaks what already works should not
// be able to hide behind the ROM it was written to fix.
//
// NOT WIRED, AND WHY - three ROMs in dmc_dma_during_read4 draw nothing at all,
// and one prints no verdict:
//
//   dma_2007_read, dma_2007_write, dma_4016_read
//       Blank, and NOT hung. Measured over 300 frames: 8.9M CPU cycles with the
//       PC oscillating over two or three addresses around $E066 - a tight wait
//       loop. They are waiting on something that never happens, and the obvious
//       candidate is the behaviour this suite exists to test. They are fetched
//       so that becomes checkable the moment the stall lands, but asserting on
//       a blank screen now would only pin our own ignorance.
//   double_2007_read
//       Prints a table and the CRC D84F6815 and stops. Compare-by-eye, not
//       self-checking, so it cannot be an automatic oracle without a known-good
//       CRC from hardware.
//
// The four dmc_tests ROMs behave the same way and are not fetched at all yet.
//
// ONE ATTEMPT AT THE STALL HAS ALREADY BEEN MADE AND REVERTED, and what it
// established is worth more than the code was. It built the documented
// sequence - halt (refused on a write cycle and retried), always-present dummy,
// optional alignment, get - with DMC DMA outranking OAM DMA and pausing it.
// Structurally it worked: nothing else in the suite regressed, including every
// APU ROM and all five cpu_interrupts_v2. It was still wrong, and measurably so:
//
//   The table went from 512/513 to 783, where the expected answer is ~515-517.
//   A DMC DMA costs 4 cycles, or 2 when it collides with an OAM DMA. The
//   attempt's cost was 2 or 3 - it held the bus for dummy/align/get but let the
//   CPU run during the halt cycle, which undercounts.
//   It fired one DMA per ~958 CPU cycles. dma_timing.inc is built around 3424,
//   which is 8 x 428: one byte at the slowest rate. Roughly 3.5x too often.
//
// A SECOND ATTEMPT FIXED THE COST and moved the failure somewhere sharper.
// Making the halt cycle itself stolen - all four of halt/dummy/align/get, not
// just the last three - gives exactly 4.00 cycles per DMA, measured in
// isolation as the shortfall between master ticks and CPU cycles executed with
// no OAM DMA present: 40000 ticks, 39952 executed, 48 stolen over 12 fetches.
// That number is now right and should not be re-derived.
//
// The ROM then stopped reaching a verdict at all, and the reason is worth
// recording because it is NOT the DMA cost. It hangs in the shell's sync_dmc
// (common/sync_dmc.s in koute/pinky), which is the routine every iteration
// starts from:
//
//   It sets rate $0F - period 54, so 8 bits is 432 cycles - and a 1-byte
//   sample, then runs a loop of 433 cycles that restarts the sample with
//   $4015 = $1F and reads $4015 six cycles later. Its own comment: "DMC sample
//   takes 432 cycles, thus this loop will slowly creep up on it until it
//   completes just before the final bit SNDCHN". One cycle of drift per
//   iteration, so it should converge in a few hundred iterations.
//
//   Ours does not converge in 3000 frames. Sampled during the hang the DMC
//   reads bytes_remaining=0, buffer filled, period=54 - so bit 4 is clear only
//   in a narrow window, and the creep never lands in it.
//
// A THIRD ATTEMPT FOUND WHY IT DID NOT CONVERGE, and the cause is worth keeping
// because it is invisible from the symptom.
//
// THE GET/PUT PARITY WAS INVERTED. The alignment cycle is spent "if the next
// cycle is not a get cycle", so getting the phase backwards makes every DMA
// cost 3 where it should cost 4, and 4 where it should cost 3. In sync_dmc that
// turns the loop into 432 cycles against a 432-cycle sample - EXACTLY zero
// drift - so the creep that is supposed to walk one cycle per iteration never
// moves and the loop spins forever. A one-bit error produced a hang rather than
// a wrong number, which is why it read as "stuck" rather than "off by one".
//
// Flipping it converges immediately and the ROM prints its whole table. So the
// alignment phase is pinned by this ROM, and it is: the get cycle is the ODD
// half of Bus::cpu_cycles, not the even one.
//
// WHAT IS STILL WRONG is now visible as data rather than as a hang. Every row
// reads 527 or 529, alternating, against a 512/513 baseline and an expected
// ~515-517. Two things follow:
//
//   The per-row alternation of 2 is just the OAM DMA's own 513/514 parity
//   showing through, so the DMA cost we add is a constant ~14-16 cycles.
//
//   The table should NOT be uniform. Each of the 16 iterations walks the DMC
//   DMA closer to the OAM DMA - the early ones land before it, the rest inside -
//   and a colliding DMA costs 2 instead of 4, so a correct run steps partway
//   down the table.
//
// A FOURTH PASS FOUND THE STEP, and corrected a wrong conclusion above. "The
// walk has no effect" was inferred from the flat table and was WRONG. Counting
// collisions per OAM DMA shows the walk working exactly as documented:
// iterations 1-6 collide zero times, 7-15 collide once each. The DMA was moving
// all along.
//
// The table was flat because a colliding DMA cost the same as a lone one. It
// should not: the halt and dummy cycles exist to stop the CPU and let it release
// the bus, and during an OAM DMA the CPU is ALREADY stopped, so only the access
// itself collides - 2 cycles taken from the OAM DMA rather than 4. Charging 2
// when ppu.dma_in_progress() makes the step appear in the right place: rows
// 00-05 at 527/529 and rows 06-0F at 525/527.
//
// WHAT IS LEFT IS AN ABSOLUTE OFFSET, not a shape. Every row is ~11 cycles high
// against an expected ~515-517, uniformly, both before and after the step. That
// is roughly three extra DMAs' worth inside the timed section, and the timed
// section should contain about one: the test writes $4015 twice with a one-byte
// sample, so two fetches, and the sample then stops.
//
// THAT MEASUREMENT HAS NOW BEEN MADE, and it rules the fetch count OUT. The
// timed section is delimited by begin_dmc_timer writing $4010 = $00 - it drops
// to the slowest rate, 428, so one byte is the 3424 cycles of
// dmc_timer_modulo - and it contains just the test's two $4015 writes.
//
// Counting fetches from that write to the OAM DMA gives exactly 1 for
// iterations 0-5 and exactly 0 for 6-15: the fetch walking past the start of
// the OAM DMA, which is precisely what the ROM is built to do. Two fetches in
// the section, one either side, moving correctly.
//
// So the ~11-cycle offset is NOT too many fetches, which was the hypothesis
// and is now dead. Nor is it the cost of one (4 cycles, measured in isolation),
// nor the collision discount (2, which produces the step in the right place),
// nor the alignment phase (pinned by sync_dmc converging at all).
//
// THAT HYPOTHESIS WAS TESTED AND IS ALSO DEAD, and killing it produced the
// finding that matters.
//
// Removing the write-cycle gate entirely changes the table's SHAPE - 527/529
// alternating becomes a uniform 528 - but not its offset, which stays ~15 above
// the 512/513 baseline either way. So the start cycle is not it.
//
// The measurement that settles it: across the WHOLE timed section, from
// $4010 = $00 to the $4015 write that begins end_dmc_timer, there is EXACTLY
// ONE fetch, every iteration, walking from before the OAM DMA to inside it at
// iteration 6. One fetch costs 4 cycles. The table is 15-16 cycles high.
//
// SO THE OFFSET IS NOT CYCLES THE DMA STOLE. It cannot be - there are only four
// of them to steal.
//
// What that points at instead is the measurement. end_dmc_timer does not count
// cycles; it works out how long the section took by seeing where the DMC's own
// sample cycle has got to, modulo 3424. So an error in the DMC's PHASE is
// reported as an error in the elapsed time, with no extra cycles anywhere. And
// begin_dmc_timer sets that phase up in an awkward way on purpose: it writes
// $4010 = $00 mid-sample, switching the period from 54 to 428.
//
// One alternative was tried and is decisively wrong: making a $4010 write
// reload the timer immediately, rather than letting the new period take effect
// at the next reload, sends the first row from 527 to 125. So the current
// behaviour - period applies at the next reload - is right.
//
// AND THE BYTE PERIOD ITSELF IS EXACT. Measured directly at rate index 0, the
// gap between consecutive fetches is 3424 CPU cycles, twelve times in a row -
// the ROM's own dmc_timer_modulo, to the cycle. So the timer's base rate is not
// the phase error either.
//
// EVERY COMPONENT IS NOW MEASURED AND CORRECT, and the table is still ~11
// cycles high:
//
//   4 cycles per DMA           measured in isolation
//   2 cycles when colliding    puts the step in the right rows
//   get = odd half             or sync_dmc cannot converge at all
//   1 fetch in the section     counted end to end, and it walks correctly
//   3424-cycle byte period     to the cycle, matching dmc_timer_modulo
//   period applies at reload   the alternative is off by 400
//
// Going to the documentation rather than picking a seventh suspect found two
// corrections, one of which had been wrong from the very first attempt.
//
// A HALT ON A WRITE CYCLE IS NOT DEFERRED - IT IS CHEAPER. Every version here
// read the wiki's "if the CPU is writing, it ignores the halt...repeating until
// successful" as the DMA waiting for a read cycle. The nesdev forum is explicit
// that it does not: "DMC DMAs appear to try to halt during the 'put' phase -
// meaning they will take 4 cycles normally", and "when DMC halt happens 'on a
// write cycle', this makes it take 3 cycles because alignment can be skipped".
// The write makes the DMA shorter, not later.
//
// Implementing that fixes the table's SHAPE. Rows alternated 527/529 - a
// difference of 2 - where the 512/513 baseline alternates by 1. With the halt
// costing 3 on a write they alternate 527/528, matching the baseline's own
// pattern, and the collision step stays where it belongs.
//
// THE COLLISION MECHANISM IS ALSO MORE SPECIFIC THAN "PAUSE OAM DMA FOR 2".
// Fiskbit, same source: "The CPU is already halted during the first 3 due to
// OAM DMA, which is doing its reads and writes as normal, so they have no cost
// here. Then OAM DMA is suspended for 1 cycle while DMC DMA reads. Finally, OAM
// DMA resumes, but is misaligned, so it spends 1 cycle aligning." The total is
// the same 2 cycles this implements, but by a different route - OAM DMA keeps
// running through halt/dummy/alignment rather than being paused for them - and
// the difference would show anywhere the OAM DMA's own position matters.
//
// Two further cases from the same source, neither implemented nor yet needed by
// this ROM: a halt during the FIRST write of a read-modify-write still costs 4,
// and across the three consecutive writes of an interrupt it costs 3 on the
// first and third but 4 on the second.
//
// The ~11-cycle offset survives both corrections. Two more things were tried
// against it and neither is the cause:
//
//   Scheduling the halt only on a put cycle - the wiki says reload DMAs are
//   "scheduled to halt the CPU on a put cycle" - makes the shape WORSE, sending
//   the alternation back to 2. The other phase hangs the ROM outright. Starting
//   the halt on whatever cycle the request lands on is what matches best, which
//   is worth knowing because the wiki's wording suggests otherwise.
//
//   The two ROM variants were compared, and they do differ from each other: the
//   base steps at row 06, the _512 steps at row 08 in the opposite direction.
//   Both produce exactly two value-pairs, 527/528 uncollided and 525/526
//   collided. So the walk, the step and the discount are all responding; the
//   whole table is simply shifted.
//
// THE ANSWER IS PROBABLY THAT THERE ARE TWO KINDS OF DMC DMA, WITH INVERTED
// RULES, and this implementation has only ever had one. From erspicu/AprNes,
// whose write-ups document taking these exact ROMs from failing to 174/174:
//
//   Load DMA    triggered by a $4015 write. Schedules its halt at a GET cycle:
//               3 cycles normally, 4 if write-delayed.
//   Reload DMA  triggered by the sample buffer emptying. Schedules its halt at
//               a PUT cycle: 4 cycles normally, 3 if write-delayed.
//
// The Reload rule is the one already implemented here, arrived at from the
// nesdev forum - so that half is right. Load is its mirror image.
//
// IT HAS NOW BEEN IMPLEMENTED, AND IT CHANGES NOTHING HERE - which is a
// finding, not a disappointment. Classifying each fetch and inverting the
// write-delay rule for loads leaves the table byte-identical, because across
// 200 frames this ROM produces 349 reloads and ZERO loads. Every $4015 write it
// makes finds the sample buffer already full, so no load is ever scheduled.
//
// That is the interesting part. AprNes needed this same distinction to pass
// this same ROM, so hardware must be taking loads where we take reloads - and
// the difference is therefore not in the DMA rules, which now match the
// reference, but in WHEN OUR SAMPLE BUFFER IS FULL. A buffer that is occupied
// at a moment hardware's is empty turns every load into a reload, and a reload
// costs a cycle more.
//
// THE OCCUPANCY HAS NOW BEEN MEASURED, and it clears the channel of the charge
// while turning up a real bug elsewhere. Every $4015 write that enables the DMC
// during a 200-frame run, classified by the two conditions that decide what
// happens:
//
//   buffer full,  no bytes remaining   2335   restart, fetch deferred -> reload
//   buffer empty, no bytes remaining     32   restart + fetch now     -> LOAD
//   buffer full,  bytes remaining        32   no restart at all
//   buffer empty, bytes remaining         0
//
// So loads DO occur here - 32 of them - and the classifier that reported zero is
// simply wrong; its flag is cleared before the DMA reads it. That is a defect in
// the unlanded state machine, worth fixing when it lands.
//
// But it is not the offset. 32 loads in 2367 enables is 1.3%, and the timed
// section contains exactly one fetch per iteration, so getting every one of them
// wrong is worth at most a cycle - not eleven, and not uniformly across all
// sixteen rows. The dominant path by far is the 2335 case, and our treatment of
// it as a reload matches Mesen2: a $4015 write with the buffer already occupied
// schedules no transfer, and the fetch that eventually happens is a reload.
//
// The channel's occupancy is therefore correct, and is now pinned by
// tests/dmc_buffer_tests.cpp so it stays that way. The offset remains
// unexplained, and every subsystem reached by reasoning outward from the stall
// has now been measured and found right.
//
// Mesen2 sets the Load DMA's start delay by parity rather than by bus
// direction, which AprNes adopted as its final fix:
//
//     if((GetCpu()->GetCycleCount() & 0x01) == 0) _transferStartDelay = 2;
//     else                                        _transferStartDelay = 3;
//
// THE COLLISION COST IS NOT A FLAT 2 EITHER. AprNes: middle bytes cost 2 extra
// cycles, byte 254 costs 1, and byte 255 costs 3. A flat 2 is right for most of
// an OAM DMA and wrong at its end.
//
// AND THE BLANK ROMS ARE EXPLAINED. They are not waiting on the stall at all -
// they test PHANTOM READS, which are not implemented here: while the CPU is
// halted it repeatedly reads whatever address is on the bus, with $4016/$4017
// triggering only on the halt cycle and other registers on every no-op cycle.
// AprNes lists "$4016 phantom read not implemented" as the cause of
// dma_4016_read failing, which is one of the three that draw nothing here.
//
// double_2007_read IS NOT A DMC PROBLEM AT ALL, and the evidence is exact: the
// CRC D84F6815 recorded in this file's baseline is the same value AprNes
// reported before fixing a PPU bug. The expected CRCs are 85CFD627, F018C287,
// 440EF923 or E52F41A5 - four of them, because CPU/PPU sync varies - and the
// cause is a missing ~6-dot cooldown after a $2007 read, during which a second
// read returns open bus without swapping the buffer or incrementing the VRAM
// address. That is a PPU feature, filed here only because this suite is where
// it shows.
//
// christopherpow/nes-test-roms carries a status.txt listing dma_2007_read's
// expected CRC as 5E3DF9C4, and notes these CRC-only ROMs never print "Passed".
//
// RECOVERING THIS ROM'S OWN EXPECTED TABLE FROM ITS CRC WAS ATTEMPTED AND DID
// NOT WORK. The failure is recorded so the next attempt starts past it.
//
// Established, and reusable: update_crc is at $E788, found by searching PRG for
// the reflected polynomial's own bytes - EOR #$ED and EOR #$B8 - rather than by
// guessing. Its guard is "bit checksum_off_ / bmi" with checksum_off_ at zero
// page $17, and $E78C is update_crc_, the entry PAST that guard. Callers use
// $E78C almost exclusively: hooking $E788 alone caught 2 bytes in a whole run,
// hooking $E78C caught 4207.
//
// Not established: that the capture is what the checksum actually consumes. No
// prefix of those 4207 bytes reproduces the $3AB91D8C this ROM prints here, and
// neither does any sub-range - every window was checked. So bytes are missing
// or extra ones are included, and searching an unvalidated stream for
// $FBADA48D would produce a number with nothing behind it.
//
// Three wrong turns, each cheap to repeat by accident: a hook gated on a
// cycle-phase condition that was off by one caught nothing; treating $E78C as a
// second entry point double-counted every call, since it sits three bytes
// inside $E788's own routine; and applying the checksum_off_ filter at $E78C is
// wrong, because reaching it means the guard has already been passed.
//
// The approach is still sound - the search space is genuinely small, being two
// alternating values and a step position - but the capture must reproduce a
// checksum already known before it can be trusted to find one that is not.
//
// ONE MORE MEASUREMENT, AND A DECISION TO STOP SWEEPING.
//
// The cycles the CPU actually loses inside the timed section were counted, and
// they are exactly right: 517 with a 513-cycle OAM DMA and 515 when the DMC
// collides, which is 4 and 2 as documented. So the stall costs what it should,
// and the ~11-cycle excess in the printed number is provably NOT stolen cycles.
//
// The cost was then swept anyway, 1 to 6, with the collision cost swept 0 to 3
// alongside it. Nothing passes. Only two produce anything coherent at all:
//
//   4 cycles  527/528 stepping to 525/526 - right shape, wrong offset
//   1 cycle   a flat 514 on every row - right magnitude, no alternation and no
//             step, so the structure the ROM is built to expose is gone
//
//   3, 5 and 6 hang the ROM outright; 2 reproduces the original 783.
//
// That is where this stops. Sweeping constants until a ROM passes is the
// failure mode this repository exists to prevent - it cannot distinguish a
// lucky number from a correct one, and the 4-cycle version is the one the
// documentation and the shape both support. The offset needs an explanation,
// not a fitted parameter.
//
// AND THE EXPLANATION HAS A NAME: A TRANSFER START DELAY, which this
// implementation does not have at all.
//
// NESdev's DMA page carries cycle-by-cycle example listings, and the categories
// alone settle it. It documents "DMC DMA load examples with various delay
// states (0, 1, 2, 3 cycles)" and "DMC reload examples at different delay
// stages" - so a DMA does not begin on the cycle it is wanted. It is SCHEDULED,
// with a phase-dependent delay of up to 3 cycles before the halt is even
// attempted. Mesen2 has the same thing under the name _transferStartDelay,
// setting it to 2 or 3 from cycle parity.
//
// Ours starts the instant dmc_wants_sample_byte() goes true. That is a fixed
// shift in WHEN the fetch lands relative to the DMC's own sample clock, and it
// costs no cycles - which is exactly the shape of the residual: constant,
// surviving the collision step, and invisible to every measurement of how many
// cycles the CPU loses.
//
// The same page also breaks the OAM DMA collision down by position - "at start,
// middle, second-to-last, and last put positions" - which matches AprNes's 2 /
// 1 / 3 split and is finer than the flat 2 implemented here. And it documents
// DMC DMA bugs this emulator has never modelled: aborted DMAs from an explicit
// stop, implicit-stop variations, and register conflicts with $2007 and the
// joypad ports. Those last are almost certainly why dma_2007_read,
// dma_2007_write and dma_4016_read draw nothing.
//
// THE LISTINGS HAVE NOW BEEN READ, via the wiki's action=raw source rather than
// the rendered page, and the model they give is more specific than anything
// used here before. The essential correction:
//
//   The 3-or-4 cycle length is NOT a rule of its own. It falls out of which
//   PHASE the halt lands on. A load halts on a GET cycle, so its dummy lands on
//   a put and the following get needs no alignment - three cycles. A reload
//   halts on a PUT, so its dummy lands on a get, an alignment cycle is spent,
//   and the read follows - four cycles. Load DMAs are additionally scheduled
//   for the 2nd APU cycle after the $4015 write.
//
//   During OAM DMA the common case is 2 cycles - one for the DMC get, one for
//   OAM DMA to realign - but at the second-to-last put it costs 1, and at the
//   last put it extends 3 cycles past the end of the transfer.
//
// That was implemented faithfully: phase-scheduled halts, the length left to
// emerge from the alignment rule rather than being set from the write flag.
// THE TABLE DID NOT MOVE. Still 527/528 stepping to 525/526, still about eleven
// high, on rows with no collision as much as on rows with one.
//
// Since the offset is present on rows where no DMC DMA touches the OAM DMA at
// all, the collision rules cannot account for it either, and those are the last
// part of the DMA model that was still approximated here.
// ---------------------------------------------------------------------------
// AUDIT OF THE CLAIMS ABOVE. Every figure in this file was re-examined for how
// it was actually obtained, after several turned out to rest on a broken
// instrument rather than on a measurement. Sorted by how much weight each can
// carry.
//
// MEASURED, with the instrument checked against something already known:
//
//   3424-cycle byte period at rate 0    intervals between consecutive fetches,
//                                       twelve in a row, all identical
//   OAM DMA 513 even / 514 odd          read from PPU::remaining_dma_cycles and
//                                       pinned in oam_dma_timing_tests.cpp
//   one fetch in the timed section      counted end to end, walking correctly
//   32 loads against 2335 reloads       counted at every $4015 enable
//   the CRC capture is faithful         its running value tracks the ROM's own
//                                       checksum with zero divergence across
//                                       4203 calls
//   a load costs 3 cycles, a reload 4   47 stolen across 12 counted fetches,
//                                       which is 3 + 11 x 4 exactly
//
// SOURCED, from documentation rather than measured here:
//
//   load halts on a get, reload on a put         NESdev DMA page
//   collision costs 2, or 1 and 3 at the end     NESdev DMA page, AprNes
//   transfer start delay of 2 or 3 by parity     Mesen2 DeltaModulationChannel
//   accepted CRCs for the CRC-only ROMs          AprNes' test catalogue
//
// ASSUMPTIONS AND WEAK CLAIMS - these are the ones to distrust:
//
//   "the expected table is ~515-517"   NEVER MEASURED. It is an inference from
//     a forum paraphrase of how many cycles a DMA adds, and no published
//     expected table exists for this ROM - it self-checks. Everything phrased
//     as "about eleven cycles high" depends on it, so the SIZE of the residual
//     is unfounded even though its existence is not: the ROM does report a
//     failure.
//
//   "512/513 is the correct baseline"   Our own output with a zero-cost fetch,
//     never compared against hardware or another emulator. If that baseline is
//     itself wrong, the residual is measuring the wrong thing entirely.
//
//   "the get cycle is the odd half of cpu_cycles"   Inferred from sync_dmc
//     converging with one phase and hanging with the other. That is strong
//     evidence of a phase relationship but it is OUR convention being fitted to
//     the ROM, not a documented mapping.
//
//   "the checksum starts at $FFFFFFFF"   Observed once, before the first
//     update_crc call. It may equally be uninitialised RAM rather than intent;
//     reset_crc as published sets zero.
//
// DO NOT TRY TO RECOVER THE EXPECTED TABLE FROM THE CRC. It was attempted and
// it cannot work, for a reason that is arithmetic rather than practical.
//
// The sixteen printed values are three ASCII digits each, so substituting them
// leaves the message length fixed, and CRC-32 is linear over a fixed length -
// which makes the search tractable by meet-in-the-middle rather than brute
// force. That is the appealing part, and it is a trap. Allowing each row a
// delta of 0-5 gives 6^16 candidates against a 32-bit constraint, so roughly
// 6^16 / 2^32 = 657 different tables satisfy the target checksum. A match
// therefore carries almost no information: the first one found had deltas of
// 3,4,5,3,2,3,2,4,0,0,0,0,2,1,3,3, which requires a DMC DMA to cost nothing at
// all on four consecutive rows.
//
// Widening the per-row range makes it worse, and narrowing it enough to be
// unique means already knowing the answer. A 32-bit checksum simply does not
// carry enough information to identify 16 unknown values.
//
// A FOURTH ASSUMPTION, NOW DISPROVEN: that the 8-hex-digit value this ROM
// prints under its table is the checksum check_crc compares. It is not.
//
// The CRC model here is validated to the byte - its running value tracks the
// ROM's own zero-page checksum with zero divergence across all 4203 updates,
// and reproduces the end-of-run state exactly (EB161E94 both ways). With that
// established, every checksum state the ROM passes through was recorded, and
// the printed value appears in NONE of them.
//
// So reverse-engineering the expected table from that number was never going to
// work, whatever the capture did - the number is not what it was assumed to be.
// What it actually is remains open; check_crc_ on failure does print_newline,
// print_crc, test_failed, and print_crc reads checksum,x with the CRC disabled,
// so on the face of it it should be a checksum state.
//
// AND ONE CORRECTED OUTRIGHT: an earlier version of this file said a DMA costs
// "exactly 4.00 cycles, 48 stolen across 12 fetches". The probe behind it
// computed the fetch count as stolen/4 and then printed a hard-coded 4.00 - it
// could not have produced any other answer. Counting fetches independently
// gives 47 over 12, which is 3 + 11 x 4: one load and eleven reloads, at the
// documented 3 and 4. The corrected figure is better evidence than the wrong
// one was, because it distinguishes the two kinds.
// ---------------------------------------------------------------------------
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nametable_screen.h"
#include "rom_fixture.h"

namespace tests
{
namespace dmc_dma
{
namespace
{

std::string rom_path(const std::string& name) { return std::string(NES_TEST_FILES_DIR) + "/dmc_dma/" + name + ".nes"; }

constexpr const char* kFetch = "run tests/test_files/fetch_dmc_dma.sh";

// Measured: read_write_2007 settles at frame 14, the sprdma pair at 157. 900 is
// far above that on purpose - these are a FLOOR and rise as ROMs get further.
constexpr uint64_t kMaxFrames = 900;
constexpr uint64_t kSettleFrames = 90;

#define REQUIRE_DMC_DMA_ROM(name) REQUIRE_ROM(rom_path(name), kFetch)

// Runs until the whole screen stops changing. The whole screen, not the first
// row: these ROMs print a table and then a verdict underneath it, so a reader
// watching one row would stop while the table was still filling.
std::string run_until_settled(const std::string& name)
{
    Bus console;
    EXPECT_TRUE(console.load_cartridge(rom_path(name))) << "the ROM is present but did not load: " << rom_path(name);
    console.cpu.reset();

    std::string last;
    uint64_t stable = 0;
    for (uint64_t frame = 0; frame < kMaxFrames; ++frame) {
        nametable_screen::run_one_frame(console);

        std::string text = nametable_screen::read_text(console);
        if (text == last) {
            if (++stable >= kSettleFrames) {
                break;
            }
            continue;
        }
        stable = 0;
        last = std::move(text);
    }
    return last;
}

bool screen_says(const std::string& screen, const std::string& word) { return screen.find(word) != std::string::npos; }

// The sixteen "NN NNN" rows of the table, as integers. Returns what it found
// rather than asserting, so the caller can report the whole screen when the row
// count is wrong - a ROM that printed nothing and one that printed a bad table
// are different failures.
std::vector<int> table_lengths(const std::string& screen)
{
    std::vector<int> out;
    std::istringstream lines(screen);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string index;
        int length = 0;
        // "0A 513" - a two-digit hex index then the decimal length.
        if ((fields >> index >> length) && index.size() == 2 && std::isxdigit(static_cast<unsigned char>(index[0])) &&
            std::isxdigit(static_cast<unsigned char>(index[1]))) {
            out.push_back(length);
        }
    }
    return out;
}

}  // namespace

// --- what already works ------------------------------------------------------

GTEST_TEST(dmcDma, a_2007_read_and_write_around_a_dma_still_pass)
{
    REQUIRE_DMC_DMA_ROM("read_write_2007");

    const std::string screen = run_until_settled("read_write_2007");

    EXPECT_TRUE(screen_says(screen, "Passed")) << "read_write_2007 passed before the DMC stall existed and must keep\n"
                                                  "  passing after it. Full screen:\n"
                                               << screen;
}

// --- the queue ---------------------------------------------------------------

// Pinned to its current failure, like every other unfinished feature here. When
// the stall lands this fails, and the message says to promote it.
class SprdmaAndDmcDma : public ::testing::TestWithParam<const char*>
{
};

TEST_P(SprdmaAndDmcDma, is_blocked_on_the_cpu_stall)
{
    const std::string name = GetParam();
    REQUIRE_DMC_DMA_ROM(name);

    const std::string screen = run_until_settled(name);

    ASSERT_TRUE(screen_says(screen, "Failed") || screen_says(screen, "Passed"))
        << name
        << " reached no verdict at all, which is neither the recorded failure nor a\n"
           "  pass. Full screen:\n"
        << screen;

    EXPECT_TRUE(screen_says(screen, "Failed"))
        << name
        << " now PASSES. The DMC's CPU stall must be implemented: delete this pin\n"
           "  and assert the pass instead. Full screen:\n"
        << screen;

    // The failure is pinned to its CAUSE, not just its verdict: every row of the
    // table is asserted at the value this emulator currently produces.
    //
    // WHY EXACT VALUES RATHER THAN "512 OR 513". This pin used to require every
    // row to be a plain uninterrupted OAM DMA length, on the reasoning that the
    // DMC's CPU stall was absent. It is not absent - it is present and
    // imperfect, so every row reads 525-528, and the pin failed on all sixteen.
    //
    // That failure was left standing, and it turned CI red on every push for six
    // weeks. A permanently-red pipeline is worse than none: it hides the next
    // real regression behind a failure everyone has learned to scroll past,
    // which is the same false signal this project rejects in the other
    // direction. Pinning the measured values is what the rest of this repository
    // already does with a known divergence - opcode $AB in instr_test_roms.cpp
    // asserts blargg's ROM fails on exactly ATX, and 6-MMC3_alt is asserted to
    // fail on exactly its subtest. Neither is "ignored"; both are nailed down so
    // that any MOVEMENT surfaces.
    //
    // So these tables are the divergence, recorded. Hardware would give 512 or
    // 513 with no DMC interference and a correct stall gives documented longer
    // figures; this gives neither. Any change to the DMC's timing shifts a row
    // and fails here with the row named, which is the signal worth having.
    //
    // Each row is parsed, rather than searching the whole screen for "512" and
    // "513" as an earlier version did. That search passed on any screen
    // containing one row of each - a table of fourteen 783s with a single 512
    // and 513 satisfied it - and let a wrong stall through undetected in an
    // adversarial review.
    //
    // MEASURED 2026-08-29. Delete the whole pin when the stall is implemented
    // properly, and assert the pass instead; do not "update" these numbers to
    // make a red run green without understanding which row moved and why.
    static const std::map<std::string, std::vector<int>> kMeasured = {
        {"sprdma_and_dmc_dma", {527, 528, 527, 528, 527, 528, 525, 526, 525, 526, 525, 526, 525, 526, 525, 526}},
        {"sprdma_and_dmc_dma_512", {525, 526, 525, 526, 525, 526, 525, 526, 527, 528, 527, 528, 527, 528, 527, 528}},
    };

    const std::vector<int> lengths = table_lengths(screen);
    ASSERT_EQ(16u, lengths.size()) << name << ": expected 16 table rows, parsed " << lengths.size()
                                   << ". Full screen:\n"
                                   << screen;

    const auto expected = kMeasured.find(name);
    ASSERT_NE(kMeasured.end(), expected) << name << " has no recorded table; add one rather than skipping the check";

    for (size_t row = 0; row < lengths.size(); ++row) {
        EXPECT_EQ(expected->second[row], lengths[row])
            << name << " row " << row << " reads " << lengths[row] << ", not the recorded " << expected->second[row]
            << ".\n"
               "  This is the parked DMC/OAM collision divergence, and a row moving means the\n"
               "  DMC's timing changed. Hardware gives 512 or 513 with no interference; if the\n"
               "  stall is now correct, delete this pin and assert the ROM's pass instead.\n"
               "  Full screen:\n"
            << screen;
    }
}

INSTANTIATE_TEST_SUITE_P(DmcDma,
                         SprdmaAndDmcDma,
                         ::testing::Values("sprdma_and_dmc_dma", "sprdma_and_dmc_dma_512"),
                         [](const ::testing::TestParamInfo<const char*>& info) { return std::string(info.param); });

}  // namespace dmc_dma
}  // namespace tests
