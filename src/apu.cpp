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

// The envelope and linear counter clock. No channels exist yet.
void APU::clock_quarter_frame() {}

// The length counter and sweep clock. No channels exist yet.
void APU::clock_half_frame() {}

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
        // Channel enables. Writing $4015 also clears the DMC interrupt, which
        // does not exist yet; it does NOT touch the frame interrupt.
        break;

    case FRAMECOUNTER: {
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

    // Bit 6 is the frame interrupt flag. Bit 7 would be the DMC interrupt and
    // bits 0-4 the channel length-counter statuses; none exist yet.
    const uint8_t status = frame_irq_flag ? 0x40 : 0x00;

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
