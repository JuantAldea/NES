#pragma once
#include <cstdint>

#include "device.h"

// Audio generation is not implemented. What IS implemented is the frame
// counter - because it is the NES's only source of maskable interrupts and the
// CPU's /IRQ path is otherwise untestable, the SingleStepTests vectors carrying
// no interrupts and the PPU driving only /NMI - and the length counters, which
// are the first piece of the channels themselves and are entirely CPU-visible,
// so they are verifiable long before anything makes a sound.
//
// The frame counter is a divider driving a 4- or 5-step sequence. Quarter-frame
// steps clock the envelope and the triangle's linear counter, neither of which
// exists yet; half-frame steps clock the length counters and the sweep, of
// which only the length counters exist. In 4-step mode the final step also
// asserts /IRQ unless inhibited.
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
    uint8_t length_counter(const int channel) const { return lengths[channel].value; }

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
    };

    void clock_sequencer();
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
};
