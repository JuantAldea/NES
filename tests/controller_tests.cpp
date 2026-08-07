// Guards the standard controller ports at $4016/$4017.
//
// Expected values here are transcribed from the NESdev "Standard controller"
// page, not derived from the implementation, so this is a check against the
// spec rather than a restatement of the code:
//
//   - "While S (strobe) is high, the shift registers in the controllers are
//      continuously reloaded from the button states, and reading $4016/$4017
//      will keep returning the current state of the first button (A)."
//   - "Each read reports one bit at a time through D0."
//   - "0 - A, 1 - B, 2 - Select, 3 - Start, 4 - Up, 5 - Down, 6 - Left,
//      7 - Right"
//   - "After 8 bits are read, all subsequent bits will report 1 on a standard
//      NES controller."
//   - "In the NES and Famicom, the top three (or five) bits are not driven,
//      and so retain the bits of the previous byte on the bus."
#include <cstdint>

#include "gtest/gtest.h"

#include "../include/bus.h"

namespace tests
{
namespace controller
{
namespace
{

// The 1-then-0 write sequence a game performs before reading. NESdev: "a 1/0
// write sequence is required to get the button states, after which the buttons
// can be read back one at a time."
void latch(Bus& console)
{
    console.write(0x4016, 0x01);
    console.write(0x4016, 0x00);
}

// Reads the eight button bits back as a mask in the same bit order the
// Button enum uses, so an expected value can be written as a set of buttons
// rather than as a bit sequence.
uint8_t read_port(Bus& console, const uint16_t addr)
{
    uint8_t mask = 0;
    for (int i = 0; i < 8; ++i) {
        if (console.read(addr) & 0x01) {
            mask |= static_cast<uint8_t>(1 << i);
        }
    }
    return mask;
}

}  // namespace

GTEST_TEST(controllers, report_the_buttons_in_the_documented_order)
{
    Bus console;

    // A and Start: the first and fourth bits out.
    console.controllers.set_port(0, Controllers::A | Controllers::Start);
    latch(console);

    EXPECT_EQ(0x01, console.read(0x4016) & 1) << "bit 0 is A";
    EXPECT_EQ(0x00, console.read(0x4016) & 1) << "bit 1 is B";
    EXPECT_EQ(0x00, console.read(0x4016) & 1) << "bit 2 is Select";
    EXPECT_EQ(0x01, console.read(0x4016) & 1) << "bit 3 is Start";
    EXPECT_EQ(0x00, console.read(0x4016) & 1) << "bit 4 is Up";
}

GTEST_TEST(controllers, every_button_round_trips)
{
    Bus console;
    const uint8_t all = Controllers::A | Controllers::B | Controllers::Select | Controllers::Start |
                        Controllers::Up | Controllers::Down | Controllers::Left | Controllers::Right;

    for (int i = 0; i < 8; ++i) {
        const uint8_t one = static_cast<uint8_t>(1 << i);
        console.controllers.set_port(0, one);
        latch(console);
        EXPECT_EQ(one, read_port(console, 0x4016)) << "button bit " << i << " did not come back alone";
    }

    console.controllers.set_port(0, all);
    latch(console);
    EXPECT_EQ(all, read_port(console, 0x4016));
}

// The property that makes the 1/0 sequence necessary in the first place.
GTEST_TEST(controllers, while_the_strobe_is_high_every_read_returns_button_a)
{
    Bus console;
    console.controllers.set_port(0, Controllers::B);  // B, deliberately NOT A

    console.write(0x4016, 0x01);  // strobe high and left there

    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(0x00, console.read(0x4016) & 1)
            << "read " << i << " should still be reporting A (not pressed), not shifting on to B";
    }

    console.controllers.press(0, Controllers::A);
    EXPECT_EQ(0x01, console.read(0x4016) & 1) << "while strobing, the register reloads continuously";
}

GTEST_TEST(controllers, reads_after_the_eighth_report_one)
{
    Bus console;
    console.controllers.set_port(0, 0x00);  // nothing pressed: the 8 real bits are all 0
    latch(console);

    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(0x00, console.read(0x4016) & 1) << "real button bit " << i << " should be 0";
    }
    for (int i = 8; i < 12; ++i) {
        EXPECT_EQ(0x01, console.read(0x4016) & 1) << "read " << i << " past the end must report 1";
    }
}

// Buttons are sampled at the latch, not at each read. A key pressed part-way
// through a read sequence must not appear until the next latch, or a game
// polling across a frame boundary sees a button that was never held.
GTEST_TEST(controllers, a_press_after_the_latch_is_not_visible_until_the_next_latch)
{
    Bus console;
    console.controllers.set_port(0, 0);
    latch(console);

    console.read(0x4016);  // consume the A bit
    console.controllers.press(0, Controllers::B);

    EXPECT_EQ(0x00, console.read(0x4016) & 1) << "B was pressed after the latch and must not appear yet";

    latch(console);
    EXPECT_EQ(0x00, console.read(0x4016) & 1) << "A still not pressed";
    EXPECT_EQ(0x01, console.read(0x4016) & 1) << "B is visible after the next latch";
}

GTEST_TEST(controllers, the_two_ports_are_independent_but_share_one_strobe)
{
    Bus console;
    console.controllers.set_port(0, Controllers::Left);
    console.controllers.set_port(1, Controllers::Right);

    // A single write to $4016 latches BOTH controllers - there is one strobe
    // line. Nothing is ever written to $4017 for this to work; that address is
    // the APU frame counter on write.
    latch(console);

    EXPECT_EQ(Controllers::Left, read_port(console, 0x4016));
    EXPECT_EQ(Controllers::Right, read_port(console, 0x4017));
}

// $4017 is the one address where a read and a write reach different devices.
GTEST_TEST(controllers, writing_4017_drives_the_apu_and_not_the_controller)
{
    Bus console;
    console.controllers.set_port(1, Controllers::Start);
    latch(console);

    // A write to $4017 is the APU frame counter. If it were routed to the
    // controllers it would act as a strobe and re-latch, which is observable:
    // the shift register would rewind to bit 0.
    console.read(0x4017);  // consume bit 0 (A, not pressed)
    console.write(0x4017, 0x80);

    EXPECT_EQ(0x00, console.read(0x4017) & 1) << "bit 1 is B; a rewind here would mean $4017 wrote the controller";
    EXPECT_EQ(0x00, console.read(0x4017) & 1) << "bit 2 is Select";
    EXPECT_EQ(0x01, console.read(0x4017) & 1) << "bit 3 is Start - the sequence continued rather than restarting";
}

// NESdev: the top bits "are not driven, and so retain the bits of the previous
// byte on the bus". This emulator returns $40 for them, which is what the real
// bus holds during `LDA $4016` - the high byte of the address. Pinned so the
// simplification is visible rather than incidental; see the comment on
// Controllers::open_bus_bits.
GTEST_TEST(controllers, the_undriven_bits_read_back_as_the_documented_approximation)
{
    Bus console;
    console.controllers.set_port(0, Controllers::A);
    latch(console);

    EXPECT_EQ(0x41, console.read(0x4016)) << "A pressed, plus the undriven high bits";
    EXPECT_EQ(0x40, console.read(0x4016)) << "B not pressed, plus the undriven high bits";
}

// A write to $4016 must not be observable as memory, and reading must not
// disturb RAM: the regression that would follow a decode mistake aliasing
// $4016 into ram[$16].
GTEST_TEST(controllers, the_strobe_write_does_not_land_in_ram)
{
    Bus console;
    console.write(0x0016, 0x5A);
    console.write(0x4016, 0x01);
    console.write(0x4016, 0x00);

    EXPECT_EQ(0x5A, console.read(0x0016)) << "$4016 aliased into RAM";
}

}  // namespace controller
}  // namespace tests
