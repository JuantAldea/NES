#pragma once
#include <cstdint>
#include <memory>
#include <string>

class ROM;
class Mapper;

// Every mapper number this emulator has a NAME for. Deliberately far longer
// than the list it has a BOARD for, and the two must not be conflated: what is
// implemented is decided by the board table in mapper.cpp and nowhere else.
//
// The point of carrying the unimplemented ones is the error message. "mapper 69
// (Sunsoft FME-7) is not supported" tells you what to go and read; "mapper 69"
// sends you to a wiki to find out what you even have. Adding a number here
// costs a table row and claims nothing.
//
// Numbers are the iNES assignments, so the gaps are real - they are the
// mappers nobody has bothered to name here yet, not an ordering.
enum class MapperId : uint16_t {
    nrom = 0,
    mmc1 = 1,
    uxrom = 2,
    cnrom = 3,
    mmc3 = 4,
    mmc5 = 5,
    axrom = 7,
    mmc2 = 9,
    mmc4 = 10,
    color_dreams = 11,
    cprom = 13,
    bandai_fcg = 16,
    jaleco_ss88006 = 18,
    namco_163 = 19,
    vrc4a = 21,
    vrc2a = 22,
    vrc4b = 23,
    vrc6a = 24,
    vrc4c = 25,
    vrc6b = 26,
    action53 = 28,
    unrom512 = 30,
    irem_g101 = 32,
    taito_tc0190 = 33,
    bnrom = 34,
    rambo1 = 64,
    gxrom = 66,
    sunsoft4 = 68,
    sunsoft_fme7 = 69,
    codemasters = 71,
    vrc3 = 73,
    vrc1 = 75,
    holy_diver = 78,
    nina003 = 79,
    vrc7 = 85,
    txsrom = 118,
    tqrom = 119,
    unrom_7408 = 180,
    dxrom = 206,
    namco_175 = 210,
};

// The board name for a mapper number, or nullptr when the table above has never
// heard of it. Naming is independent of support - most of these have no board.
const char* mapper_name(MapperId id);

// Whether a board exists for this mapper number. The ONE place a number is
// turned into an implementation is make_mapper below, and this asks the same
// table, so the two cannot drift apart the way a header check and a
// construction switch did.
bool mapper_supported(MapperId id);

// Empty when the header's bank counts are consistent with what the board can
// physically be wired as; otherwise the complaint, ready for load() to print.
// Returns empty for an unsupported mapper - mapper_supported answers that, and
// answering it twice would produce two different messages for one cause.
std::string mapper_header_error(MapperId id, size_t prg_16k_banks, size_t chr_8k_banks);

// Constructs the board, or returns null when there is none. Called once per
// cartridge by ROM::load.
std::unique_ptr<Mapper> make_mapper(MapperId id, ROM& rom);

// One cartridge board's decoding logic, as an object rather than as a run of
// `if (mapper_id == N)` branches inside ROM.
//
// WHY THIS EXISTS. Every mapper used to add a branch to each of ROM::read,
// ROM::write and ROM::chr_read, and a clause to the load-time header checks -
// so a board's behaviour was spread across four functions and interleaved with
// three other boards'. Four mappers was already twelve sites, and MMC1 was the
// one that made it untenable: its registers are not latches but a 5-bit shift
// register clocked one bit per write, which does not read as another branch
// beside CNROM's single assignment.
//
// WHAT STAYS ON ROM, and why it is not laziness. The header-derived sizes, the
// mirroring the PPU reads live, and the PRG-RAM enable/protect bits: Bus::decode
// consults the last of those on every access to $6000-$7FFF and it holds a ROM,
// not a Mapper. Everything that is one board's business - the register file,
// the mode bits, the MMC3's IRQ counter - lives on the subclass.
class Mapper
{
public:
    explicit Mapper(ROM& rom) : rom{rom} {}
    virtual ~Mapper() = default;

    Mapper(const Mapper&) = delete;
    Mapper& operator=(const Mapper&) = delete;

    // A CPU write into cartridge space. Only $8000-$FFFF reaches a mapper -
    // $6000-$7FFF is PRG-RAM, which is a separate Bus device.
    virtual void cpu_write(const uint16_t addr, const uint8_t data) = 0;

    // A CPU read from $8000-$FFFF, already known to have PRG-ROM behind it.
    virtual uint8_t prg_read(const uint16_t addr) const = 0;

    // Which byte of the cartridge's PATTERN MEMORY a PPU address reads through
    // right now. An offset rather than a byte, because the same mapping has to
    // serve CHR-ROM and CHR-RAM and only one of those is the cartridge's to
    // read - the RAM array lives in the PPU (see PPU::chr_ram).
    //
    // Splitting them was the bug. CHR-RAM was addressed FLAT, straight by
    // `chr_ram[addr]`, on the assumption that RAM the console supplies is not
    // banked. It is not the console's: CHR-RAM sits on the cartridge behind the
    // mapper's CHR address lines exactly as CHR-ROM does, so MMC1 in 4KB mode
    // pages an 8KB chip as two halves. Holy Mapperel's three CHR-RAM boards
    // caught it and its six CHR-ROM boards could not have.
    //
    // The default is the single 8KB window at whatever bank a one-latch board
    // selected, which is NROM, UNROM and CNROM exactly - chr_bank is simply
    // always 0 on the first two.
    virtual uint32_t chr_offset(const uint16_t ppu_addr) const;

    // Where a CPU address in $6000-$7FFF lands in the cartridge's work RAM.
    // Defaults to the flat window, which is every board that carries a single
    // 8KB chip - only MMC1's SOROM and SXROM page more than the window shows.
    //
    // Returned unclamped: ROM::prg_ram_offset folds it into the size the header
    // declares, so a board cannot reach RAM it does not have and no override
    // has to repeat the bound.
    virtual uint16_t prg_ram_offset(const uint16_t cpu_addr) const;

    // Does this board need a tick on every CPU cycle? Asked once, at load, and
    // cached on ROM - the alternative is a virtual call on the emulator's
    // hottest path for the seven boards out of eight that would ignore it.
    virtual bool wants_cpu_clock() const { return false; }

    // Does the cartridge drive CIRAM A10 itself, rather than through the
    // horizontal/vertical/one-screen choice in ROM::mirroring? Asked once at
    // load and cached, for the same reason as wants_cpu_clock: nametable
    // decoding runs on every background fetch.
    //
    // TxSROM is the only board here that does. On it CHR A17 is wired straight
    // to CIRAM A10 instead of to the MMC3's own mirroring output, which makes
    // the nametable a property of the CHR bank registers and gives the board
    // one-screen mirroring the MMC3 does not otherwise have.
    virtual bool drives_ciram_a10() const { return false; }

    // Which of the two 1KB CIRAM pages nametable slot `screen` (0-3) reads
    // through. Only reached when drives_ciram_a10() returned true.
    virtual uint8_t ciram_page(const uint16_t screen) const
    {
        (void)screen;
        return 0;
    }

    // One M2 cycle. Only reached when wants_cpu_clock() returned true.
    virtual void clock_cpu_cycle() {}

    // A completed PPU read from $0000-$1FFF, reported AFTER the byte was
    // served. MMC2 and MMC4 change CHR bank when particular tiles are fetched,
    // and the triggering read itself comes from the old bank.
    //
    // Separate from observe_a12 below because the two want opposite ordering:
    // the A12 filter times an edge and must see the address as it is put on the
    // bus, while a latch must not apply to the fetch that set it. One hook
    // serving both would silently be wrong for whichever came second.
    virtual void observe_pattern_fetch(const uint16_t ppu_addr) { (void)ppu_addr; }

    // PPU address line A12, watched for the MMC3's scanline counter. A default
    // rather than a pure virtual because three of the four boards genuinely do
    // not have the wire - making them each write an empty override would state
    // the same thing four times and invite one of them to be filled in.
    virtual void observe_a12(const uint16_t ppu_addr, const uint64_t ppu_cycle)
    {
        (void)ppu_addr;
        (void)ppu_cycle;
    }

protected:
    ROM& rom;
};

// NROM (0): no banking at all. A 16KB image is mirrored across $8000-$FFFF and
// a 32KB one fills it.
class NRom final : public Mapper
{
public:
    using Mapper::Mapper;
    void cpu_write(const uint16_t addr, const uint8_t data) override;
    uint8_t prg_read(const uint16_t addr) const override;
};

// MMC1 (1): the SxROM family. The first board here whose registers are not
// latches - the CPU cannot present five bits at once, so writes are serialised
// through a shift register one bit at a time.
class Mmc1 final : public Mapper
{
public:
    explicit Mmc1(ROM& rom);

    void cpu_write(const uint16_t addr, const uint8_t data) override;
    uint8_t prg_read(const uint16_t addr) const override;
    uint32_t chr_offset(const uint16_t ppu_addr) const override;
    uint16_t prg_ram_offset(const uint16_t cpu_addr) const override;

    // The serial port. Five writes carrying one bit each in bit 0 fill this,
    // and the fifth commits it to whichever register the LAST write's address
    // selects. Bit 7 of any write empties it instead.
    uint8_t shift = 0;
    uint8_t shift_count = 0;

    // $8000-$9FFF. bits 0-1 mirroring, bits 2-3 PRG mode, bit 4 CHR mode.
    //
    // Power-on is $0C and that value is load-bearing, not arbitrary: PRG mode 3
    // fixes the LAST bank at $C000-$FFFF, which is where the reset vector is
    // read from. Come up in any other mode and the console jumps into whatever
    // bank happens to be there. Hardware guarantees it, and the reset that bit
    // 7 of a write performs ORs $0C in for the same reason.
    uint8_t control = 0x0C;

    // $A000-$BFFF and $C000-$DFFF: 5-bit CHR bank numbers, 4KB each. On boards
    // larger than 256KB of PRG or 8KB of work RAM these registers carry more
    // than CHR - see prg_bank_offset and wram_disabled.
    uint8_t chr_reg[2] = {0, 0};

    // $E000-$FFFF: bits 0-3 the 16KB PRG bank, bit 4 the work-RAM disable.
    uint8_t prg_reg = 0;

    // $E000 bit 4, and on SNROM also $A000 bit 4: hold the work RAM disabled so
    // a dying power rail cannot corrupt a save. Inverted from the register bit,
    // which is a disable rather than an enable.
    bool wram_disabled() const { return (prg_reg & 0x10) != 0; }

    // bits 2-3 of control.
    uint8_t prg_mode() const { return static_cast<uint8_t>((control >> 2) & 0x03); }

    // bit 4 of control: 0 switches 8KB of CHR at once, 1 switches two 4KB
    // halves independently.
    bool chr_4k_mode() const { return (control & 0x10) != 0; }

    // Which 16KB PRG bank and 4KB CHR bank the given address currently reads
    // through. Pure functions of the registers, so they can be reasoned about
    // and tested without driving a read.
    uint32_t prg_bank_offset(const uint16_t cpu_addr) const;
    uint32_t chr_bank_offset(const uint16_t ppu_addr) const;

private:
    // Commits a completed 5-bit value to the register the address selects, and
    // applies whatever side effects it has outside this class - mirroring lives
    // on ROM because the PPU reads it live.
    void write_register(const uint16_t addr, const uint8_t value);

    void apply_mirroring();

    // MMC1 ignores a write that lands on the CPU cycle immediately after
    // another one. That is not an optimisation: a read-modify-write instruction
    // targeting $8000-$FFFF puts two writes back to back, and games rely on
    // only the FIRST being seen. Without this, an INC $E000 clocks two bits in
    // and desynchronises the shift register for every write that follows.
    //
    // Held as an absolute cycle rather than a flag so that "the previous cycle"
    // stays true across whatever else the bus did in between.
    uint64_t last_write_cycle = 0;
    bool wrote_before = false;
};

// UNROM (2): a switchable 16KB PRG window at $8000-$BFFF, with $C000-$FFFF
// wired to the last bank so the vectors cannot be banked away. No CHR-ROM - the
// console's CHR-RAM is the pattern memory.
class UnRom final : public Mapper
{
public:
    using Mapper::Mapper;
    void cpu_write(const uint16_t addr, const uint8_t data) override;
    uint8_t prg_read(const uint16_t addr) const override;
};

// UNROM 7408 (180): UNROM with the two PRG windows the other way round.
//
// The switchable bank is at $C000 and the FIXED one - always bank 0 - is at
// $8000, which is the mirror image of UnRom above and the whole difference.
//
// Crazy Climber is the board's reason to exist. UxROM boards without the
// protection diode suffer bus conflicts: the write that selects a bank is also a
// read of the ROM at that address, and the two must agree. Putting the fixed
// bank in the window the code writes through makes the value it reads there a
// constant, so a bank-select routine living in the fixed half cannot conflict
// with whichever bank it is selecting.
class UnRom7408 final : public Mapper
{
public:
    using Mapper::Mapper;
    void cpu_write(const uint16_t addr, const uint8_t data) override;
    uint8_t prg_read(const uint16_t addr) const override;
};

// GxROM (66): one register, both halves of the cartridge.
//
// The simplest board here that switches PRG and CHR at once, and it does it with
// a single write and no state of its own beyond that byte:
//
//     xxPP xxCC     PP = 32KB PRG bank at $8000, CC = 8KB CHR bank at $0000
//
// Two bits each, so four banks of each at most - 128KB PRG and 32KB CHR. GNROM,
// MHROM and the Nintendo/Bandai variants are all this.
class GxRom final : public Mapper
{
public:
    using Mapper::Mapper;
    void cpu_write(const uint16_t addr, const uint8_t data) override;
    uint8_t prg_read(const uint16_t addr) const override;

private:
    uint8_t bank_select = 0;
};

// CNROM (3): NROM's PRG with a switchable 8KB CHR window.
class CnRom final : public Mapper
{
public:
    using Mapper::Mapper;
    void cpu_write(const uint16_t addr, const uint8_t data) override;
    uint8_t prg_read(const uint16_t addr) const override;
};

// The shared half of MMC2 (9) and MMC4 (10): a CHR window pair whose bank is
// chosen by what the PPU last looked at.
//
// THIS IS THE FIRST BOARD HERE THE CPU DOES NOT FULLY DRIVE. Every other mapper
// changes state only when written to. These two watch the PPU fetch a tile, and
// if it is tile $FD or $FE out of a particular window, swap that window's bank
// for the next fetch. Punch-Out!!'s big animated faces are the reason: two 4KB
// halves alternating under the PPU's nose gets far more sprite detail on screen
// than the CHR budget of the era otherwise allows.
//
// Each window therefore has TWO bank registers and a latch saying which is
// live - hence chr_bank[window][latch] rather than one register per window.
class LatchedChr : public Mapper
{
public:
    using Mapper::Mapper;

    void cpu_write(const uint16_t addr, const uint8_t data) override;
    uint32_t chr_offset(const uint16_t ppu_addr) const override;
    void observe_pattern_fetch(const uint16_t ppu_addr) override;

    // [window][latch]: window 0 is PPU $0000-$0FFF and window 1 is $1000-$1FFF;
    // latch 0 is the $FD register and latch 1 the $FE one. Written through
    // $B000, $C000, $D000 and $E000 respectively, 5 bits each.
    uint8_t chr_bank[2][2] = {{0, 0}, {0, 0}};

    // Which register each window is currently reading through. Indexes the
    // second subscript above, so 0 means $FD is live.
    uint8_t latch[2] = {0, 0};

    // $A000-$AFFF, 4 bits. What it means depends on the board: an 8KB bank for
    // MMC2, a 16KB one for MMC4, which is the only reason these are two classes
    // and not one.
    uint8_t prg_bank = 0;

protected:
    // Does an address in the LOWER window latch, and to which register? MMC2
    // decodes exactly $0FD8 and $0FE8; MMC4 decodes the eight-byte runs
    // $0FD8-$0FDF and $0FE8-$0FEF. Both decode runs in the upper window, which
    // is why only this half is virtual.
    //
    // The asymmetry is real silicon, not a documentation artefact: MMC2 came
    // first and its lower-window comparator is narrower.
    virtual bool lower_window_latches(const uint16_t ppu_addr, uint8_t& which) const = 0;
};

// MMC2 (9), PxROM. One game: Punch-Out!!. An 8KB switchable PRG window at
// $8000-$9FFF with the remaining three 8KB banks wired down.
class Mmc2 final : public LatchedChr
{
public:
    using LatchedChr::LatchedChr;
    uint8_t prg_read(const uint16_t addr) const override;

protected:
    bool lower_window_latches(const uint16_t ppu_addr, uint8_t& which) const override;
};

// MMC4 (10), FxROM. Fire Emblem, Famicom Wars. MMC2's CHR latch with UNROM's
// PRG layout - a 16KB switchable window at $8000-$BFFF and the last bank fixed
// at $C000 - plus work RAM, which PxROM has none of.
class Mmc4 final : public LatchedChr
{
public:
    using LatchedChr::LatchedChr;
    uint8_t prg_read(const uint16_t addr) const override;

protected:
    bool lower_window_latches(const uint16_t ppu_addr, uint8_t& which) const override;
};

// FME-7 (69), Sunsoft JLROM/JSROM. Batman: Return of the Joker, Gimmick!.
//
// A COMMAND/PARAMETER PAIR rather than a register per address: $8000 selects
// which of fourteen registers the next $A000 write lands in. That is a
// different shape from every other board here - MMC1 serialises one register
// over five writes, MMC3 has a select/data pair for eight of its registers but
// decodes the rest by address, and this decodes nothing by address at all.
//
// AND IT IS THE FIRST BOARD THAT COUNTS CPU CYCLES. MMC3's counter watches PPU
// A12 and therefore counts scanlines by accident; this one is a 16-bit
// down-counter clocked by M2, so it measures time regardless of what the PPU is
// doing - it fires with rendering off, mid-frame, or during a long DMA. That is
// why Mapper::wants_cpu_clock exists.
class Fme7 final : public Mapper
{
public:
    using Mapper::Mapper;

    void cpu_write(const uint16_t addr, const uint8_t data) override;
    uint8_t prg_read(const uint16_t addr) const override;
    uint32_t chr_offset(const uint16_t ppu_addr) const override;

    bool wants_cpu_clock() const override { return true; }
    void clock_cpu_cycle() override;

    // $8000, low nibble: which register the next $A000 write addresses.
    uint8_t command = 0;

    // Commands $0-$7: eight 1KB CHR banks, one per 1KB of pattern space.
    uint8_t chr[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    // Commands $9-$B: 8KB PRG banks at $8000, $A000 and $C000. $E000-$FFFF is
    // wired to the last bank, so the vectors cannot be switched away.
    uint8_t prg[3] = {0, 0, 0};

    // Command $8, the $6000-$7FFF window:
    //
    //   bit 6  1 selects work RAM, 0 selects PRG-ROM
    //   bit 7  with bit 6 set, enables the RAM
    //   0-5    the bank, used only in the ROM case
    //
    // Bit 6 selects and bit 7 enables, in that order - see write_register for
    // the evidence, because the obvious reading of the two is the wrong one.
    //
    // This is the only board here that can put PRG-ROM in that window, and
    // supporting it is what made Bus::decode ask the cartridge about $6000
    // before assuming work RAM (ROM::prg_rom_at_6000). Before that it could
    // not, and this register was implemented as "ROM selected means open bus"
    // - which cost three of the four digits of the M69 oracle's detail code,
    // not the one it looked like. See the M69 rows in
    // tests/holy_mapperel_tests.cpp.
    uint8_t prg_ram_control = 0;

    // Command $D: bit 7 runs the counter, bit 0 lets it assert /IRQ. Writing
    // the register also acknowledges a pending assertion, which is how a
    // handler clears it - there is no separate acknowledge port.
    bool irq_counter_enabled = false;
    bool irq_enabled = false;

    // Commands $E and $F: the low and high halves of a 16-bit down-counter that
    // ticks once per CPU cycle and asserts on the $0000 -> $FFFF wrap. It keeps
    // counting afterwards rather than stopping, so a handler that does not
    // reload gets an interrupt every 65536 cycles.
    uint16_t irq_counter = 0;
    bool irq_asserted = false;

private:
    void write_register(const uint8_t value);
};

// AxROM (7): one 32KB PRG window covering the whole of $8000-$FFFF, and
// one-screen mirroring the game picks at runtime.
//
// It is the only board here with NO fixed PRG bank. Every other mapper wires
// part of the address space down so the vectors at $FFFA-$FFFF survive a bank
// switch; AxROM switches all 32KB at once, so an AxROM game has to keep a copy
// of its vectors and its interrupt handlers in every bank. That is a constraint
// on the cartridge, not on the emulator, and it is why nothing here fixes a
// half - reproducing the board means reproducing the hazard.
//
// One-screen mirroring is the other half. AxROM ties CIRAM A10 to a register
// bit instead of to a PPU line, so all four nametable slots show the same
// screen and the game chooses which. That is exactly the mode MMC1 already
// needed, which is why this board costs a register and no new PPU work.
class AxRom final : public Mapper
{
public:
    explicit AxRom(ROM& rom);

    void cpu_write(const uint16_t addr, const uint8_t data) override;
    uint8_t prg_read(const uint16_t addr) const override;

    // The single register, latched from the data of a write anywhere in
    // $8000-$FFFF. Bits 0-2 are the 32KB bank; bit 4 chooses the screen.
    uint8_t bank_select = 0;
};

// MMC3 (4): eight bank registers behind a select/data pair, runtime mirroring,
// PRG-RAM gating, and an IRQ counter clocked by PPU A12. The register semantics
// are documented on the fields in rom.h, which is still where they live.
// Not final: TxSROM below is an MMC3 with one wire moved, and inherits the
// whole register file, the PRG/CHR banking and the A12 IRQ counter unchanged.
class Mmc3 : public Mapper
{
public:
    using Mapper::Mapper;
    void cpu_write(const uint16_t addr, const uint8_t data) override;
    uint8_t prg_read(const uint16_t addr) const override;
    uint32_t chr_offset(const uint16_t ppu_addr) const override;
    void observe_a12(const uint16_t ppu_addr, const uint64_t ppu_cycle) override;

    // Eight bank registers behind a select/data pair, rather than a single
    // latch. $8000 chooses which of R0-R7 the next $8001 write lands in, and
    // also carries the two mode bits that decide WHICH window each register
    // drives:
    //
    //   R0, R1   2KB CHR banks   (bit 0 ignored - "R0 and R1 ignore the bottom bit")
    //   R2-R5    1KB CHR banks
    //   R6, R7   8KB PRG banks   (top two bits ignored - only 6 PRG address lines)
    //
    // The two mode bits invert which half of the address space the switchable
    // windows occupy, which is why they are kept rather than folded in at write
    // time: a later mode change has to re-point the existing register values.
    uint8_t bank_select = 0;  // the last value written to $8000
    uint8_t bank[8] = {0};    // R0-R7

    // $8000 bit 6: 0 -> $8000-$9FFF switchable, $C000-$DFFF fixed to second-last
    //              1 -> $C000-$DFFF switchable, $8000-$9FFF fixed to second-last
    bool prg_mode_swapped() const { return (bank_select & 0x40) != 0; }

    // $8000 bit 7: swaps the 2KB and 1KB CHR windows between $0000 and $1000.
    bool chr_a12_inverted() const { return (bank_select & 0x80) != 0; }

    // --- scanline IRQ counter -----------------------------------------------
    //
    // The MMC3 has no idea what a scanline is. It counts RISING EDGES OF PPU
    // ADDRESS LINE A12, and a scanline happens to produce exactly one of them
    // in the usual configuration: the background fetches sit in one pattern
    // table and the sprite fetches in the other, so A12 goes up once per line
    // when the fetch phase crosses over. Games get "interrupt at scanline N" by
    // counting those edges - which is why a game that puts both tables on the
    // same side gets no IRQs at all, and why the counter is clocked just as
    // happily by a program poking $2006 with rendering switched off.
    //
    // blargg's readme for the IRQ ROMs is explicit about that second case:
    // "The ROMs mainly test behavior by manually clocking the MMC3's IRQ
    // counter by writing to $2006 to change the current VRAM address." A design
    // that hooked only the rendering fetches would pass none of them.
    uint8_t irq_latch = 0;            // $C000, the value reloaded into the counter
    uint8_t irq_counter = 0;          // the live count
    bool irq_reload_pending = false;  // set by $C001, consumed by the next edge
    bool irq_enabled = false;         // $E001 enables, $E000 disables
    bool irq_asserted = false;        // are we currently pulling /IRQ low?

    // The A12 line as the mapper last saw it, and the PPU dot at which it went
    // low. Only the duration matters, so one timestamp is enough.
    bool a12_high = false;
    uint64_t a12_low_since = 0;

    // A rising edge only counts if A12 has been low for a while first. NESdev:
    // the counter is "triggered on a rising edge after the line has remained
    // low for three falling edges of M2" - M2 is the CPU clock, so three of its
    // cycles, and the PPU runs three times faster, giving nine PPU dots.
    //
    // The filter is not an optimisation, it is what makes the count mean
    // "scanline". Within one background tile fetch A12 drops for exactly four
    // dots (the nametable and attribute reads at $2xxx, between two pattern
    // reads at $1xxx); without the filter every tile would clock the counter
    // and an IRQ meant for one scanline would arrive 32 times a line.
    //
    // Nine rather than four-plus-one because the margin has to survive both
    // sides: real clocks follow low periods of 16 dots and up, so there is a
    // wide gap between "noise" and "signal" and the threshold only has to land
    // inside it. 3-A12_clocking is what pins this - see mmc3_rom_tests.cpp.
    static constexpr uint64_t a12_filter_dots = 9;

private:
    // Which 1KB CHR bank the given PPU address currently reads through, and
    // which 8KB PRG bank the given CPU address does. Split out because both are
    // pure functions of the register file and the mode bits, and are far easier
    // to test and to reason about than the same arithmetic inlined into the
    // read paths.
    uint16_t chr_bank_for(const uint16_t ppu_addr) const;
    uint16_t prg_bank_for(const uint16_t cpu_addr) const;

    // One filtered rising edge of A12: reload or decrement, then decide whether
    // to pull /IRQ low. Split out from observe_a12 so the edge DETECTION and the
    // counter SEMANTICS can be read - and mutated - independently.
    void clock_irq_counter();
};

// TxSROM (118): the TKSROM and TLSROM boards. An MMC3 with ONE WIRE MOVED, and
// nothing else different - same registers, same PRG and CHR banking, same A12
// IRQ counter, which is why this inherits all of it and overrides two calls.
//
// NESdev: "The CHR A17 line connects directly to CIRAM A10 line instead of
// MMC3's CIRAM A10 output, to compensate for the MMC3's lack of single-screen
// mirroring."
//
// So the nametable a slot shows is bit 7 of whichever CHR bank register covers
// it, and $A000 - the MMC3's own mirroring register - is "bypassed by the
// configuration described above, so writing here has no effect". This class
// does not intercept that write: it lets Mmc3::cpu_write latch it into
// ROM::mirroring as usual, where nothing reads it, because ROM::nametable_page
// consults the board instead. Suppressing the write would model the same
// behaviour and hide the reason.
//
// WHICH register drives which slot depends on the $8000 bit 7 CHR inversion,
// because it decides which registers are mapped low:
//
//   inversion off   R0 -> $2000-$27FF    R1 -> $2800-$2FFF   (the 2KB banks)
//   inversion on    R2 -> $2000-$23FF    R3 -> $2400-$27FF
//                   R4 -> $2800-$2BFF    R5 -> $2C00-$2FFF   (the 1KB banks)
//
// The rule behind the table is one sentence - "those bits are ignored if
// corresponding CHR banks are mapped at $1000-$1FFF via $8000" - and it is not
// arbitrary: only the registers currently driving $0000-$0FFF have their A17 on
// the CIRAM line, so inverting the CHR windows swaps which set is listened to.
class TxSRom final : public Mmc3
{
public:
    using Mmc3::Mmc3;

    bool drives_ciram_a10() const override { return true; }
    uint8_t ciram_page(const uint16_t screen) const override;
};
