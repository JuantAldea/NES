#include "../include/apu.h"

#include "../include/bus.h"

// Frame counter step boundaries, in CPU cycles from the sequence start.
//
// Mode 0 (4-step) ends by asserting /IRQ on three consecutive cycles - 29828,
// 29829 and 29830 - rather than on a single one. That is not an approximation:
// the flag is set at 29828, the last envelope/length clock happens at 29829,
// and the counter wraps at 29830, with the line held low across all three.
//
// Mode 1 (5-step) never asserts /IRQ. Its step 4 (29829) clocks nothing at all,
// which is why there is no constant for it.
namespace
{
constexpr uint32_t kQuarter1 = 7457;
constexpr uint32_t kQuarter2 = 14913;
constexpr uint32_t kQuarter3 = 22371;
constexpr uint32_t kMode0Step4 = 29829;
constexpr uint32_t kMode1Step5 = 37281;
}  // namespace

// NTSC only. These are PERIODS IN CPU CYCLES between output-level changes, not
// frequencies and not APU cycles, which is what "Rate 0's period is too short"
// was telling us when the table did not exist: index 0 is the slowest at 428.
//
// The table is not a formula - the values come from the chip's own divider
// taps - so 8-dmc_rates times all sixteen rather than checking a few.
namespace
{
constexpr uint16_t kDmcRates[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54,
};
}  // namespace

void APU::set_dmc_irq(const bool asserted)
{
    dmc.irq_flag = asserted;

    // Its own bit of the CPU's /IRQ input, ORed with the frame counter's and
    // the cartridge's rather than overwriting them.
    bus->cpu.set_IRQ_line(CPU::IRQSource::apu_dmc, dmc.irq_flag);
}

void APU::dmc_restart_sample()
{
    dmc.current_address = dmc.sample_address;
    dmc.bytes_remaining = dmc.sample_length;
}

// Requests a fetch, if one is actually due. Mesen2's StartDmcTransfer.
void APU::dmc_start_transfer(const bool is_load)
{
    if (!dmc.sample_buffer_filled && dmc.bytes_remaining > 0) {
        dmc.transfer_requested = true;
        dmc.transfer_is_load = is_load;
    }
}

// Called by the Bus on the get cycle of a DMC DMA, with the byte it read.
void APU::dmc_deliver_sample_byte(const uint8_t data)
{
    dmc.transfer_requested = false;
    if (dmc.bytes_remaining == 0) {
        return;
    }

    // No CPU stall here - see DeltaModulation's comment. The read goes through
    // the bus because samples live in PRG-ROM at $8000-$FFFF, where a read has
    // no side effect; this must never be pointed at $2000-$401F, where it
    // would.
    dmc.sample_buffer = data;
    dmc.sample_buffer_filled = true;

    // "The address is incremented; if it exceeds $FFFF, it is wrapped around to
    // $8000" - the sample window is the cartridge's, so it wraps to $8000 and
    // not to zero.
    //
    // NOT VERIFIED BY ANY ORACLE HERE. Wrapping to $0000 instead passes all of
    // apu_test, apu_reset and blargg_apu_2005, because their samples are a
    // 33-byte block that never reaches $FFFF. Taken from the NESdev wiki, and
    // the first ROM that plays a sample off the end of the address space is
    // what will confirm or refute it.
    dmc.current_address = (dmc.current_address == 0xFFFF) ? 0x8000 : static_cast<uint16_t>(dmc.current_address + 1);
    --dmc.bytes_remaining;

    if (dmc.bytes_remaining == 0) {
        if (dmc.loop) {
            dmc_restart_sample();
        } else if (dmc.irq_enabled) {
            set_dmc_irq(true);
        }
    }
}

void APU::clock_dmc()
{
    // Mesen2's ProcessClock. A disable that expires clears bytes-remaining and
    // aborts any transfer that has not yet been performed; an expiring start
    // delay is what finally requests the fetch a $4015 enable asked for.
    if (dmc.disable_delay != 0 && --dmc.disable_delay == 0) {
        dmc.bytes_remaining = 0;
        dmc.transfer_requested = false;
    }
    if (dmc.transfer_start_delay != 0 && --dmc.transfer_start_delay == 0) {
        dmc_start_transfer(true);  // scheduled by the $4015 write
    }

    // A period of 0 means no rate has been selected yet, and must not free-run.
    if (dmc.timer_period == 0) {
        return;
    }

    if (dmc.timer > 0) {
        --dmc.timer;
        return;
    }

    // period - 1, not period. Counting down from the period itself spends
    // period+1 cycles between output changes, which 8-dmc_rates reported as
    // "Rate 0's period is too long" - one cycle in 428, and it still saw it.
    dmc.timer = static_cast<uint16_t>(dmc.timer_period - 1);

    // The output unit. Silence still clocks the shift register and the counter -
    // it only suppresses the level change - which is what keeps a silenced
    // channel in step with the sample stream.
    if (!dmc.silence) {
        // Clamped rather than wrapped: the level is 7 bits and hardware simply
        // does not step past either end.
        //
        // ALSO NOT VERIFIED HERE. Removing the upper clamp passes every APU ROM
        // in this repo - the output level is an analogue quantity and none of
        // them read it back, so only something that renders audio can catch it.
        // From the wiki, and pinned here in a comment rather than by a test
        // because there is nothing to assert against yet.
        if ((dmc.shift_register & 0x01) != 0) {
            if (dmc.output_level <= 125) {
                dmc.output_level = static_cast<uint8_t>(dmc.output_level + 2);
            }
        } else if (dmc.output_level >= 2) {
            dmc.output_level = static_cast<uint8_t>(dmc.output_level - 2);
        }
    }

    dmc.shift_register >>= 1;

    if (dmc.bits_remaining > 0) {
        --dmc.bits_remaining;
    }

    if (dmc.bits_remaining == 0) {
        dmc.bits_remaining = 8;
        if (dmc.sample_buffer_filled) {
            dmc.silence = false;
            dmc.shift_register = dmc.sample_buffer;
            dmc.sample_buffer_filled = false;

            // Immediately - this is a reload, and only a LOAD is delayed. The
            // guard is Mesen2's: "don't trigger the DMA if the channel was just
            // enabled by a 4015 write", because that one is already scheduled.
            if (dmc.transfer_start_delay == 0) {
                dmc_start_transfer(false);  // the buffer emptied: a reload
            }
        } else {
            dmc.silence = true;
        }
    }
}

void APU::set_frame_irq(const bool asserted)
{
    frame_irq_flag = asserted;

    // The flag and the line are the same thing: /IRQ is level-sensitive, so the
    // CPU keeps taking the interrupt until the handler acknowledges by reading
    // $4015. Setting a flag without driving the line would produce an interrupt
    // that fires once and never again.
    //
    // The frame counter owns one bit of the CPU's /IRQ input; other sources
    // (DMC, mapper counters) own their own, and the CPU sees the OR.
    bus->cpu.set_IRQ_line(CPU::IRQSource::apu_frame_counter, frame_irq_flag);
}

// One envelope, one quarter-frame clock.
//
// nesdev, verbatim, and the two paragraphs are in this order for a reason - the
// start-flag branch is EXCLUSIVE, so a clock that consumes the start flag does
// not also clock the divider:
//
//   "if the start flag is clear, the divider is clocked, otherwise the start
//    flag is cleared, the decay level counter is loaded with 15, and the
//    divider's period is immediately reloaded"
//
//   "When the divider is clocked while at 0, it is loaded with V and clocks the
//    decay level counter. Then one of two actions occurs: If the counter is
//    non-zero, it is decremented, otherwise if the loop flag is set, the decay
//    level counter is loaded with 15."
//
// `loop` is passed in rather than stored: it is the length counter's halt bit,
// the same physical bit 5, and duplicating it here would let the two drift.
void APU::clock_envelope(Envelope& envelope, const bool loop)
{
    if (envelope.start) {
        envelope.start = false;
        envelope.decay = 15;
        envelope.divider = envelope.period;
        return;
    }

    if (envelope.divider > 0) {
        --envelope.divider;
        return;
    }

    envelope.divider = envelope.period;
    if (envelope.decay > 0) {
        --envelope.decay;
    } else if (loop) {
        envelope.decay = 15;
    }
}

// The triangle's linear counter, one quarter-frame clock. nesdev and blargg
// give the same two steps in the same order:
//
//   "1. If the linear counter reload flag is set, the linear counter is
//       reloaded with the counter reload value, otherwise if the linear counter
//       is non-zero, it is decremented.
//    2. If the control flag is clear, the linear counter reload flag is
//       cleared."
//
// Step 2 is why holding the control flag set makes every $4008 write reassert
// on the next clock, indefinitely: the reload flag never self-clears in that
// mode. The control flag is lengths[triangle].halt - one bit, two units.
void APU::clock_linear_counter()
{
    if (linear_reload) {
        linear_counter = linear_reload_value;
    } else if (linear_counter > 0) {
        --linear_counter;
    }

    if (!lengths[triangle].halt) {
        linear_reload = false;
    }
}

// One APU cycle of a pulse channel's timer.
//
// "this timer is updated every APU cycle (i.e., every second CPU cycle), and
// counts t, t-1, ..., 0, t, t-1, ..., clocking the waveform generator when it
// goes from 0 to t" - so the sequencer advances on the RELOAD, not on every
// tick, and the waveform period is 8*(t+1) APU cycles.
//
// The sequence index counts DOWN. See kPulseDuty in apu.h for why, and for how
// that was cross-checked against blargg's waveform diagrams rather than taken
// from one source.
void APU::clock_pulse_timer(const int pulse)
{
    if (pulses[pulse].timer > 0) {
        --pulses[pulse].timer;
        return;
    }

    pulses[pulse].timer = pulse_period[pulse];
    pulses[pulse].sequence = static_cast<uint8_t>((pulses[pulse].sequence + 7) & 0x07);
}

// One CPU cycle of the triangle's timer - CPU, not APU. See kTriangleSequence.
//
// The gate is on the CLOCK, not on the output: with either counter at zero the
// sequencer does not advance at all and the channel holds its current step.
void APU::clock_triangle_timer()
{
    if (linear_counter == 0 || lengths[triangle].value == 0) {
        return;
    }

    if (triangle_timer > 0) {
        --triangle_timer;
        return;
    }

    triangle_timer = triangle_period;
    triangle_sequence = static_cast<uint8_t>((triangle_sequence + 1) & 0x1F);
}

// One APU cycle of the noise channel's timer and its shift register.
//
// nesdev numbers the three steps and the order is the whole content:
//
//   "1. Feedback is calculated as the exclusive-OR of bit 0 and one other bit:
//       bit 6 if Mode flag is set, otherwise bit 1.
//    2. The shift register is shifted right by one bit.
//    3. Bit 14, the leftmost bit, is set to the feedback calculated earlier."
//
// blargg says the same with the emphasis on the trap: the XOR is of the
// "*pre-shifted*" bits. Computing it after the shift is the classic version of
// this bug and produces a sequence that is wrong but still noisy, so it sounds
// approximately right - which is exactly why no listening test would catch it.
void APU::clock_noise_timer()
{
    if (noise_timer > 0) {
        --noise_timer;
        return;
    }

    // HALVED, because the table is in CPU CYCLES and this divider is clocked at
    // the APU rate. nesdev: "The period determines how many CPU cycles happen
    // between shift register clocks. These periods are all even numbers because
    // there are 2 CPU cycles in an APU cycle." Using the table value directly as
    // an APU-cycle count makes every noise pitch an octave too low - the same
    // factor of two the triangle's timer and the frame counter's published
    // boundaries both carry, and the third time it has bitten in this file.
    //
    // The -1 is the ordinary divider convention: counting down from N and
    // reloading at zero spends N+1 cycles per clock.
    noise_timer = static_cast<uint16_t>((noise_period / 2) - 1);

    const uint16_t other = noise_mode ? ((noise_shift >> 6) & 1u) : ((noise_shift >> 1) & 1u);
    const uint16_t feedback = (noise_shift & 1u) ^ other;

    noise_shift = static_cast<uint16_t>(noise_shift >> 1);
    noise_shift = static_cast<uint16_t>((noise_shift & 0x3FFFu) | (feedback << 14));
}

// The envelopes and the triangle's linear counter.
void APU::clock_quarter_frame()
{
    clock_envelope(envelopes[envelope_index(pulse1)], lengths[pulse1].halt);
    clock_envelope(envelopes[envelope_index(pulse2)], lengths[pulse2].halt);
    clock_envelope(envelopes[envelope_index(noise)], lengths[noise].halt);
    clock_linear_counter();
}

// Where the sweep would move this channel's period.
//
// The zero clamp below is the WIKI'S RULE, not a local choice about unsigned
// arithmetic: "The target period is the sum of the current period and the
// change amount, clamped to zero if this sum is negative." An earlier version
// of this comment presented it as an implementation decision to avoid a
// uint16_t wrap, which would have read to the next person as negotiable.
//
// THE ONE LINE IN THIS FILE MOST LIKELY TO BE WRITTEN BACKWARDS. Pulse 1 adds
// the ones' complement of the change amount and pulse 2 adds the two's, so
// pulse 1 subtracts one MORE. nesdev states it as "-c - 1" against "-c";
// blargg states the same asymmetry as the second channel's inverted value being
// "incremented by 1". Two independent sources, and no oracle here to catch it
// if this is inverted - which is exactly why both are quoted.
uint16_t APU::sweep_target(const int pulse) const
{
    const uint16_t period = pulse_period[pulse];
    const uint16_t change = static_cast<uint16_t>(period >> sweeps[pulse].shift);

    if (!sweeps[pulse].negate) {
        return static_cast<uint16_t>(period + change);
    }

    // Pulse 1 is the ones' complement channel. Clamped at 0 rather than allowed
    // to wrap: the adder is 11 bits and a negative result cannot be represented,
    // and a wrapped uint16_t would read as an enormous period and mute the
    // channel through the wrong branch below.
    const uint16_t subtract = static_cast<uint16_t>(change + (pulse == pulse1 ? 1u : 0u));
    return subtract > period ? 0u : static_cast<uint16_t>(period - subtract);
}

// nesdev: "Muting happens regardless of whether the sweep unit is disabled
// (because either the Enabled flag or the Shift count are zero) and regardless
// of whether the sweep divider is outputting a clock signal."
//
// So this consults neither `enabled` nor `shift`, deliberately. It is also why
// the target is computed continuously rather than cached at the last sweep
// clock - a channel can be silenced by a sweep unit that is switched off, which
// is why "several publishers' NES games never seem to use the bottom octave of
// the pulse waves".
//
// Strictly less than 8 and strictly greater than $7FF: both boundaries are
// inclusive on the audible side.
bool APU::sweep_muting(const int pulse) const { return pulse_period[pulse] < 8 || sweep_target(pulse) > 0x7FF; }

// One sweep unit, one half-frame clock. nesdev gives the two steps in this
// order, and the divider-is-zero test in step 1 uses the value the counter held
// COMING IN.
//
// An earlier version of this comment justified that with a case where the two
// orderings AGREE, which is no justification at all: at P=0 a reload-first
// implementation sets the divider back to 0 and the test still passes, so both
// update on every clock. The real difference is at P>0 - reload-first makes
// step 1 see a non-zero divider and SKIPS the first period update entirely,
// costing a whole half-frame of pitch on every $4001 write. Pinned by
// sweep_updates_the_period_on_the_first_half_frame_after_a_write.
//
// Step 1 is abridged here; the wiki's full clause continues "...and the sweep
// unit IS muting the channel: the pulse's period remains unchanged, but the
// sweep unit's divider continues to count down and reload the divider's period
// as normal." That is why the two ifs below are independent rather than nested.
//
//   "1. If the divider's counter is zero, the sweep is enabled, the shift count
//       is nonzero, and the sweep unit is not muting the channel: The pulse's
//       period is set to the target period. [...]
//    2. If the divider's counter is zero or the reload flag is true: The
//       divider counter is set to P and the reload flag is cleared. Otherwise
//       the divider counter is decremented."
void APU::clock_sweep(const int pulse)
{
    Sweep& sweep = sweeps[pulse];

    if (sweep.divider == 0 && sweep.enabled && sweep.shift != 0 && !sweep_muting(pulse)) {
        pulse_period[pulse] = sweep_target(pulse);
    }

    if (sweep.divider == 0 || sweep.reload) {
        sweep.divider = sweep.period;
        sweep.reload = false;
    } else {
        --sweep.divider;
    }
}

// The length counters and the sweeps.
void APU::clock_half_frame()
{
    clock_sweep(pulse1);
    clock_sweep(pulse2);

    for (LengthCounter& counter : lengths) {
        // Recorded for apply_pending_loads, which must decide on the value as
        // it was when the clock arrived, not as it is afterwards.
        counter.clocked_this_cycle = true;
        counter.value_before_clock = counter.value;

        // Halt suspends the countdown without clearing it, so a halted channel
        // resumes from where it stopped rather than restarting - blargg's
        // 1-len_ctr #8. The counter stops AT zero rather than wrapping; zero is
        // the silent state and there is nothing below it.
        if (!counter.halt && counter.value > 0) {
            --counter.value;
        }
    }
}

// $4003/$4007/$400B/$400F bits 3-7 index this; bits 0-2 are the timer high
// bits and belong to the channel, not here.
//
// The table is NOT a formula. The low half counts 10, 20, 40, 80, 160, 60, 14,
// 26 - musical note lengths - while the odd entries count 254 down to 2 in
// twos. Anything derived would be wrong somewhere, which is exactly what
// 2-len_table checks by loading all 32 and timing each.
void APU::load_length(const int channel, const uint8_t data)
{
    static constexpr uint8_t table[32] = {
        10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
        12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
    };

    // A disabled channel ignores the load entirely - see LengthCounter's
    // comment. Silently, and that IS the hardware behaviour rather than a
    // shortcut.
    if (!lengths[channel].enabled) {
        return;
    }
    lengths[channel].value = table[data >> 3];
}

// The write goes through the normal $4017 path rather than assigning the state
// directly, so the divider reset takes its usual 3-4 cycle delay and resolves
// partway through the loop below - which is what leaves frame_cycle at a small
// non-zero value when the CPU's first instruction runs, exactly as on hardware.
// Assigning the fields here instead would start the sequence at 0 and lose the
// delay this function exists to model.
//
// The loop length is even (see power_on_delay), so this preserves the parity of
// apu_cycles and cannot invert the CPU/APU phase. That matters on RESET, which
// can happen at any point in a run: an odd-length restart would silently shift
// every subsequent $4017 write by a cycle from that moment on.
void APU::restart_frame_counter(const uint8_t value_written_to_4017)
{
    write(FRAMECOUNTER, value_written_to_4017);
    for (int i = 0; i < power_on_delay; ++i) {
        clock();
    }
}

void APU::power_on() { restart_frame_counter(0x00); }

void APU::reset()
{
    // Order matters. $4015 first, because the $4017 replay can re-arm the frame
    // IRQ, and clearing the flag afterwards would then hide a legitimate one.
    //
    // Written through the register rather than by zeroing the array, so a
    // disabled channel also has its counter cleared and blocked from reloading
    // exactly as a $4015 write does - that behaviour lives in one place.
    write(APUSTATUS, 0x00);
    set_frame_irq(false);

    // "the last value written to $4017 is written again, rather than $00". Note
    // that this re-applies its inhibit bit, which is what the readme means by
    // the flag being "sometimes" cleared at reset: it depends on the byte.
    restart_frame_counter(last_4017_write);
}

// Applied at the END of a clock, after the sequencer has run, so a halt stored
// on cycle N is not visible to the length clock until cycle N+1 has already
// clocked. That one cycle is the whole of 10.len_halt_timing.
void APU::apply_pending_halts()
{
    for (LengthCounter& counter : lengths) {
        if (counter.halt_write_pending) {
            counter.halt = counter.pending_halt;
            counter.halt_write_pending = false;
        }
    }
}

// The counterpart, and the reason clock_half_frame records value_before_clock:
// a load stored on cycle N lands after cycle N+1 has clocked, and if that clock
// found the counter non-zero the load is dropped. At zero it is honoured, which
// is what lets a program restart a finished note on the clock edge.
void APU::apply_pending_loads()
{
    for (int channel = 0; channel < length_channels; ++channel) {
        LengthCounter& counter = lengths[channel];
        if (counter.load_write_pending) {
            if (!(counter.clocked_this_cycle && counter.value_before_clock != 0)) {
                load_length(channel, counter.pending_load);
            }
            counter.load_write_pending = false;
        }
        counter.clocked_this_cycle = false;
    }
}

void APU::clock()
{
    ++apu_cycles;

    // Clocked every CPU cycle because the rate table is in CPU cycles, and
    // independently of the frame counter - the DMC has its own divider and does
    // not take part in the 4/5-step sequence at all.
    clock_dmc();

    // The waveform generators, on their own dividers and likewise independent of
    // the 4/5-step sequence. TWO DIFFERENT RATES, and the split is the point:
    // the pulses and the noise run at the APU rate (every second CPU cycle)
    // while the TRIANGLE runs at the CPU rate. Clocking the triangle with the
    // others would halve its pitch and is the documented trap.
    //
    // apu_cycles counts CPU cycles from power-on and its low bit is already the
    // CPU/APU phase used for $4017 write timing, so the parity is taken from
    // there rather than from a second counter that could drift from it.
    clock_triangle_timer();
    if ((apu_cycles & 1) == 0) {
        clock_pulse_timer(pulse1);
        clock_pulse_timer(pulse2);
        clock_noise_timer();
    }

    // A pending $4017 write takes effect here rather than at the write, having
    // been delayed 3 or 4 CPU cycles.
    if (reset_countdown > 0) {
        --reset_countdown;
        if (reset_countdown == 0) {
            reset_countdown = -1;
            five_step_mode = pending_five_step_mode;
            frame_cycle = 0;

            // Switching to 5-step clocks the whole sequence immediately;
            // switching to 4-step does not.
            //
            // This calls the unit clocks directly and NOT clock_sequencer():
            // that dispatches on frame_cycle, which is 0 here and matches no
            // boundary, so routing through it would make this a silent no-op
            // that only becomes visible - as a missing clock - once the
            // channels exist.
            if (five_step_mode) {
                clock_quarter_frame();
                clock_half_frame();
            }
            apply_pending_halts();
            apply_pending_loads();
            return;
        }
    }

    ++frame_cycle;
    clock_sequencer();
    apply_pending_halts();
    apply_pending_loads();
}

void APU::clock_sequencer()
{
    if (frame_cycle == kQuarter1 || frame_cycle == kQuarter3) {
        clock_quarter_frame();
        return;
    }

    if (frame_cycle == kQuarter2) {
        clock_quarter_frame();
        clock_half_frame();
        return;
    }

    if (!five_step_mode) {
        if (frame_cycle == kMode0Step4) {
            clock_quarter_frame();
            clock_half_frame();
        }

        // The IRQ window spans the last three cycles of the sequence, so a
        // $4015 read placed anywhere in it sees the flag.
        if (!irq_inhibit && frame_cycle >= mode0_irq_cycle && frame_cycle <= mode0_length) {
            set_frame_irq(true);
        }

        if (frame_cycle >= mode0_length) {
            frame_cycle = 0;
        }
        return;
    }

    if (frame_cycle == kMode1Step5) {
        clock_quarter_frame();
        clock_half_frame();
    }

    if (frame_cycle >= mode1_length) {
        frame_cycle = 0;
    }
}

void APU::write(const uint16_t addr, const uint8_t data)
{
    switch (addr) {
    case APUSTATUS:
        // Channel enables, one bit each in Channel order. Writing $4015 also
        // clears the DMC interrupt, which does not exist yet; it does NOT touch
        // the frame interrupt, which is why set_frame_irq is absent here.
        for (int channel = 0; channel < length_channels; ++channel) {
            const bool enable = (data & (1 << channel)) != 0;
            lengths[channel].enabled = enable;

            // Clearing the enable clears the counter outright. Not "stops
            // counting" - blargg's 1-len_ctr #6 is "disabling via $4015 should
            // clear length counter", and #7 then checks the channel cannot be
            // reloaded while it stays disabled.
            if (!enable) {
                lengths[channel].value = 0;
            }
        }

        // Bit 4 is the DMC, and it does not behave like the other four. It does
        // not load a length: enabling starts the sample only if none is already
        // playing, so a program can write $4015 repeatedly without restarting
        // it, and disabling silences it by zeroing bytes-remaining.
        // The acknowledge comes FIRST, and the order is load-bearing. Clearing
        // afterwards also wipes an interrupt the restart below has just raised,
        // which is 7-dmc_basics #19: a one-byte sample is fetched immediately,
        // so it ends inside this very write and must leave $4015 reading $80 -
        // IRQ set, bytes-remaining clear.
        //
        // Unlike the frame interrupt, which a $4015 write deliberately leaves
        // alone.
        set_dmc_irq(false);

        dmc.enabled = (data & 0x10) != 0;
        if (!dmc.enabled) {
            // Disabling takes effect 2 or 3 cycles later, and a transfer that
            // starts inside that window is cancelled - though the CPU has still
            // been halted for it.
            if (dmc.disable_delay == 0) {
                dmc.disable_delay = (apu_cycles % 2 == 0) ? 2 : 3;
            }
        } else if (dmc.bytes_remaining == 0) {
            dmc_restart_sample();

            // Scheduled, not performed. The 2-or-3 cycle delay is what
            // dmc_dma_start_test measures, and it is the one mechanism this
            // implementation was missing entirely.
            dmc.transfer_start_delay = (apu_cycles % 2 == 0) ? 2 : 3;
        }
        break;

    // The DMC's four registers.
    case 0x4010:
        dmc.irq_enabled = (data & 0x80) != 0;
        dmc.loop = (data & 0x40) != 0;
        dmc.timer_period = kDmcRates[data & 0x0F];

        // "If clear, the interrupt flag is cleared" - clearing the enable is
        // one of the two ways software acknowledges a DMC interrupt, the other
        // being a $4015 write.
        if (!dmc.irq_enabled) {
            set_dmc_irq(false);
        }
        break;

    case 0x4011:
        // Seven bits, loaded straight into the output level. Writable at any
        // time and by design: this is the register games hammer from a timed
        // loop to play PCM without using the sample machinery at all.
        dmc.output_level = data & 0x7F;
        break;

    case 0x4012:
        dmc.sample_address = static_cast<uint16_t>(0xC000 + (static_cast<uint16_t>(data) * 64));
        break;

    case 0x4013:
        dmc.sample_length = static_cast<uint16_t>((static_cast<uint16_t>(data) * 16) + 1);
        break;

    // The halt bits. Same bit position on both pulses and the noise, and a
    // different one on the triangle, where $4008 bit 7 does double duty as the
    // linear counter's control flag - one flag, two jobs, which is the
    // hardware's doing and not a simplification here.
    case 0x4000:
        lengths[pulse1].pending_halt = (data & 0x20) != 0;
        lengths[pulse1].halt_write_pending = true;
        // Bits 3-0 are the envelope period AND the constant volume, bit 4
        // chooses which of those two the mixer sees. Neither is deferred the
        // way the halt bit is: 10.len_halt_timing pins the halt bit's one-cycle
        // delay and says nothing about the rest of the register, so applying
        // the rest immediately is what the evidence supports.
        envelopes[envelope_index(pulse1)].period = data & 0x0F;
        envelopes[envelope_index(pulse1)].constant_volume = (data & 0x10) != 0;
        // Bits 7-6. nesdev: "The duty cycle is changed ... but the sequencer's
        // current position isn't affected."
        pulses[pulse1].duty = (data >> 6) & 0x03;
        break;
    case 0x4004:
        lengths[pulse2].pending_halt = (data & 0x20) != 0;
        lengths[pulse2].halt_write_pending = true;
        envelopes[envelope_index(pulse2)].period = data & 0x0F;
        envelopes[envelope_index(pulse2)].constant_volume = (data & 0x10) != 0;
        // Bits 7-6. nesdev: "The duty cycle is changed ... but the sequencer's
        // current position isn't affected."
        pulses[pulse2].duty = (data >> 6) & 0x03;
        break;
    case 0x4008:
        lengths[triangle].pending_halt = (data & 0x80) != 0;
        lengths[triangle].halt_write_pending = true;
        // Bits 6-0. Bit 7 is the control flag, which is the halt bit above.
        linear_reload_value = data & 0x7F;
        break;
    // $400A/$400B carry the triangle's 11-bit period the same way $4002/$4003
    // carry a pulse's.
    case 0x400A:
        triangle_period = static_cast<uint16_t>((triangle_period & 0x0700) | data);
        break;

    case 0x400E:
        // Bit 7 is the LFSR's Mode flag; bits 3-0 index the NTSC period table.
        noise_mode = (data & 0x80) != 0;
        noise_period = kNoisePeriods[data & 0x0F];
        break;

    case 0x400C:
        lengths[noise].pending_halt = (data & 0x20) != 0;
        lengths[noise].halt_write_pending = true;
        envelopes[envelope_index(noise)].period = data & 0x0F;
        envelopes[envelope_index(noise)].constant_volume = (data & 0x10) != 0;
        break;

    // The sweep registers. EPPP.NSSS, and any write sets the reload flag.
    case 0x4001:
        sweeps[pulse1].enabled = (data & 0x80) != 0;
        sweeps[pulse1].period = (data >> 4) & 0x07;
        sweeps[pulse1].negate = (data & 0x08) != 0;
        sweeps[pulse1].shift = data & 0x07;
        sweeps[pulse1].reload = true;
        break;
    case 0x4005:
        sweeps[pulse2].enabled = (data & 0x80) != 0;
        sweeps[pulse2].period = (data >> 4) & 0x07;
        sweeps[pulse2].negate = (data & 0x08) != 0;
        sweeps[pulse2].shift = data & 0x07;
        sweeps[pulse2].reload = true;
        break;

    // The pulse timer periods. Low eight bits here, high three in $4003/$4007
    // below - and the sweep unit rewrites them, which is why they are not the
    // register value but state of their own.
    case 0x4002:
        pulse_period[pulse1] = static_cast<uint16_t>((pulse_period[pulse1] & 0x0700) | data);
        break;
    case 0x4006:
        pulse_period[pulse2] = static_cast<uint16_t>((pulse_period[pulse2] & 0x0700) | data);
        break;

    // The length loads.
    // Each also sets its channel's restart flag. nesdev lists the side effects
    // of $4003/$4007 as "The sequencer is immediately restarted at the first
    // value of the current sequence. The envelope is also restarted. The period
    // divider is not reset." - so the envelope start flag is set HERE, and is
    // consumed by the next quarter-frame clock rather than at the write.
    case 0x4003:
        lengths[pulse1].pending_load = data;
        lengths[pulse1].load_write_pending = true;
        pulse_period[pulse1] = static_cast<uint16_t>((pulse_period[pulse1] & 0x00FF) | ((data & 0x07) << 8));
        envelopes[envelope_index(pulse1)].start = true;
        // "The sequencer is immediately restarted at the first value of the
        // current sequence ... The period divider is not reset." So the phase
        // moves and pulses[].timer deliberately does not.
        pulses[pulse1].sequence = 0;
        break;
    case 0x4007:
        lengths[pulse2].pending_load = data;
        lengths[pulse2].load_write_pending = true;
        pulse_period[pulse2] = static_cast<uint16_t>((pulse_period[pulse2] & 0x00FF) | ((data & 0x07) << 8));
        envelopes[envelope_index(pulse2)].start = true;
        pulses[pulse2].sequence = 0;
        break;
    case 0x400B:
        lengths[triangle].pending_load = data;
        lengths[triangle].load_write_pending = true;
        // The triangle has no envelope; $400B sets the linear counter's reload
        // flag instead. blargg calls this flag the "halt flag", which is NOT
        // the $4008 bit 7 that nesdev calls the halt flag - see apu.h.
        linear_reload = true;
        triangle_period = static_cast<uint16_t>((triangle_period & 0x00FF) | ((data & 0x07) << 8));
        break;
    case 0x400F:
        lengths[noise].pending_load = data;
        lengths[noise].load_write_pending = true;
        envelopes[envelope_index(noise)].start = true;
        break;

    case FRAMECOUNTER: {
        last_4017_write = data;
        pending_five_step_mode = (data & 0x80) != 0;
        irq_inhibit = (data & 0x40) != 0;

        // Setting the inhibit bit clears any frame interrupt already pending -
        // one of the two ways software acknowledges it.
        if (irq_inhibit) {
            set_frame_irq(false);
        }

        // The parity that matters is the CPU cycle the write LANDS on.
        // APU::clock has already ticked for that cycle by the time Bus::clock
        // runs the CPU's store, so apu_cycles IS it. Getting this wrong is
        // invisible in isolation: it shifts the reset by one cycle, which is
        // why apu_tests pins the parity DIFFERENCE rather than only an absolute
        // cycle.
        const bool write_cycle_is_odd = (apu_cycles % 2) != 0;
        reset_countdown = write_cycle_is_odd ? write_delay_odd_cycle : write_delay_even_cycle;
        break;
    }

    default:
        // The channel registers ($4000-$4013) are accepted and discarded.
        break;
    }
}

uint8_t APU::read(const uint16_t addr)
{
    if (addr != APUSTATUS) {
        // The rest of the range this device is mapped to is write-only, so
        // nothing drives the bus and the read sees whatever was last on it.
        //
        // Returning 0 here instead is what blargg's cpu_exec_space_apu catches:
        // it executes code through $4000-$40FF, so the bytes fetched there ARE
        // the open-bus value, and a constant 0 sends execution somewhere else.
        //
        // $4017 no longer reaches this path on a read. It is the frame counter
        // on write and controller port 2 on read, and Bus::decode routes the
        // two directions to different devices.
        return open_bus();
    }

    // Bits 0-3 report whether each length counter is still running - the
    // COUNTER, not the enable. A channel enabled at $4015 with a counter that
    // has reached zero reads back as 0, which is how a program waits for a note
    // to finish. Bit 4 would be the DMC's bytes-remaining and bit 7 its
    // interrupt; neither exists yet.
    uint8_t status = frame_irq_flag ? 0x40 : 0x00;
    for (int channel = 0; channel < length_channels; ++channel) {
        if (lengths[channel].value > 0) {
            status |= static_cast<uint8_t>(1 << channel);
        }
    }

    // Bit 4 is BYTES REMAINING, not an enable - it reports whether the sample
    // is still playing, which is how a program waits for one to finish. Bit 7
    // is the DMC interrupt, and unlike bit 6 it is NOT cleared by this read.
    if (dmc.bytes_remaining > 0) {
        status |= 0x10;
    }
    if (dmc.irq_flag) {
        status |= 0x80;
    }

    // Reading acknowledges: the flag clears and the line is released.
    //
    // A read landing on the same cycle the flag is set returns bit 6 SET, as it
    // does on hardware: APU::clock has already run for this cycle by the time
    // Bus::clock issues the CPU's read. What is still missing is that on
    // hardware such a read does not clear the flag either. That is unobservable
    // in mode 0, whose window re-asserts on the next two cycles anyway; it
    // would show only on a read placed on the window's LAST cycle.
    set_frame_irq(false);

    return status;
}
