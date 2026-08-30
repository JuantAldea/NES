// The headless driver: runs a cartridge with no window and no sound, and exits
// when the CPU traps.
//
// It exists for the ROMs that report by trapping - most of blargg's do - and for
// checking that a cartridge loads at all without starting a GUI. The exit code
// is the trap PC, which is what makes it usable from a shell.
#include <argparse/argparse.hpp>
#include <iostream>
#include <string>

#include "bus.h"

int main(int argc, char** argv)
{
    // help only, not `all`: `all` adds --version, and this project has no
    // version to report - see the same choice in frontend/main.cpp.
    argparse::ArgumentParser args("NES", "", argparse::default_arguments::help);
    args.add_description("Runs a cartridge headlessly until the CPU traps, then exits with the trap address.");

    args.add_argument("rom").help("cartridge image to run");
    args.add_argument("--feedback")
        .help("address whose bits 0 and 1 raise IRQ and NMI, for test harnesses that drive interrupts from memory")
        .scan<'x', uint16_t>()
        .metavar("ADDR");

    try {
        args.parse_args(argc, argv);
    } catch (const std::exception& error) {
        // A missing ROM argument arrives here, which is what makes "no arguments
        // exits 1" true - the functional check asserts it. Letting the exception
        // escape would abort instead, and an abort is not a rejection.
        std::cerr << error.what() << "\n\n" << args;
        return 1;
    }

    const std::string rom = args.get<std::string>("rom");

    Bus console;
    if (!console.load_cartridge(rom)) {
        std::cerr << "Failed to load cartridge: " << rom << std::endl;
        return 1;
    }
    console.cpu.reset();

    // Was a hard-coded 0xbffc behind an `if (feedback_register)` that was always
    // false, next to a whole second main() behind `#if 1 / #else` containing an
    // OAM DMA experiment. Both are gone; the useful half is this flag.
    const uint16_t feedback_register = args.present<uint16_t>("--feedback").value_or(0);
    if (feedback_register != 0) {
        console.write(feedback_register, 0x0);
    }

    while (true) {
        const uint16_t previous_pc = console.cpu.registers.PC;
        const bool executed = console.cpu.clock(false);

        if (feedback_register != 0) {
            const uint8_t feedback = console.cpu.read(feedback_register);
            if (feedback & 0x2) {
                console.write(feedback_register, static_cast<uint8_t>(feedback & ~0x2));
                console.cpu.raise_NMI();
                continue;
            }
            if (feedback & 0x1) {
                console.write(feedback_register, static_cast<uint8_t>(feedback & ~0x1));
                console.cpu.raise_IRQ();
                continue;
            }
        }

        if (executed && previous_pc == console.cpu.registers.PC) {
            // A trap is this driver's only exit, so it is also its only chance
            // to write a save. It costs two lines and it is the difference
            // between "the CLI does not persist" being a decision and being an
            // omission nobody noticed.
            console.save_battery_ram();
            std::cerr << "TRAP " << std::hex << previous_pc << std::endl;
            return previous_pc;
        }
    }
}
