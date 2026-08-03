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

    // Sequence step boundaries in CPU cycles. Mode 0 asserts /IRQ across its
    // last three cycles - not on one of them - which is why a read of $4015
    // placed anywhere in that window sees the flag.
    static constexpr uint32_t mode0_length = 29830;
    static constexpr uint32_t mode1_length = 37282;

private:
    void clock_sequencer();
    void set_frame_irq(bool asserted);

    uint32_t frame_cycle = 0;
    bool five_step_mode = false;
    bool irq_inhibit = false;
    bool frame_irq_flag = false;

    // A write to $4017 resets the divider, but not immediately: the reset lands
    // 3 or 4 CPU cycles later depending on the parity of the write.
    int8_t reset_countdown = -1;
    bool pending_five_step_mode = false;
};
