#pragma once
#include <cstdint>

#include "device.h"

// Audio generation is not implemented. What IS implemented is the frame
// counter, because it is the NES's only source of maskable interrupts and the
// CPU's /IRQ path is otherwise untestable - the SingleStepTests vectors carry
// no interrupts, and the PPU only drives /NMI.
//
// The frame counter is a divider driving a 4- or 5-step sequence. Each step
// clocks the envelope/sweep/length units (none of which exist yet), and in
// 4-step mode the final step also asserts /IRQ unless inhibited.
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

    // Sequence lengths in CPU cycles. Mode 0 asserts /IRQ across its last three
    // cycles - not on one of them - which is why a read of $4015 placed
    // anywhere in that window sees the flag.
    static constexpr uint32_t mode0_length = 29830;
    static constexpr uint32_t mode1_length = 37282;

    // The first cycle of mode 0's three-cycle IRQ window. Distinct from
    // mode0_length: that is the period, this is when the flag appears.
    static constexpr uint32_t mode0_irq_cycle = 29828;

    // A $4017 write resets the divider 3 or 4 CPU cycles later, depending on
    // the parity of the cycle it landed on. The values here are one higher
    // because Bus::clock runs the CPU's store before APU::clock in the same
    // tick, so the write cycle itself consumes the first decrement.
    static constexpr int8_t write_delay_odd_cycle = 4;
    static constexpr int8_t write_delay_even_cycle = 5;

private:
    void clock_sequencer();
    void clock_quarter_frame();
    void clock_half_frame();
    void set_frame_irq(bool asserted);

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
