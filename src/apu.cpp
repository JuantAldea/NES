#include "../include/apu.h"

#include "../include/bus.h"

// Frame counter step boundaries, in CPU cycles from the sequence start.
//
// Mode 0 (4-step) ends by asserting /IRQ on three consecutive cycles - 29828,
// 29829 and 29830 - rather than on a single one. That is not an approximation:
// the flag is set at 29828, the last envelope/length clock happens at 29829,
// and the counter wraps at 29830, with the line held low across all three.
//
// Mode 1 (5-step) never asserts /IRQ at all.
namespace
{
constexpr uint32_t kQuarter1 = 7457;
constexpr uint32_t kQuarter2 = 14913;
constexpr uint32_t kQuarter3 = 22371;
constexpr uint32_t kMode0IrqFirst = 29828;
constexpr uint32_t kMode0Last = 29830;
constexpr uint32_t kMode1Step5 = 37281;
constexpr uint32_t kMode1Last = 37282;
}  // namespace

void APU::set_frame_irq(const bool asserted)
{
    frame_irq_flag = asserted;

    // The flag and the line are the same thing: /IRQ is level-sensitive, so the
    // CPU keeps taking the interrupt until the handler acknowledges by reading
    // $4015. Setting a flag without driving the line would produce an interrupt
    // that fires once and never again.
    if (bus != nullptr) {
        bus->cpu.set_IRQ_line(frame_irq_flag);
    }
}

void APU::clock()
{
    // A pending $4017 write takes effect here rather than at the write, having
    // been delayed 3 or 4 cycles.
    if (reset_countdown > 0) {
        --reset_countdown;
        if (reset_countdown == 0) {
            reset_countdown = -1;
            five_step_mode = pending_five_step_mode;
            frame_cycle = 0;

            // Switching to 5-step clocks the sequence immediately; 4-step does
            // not. No channels exist to clock yet, but the distinction matters
            // for where the sequence restarts.
            if (five_step_mode) {
                clock_sequencer();
            }
            return;
        }
    }

    ++frame_cycle;
    clock_sequencer();
}

void APU::clock_sequencer()
{
    if (!five_step_mode) {
        // The IRQ window spans the last three cycles of the sequence.
        if (!irq_inhibit && frame_cycle >= kMode0IrqFirst && frame_cycle <= kMode0Last) {
            set_frame_irq(true);
        }

        if (frame_cycle >= kMode0Last) {
            frame_cycle = 0;
        }
        return;
    }

    if (frame_cycle >= kMode1Last) {
        frame_cycle = 0;
    }

    // kQuarter1/2/3 and kMode1Step5 are where the envelope, sweep and length
    // units would be clocked. Referenced so the boundaries are not silently
    // lost when those units arrive.
    (void)kQuarter1;
    (void)kQuarter2;
    (void)kQuarter3;
    (void)kMode1Step5;
}

void APU::write(const uint16_t addr, const uint8_t data)
{
    switch (addr) {
    case APUSTATUS:
        // Channel enables. Writing $4015 also clears the DMC interrupt, which
        // does not exist yet; it does NOT touch the frame interrupt.
        break;

    case FRAMECOUNTER:
        pending_five_step_mode = (data & 0x80) != 0;
        irq_inhibit = (data & 0x40) != 0;

        // Setting the inhibit bit clears any frame interrupt already pending -
        // this is one of the two ways software acknowledges it.
        if (irq_inhibit) {
            set_frame_irq(false);
        }

        // The divider reset is delayed 3 cycles when the write lands on an odd
        // CPU cycle and 4 when it lands on an even one.
        reset_countdown = (bus != nullptr && (bus->cpu.total_cycles % 2) != 0) ? 3 : 4;
        break;

    default:
        // The channel registers ($4000-$4013) are accepted and discarded.
        break;
    }
}

uint8_t APU::read(const uint16_t addr)
{
    if (addr != APUSTATUS) {
        // The rest of the APU range is write-only and reads as open bus.
        return 0;
    }

    // Bit 6 is the frame interrupt flag. Bit 7 would be the DMC interrupt and
    // bits 0-4 the channel length-counter statuses; none exist yet.
    const uint8_t status = frame_irq_flag ? 0x40 : 0x00;

    // Reading acknowledges: the flag clears and the line is released. The
    // hardware race where a read landing exactly on the cycle the flag is set
    // returns it set without clearing is not modelled.
    set_frame_irq(false);

    return status;
}
