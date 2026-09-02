#pragma once
#include <cassert>
#include <cstdint>

#include "device.h"

// The frame counter is a divider driving a 4- or 5-step sequence. Quarter-frame
// steps clock the envelopes and the triangle's linear counter; half-frame steps
// clock the length counters and the sweeps. In 4-step mode the final step also
// asserts /IRQ unless inhibited - the NES's only source of maskable interrupts,
// and the reason this was built before anything made a sound: the CPU's /IRQ
// path is otherwise untestable, the SingleStepTests vectors carrying none and
// the PPU driving only /NMI.
//
// All five channels and the non-linear mixer are here. The one piece that is
// not is the DMC's CPU stall - see the parked rows in dmc_dma_tests.cpp.
//
// MOST OF THIS HAS NO ORACLE. The envelope, the sweep, the waveform generators
// and the mixer are invisible to the CPU, so no test ROM can reach them; the
// accessors below exist for that reason and the coverage is unit tests, cited
// documentation and mutation. What DOES have one is the channel balance, which
// volume_tests measures against a recording from real hardware.
class APU : public Device
{
public:
    APU(Bus* b) : Device{b} {};

    void write(const uint16_t addr, const uint8_t data);
    uint8_t read(const uint16_t addr);

    // Once per CPU cycle. The frame counter is specified in CPU cycles even
    // though it physically divides them down, because the step boundaries fall
    // on half-cycles and integer CPU cycles are the honest unit.
    void clock();

    // Power-on is not the same as reset, and is not "everything zero". Blargg's
    // apu_reset readme states it exactly: at power it is as if $00 were written
    // to $4017, then a 9-12 clock delay, then execution from the reset vector.
    // Without this the frame IRQ arrives far too early - apu_reset/4017_timing
    // measured 4 against hardware's 9-12 - and $4017/$4015 are not usable from
    // the first instruction.
    void power_on();

    // RESET is the same shape as power-on, with two differences the apu_reset
    // readme spells out:
    //
    //   "At reset, same as above, except last value written to $4017 is written
    //    again, rather than $00."
    //   "At power and reset, $4015 is cleared" / "IRQ flag is clear".
    //
    // So the mode survives a reset while the divider does not, which is why
    // last_4017_write exists at all. Everything else the channels hold - the
    // halt bits, the timers - is deliberately untouched: the readme lists what
    // reset clears, and it is a short list.
    void reset();

    // Why 10, and not the 9 that blargg's readme calls typical.
    //
    // 4017_timing PRINTS the delay it measures and accepts the whole 9-12
    // window, so it does not choose between them: 9, 10 and 12 all take it to
    // $81. What does choose is cpu_interrupts_v2/4-irq_and_dma, and it is
    // unambiguous - measured across 8, 9, 10, 11 and 12, every EVEN delay
    // passes and every odd one fails.
    //
    // The mechanism is parity, not magnitude. apu_cycles counts CPU cycles from
    // power-on and its low bit selects write_delay_odd/even_cycle for every
    // later $4017 write, so an odd-length power-on delay inverts the CPU/APU
    // phase for the rest of the run and shifts each divider reset by a cycle.
    // 4-irq_and_dma is an IRQ-timing ROM driven by this very counter, so it
    // sees that directly. Three testAPU cases fail on an odd delay for the same
    // reason - they were not encoding a stale power-on assumption, they were
    // detecting the phase inversion.
    //
    // So the admissible values are the even ones inside blargg's window, 10 and
    // 12, and 10 is the nearer to 9.
    static constexpr int power_on_delay = 10;

    enum RegisterMMap : uint16_t {
        APUSTATUS = 0x4015,
        FRAMECOUNTER = 0x4017,
    };

    bool frame_irq_asserted() const { return frame_irq_flag; }

    // The four channels that HAVE a length counter, in $4015 bit order. The DMC
    // is bit 4 of that register but counts bytes remaining rather than length,
    // so it is deliberately not one of these.
    enum Channel : int { pulse1 = 0, pulse2 = 1, triangle = 2, noise = 3, length_channels = 4 };

    // Non-destructive, for tests. Reading $4015 reports the same bits, but that
    // read also acknowledges the frame interrupt - an observer using it would
    // swallow an IRQ the program was waiting for. Same rule as the controller's
    // shift register.
    uint8_t length_counter(const int channel) const
    {
        // Asserted rather than clamped: a caller asking for channel 4 has
        // confused the length counters with $4015's five bits, where bit 4 is
        // the DMC. Clamping would answer that question with a plausible number
        // instead of stopping. Asserts are live in the default Checked build,
        // which is what makes this worth writing.
        assert(channel >= 0 && channel < length_channels);
        return lengths[channel].value;
    }

    // Non-destructive, for tests, and for the same reason length_counter() is:
    // $4015 reports bytes-remaining in bit 4, but reading it also acknowledges
    // the frame interrupt, so an observer using that would swallow an IRQ the
    // program was waiting for.
    //
    // The buffer's occupancy is not reported by any register at all, and it is
    // the thing that decides whether a $4015 write schedules a load DMA or a
    // reload - two fetches that cost different numbers of cycles. That makes it
    // worth being able to assert on directly rather than only through timing.
    uint16_t dmc_bytes_remaining() const { return dmc.bytes_remaining; }

    // Inspection for the units NO REGISTER REPORTS. The length counters get
    // accessors because reading $4015 would acknowledge an IRQ; these get them
    // because there is nothing to read at all - the envelope, the sweep and the
    // linear counter are invisible to the CPU, which is precisely why no test
    // ROM can cover them and why the tests that do have to reach in here.
    uint8_t envelope_volume(const int channel) const
    {
        assert(channel == pulse1 || channel == pulse2 || channel == noise);
        return envelopes[envelope_index(channel)].volume();
    }
    uint8_t envelope_decay(const int channel) const
    {
        assert(channel == pulse1 || channel == pulse2 || channel == noise);
        return envelopes[envelope_index(channel)].decay;
    }
    uint8_t linear_counter_value() const { return linear_counter; }
    uint16_t pulse_timer_period(const int pulse) const
    {
        assert(pulse == pulse1 || pulse == pulse2);
        return pulse_period[pulse];
    }
    uint16_t sweep_target_period(const int pulse) const
    {
        assert(pulse == pulse1 || pulse == pulse2);
        return sweep_target(pulse);
    }
    bool sweep_is_muting(const int pulse) const
    {
        assert(pulse == pulse1 || pulse == pulse2);
        return sweep_muting(pulse);
    }

    // The waveform generators' live output, upstream of the mixer. Same
    // justification as the accessors above: nothing in the register file reports
    // any of this.
    //
    // pulse_output is the DUTY BIT, not the volume. The gating that turns it into
    // a mixer level - length counter, sweep muting, envelope - is deliberately
    // not folded in, so a test of the sequencer is a test of the sequencer.
    // pulse_level() is the gated one.
    uint8_t pulse_output(const int pulse) const
    {
        assert(pulse == pulse1 || pulse == pulse2);
        return kPulseDuty[pulses[pulse].duty][pulses[pulse].sequence];
    }
    uint8_t pulse_sequence_position(const int pulse) const
    {
        assert(pulse == pulse1 || pulse == pulse2);
        return pulses[pulse].sequence;
    }
    uint8_t triangle_output() const { return kTriangleSequence[triangle_sequence]; }

    // The INDEX, not the value. The sequence visits 15 and 0 twice each, so a
    // test that walked to an output value could not tell which half of the
    // ramp it was on.
    uint8_t triangle_sequence_position() const { return triangle_sequence; }
    uint16_t noise_shift_register() const { return noise_shift; }

    // Bit 0 SET means silence, which is the opposite polarity from the pulse
    // channels' "sequencer output is zero". Exposed as its own question rather
    // than left for a caller to get backwards.
    //
    // THIS IS HALF THE MIXER'S CONDITION, not all of it. nesdev lists two: "Bit
    // 0 of the shift register is set, or The length counter is zero." Only the
    // first is here, for the same reason pulse_output() returns a raw duty bit:
    // the gating belongs to noise_level(). A mixer using this as its whole gate
    // would emit noise from a channel whose length counter has expired, so the
    // name is about the SHIFT REGISTER's contribution and nothing more.
    bool noise_output_is_silent() const { return (noise_shift & 1u) != 0; }

    // --- the mixer ----------------------------------------------------------
    //
    // What each channel actually presents to the mixer, after its gates. These
    // are the levels the formula below consumes, and they are separate from the
    // raw sequencer accessors above on purpose: pulse_output() answers "what is
    // the duty bit", these answer "what does the DAC see".
    //
    // PULSE. nesdev: "The mixer receives the pulse channel's current envelope
    // volume except when: The sequencer output is zero, or overflow from the
    // sweep unit's adder is silencing the channel, or the length counter is
    // zero, or the timer has a value less than eight. If any of the above are
    // true, then the pulse channel sends zero (silence) to the mixer." The last
    // two of those four are both sweep_muting(), which already tests period < 8
    // and target > $7FF.
    uint8_t pulse_level(const int pulse) const
    {
        assert(pulse == pulse1 || pulse == pulse2);
        if (pulse_output(pulse) == 0 || lengths[pulse].value == 0 || sweep_muting(pulse)) {
            return 0;
        }
        return envelopes[envelope_index(pulse)].volume();
    }

    // TRIANGLE, and it is NOT gated here - deliberately. Its silencing works by
    // stopping the sequencer, which then holds its last step, so the value the
    // mixer receives when the channel is "silent" is that held step and not
    // zero. Gating it to zero here would be the same mistake as clocking it at
    // the APU rate: it would sound approximately right and be wrong in the way
    // the hardware is audibly not.
    uint8_t triangle_level() const { return triangle_output(); }

    // NOISE. "The mixer receives the current envelope volume except when: Bit 0
    // of the shift register is set, or The length counter is zero." Both, which
    // is what noise_output_is_silent() deliberately does not cover on its own.
    uint8_t noise_level() const
    {
        if (noise_output_is_silent() || lengths[noise].value == 0) {
            return 0;
        }
        return envelopes[envelope_index(noise)].volume();
    }

    // DMC. Seven bits, 0-127, and it is the channel's output level directly -
    // there is no gate. Note the RANGE: the other three are 0-15, and treating
    // this one as 4-bit collapses the tnd group.
    uint8_t dmc_level() const { return dmc.output_level; }

    // The combined analogue output, normalised to 0.0-1.0.
    //
    // THE EXACT FORM, not the lookup tables. nesdev gives both, and the tables
    // are explicitly an approximation - "The tnd_out table is approximated
    // (within 4%)" - because they collapse three independent variables into the
    // single index 3*triangle + 2*noise + dmc. That is a real loss: the exact
    // tnd_out is a function of three reciprocals and does not factor through any
    // one index. This project spends cycles for exactness elsewhere; a 4% error
    // in the one place the whole APU becomes audible is not the place to stop.
    //
    // The tables also carry deliberately different numerators (95.52 and 163.67
    // against 95.88 and 159.79) to renormalise after that approximation, which
    // is worth knowing before anyone "corrects" one to match the other.
    float mixer_output() const;

    // The formula alone, on levels handed in rather than read from live state.
    //
    // Split out so it can be tested at inputs the channels cannot easily be
    // driven to - every DMC level from 0 to 127, a group at exactly zero, one
    // channel at a time. Reaching those through the register file would mean a
    // test of the mixer that is mostly a test of the sequencers.
    // The parameter names avoid `triangle`, `noise` and `dmc`: the first two are
    // Channel enumerators and the third is a member, and this header already
    // documents what that collision costs elsewhere.
    static float mix_levels(uint8_t pulse1_level,
                            uint8_t pulse2_level,
                            uint8_t triangle_value,
                            uint8_t noise_value,
                            uint8_t dmc_value);

    // The memory reader, driven by the Bus: only the Bus can halt the CPU.
    //
    // A transfer is REQUESTED rather than inferred from the buffer being empty.
    // A $4015 enable does not start one on the spot - it schedules it 2 or 3
    // cycles later depending on cycle parity - while the output unit emptying
    // the buffer starts one at once. Mesen2 calls the first _transferStartDelay
    // and matches dmc_dma_start_test with it.
    bool dmc_wants_sample_byte() const { return dmc.transfer_requested; }
    uint16_t dmc_sample_address() const { return dmc.current_address; }
    void dmc_deliver_sample_byte(uint8_t data);
    void dmc_cancel_transfer() { dmc.transfer_requested = false; }

    // Which kind is pending. A load halts on a GET cycle and so takes 3 cycles;
    // a reload halts on a PUT and takes 4, because its dummy then lands on a
    // get and an alignment cycle is needed before the read. The count is a
    // consequence of the phase, not a separate rule.
    bool dmc_transfer_is_load() const { return dmc.transfer_is_load; }
    bool dmc_sample_buffer_filled() const { return dmc.sample_buffer_filled; }

    // Sequence lengths in CPU cycles. Mode 0 asserts /IRQ across its last three
    // cycles - not on one of them - which is why a read of $4015 placed
    // anywhere in that window sees the flag.
    static constexpr uint32_t mode0_length = 29830;
    static constexpr uint32_t mode1_length = 37282;

    // The first cycle of mode 0's three-cycle IRQ window. Distinct from
    // mode0_length: that is the period, this is when the flag appears.
    static constexpr uint32_t mode0_irq_cycle = 29828;

    // A $4017 write resets the divider 3 or 4 CPU cycles later, depending on
    // the parity of the cycle it landed on. APU::clock has already ticked for
    // the write cycle by the time the store runs, so the first decrement is on
    // the cycle after the write and these are the hardware figures unmodified.
    static constexpr int8_t write_delay_odd_cycle = 3;
    static constexpr int8_t write_delay_even_cycle = 4;

private:
    // One channel's length counter: how many half-frame clocks the channel has
    // left before it silences itself.
    //
    // `enabled` is a separate flag rather than being folded into `value`,
    // because $4015 distinguishes them. Clearing the enable zeroes the counter
    // AND blocks any later reload, so a channel disabled at $4015 stays silent
    // however many times $4003 is written - which is blargg's 1-len_ctr #7,
    // "when disabled via $4015, length shouldn't allow reloading".
    struct LengthCounter {
        uint8_t value = 0;
        bool halt = false;
        bool enabled = false;

        // A halt write does not reach the counter on the cycle it is made.
        // blargg's 10.len_halt_timing states the rule as "changes to length
        // counter halt occur after clocking length, not before", and pins it
        // one cycle wide: halting at 14914 stops the clock, halting at 14915
        // does not. Applying it at the store put our boundary a cycle early.
        //
        // Moving the CLOCK instead was tried and is wrong - it fixes this ROM
        // and breaks 05/06 with "first length is clocked too soon", which is
        // how we know the sequencer's own timing was never the problem.
        bool pending_halt = false;
        bool halt_write_pending = false;

        // A length load is deferred the same way, and for the same reason, but
        // it also carries a CONDITION. 11.len_reload_timing: "write to length
        // counter reload should be ignored when made during length counter
        // clocking and the length counter is not zero" - honoured at zero,
        // suppressed otherwise. The decision uses the value the counter held
        // BEFORE that clock, because on hardware both reach the unit together
        // and the decrement is what suppresses the reload.
        uint8_t pending_load = 0;
        bool load_write_pending = false;
        bool clocked_this_cycle = false;
        uint8_t value_before_clock = 0;
    };

    // The envelope generator: pulse 1, pulse 2 and the noise channel have one.
    //
    // NO ROM ORACLE COVERS THIS UNIT, and that is a statement about the whole
    // catalogue rather than an admission about this file. Nothing reads an
    // envelope back - the CPU cannot see a decay level through any register - so
    // the 25 APU ROMs that pass here cannot test it, and apu_mixer and
    // volume_tests are listening tests (see apu_rom_tests.cpp). The algorithm
    // below is transcribed from two independent sources that agree word for
    // word, and the tests are checked by mutation instead of by hardware:
    //
    //   nesdev https://www.nesdev.org/wiki/APU_Envelope
    //   blargg https://www.nesdev.org/apu_ref.txt
    //
    // ONE PART OF IT IS ORACLE-COVERED, though, and it is the part most likely
    // to be wired wrong: the loop flag IS the length counter's halt flag, the
    // same physical bit 5 of $4000/$4004/$400C. So a mistake there moves a
    // length counter, and blargg's 1-len_ctr and 01.len_ctr would catch it.
    // That is why this struct does not own the bit - LengthCounter::halt does,
    // and the envelope reads it.
    struct Envelope {
        // "if the start flag is clear, the divider is clocked, otherwise the
        // start flag is cleared, the decay level counter is loaded with 15, and
        // the divider's period is immediately reloaded". Set by a write to the
        // channel's FOURTH register ($4003/$4007/$400F), and consumed on the
        // next quarter-frame clock rather than at the write.
        bool start = false;

        // Bits 3-0 of $4000/$4004/$400C. One field, two jobs: the divider's
        // period (reloading to V, so V+1 quarter-frames per decay step) and,
        // when constant_volume is set, the output level itself.
        uint8_t period = 0;

        uint8_t divider = 0;
        uint8_t decay = 0;

        // Bit 4. "The constant volume flag has no effect besides selecting the
        // volume source; the decay level will still be updated when constant
        // volume is selected." Gating the clock on this bit is the trap.
        bool constant_volume = false;

        // What the mixer receives. `loop` is not held here - it is the length
        // counter's halt flag, passed in, for the reason in the comment above.
        uint8_t volume() const { return constant_volume ? period : decay; }
    };

    // The sweep unit: pulse 1 and pulse 2 only ($4001/$4005).
    //
    // Also unreachable by any oracle here, and transcribed from the same two
    // sources - https://www.nesdev.org/wiki/APU_Sweep and blargg's apu_ref.txt.
    //
    // THEY DO NOT AGREE WORD FOR WORD ON THIS UNIT, unlike the envelope and the
    // linear counter, and saying they did was an overclaim. blargg describes the
    // divider as "first clocked and then if there was a write to the sweep
    // register since the last sweep clock, the divider is reset" - clock-then-
    // reset, where nesdev is test-then-reload - and states the mute condition on
    // "the result of the shifter" rather than on a target period. The two are
    // behaviourally equivalent, because blargg's "clocking a divider" includes
    // its reload at zero. Named rather than smoothed over, per this repo's rule
    // about divergent sources.
    //
    // They do agree exactly on the part that is easiest to invert:
    //
    //   PULSE 1 ADDS THE ONES' COMPLEMENT (-c - 1), PULSE 2 THE TWO'S (-c).
    //
    // nesdev: "Making 20 negative produces a change amount of -21" on pulse 1
    // and "-20" on pulse 2. blargg reaches the same asymmetry from the other
    // end: "on the second square channel, the inverted value is incremented by
    // 1". Two independent statements of one fact, which is why it is written
    // down here rather than left to the code to imply.
    struct Sweep {
        bool enabled = false;
        uint8_t period = 0;   // bits 6-4; the divider reloads to this, so P+1 clocks
        bool negate = false;  // bit 3
        uint8_t shift = 0;    // bits 2-0
        uint8_t divider = 0;
        bool reload = false;  // set by any write to $4001/$4005
    };

    // The pulse channels' waveform generator: an 11-bit down-counter driving an
    // 8-step duty sequencer.
    //
    // THE SEQUENCER COUNTS DOWN, which is the part published tables disagree
    // about and the reason the raw sequences below look wrong. nesdev: "the
    // counter is initialized to zero but counts downward rather than upward.
    // Thus it reads the sequence lookup table in the order 0, 7, 6, 5, 4, 3, 2,
    // 1." Store the table as written and step the index backwards and the
    // OUTPUT comes out in the order blargg's waveform diagrams draw - which is
    // how this was cross-checked, since blargg gives pictures rather than a
    // direction. Duty 0 emits 0 1 0 0 0 0 0 0 and duty 3 emits 1 0 0 1 1 1 1 1;
    // both are asymmetric, so a table read the wrong way round is visible in
    // them and not in duties 1 and 2.
    //
    // https://www.nesdev.org/wiki/APU_Pulse
    struct PulseWave {
        uint8_t duty = 0;      // $4000/$4004 bits 7-6
        uint16_t timer = 0;    // counts down from the channel's period
        uint8_t sequence = 0;  // 0-7, stepped DOWNWARD
    };

    static constexpr uint8_t kPulseDuty[4][8] = {
        {0, 0, 0, 0, 0, 0, 0, 1},  // 12.5%
        {0, 0, 0, 0, 0, 0, 1, 1},  // 25%
        {0, 0, 0, 0, 1, 1, 1, 1},  // 50%
        {1, 1, 1, 1, 1, 1, 0, 0},  // 25% negated
    };

    // The triangle's 32-step sequencer.
    //
    // IT TICKS AT THE CPU RATE, not the APU rate the pulses use - "Unlike the
    // pulse channels, this timer ticks at the rate of the CPU clock rather than
    // the APU (CPU/2) clock". Getting that wrong is a factor of two in pitch and
    // is the same trap the frame counter's published cycle numbers carry.
    //
    // AND ITS SEQUENCER STOPS RATHER THAN BEING SILENCED. "The sequencer is
    // clocked by the timer as long as both the linear counter and the length
    // counter are nonzero" - blargg says the same. So when either counter is
    // zero the triangle FREEZES on its current step and holds that output; it
    // does not drop to zero and it does not keep advancing. That is the opposite
    // of the pulse and noise channels, where the sequencer runs on underneath
    // and only the mixer gate closes.
    //
    // The wiki contrasts this with the OTHER silencing method:
    // writing an ultrasonic period pops, while the linear/length route "will
    // instead halt it in whatever its current output position is". Mega Man 1
    // and 2 use the ULTRASONIC one, not this path - an earlier version of this
    // comment attached them to the freeze, which is the opposite of what the
    // source says.
    static constexpr uint8_t kTriangleSequence[32] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5,  4,  3,  2,  1,  0,
                                                      0,  1,  2,  3,  4,  5,  6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    uint16_t triangle_period = 0;
    uint16_t triangle_timer = 0;
    uint8_t triangle_sequence = 0;

    // The noise channel's 15-bit LFSR.
    //
    // Power-on value is 1, not 0: "On power-up, the shift register is loaded
    // with the value 1." A zero register would XOR to zero forever and the
    // channel would be silent for the whole run.
    //
    // The feedback bit is computed from the PRE-SHIFT bits and inserted at bit
    // 14 afterwards - blargg is explicit that they are "*pre-shifted* bits 0 and
    // 1 (mode = 0) or bits 0 and 6 (mode = 1)", and nesdev numbers the same
    // three steps in the same order.
    uint16_t noise_shift = 1;
    bool noise_mode = false;  // $400E bit 7
    // kNoisePeriods[0], which is what a power-on $400E of $00 selects. Not 0:
    // the reload halves this and subtracts one, so a zero period would
    // underflow to $FFFF and stall the channel for 65536 APU cycles.
    uint16_t noise_period = 4;
    uint16_t noise_timer = 0;

    // NTSC only. "The period determines how many CPU cycles happen between
    // shift register clocks. These periods are all even numbers because there
    // are 2 CPU cycles in an APU cycle." PAL differs and is not modelled.
    // Entry $F is 4068 on a production 2A03. The earliest revisions - the
    // recalled first Famicom batch, Vs. System boards, and the arcade parts -
    // used 2046 there and had no Mode flag at all. Not modelled: those are not
    // the chip this emulator targets, and nothing here can tell them apart.
    static constexpr uint16_t kNoisePeriods[16] = {4,   8,   16,  32,  64,  96,   128,  160,
                                                   202, 254, 380, 508, 762, 1016, 2034, 4068};

    void clock_pulse_timer(int pulse);
    void clock_triangle_timer();
    void clock_noise_timer();

    // The delta modulation channel. Unlike the length counters this one READS
    // MEMORY, which is why it is the last thing built: its memory reader stalls
    // the CPU, and the cycle-exact bus is the most heavily verified thing here.
    //
    // This is the channel WITHOUT that stall - the fetch happens in zero cycles.
    // Everything a program can observe by polling ($4015's bytes-remaining and
    // IRQ bits, the rate table, the output level, looping) is here; what is not
    // is the timing distortion the DMA imposes on the CPU. Two of blargg's ROMs
    // exist to measure exactly that, and they are the next step, so the stall is
    // deliberately absent rather than approximated - a wrong number of stolen
    // cycles is worse than none, because it looks implemented.
    struct DeltaModulation {
        // $4010
        bool irq_enabled = false;
        bool loop = false;
        uint16_t timer_period = 0;
        uint16_t timer = 0;

        // $4011. Seven bits, and writable directly - which is how games play
        // PCM by hammering it from a timed loop rather than using samples.
        uint8_t output_level = 0;

        // $4012/$4013, held as written. The reader works from current_address
        // and bytes_remaining so a restart can reload these unchanged.
        uint16_t sample_address = 0xC000;
        uint16_t sample_length = 1;
        uint16_t current_address = 0xC000;
        uint16_t bytes_remaining = 0;

        uint8_t sample_buffer = 0;
        bool sample_buffer_filled = false;

        uint8_t shift_register = 0;
        uint8_t bits_remaining = 0;
        bool silence = true;

        bool irq_flag = false;
        bool enabled = false;

        // A requested-but-not-yet-performed fetch.
        bool transfer_requested = false;
        bool transfer_is_load = false;

        // Cycles until a $4015 enable actually schedules its transfer, and
        // until a $4015 disable actually takes effect. Both are 2 or 3 by cycle
        // parity. A disable landing on a pending transfer cancels it.
        uint8_t transfer_start_delay = 0;
        uint8_t disable_delay = 0;
    };

    void clock_dmc();
    void dmc_restart_sample();
    void dmc_start_transfer(bool is_load);
    void set_dmc_irq(bool asserted);

    DeltaModulation dmc;

    // Shared by power_on() and reset(), which differ only in the value written.
    void restart_frame_counter(uint8_t value_written_to_4017);

    // The triangle's linear counter ($4008, reloaded via $400B).
    //
    // TWO SOURCES, TWO NAMES FOR ONE FLAG, and the collision is worth stating
    // because it is a live trap when reading either document beside this code:
    // blargg's apu_ref.txt calls the internal reload flag the "halt flag", while
    // nesdev calls $4008 bit 7 - a different flag entirely - the "length counter
    // halt flag". `reload` below is blargg's halt flag; `control` is bit 7.
    //
    // control is the SAME BIT as lengths[triangle].halt, so it is not stored
    // here either, for the reason the envelope's loop flag is not.
    uint8_t linear_counter = 0;
    uint8_t linear_reload_value = 0;
    bool linear_reload = false;

    Envelope envelopes[3];  // pulse1, pulse2, noise - indexed by Channel
    Sweep sweeps[2];        // pulse1, pulse2
    PulseWave pulses[2];

    // Which envelope belongs to a Channel. The triangle has none, so this is
    // not the identity and must not be written as one.
    //
    // The assert is the point: triangle == 2 and so does noise's envelope index,
    // meaning envelope_index(triangle) would silently hand back the NOISE
    // envelope. Nothing does that today - every caller passes a literal - but a
    // mixer looping over Channel is the obvious next caller and would get no
    // diagnostic at all.
    static int envelope_index(int channel)
    {
        assert(channel == pulse1 || channel == pulse2 || channel == noise);
        return channel == noise ? 2 : channel;
    }

    void clock_envelope(Envelope& envelope, bool loop);
    void clock_linear_counter();
    void clock_sweep(int pulse);

    // The 11-bit period a sweep would move the channel to, computed from the
    // CURRENT period. Public to the class rather than folded into clock_sweep
    // because muting consults it CONTINUOUSLY - "a target period overflow from
    // the sweep unit's adder can silence a channel even when the sweep unit is
    // disabled and even when the sweep divider is not outputting a clock
    // signal" - not only when the divider fires.
    uint16_t sweep_target(int pulse) const;

    // "Muting happens regardless of whether the sweep unit is disabled ... and
    // regardless of whether the sweep divider is outputting a clock signal."
    // Period < 8, or target > $7FF. Strictly less and strictly greater: $7FF
    // itself is fine and 8 itself is fine.
    bool sweep_muting(int pulse) const;

    // The pulse channels' 11-bit timer periods, which the sweep unit rewrites.
    // Here rather than on a channel struct because the sweep and the muting
    // test both need them before any waveform generator exists to own them.
    uint16_t pulse_period[2] = {0, 0};

    void clock_sequencer();
    void apply_pending_halts();
    void apply_pending_loads();
    void clock_quarter_frame();
    void clock_half_frame();
    void set_frame_irq(bool asserted);
    void load_length(int channel, uint8_t data);

    LengthCounter lengths[length_channels];

    uint32_t frame_cycle = 0;
    bool five_step_mode = false;
    bool irq_inhibit = false;
    bool frame_irq_flag = false;

    // The APU's own free-running cycle count, used only for the parity of a
    // $4017 write. Deliberately NOT CPU::total_cycles: that stops advancing
    // during OAM-DMA-stolen cycles while this clock keeps running, so the two
    // drift apart by the length of every DMA. The divider is not reset by a
    // $4017 write on hardware, so neither is this.
    uint64_t apu_cycles = 0;

    int8_t reset_countdown = -1;
    bool pending_five_step_mode = false;

    // The last value the CPU stored to $4017, replayed by reset(). Not derivable
    // from five_step_mode and irq_inhibit: those are what the write MEANT, and a
    // reset re-writes the byte, so the two would drift the moment a bit that is
    // not yet decoded starts mattering. $00 at power, which is the value
    // power_on() writes anyway.
    uint8_t last_4017_write = 0x00;
};
