#pragma once
#include <cassert>
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

    // Shared by power_on() and reset(), which differ only in the value written.
    void restart_frame_counter(uint8_t value_written_to_4017);

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

    // The last value the CPU stored to $4017, replayed by reset(). Not derivable
    // from five_step_mode and irq_inhibit: those are what the write MEANT, and a
    // reset re-writes the byte, so the two would drift the moment a bit that is
    // not yet decoded starts mattering. $00 at power, which is the value
    // power_on() writes anyway.
    uint8_t last_4017_write = 0x00;
};
