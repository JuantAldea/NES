#pragma once
#include "apu.h"
#include "audio.h"
#include "controller.h"
#include "cpu.h"
#include "device.h"
#include "ppu.h"
#include "ram.h"
#include "rom.h"

class Bus
{
public:
    Bus();
    void write(const uint16_t addr, const uint8_t data);
    void write_ram(const uint16_t start_addr, const size_t n_bytes, const uint8_t* bytes);
    uint8_t read(const uint16_t addr);
    // Inserts a cartridge, and restores its battery-backed save if it has one.
    //
    // Flushes the OUTGOING cartridge's save first. That is not a nicety: this is
    // the drag-and-drop path in the frontend, so without it every ROM swap
    // discards whatever the previous game had written.
    bool load_cartridge(const std::string& path);

    // Writes the battery-backed work RAM to `<rom>.sav`, beside the cartridge.
    //
    // NOT called from the destructor, deliberately. Every ROM-backed test builds
    // a Bus and several load battery images, so a destructor that wrote files
    // would scatter .sav files through tests/test_files/ on every suite run -
    // and the fetch scripts assert how many files their directory holds. Saving
    // is an application-level act, so the two applications ask for it.
    //
    // Returns false only when there was something to write and writing failed;
    // "this cartridge has no battery" and "nothing was written to it" are both
    // true, because neither is an error the caller can act on.
    bool save_battery_ram();

    // Where `<rom>.sav` goes for a given cartridge path: the ROM's own name with
    // a .nes extension replaced, or .sav appended if it has none. Exposed for
    // the tests, which have to look at the file this produced.
    static std::string battery_save_path(const std::string& rom_path);

    // The RESET button: what the whole machine does, as distinct from
    // CPU::reset(), which is only the processor's share of it.
    //
    // RAM, PRG-RAM, VRAM and OAM all survive - blargg's ROMs count their own
    // resets in RAM across one, so a reset that cleared memory would look like
    // a power cycle and the second half of every apu_reset ROM would never run.
    void reset();

    // Whether the CPU cycle that just ran performed a WRITE, observed at
    // Bus::write rather than reported by the CPU - see the comment in
    // Bus::clock. Reads back meaningfully only immediately after a CPU cycle.
    //
    // The DMC halt asks CPU::next_cycle_is_write() instead, because the cycle
    // it cares about is the one the halt would land on rather than the one that
    // just ended. This remains as the independent observation of the same fact:
    // one derived from the CPU's schedule, one seen on the bus.
    bool cpu_wrote_this_cycle = false;

    // The joypads are NOT clocked by the address bus. NESdev's DMA page: "Joypads
    // are clocked via direct lines from the CPU, called joypad 1 /OE and joypad 2
    // /OE, rather than going over the address bus", and the NES-001 and AV
    // Famicom clock once per CONTIGUOUS SET of reads rather than once per read.
    // Only a DMA's repeated reads put two controller reads back to back, so this
    // is what stops each repeat clocking the pad. Set by Bus::read before the
    // device sees the access.
    bool controller_read_is_continuation = false;
    bool prev_bus_read_was_controller = false;

    // The address of the last read the CPU performed. A DMA's no-operation
    // cycles re-issue it - see the phantom-read block in Bus::clock.
    //
    // The initial value is unobservable, and mutating it survives for that
    // reason rather than because nothing tests it: a DMC DMA cannot happen
    // before the CPU has read at least once, the reset vector fetch being a
    // read, so this is always overwritten before anything repeats it.
    uint16_t last_cpu_read_addr = 0;

    // Whether a DMC halt has already been refused because the cycle it would
    // land on is a write. The retry then skips the phase wait in
    // advance_dmc_dma: both are the same one-cycle deferral, and serving them
    // in sequence delays the halt twice for one cause.
    bool halt_refused_for_write = false;

    // The cycle the DMC's read happened on, or a sentinel meaning "no fetch yet".
    // An OAM DMA requested on the very next cycle skips its halt and alignment,
    // because the DMC DMA has already stopped the CPU and left the bus on a get -
    // see PPU::request_OAM_DMA. Zero would be a real cycle number, and the first
    // DMA of a run is exactly where an off-by-one would hide.
    static constexpr uint64_t kNoDmcFetch = UINT64_MAX;
    uint64_t dmc_fetch_cycle = kNoDmcFetch;

    // The cartridge currently inserted, and whether anything has written to its
    // work RAM since it was loaded.
    //
    // The dirty flag is what stops a game that never saved from having its
    // untouched (or freshly restored) RAM written back over a good .sav - and
    // more to the point, stops a cartridge that failed to boot from replacing
    // real save data with zeroes. Set in Bus::write, which is the only path the
    // CPU reaches PRG-RAM through.
    std::string cartridge_path;
    bool prg_ram_dirty = false;

    // The audio output path. OFF BY DEFAULT, and that is a cost decision:
    // sampling means calling APU::mixer_output() on every CPU cycle, which is
    // several divisions at 1.79 MHz, and the ~1000 headless tests have no use
    // for a sample stream. The frontend turns it on.
    //
    // Note that "off" does not mean the APU stops - it means nothing collects
    // its output. Every APU test still exercises the same code it always did.
    AudioSampler audio;
    bool audio_enabled = false;

    // Set whenever a cartridge is inserted, cleared by whoever owns the audio
    // device once it has stopped it.
    //
    // AudioSampler::clear() moves both ring indices and so cannot be called
    // while a consumer is reading - but a cartridge swap must discard the
    // outgoing game's samples, or up to half a second of the previous game
    // plays over the start of the new one. Bus cannot stop the device and the
    // frontend cannot see the swap, so this flag is the handshake: Bus raises
    // it, the frontend lowers it at a point where the device is paused.
    bool audio_reset_pending = false;

    uint64_t total_cycles = 0;

    // CPU cycles elapsed, counting the ones OAM DMA steals - which CPU::clock
    // never sees, so CPU::total_cycles does not count them.
    //
    // This is the divide-by-two everything phase-sensitive on the CPU bus hangs
    // off: the APU frame counter's get/put alignment and OAM DMA's are the same
    // one. Deriving DMA's from CPU::total_cycles made the two disagree by the
    // length of every DMA that had already run.
    uint64_t cpu_cycles = 0;

    // The CPU data bus is not driven by anything on a read of a write-only
    // register or of unmapped space, so it holds whatever was last put there -
    // by the previous read, or by the value half of a write.
    //
    // Returning 0 for those instead is what blargg's cpu_exec_space_apu
    // catches: it executes code THROUGH $4000-$40FF, so the bytes the CPU
    // fetches there ARE the open-bus value, and a constant 0 sends execution to
    // the wrong address.
    //
    // Unlike the PPU's own latch (PPU::open_bus) this one does not decay. The
    // CPU bus is refreshed every cycle, so the decay is not observable; the
    // PPU's is, and ppu_open_bus measures it.
    uint8_t cpu_open_bus = 0;

    // Returns true on the master tick that completed a CPU instruction, which
    // is the only moment an outside observer can safely inspect CPU state: in
    // between, the schedule has a half-resolved address and a latched operand
    // that belong to no architectural register.
    //
    // Every existing caller ignores the return and is unaffected.
    bool clock();
    bool clock_CPU();
    void clock_PPU();

    // Set for the duration of clock_CPU() only, so Bus::write can tell a CPU
    // write cycle from every other caller of the same public function.
    bool watching_cpu_access = false;

    // DMC DMA, as four cycles rather than one. NESdev: "DMC DMA normally takes
    // 3 or 4 cycles, depending on whether alignment is needed."
    //
    //   Halt    the CPU is stopped. This cycle IS stolen - an earlier version
    //           let the CPU run through it, which made a DMA cost 2-3 cycles
    //           where hardware costs 4, and sprdma_and_dmc_dma printed 783
    //           where the reference table reads 527. (That "expected ~515" this
    //           line used to cite was an estimate, and wrong - see the header of
    //           tests/test_files/fetch_dmc_dma.sh.)
    //   Dummy   always spent, no work done.
    //   Align   spent only "if the next cycle is not a get cycle".
    //   Get     the read itself.
    //
    // All four are stolen from the CPU, which is what makes the total the
    // documented 4 cycles, or 3 when alignment is not needed.
    //
    // The halt is not ENTERED on a write cycle: "the CPU only allows this on
    // read cycles. If the CPU is writing, it ignores the halt...repeating until
    // successful". So the wait happens in Idle, before any cycle is stolen,
    // rather than as a Halting state that burns cycles while it retries.
    // Extend exists only for a DMC read landing on an OAM transfer's LAST cycle,
    // where the collision costs 3 rather than 2 - it is the one extra cycle, and
    // spending it before Align keeps the read itself on a get. See
    // advance_dmc_dma.
    enum class DmcDma { Idle, Halt, Dummy, Align, Extend, Get };
    DmcDma dmc_dma = DmcDma::Idle;

    bool dmc_dma_holds_the_bus() const { return dmc_dma != DmcDma::Idle; }

    void advance_dmc_dma();

    // Puts the access the CPU is ATTEMPTING back on the bus without letting it
    // progress - what a DMA's no-operation cycles do. Shared by both DMAs.
    void repeat_halted_cpu_access();

    // Passed to CPU::clock, which prints one line per instruction to stdout.
    // Off by default so the headless harnesses stay silent; the frontend's
    // Trace checkbox is the only thing that sets it.
    bool trace_cpu = false;

    // Runs until the CPU completes the instruction it is in the middle of.
    // Returns false if it gave up instead - see the definition for why that is
    // a real outcome and not defensive padding.
    bool step_instruction();

    // Runs until the PPU finishes the frame it is in the middle of.
    //
    // This is not the same as clocking a fixed number of times, which is what
    // everything that needed a frame used to do. An NTSC frame is 341x262
    // dots and the master clock runs four times per dot, so 341*262*4 looks
    // exact - but odd frames with rendering enabled skip their last dot, so
    // that count is one dot too many every other frame. It was harmless while
    // nothing rendered and the skip never armed; it is not now.
    //
    // Watching the PPU's own frame counter is right by construction and cannot
    // drift, whatever the PPU decides a frame is.
    void run_frame();

    CPU cpu;
    APU apu;
    PPU ppu;
    SystemRAM ram;
    PrgRAM prg_ram;
    ROM rom;
    Controllers controllers;

protected:
    // Reads and writes go through ONE decode function, so the two paths cannot
    // drift apart on any address where hardware agrees - which is every address
    // but one.
    //
    // $4017 is the exception, and it is a real property of the NES rather than
    // an emulator convenience: writing it sets the APU frame counter, reading
    // it returns controller port 2. They are different devices. That is why
    // decode takes a direction instead of the read and write paths each growing
    // their own special case, which is exactly how the two would come to
    // disagree about the addresses where hardware does NOT.
    enum class Access { Read, Write };

    struct DecodedAddress {
        Device* device;  // nullptr for open-bus ranges (no device backs them)
        uint16_t effective_addr;
    };
    DecodedAddress decode(const uint16_t addr, const Access access);
};
