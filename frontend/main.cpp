// SDL2 + Dear ImGui frontend: the NES screen and the debugger, in one window.
//
// This file owns everything backend-specific - the window, the renderer, the
// screen texture, the event loop and the frame pacing - and nothing else. The
// panels live in debugger.cpp and touch none of it. That split is the whole
// point: this is the piece that gets rewritten if the backend changes again,
// and it is deliberately kept small enough that rewriting it is not a project.
//
// It replaces the Qt debugger (qhex). Qt bought a hex widget and cost a
// heavyweight system dependency, an events-and-signals model that fits a
// document editor rather than a machine being single-stepped, and no answer at
// all for the audio clock that the APU will eventually need. SDL is that
// answer, ImGui is vendored source rather than a system package, and immediate
// mode is what makes a panel that reads live emulator state a dozen lines
// instead of a widget subclass.
#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "../include/bus.h"
#include "../include/frame_dump.h"
#include "debugger.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

namespace
{

// NTSC, to four places: 21477272 / 4 master clocks per dot, 341 dots per
// scanline, 262 scanlines minus the odd-frame skip. Not 60 - running the
// emulator at exactly 60 Hz would drift about a second every twenty minutes,
// which is inaudible now and will not be once the APU is generating samples
// against this same clock.
constexpr double frames_per_second = 60.0988;
constexpr double seconds_per_frame = 1.0 / frames_per_second;

// The most frames a single iteration will run to catch up. Without a cap, a
// stall - the window dragged, the process stopped under a debugger, a laptop
// resumed from suspend - turns into a burst of hundreds of frames that looks
// like a hang. Dropping the missed time is the right trade for an emulator.
constexpr int max_catchup_frames = 4;

constexpr int screen_pixels = PPU::screen_width * PPU::screen_height;

// Converts the PPU's palette-index framebuffer into the ARGB the texture wants.
//
// The conversion goes through nes::palette_index_to_rgb, which is also what the
// PPM dump and the palette panel use. One home for it means the screen cannot
// quietly disagree with a dump of the same frame.
void upload_framebuffer(SDL_Texture* texture, const PPU& ppu, std::vector<uint32_t>& scratch)
{
    for (int i = 0; i < screen_pixels; ++i) {
        scratch[i] = 0xFF000000u | nes::palette_index_to_rgb(ppu.framebuffer[i], PPU::nes_palette, 64);
    }

    SDL_UpdateTexture(texture, nullptr, scratch.data(), PPU::screen_width * static_cast<int>(sizeof(uint32_t)));
}

// Keyboard to controller port 1.
//
// Sampled once per iteration from SDL's key state rather than from key events,
// because the controller reports which buttons are HELD at the moment the game
// strobes $4016. Driving it from keydown/keyup edges would mean a button
// pressed and released between two strobes was never seen at all, and one held
// across a lost focus event would stick down forever.
//
// Nothing is written while ImGui wants the keyboard: without that check, typing
// a ROM path into the text field also walks the player left.
void poll_controller(Bus& console)
{
    if (ImGui::GetIO().WantCaptureKeyboard) {
        console.controllers.set_port(0, 0);
        return;
    }

    const Uint8* keys = SDL_GetKeyboardState(nullptr);

    uint8_t mask = 0;
    if (keys[SDL_SCANCODE_X])
        mask |= Controllers::A;
    if (keys[SDL_SCANCODE_Z])
        mask |= Controllers::B;
    if (keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_LSHIFT])
        mask |= Controllers::Select;
    if (keys[SDL_SCANCODE_RETURN])
        mask |= Controllers::Start;
    if (keys[SDL_SCANCODE_UP])
        mask |= Controllers::Up;
    if (keys[SDL_SCANCODE_DOWN])
        mask |= Controllers::Down;
    if (keys[SDL_SCANCODE_LEFT])
        mask |= Controllers::Left;
    if (keys[SDL_SCANCODE_RIGHT])
        mask |= Controllers::Right;

    console.controllers.set_port(0, mask);
}

void draw_screen_panel(SDL_Texture* texture, nes_gui::FrontendState& state)
{
    ImGui::Begin("Screen");

    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("scale", &state.screen_scale, 1, 4);

    const ImVec2 size(static_cast<float>(PPU::screen_width * state.screen_scale),
                      static_cast<float>(PPU::screen_height * state.screen_scale));
    ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture)), size);

    ImGui::End();
}

constexpr int window_width = 1280;
constexpr int window_height = 820;

// Positions the panel that the next Begin() opens.
//
// FirstUseEver, not Always: ImGui writes the layout to imgui.ini in the working
// directory and restores it on the next run, so this only decides where things
// land on a fresh install. Forcing it every frame would make the panels
// undraggable, which is a surprisingly easy mistake to leave in.
void place(const float x, const float y, const float w, const float h)
{
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
}

}  // namespace

int main(int argc, char** argv)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("NES", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, window_width,
                                          window_height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (window == nullptr) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Vsync paces the *presentation*, not the emulation - the emulator is
    // clocked off SDL_GetPerformanceCounter below. On a 144 Hz display, driving
    // frames off vsync would run the machine at 144 Hz.
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == nullptr) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_Texture* screen = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                            PPU::screen_width, PPU::screen_height);
    if (screen == nullptr) {
        SDL_Log("SDL_CreateTexture failed: %s", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Nearest neighbour, not linear: the NES output is 256x240 and every pixel
    // is meaningful. Smoothing it makes a one-pixel rendering bug invisible,
    // which is the opposite of what a debugger's screen is for.
    SDL_SetTextureScaleMode(screen, SDL_ScaleModeNearest);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    Bus console;
    nes_gui::FrontendState state;
    std::vector<uint32_t> scratch(screen_pixels, 0);

    if (argc > 1) {
        std::snprintf(state.rom_path, sizeof(state.rom_path), "%s", argv[1]);
        nes_gui::load_cartridge(console, state, argv[1]);
        state.running = true;
    }

    bool quit = false;
    uint64_t previous_counter = SDL_GetPerformanceCounter();
    double accumulator = 0.0;

    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            ImGui_ImplSDL2_ProcessEvent(&event);

            if (event.type == SDL_QUIT) {
                quit = true;
            } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                       event.window.windowID == SDL_GetWindowID(window)) {
                quit = true;
            } else if (event.type == SDL_DROPFILE) {
                std::snprintf(state.rom_path, sizeof(state.rom_path), "%s", event.drop.file);
                nes_gui::load_cartridge(console, state, event.drop.file);
                SDL_free(event.drop.file);
            }
        }

        const uint64_t now = SDL_GetPerformanceCounter();
        const double elapsed =
            static_cast<double>(now - previous_counter) / static_cast<double>(SDL_GetPerformanceFrequency());
        previous_counter = now;

        // Before the emulation below, so the frame about to run sees the keys
        // held now rather than the ones held a frame ago.
        poll_controller(console);

        if (state.running) {
            accumulator += elapsed;
            if (accumulator > max_catchup_frames * seconds_per_frame) {
                accumulator = max_catchup_frames * seconds_per_frame;
            }

            while (state.running && accumulator >= seconds_per_frame) {
                // Stepped an instruction at a time rather than by Bus::run_frame
                // so that a CPU branching to itself - how every test ROM ends -
                // is caught and reported instead of spinning invisibly. The
                // frame therefore ends at most one instruction late.
                const uint64_t started_on = console.ppu.frame;
                while (console.ppu.frame == started_on && nes_gui::step_instruction(console, state)) {
                }
                accumulator -= seconds_per_frame;
            }
        } else {
            // Discarded rather than banked: time spent paused is not time the
            // machine owes.
            accumulator = 0.0;
        }

        upload_framebuffer(screen, console.ppu, scratch);

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        place(8, 8, 530, 545);
        draw_screen_panel(screen, state);

        place(8, 561, 530, 251);
        nes_gui::draw_memory_panel(console);

        place(546, 8, 726, 208);
        nes_gui::draw_controls_panel(console, state);

        place(546, 224, 360, 260);
        nes_gui::draw_cpu_panel(console);

        place(912, 224, 360, 260);
        nes_gui::draw_ppu_panel(console);

        place(546, 492, 726, 120);
        nes_gui::draw_palette_panel(console);

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyTexture(screen);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
