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

void APU::dmc_fill_sample_buffer()
{
    if (dmc.sample_buffer_filled || dmc.bytes_remaining == 0) {
        return;
    }

    // No CPU stall here - see DeltaModulation's comment. The read goes through
    // the bus because samples live in PRG-ROM at $8000-$FFFF, where a read has
    // no side effect; this must never be pointed at $2000-$401F, where it
    // would.
    dmc.sample_buffer = bus->read(dmc.current_address);
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
        } else {
            dmc.silence = true;
        }
    }

    dmc_fill_sample_buffer();
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

// The envelope and linear counter clock. Neither exists yet.
void APU::clock_quarter_frame() {}

// The length counter and sweep clock. The sweep does not exist yet.
void APU::clock_half_frame()
{
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
            dmc.bytes_remaining = 0;
        } else if (dmc.bytes_remaining == 0) {
            dmc_restart_sample();

            // "A one-byte buffer that's filled immediately if empty" - not on
            // the next timer tick. With a one-byte sample the fetch drains it
            // here, so bytes-remaining is already zero when the CPU's next
            // instruction reads $4015.
            dmc_fill_sample_buffer();
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
        break;
    case 0x4004:
        lengths[pulse2].pending_halt = (data & 0x20) != 0;
        lengths[pulse2].halt_write_pending = true;
        break;
    case 0x4008:
        lengths[triangle].pending_halt = (data & 0x80) != 0;
        lengths[triangle].halt_write_pending = true;
        break;
    case 0x400C:
        lengths[noise].pending_halt = (data & 0x20) != 0;
        lengths[noise].halt_write_pending = true;
        break;

    // The length loads.
    case 0x4003:
        lengths[pulse1].pending_load = data;
        lengths[pulse1].load_write_pending = true;
        break;
    case 0x4007:
        lengths[pulse2].pending_load = data;
        lengths[pulse2].load_write_pending = true;
        break;
    case 0x400B:
        lengths[triangle].pending_load = data;
        lengths[triangle].load_write_pending = true;
        break;
    case 0x400F:
        lengths[noise].pending_load = data;
        lengths[noise].load_write_pending = true;
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
