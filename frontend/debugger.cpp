#include "debugger.h"

#include <cstdio>

#include "imgui.h"

#include "../include/bus.h"
#include "../include/frame_dump.h"
#include "../include/instruction.h"

namespace nes_gui
{
namespace
{

// Highlight colours for the memory view, carried over from the Qt debugger so
// the two read the same way.
const ImVec4 colour_pc(1.00f, 0.35f, 0.35f, 1.0f);
const ImVec4 colour_sp(0.45f, 0.65f, 1.00f, 1.0f);
const ImVec4 colour_stack_base(0.40f, 0.90f, 0.45f, 1.0f);
const ImVec4 colour_zero(0.45f, 0.45f, 0.45f, 1.0f);

// Reading the opcode byte at PC is a bus read, and reads are not free
// everywhere: $2000-$3FFF and $4000-$401F are hardware ports where a read
// clears the vblank flag, advances the VRAM address, or acknowledges an IRQ.
//
// Code never legitimately executes from there, so a PC pointing into that range
// already means the program has gone wrong - and fetching the byte to put a
// label on it would corrupt PPU and APU state on top of that, turning a
// visible bug into an invisible one.
bool safe_to_peek(const uint16_t addr) { return addr < 0x2000 || addr > 0x401F; }

const char* instruction_name_at_pc(Bus& console)
{
    const uint16_t pc = console.cpu.registers.PC;
    if (!safe_to_peek(pc)) {
        return "(unreadable)";
    }
    return InstructionSet::Table[console.read(pc)].name.c_str();
}

void flag_checkbox(Bus& console, const char* label, const CPU::FLAGS flag)
{
    bool value = console.cpu.get_flag(flag);
    if (ImGui::Checkbox(label, &value)) {
        console.cpu.set_flag(flag, value);
    }
}

// A read-only hex byte, coloured by what the CPU is pointing at.
void hex_byte(const uint8_t value, const ImVec4* colour)
{
    if (colour != nullptr) {
        ImGui::TextColored(*colour, "%02X", value);
    } else {
        ImGui::Text("%02X", value);
    }
}

}  // namespace

bool step_instruction(Bus& console, FrontendState& state)
{
    const uint16_t previous_pc = console.cpu.registers.PC;

    if (!console.step_instruction()) {
        state.trapped = true;
        state.trap_pc = previous_pc;
        state.running = false;
        state.status = "CPU never completed an instruction - jammed opcode?";
        return false;
    }

    // Every one of the test ROMs ends on a branch to itself, and so does a
    // runaway program that fell into one. Catching it is the difference between
    // "the emulator stopped" and a spinning UI.
    if (console.cpu.registers.PC == previous_pc) {
        state.trapped = true;
        state.trap_pc = previous_pc;
        state.running = false;
        return false;
    }

    state.trapped = false;
    return true;
}

bool load_cartridge(Bus& console, FrontendState& state, const std::string& path)
{
    if (path.empty()) {
        state.status = "No ROM path given";
        return false;
    }

    if (!console.load_cartridge(path)) {
        state.status = "Failed to load " + path;
        return false;
    }

    // The reset vector lives in the cartridge, so it can only be fetched once
    // the cartridge is mapped.
    console.cpu.reset();
    state.trapped = false;
    state.running = false;
    state.status = "Loaded " + path;
    return true;
}

void draw_controls_panel(Bus& console, FrontendState& state)
{
    ImGui::Begin("Controls");

    ImGui::InputText("ROM", state.rom_path, sizeof(state.rom_path));
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        load_cartridge(console, state, state.rom_path);
    }
    ImGui::TextDisabled("or drag a .nes file onto the window");

    ImGui::Separator();

    if (ImGui::Button(state.running ? "Pause" : "Run")) {
        state.running = !state.running;
        state.trapped = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Step")) {
        state.running = false;
        step_instruction(console, state);
    }
    ImGui::SameLine();
    if (ImGui::Button("Step frame")) {
        state.running = false;
        console.run_frame();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        state.running = false;
        state.trapped = false;
        console.cpu.reset();
        state.status = "Reset";
    }

    if (ImGui::Checkbox("Trace to stdout", &state.trace)) {
        console.trace_cpu = state.trace;
    }

    ImGui::Separator();
    ImGui::Text("frame %llu", static_cast<unsigned long long>(console.ppu.frame));
    ImGui::Text("CPU cycles %llu", static_cast<unsigned long long>(console.cpu_cycles));

    if (state.trapped) {
        ImGui::TextColored(colour_pc, "TRAP at $%04X", state.trap_pc);
    }
    if (!state.status.empty()) {
        ImGui::TextWrapped("%s", state.status.c_str());
    }

    ImGui::End();
}

void draw_cpu_panel(Bus& console)
{
    ImGui::Begin("CPU");

    const auto& r = console.cpu.registers;
    ImGui::Text("PC $%04X", r.PC);
    ImGui::Text("A  $%02X    X  $%02X    Y  $%02X", r.A, r.X, r.Y);
    ImGui::Text("SP $%02X  (-> $%04X)", r.SP, static_cast<unsigned>(CPU::STACK_BASE_ADDR + r.SP));
    ImGui::Text("op %s", instruction_name_at_pc(console));

    ImGui::Separator();

    // Writable, as they were in the Qt debugger: setting a flag by hand is the
    // quickest way to find out whether a branch is the thing going wrong.
    flag_checkbox(console, "N", CPU::FLAGS::N);
    ImGui::SameLine();
    flag_checkbox(console, "V", CPU::FLAGS::V);
    ImGui::SameLine();
    flag_checkbox(console, "U", CPU::FLAGS::U);
    ImGui::SameLine();
    flag_checkbox(console, "B", CPU::FLAGS::B);
    ImGui::SameLine();
    flag_checkbox(console, "D", CPU::FLAGS::D);
    ImGui::SameLine();
    flag_checkbox(console, "I", CPU::FLAGS::I);
    ImGui::SameLine();
    flag_checkbox(console, "Z", CPU::FLAGS::Z);
    ImGui::SameLine();
    flag_checkbox(console, "C", CPU::FLAGS::C);

    ImGui::Separator();
    ImGui::Text("mapper %u, PRG %zu KB, CHR %zu KB", console.rom.mapper_id, console.rom.prg_rom.size() / 1024,
                console.rom.chr_rom.size() / 1024);
    if (console.rom.chr_bank_count > 1) {
        ImGui::Text("CHR bank %u/%u", console.rom.chr_bank, console.rom.chr_bank_count);
    }

    ImGui::End();
}

void draw_ppu_panel(Bus& console)
{
    ImGui::Begin("PPU");

    const PPU& ppu = console.ppu;

    ImGui::Text("scanline %3d   dot %3d   frame %llu", ppu.scanline, ppu.cycle,
                static_cast<unsigned long long>(ppu.frame));

    ImGui::Separator();

    // Loopy's registers, shown raw as well as decoded. The raw value is what
    // NESdev's pseudocode talks about, so it is the one to compare against when
    // chasing a scroll bug; the decode is for reading at a glance.
    ImGui::Text("v $%04X   t $%04X   x %u   w %u", ppu.registers.PPUADDR, ppu.temp_addr, ppu.fine_x,
                ppu.write_toggle_w() ? 1u : 0u);
    ImGui::Text("  coarse X %2u  coarse Y %2u  NT %u  fine Y %u", ppu.registers.PPUADDR & 0x1F,
                (ppu.registers.PPUADDR >> 5) & 0x1F, (ppu.registers.PPUADDR >> 10) & 0x03,
                (ppu.registers.PPUADDR >> 12) & 0x07);
    ImGui::Text("scroll  x %3u  y %3u", ppu.scroll_x(), ppu.scroll_y());

    ImGui::Separator();

    ImGui::Text("PPUCTRL $%02X  PPUMASK $%02X  PPUSTATUS $%02X", ppu.registers.PPUCTRL, ppu.registers.PPUMASK,
                ppu.registers.PPUSTATUS);
    ImGui::Text("OAMADDR $%02X", ppu.registers.OAMADDR);

    // PPUSTATUS is read from the member, never through PPU::read - reading the
    // port would clear the vblank flag and the $2005/$2006 write toggle.
    ImGui::Text("vblank %d  sprite0 %d  overflow %d", (ppu.registers.PPUSTATUS & 0x80) != 0,
                (ppu.registers.PPUSTATUS & 0x40) != 0, (ppu.registers.PPUSTATUS & 0x20) != 0);
    ImGui::Text("background %d  sprites %d  NMI on vblank %d", ppu.show_background, ppu.show_sprites,
                ppu.MMI_on_V_Blank);

    ImGui::End();
}

void draw_memory_panel(Bus& console)
{
    ImGui::Begin("RAM");

    // The 2KB internal RAM array, read directly. Deliberately NOT a view of the
    // whole CPU address space walked through Bus::read: that would touch the
    // PPU and APU ports every time the panel redrew.
    const auto& memory = console.ram.memory;
    constexpr size_t bytes_per_row = 16;
    const int rows = static_cast<int>(memory.size() / bytes_per_row);

    const uint16_t pc = console.cpu.registers.PC;
    const uint16_t sp_addr = static_cast<uint16_t>(CPU::STACK_BASE_ADDR + console.cpu.registers.SP);

    ImGui::TextDisabled("$0000-$07FF");
    ImGui::SameLine();
    ImGui::TextColored(colour_pc, "PC");
    ImGui::SameLine();
    ImGui::TextColored(colour_sp, "stack pointer");
    ImGui::SameLine();
    ImGui::TextColored(colour_stack_base, "stack base");

    ImGui::BeginChild("ram_scroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    // Clipped: only the visible rows are laid out, so the panel costs the same
    // whether it shows 2KB or the whole address space.
    ImGuiListClipper clipper;
    clipper.Begin(rows);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const size_t base = static_cast<size_t>(row) * bytes_per_row;

            ImGui::Text("%04X:", static_cast<unsigned>(base));

            char ascii[bytes_per_row + 1] = {0};
            for (size_t i = 0; i < bytes_per_row; ++i) {
                const size_t addr = base + i;
                const uint8_t value = memory[addr];

                const ImVec4* colour = nullptr;
                if (addr == pc) {
                    colour = &colour_pc;
                } else if (addr == sp_addr) {
                    colour = &colour_sp;
                } else if (addr == CPU::STACK_BASE_ADDR) {
                    colour = &colour_stack_base;
                } else if (value == 0) {
                    // Untouched memory dimmed, so the parts a ROM has actually
                    // written stand out without having to read every byte.
                    colour = &colour_zero;
                }

                ImGui::SameLine();
                hex_byte(value, colour);

                ascii[i] = (value >= 0x20 && value < 0x7F) ? static_cast<char>(value) : '.';
            }

            ImGui::SameLine();
            ImGui::TextDisabled(" %s", ascii);
        }
    }

    ImGui::EndChild();
    ImGui::End();
}

void draw_palette_panel(Bus& console)
{
    ImGui::Begin("Palette");

    const PPU& ppu = console.ppu;
    constexpr size_t entries = sizeof(ppu.palette_ram);

    ImGui::TextDisabled("$3F00-$3F1F: background then sprite palettes");

    for (size_t i = 0; i < entries; ++i) {
        // Through palette_offset, not a raw index. $3F10/$3F14/$3F18/$3F1C are
        // aliases of $3F00/$04/$08/$0C, so the cells behind them stay at
        // whatever they powered up as - showing those would put four colours on
        // screen that the PPU never renders, next to the four it does.
        const uint16_t address = static_cast<uint16_t>(0x3F00 + i);
        const uint8_t index = ppu.palette_ram[PPU::palette_offset(address)];

        // Through the same index-to-RGB conversion the PPM dump and the screen
        // use, so a colour that looks wrong here looks wrong there too rather
        // than the two disagreeing.
        const uint32_t rgb = nes::palette_index_to_rgb(index, PPU::nes_palette, 64);
        const ImVec4 colour(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                            static_cast<float>((rgb >> 8) & 0xFF) / 255.0f, static_cast<float>(rgb & 0xFF) / 255.0f,
                            1.0f);

        char id[16];
        std::snprintf(id, sizeof(id), "##pal%zu", i);
        ImGui::ColorButton(id, colour, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip, ImVec2(20, 20));

        if (ImGui::IsItemHovered()) {
            const uint16_t effective = static_cast<uint16_t>(0x3F00 + PPU::palette_offset(address));
            if (effective != address) {
                ImGui::SetTooltip("$%04X = $%02X  (mirror of $%04X)", address, index, effective);
            } else {
                ImGui::SetTooltip("$%04X = $%02X", address, index);
            }
        }

        // Sixteen swatches per row, with a wider gap every four so the four
        // separate palettes are visible as groups rather than one long strip.
        if (i % 16 != 15) {
            ImGui::SameLine(0.0f, (i % 4 == 3) ? 14.0f : 4.0f);
        }
    }

    ImGui::End();
}

}  // namespace nes_gui
