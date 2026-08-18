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

void APU::clock()
{
    ++apu_cycles;

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
            return;
        }
    }

    ++frame_cycle;
    clock_sequencer();
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
        break;

    // The halt bits. Same bit position on both pulses and the noise, and a
    // different one on the triangle, where $4008 bit 7 does double duty as the
    // linear counter's control flag - one flag, two jobs, which is the
    // hardware's doing and not a simplification here.
    case 0x4000:
        lengths[pulse1].halt = (data & 0x20) != 0;
        break;
    case 0x4004:
        lengths[pulse2].halt = (data & 0x20) != 0;
        break;
    case 0x4008:
        lengths[triangle].halt = (data & 0x80) != 0;
        break;
    case 0x400C:
        lengths[noise].halt = (data & 0x20) != 0;
        break;

    // The length loads.
    case 0x4003:
        load_length(pulse1, data);
        break;
    case 0x4007:
        load_length(pulse2, data);
        break;
    case 0x400B:
        load_length(triangle, data);
        break;
    case 0x400F:
        load_length(noise, data);
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
