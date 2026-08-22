#include "debugger.h"

#include <cfloat>
#include <cstdio>

#include "../include/bus.h"
#include "../include/frame_dump.h"
#include "imgui.h"

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

// --- the pad diagram -------------------------------------------------------
//
// Unlit and lit, for the three groups of button. Real hardware colours the
// D-pad and the two shoulder-less action buttons differently, and keeping that
// distinction means a glance tells you WHICH group is lit without reading a
// label. Lit is a wash rather than a colour change so the shape stays legible.
constexpr ImU32 pad_body = IM_COL32(46, 46, 52, 255);
constexpr ImU32 pad_edge = IM_COL32(96, 96, 108, 255);
constexpr ImU32 dpad_off = IM_COL32(64, 64, 72, 255);
constexpr ImU32 dpad_on = IM_COL32(126, 226, 126, 255);
constexpr ImU32 pill_off = IM_COL32(64, 64, 72, 255);
constexpr ImU32 pill_on = IM_COL32(240, 206, 110, 255);
constexpr ImU32 face_off = IM_COL32(122, 44, 44, 255);
constexpr ImU32 face_on = IM_COL32(255, 96, 96, 255);
constexpr ImU32 pad_label = IM_COL32(150, 150, 158, 255);

// The pad's extent, in the same `c` units as everything below. Shared because
// the panel has to reserve exactly what draw_pad covers: ImGui lays out from a
// cursor that a raw AddRectFilled does not advance, so a Dummy that disagreed
// would either clip the pad or leave a gap under it.
constexpr float pad_cells_w = 19.4f;
constexpr float pad_cells_h = 8.4f;

// Centred under a point, because the labels sit beneath round pills and circles
// whose centres are the only x worth aligning to.
//
// Takes an explicit size rather than using the UI font: at 13px, "SELECT" is
// wider than the pill it names, so the first draft rendered "SELECTSTART" as
// one word. Sized off `c` like the rest of the geometry so it stays right when
// the font does change.
void pad_text(ImDrawList* draw, const float centre_x, const float y, const float size, const char* text)
{
    ImFont* font = ImGui::GetFont();
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    draw->AddText(font, size, ImVec2(centre_x - extent.x * 0.5f, y), pad_label, text);
}

// Draws one controller at `origin`, lighting whatever `held` says is down.
//
// Geometry is in units of `c`, taken from the font size rather than fixed
// pixels: the panel is resizable and this is the only thing here that would
// otherwise stay 13px tall on a HiDPI display while everything around it grew.
void draw_pad(ImDrawList* draw, const ImVec2 origin, const float c, const uint8_t held)
{
    const auto lit = [held](const Controllers::Button b, const ImU32 on, const ImU32 off) {
        return (held & b) != 0 ? on : off;
    };
    const auto rect = [&](const float x0, const float y0, const float x1, const float y1, const ImU32 colour) {
        draw->AddRectFilled(ImVec2(origin.x + x0, origin.y + y0), ImVec2(origin.x + x1, origin.y + y1), colour);
    };

    const float w = pad_cells_w * c;
    const float h = pad_cells_h * c;
    draw->AddRectFilled(origin, ImVec2(origin.x + w, origin.y + h), pad_body, 6.0f);
    draw->AddRect(origin, ImVec2(origin.x + w, origin.y + h), pad_edge, 6.0f);

    // D-pad: four arms around a hub that is never lit, because there is no
    // centre switch to light - pressing two arms is how a diagonal is reported.
    const float cx = 4.4f * c;
    const float cy = 4.4f * c;
    const float t = 1.15f * c;  // arm thickness, and the hub's side
    const float a = 1.15f * c;  // arm length beyond the hub

    rect(cx - t / 2, cy - t / 2 - a, cx + t / 2, cy - t / 2, lit(Controllers::Up, dpad_on, dpad_off));
    rect(cx - t / 2, cy + t / 2, cx + t / 2, cy + t / 2 + a, lit(Controllers::Down, dpad_on, dpad_off));
    rect(cx - t / 2 - a, cy - t / 2, cx - t / 2, cy + t / 2, lit(Controllers::Left, dpad_on, dpad_off));
    rect(cx + t / 2, cy - t / 2, cx + t / 2 + a, cy + t / 2, lit(Controllers::Right, dpad_on, dpad_off));
    rect(cx - t / 2, cy - t / 2, cx + t / 2, cy + t / 2, dpad_off);

    // Select and Start: the flat rounded pills between the two halves.
    const float pill_w = 2.1f * c;
    const float pill_h = 0.75f * c;
    const float pill_y = 5.0f * c;
    const struct {
        float x;
        Controllers::Button button;
        const char* label;
    } pills[] = {{7.9f * c, Controllers::Select, "SELECT"}, {11.4f * c, Controllers::Start, "START"}};

    for (const auto& pill : pills) {
        draw->AddRectFilled(ImVec2(origin.x + pill.x, origin.y + pill_y),
                            ImVec2(origin.x + pill.x + pill_w, origin.y + pill_y + pill_h),
                            lit(pill.button, pill_on, pill_off), pill_h * 0.5f);
        pad_text(draw, origin.x + pill.x + pill_w * 0.5f, origin.y + pill_y + pill_h + 0.25f * c, 0.72f * c,
                 pill.label);
    }

    // B then A, left to right, which is the order they sit on the shell.
    const float face_y = 4.6f * c;
    const float face_r = 0.95f * c;
    const struct {
        float x;
        Controllers::Button button;
        const char* label;
    } faces[] = {{15.1f * c, Controllers::B, "B"}, {17.4f * c, Controllers::A, "A"}};

    for (const auto& face : faces) {
        draw->AddCircleFilled(ImVec2(origin.x + face.x, origin.y + face_y), face_r,
                              lit(face.button, face_on, face_off));
        pad_text(draw, origin.x + face.x, origin.y + face_y + face_r + 0.25f * c, 0.85f * c, face.label);
    }
}

}  // namespace

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

    // Discoverability: the mapping is otherwise guessable only by trying keys.
    ImGui::TextDisabled("pad: arrows  X=A  Z=B  Shift=Select  Enter=Start");

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
    const char* board = mapper_name(console.rom.mapper_id);
    ImGui::Text("mapper %u (%s), PRG %zu KB, CHR %zu KB", static_cast<unsigned>(console.rom.mapper_id),
                board != nullptr ? board : "unnamed", console.rom.prg_rom.size() / 1024,
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

void draw_controller_panel(Bus& console)
{
    ImGui::Begin("Controllers");

    // Both ports, side by side, even though only port 1 is ever driven today -
    // main.cpp's poll_controller writes port 0 and nothing writes port 1. A
    // permanently dark second pad is the honest picture of that, and it is the
    // panel that will show the wiring working if it is ever added.
    for (int port = 0; port < 2; ++port) {
        const ControllerSnapshot pad = peek_controller(console, port);

        ImGui::BeginGroup();
        ImGui::Text("Port %d", port + 1);

        const float c = ImGui::GetFontSize();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        draw_pad(ImGui::GetWindowDrawList(), origin, c, pad.held);
        ImGui::Dummy(ImVec2(pad_cells_w * c, pad_cells_h * c));

        // The numbers behind the picture. `shift` is the half that cannot be
        // seen any other way: it is what the next eight reads of the port will
        // return, so watching it empty out is watching the game read the pad.
        ImGui::TextDisabled("held $%02X   shift $%02X   strobe %d", pad.held, pad.shift, pad.strobe ? 1 : 0);
        ImGui::EndGroup();

        if (port == 0) {
            ImGui::SameLine(0.0f, 24.0f);
        }
    }

    ImGui::End();
}

}  // namespace nes_gui
