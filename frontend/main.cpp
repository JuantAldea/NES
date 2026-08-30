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

#include <argparse/argparse.hpp>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
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
// Through nes::EmphasisTable, which is built from the same apply_emphasis the
// PPM dump and the palette panel call. One definition of the conversion means
// the screen cannot quietly disagree with a dump of the same frame.
//
// The table rather than per-pixel conversion because this runs 256 x 240 times
// per frame at 60fps - 3.7 million conversions a second - and applying emphasis
// arithmetically there would put floating-point work on every one of them. Made
// static so the 2KB is built once for the process, not once per frame.
void upload_framebuffer(SDL_Texture* texture, const PPU& ppu, std::vector<uint32_t>& scratch)
{
    static const nes::EmphasisTable table(PPU::nes_palette, 64);

    for (int i = 0; i < screen_pixels; ++i) {
        scratch[i] = 0xFF000000u | table.rgb[ppu.framebuffer[i] & 0x1FF];
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
    // PARSED BEFORE SDL_Init, so --help prints and exits without opening a
    // window and without needing a display at all - which is also what lets the
    // help text be checked from a headless CI runner.
    // help only, not `all`: `all` would also add --version, and this project has
    // no version to report. Printing an invented one would be worse than not
    // offering the flag.
    argparse::ArgumentParser args("nes_frontend", "", argparse::default_arguments::help);
    args.add_description("NES emulator with a debugger. Runs a cartridge, or starts empty and waits for one.");

    args.add_argument("rom").help("cartridge image to load and run").nargs(argparse::nargs_pattern::optional);
    args.add_argument("-m", "--mute")
        .help("start silent - stops the sampler as well as the device, so no synthesis runs")
        .flag();
    args.add_argument("--scale")
        .help("integer scale for the 256x240 screen")
        .scan<'i', int>()
        .default_value(2)
        .metavar("N");

    // For run_functional.sh, which needs a reproducible moment to look at. A
    // wall-clock wait is not one: it landed near frame 200 here and undefined
    // elsewhere. Both flags run UNTHROTTLED - 1300 frames measured at 4.3s
    // against the 21s real-time pacing costs.
    args.add_argument("--frames")
        .help("run up to N frames - or until the ROM stops - then exit")
        .scan<'i', int>()
        .default_value(0)
        .metavar("N");
    args.add_argument("--dump-frame")
        .help("write the framebuffer as a binary PPM on exit, for checking that something was actually drawn")
        .metavar("PATH");
    args.add_argument("--pause-at")
        .help("run unthrottled to frame N, then pause with the window still up - for reproducible screenshots")
        .scan<'i', int>()
        .default_value(0)
        .metavar("N");

    try {
        args.parse_args(argc, argv);
    } catch (const std::exception& error) {
        // argparse throws for an unknown flag, a missing value, or a --scale
        // that is not a number. Exit 1 rather than letting it escape: the
        // functional check asserts this, and a terminate() here would look like
        // a crash rather than a rejected command line.
        std::cerr << error.what() << "\n\n" << args;
        return 1;
    }

    const bool start_muted = args.get<bool>("--mute");
    const int requested_scale = args.get<int>("--scale");
    const int frame_limit = args.get<int>("--frames");
    const int pause_at = args.get<int>("--pause-at");
    const std::string dump_path = args.present("--dump-frame") ? args.get<std::string>("--dump-frame") : std::string{};
    const std::string rom_argument = args.present("rom") ? args.get<std::string>("rom") : std::string{};

    if (frame_limit < 0) {
        std::cerr << "--frames cannot be negative\n";
        return 1;
    }
    if (!dump_path.empty() && frame_limit == 0 && pause_at == 0) {
        // Otherwise the dump would happen whenever the user happened to close the
        // window, which is the wall-clock problem these flags exist to remove.
        std::cerr << "--dump-frame needs --frames or --pause-at, or the dump has no defined moment\n";
        return 1;
    }
    if (frame_limit != 0 && pause_at != 0) {
        // Alternatives, not modifiers: one exits at its frame, the other stops
        // there and keeps the window up. Accepting both would have to pick one
        // silently.
        std::cerr << "--frames and --pause-at are alternatives: one exits at a frame, the other stops there and "
                     "keeps the window up\n";
        return 1;
    }

    if (requested_scale < 1 || requested_scale > 8) {
        std::cerr << "--scale must be between 1 and 8; a fractional or huge scale is not a window anyone wants\n";
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
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

    // --- audio ---------------------------------------------------------------
    //
    // The callback runs on SDL's own thread and does exactly one thing: drain
    // the ring. Everything else - synthesis, filtering, decimation - happened on
    // the emulation thread inside Bus::clock. That split is the whole reason
    // SampleRing is a lock-free SPSC queue; see include/audio.h.
    //
    // A SHORT READ IS PADDED WITH SILENCE HERE, not inside the ring. The ring
    // deliberately leaves the tail it could not fill alone, because only the
    // caller knows what its device wants there - and for SDL that is zero, or
    // the card replays whatever was in the buffer last time, which is a loud
    // repeating fragment rather than a gap.
    SDL_AudioSpec want{};
    want.freq = 44100;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    // 1024 frames is about 23 ms. Small enough that input latency is not
    // noticeable, large enough that a scheduling hiccup does not starve it.
    want.samples = 1024;
    want.userdata = &console;
    want.callback = [](void* userdata, Uint8* stream, int len) {
        Bus* const bus = static_cast<Bus*>(userdata);
        float* const out = reinterpret_cast<float*>(stream);
        const size_t wanted = static_cast<size_t>(len) / sizeof(float);
        const size_t got = bus->audio.read(out, wanted);
        for (size_t i = got; i < wanted; ++i) {
            out[i] = 0.0f;
        }
    };

    SDL_AudioSpec have{};
    const SDL_AudioDeviceID audio_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audio_device == 0) {
        // Not fatal. A machine with no sound card still emulates correctly, and
        // refusing to start over it would be the wrong trade.
        SDL_Log("no audio device (%s); continuing without sound", SDL_GetError());
    } else {
        console.audio_enabled = true;
        // Any cartridge loaded above raised this; the device has not started
        // yet, so clearing here is trivially safe.
        console.audio.clear();
        console.audio_reset_pending = false;
        SDL_PauseAudioDevice(audio_device, 0);
    }
    std::vector<uint32_t> scratch(screen_pixels, 0);

    state.muted = start_muted;
    state.screen_scale = requested_scale;

    if (!rom_argument.empty()) {
        std::snprintf(state.rom_path, sizeof(state.rom_path), "%s", rom_argument.c_str());
        nes_gui::load_cartridge(console, state, rom_argument);
        state.running = true;
    }

    // Applied once here as well as in the loop below, because the device was
    // unpaused when it opened - several frames before the loop's first pass - and
    // a --mute run that is briefly audible has not done its job.
    if (state.muted && audio_device != 0) {
        SDL_PauseAudioDevice(audio_device, 1);
        console.audio_enabled = false;
        console.audio.clear();
    }

    // BEFORE the loop, not as a condition inside it. A ROM that traps stops
    // advancing - step_instruction clears state.running - so a frame target
    // beyond the trap can never arrive, and a loop waiting for one hangs.
    // test_ppu_read_buffer traps at 1266; asking it for 1300 waited forever.
    //
    // Here there is nothing to wait for: it ends on the frame or on the trap.
    const int catch_up_to = pause_at > 0 ? pause_at : frame_limit;
    if (catch_up_to > 0) {
        while (state.running && console.ppu.frame < static_cast<uint64_t>(catch_up_to)) {
            const uint64_t started_on = console.ppu.frame;
            while (console.ppu.frame == started_on && nes_gui::step_instruction(console, state)) {
            }
        }
        state.running = false;
        std::printf("stopped at frame %llu of %d requested\n", static_cast<unsigned long long>(console.ppu.frame),
                    catch_up_to);
    }

    // --frames is dump-and-exit: no window loop at all. --pause-at falls through
    // into it, paused, so the window is there to be photographed.
    if (frame_limit > 0) {
        int rc = 0;
        if (!dump_path.empty() && !nes::write_ppm(dump_path, console.ppu.framebuffer, PPU::screen_width,
                                                  PPU::screen_height, PPU::nes_palette, 64)) {
            std::cerr << "could not write " << dump_path << "\n";
            rc = 1;
        }
        if (audio_device != 0) {
            SDL_CloseAudioDevice(audio_device);
        }
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyTexture(screen);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return rc;
    }

    bool quit = false;
    bool announced_ready = false;
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

        // The ImGui frame opens HERE, above the emulation, and the order of
        // these three lines against poll_controller is the whole point.
        //
        // poll_controller consults io.WantCaptureKeyboard, and that flag is
        // written by ImGui::NewFrame - it is not live state. Polling before
        // NewFrame therefore read a value computed during the PREVIOUS
        // iteration, i.e. from an ActiveId two widget passes old, so clicking
        // into or out of the ROM text field misrouted a frame of input in each
        // direction: one frame of keys reaching the game after the field took
        // focus, one frame dropped after it lost focus. Sixteen milliseconds,
        // invisible in play, and still the wrong answer to "who has the
        // keyboard".
        //
        // One frame of lag is the floor here and this reaches it: the click is
        // consumed by the widget pass at the bottom of the loop, so nothing
        // earlier than the next NewFrame can know about it. Moving the poll
        // below NewFrame instead of moving NewFrame above the poll would have
        // cost more than it fixed - see the note on poll_controller's position
        // below.
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Still before the emulation, so the frame about to run sees the keys
        // held now rather than the ones held a frame ago. That ordering is why
        // NewFrame moved up rather than this moving down.
        poll_controller(console);

        if (state.running) {
            accumulator += elapsed;
            if (accumulator > max_catchup_frames * seconds_per_frame) {
                accumulator = max_catchup_frames * seconds_per_frame;
            }

            while (state.running && accumulator >= seconds_per_frame) {
                // Stepped an instruction at a time rather than by Bus::run_frame
                // so that a genuinely hung CPU is caught and reported instead
                // of spinning invisibly. "Hung" means a branch to itself that
                // no interrupt can leave - see step_instruction, which is
                // deliberately not tripped by the identical-looking idle loop
                // games sit in between NMIs. The frame therefore ends at most
                // one instruction late.
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

        // The device follows the emulation, so a paused machine is silent
        // rather than looping its last buffer.
        //
        // AND THE RING IS DISCARDED WHENEVER IT STOPS, which is the only place
        // that can safely happen: AudioSampler::clear() moves both indices and
        // needs the consumer stopped. Without it, ring occupancy is permanent -
        // production and consumption both run at the output rate, so nothing
        // ever reduces a backlog, and up to half a second of latency
        // accumulates for the session. Stepping is what fills it: Step and Step
        // frame set running = false and then clock the machine, pushing into a
        // ring nobody is draining, about 735 samples per frame stepped.
        //
        // A pending cartridge swap is handled in the same breath and for the
        // same reason - it is the one moment the device is known stopped.
        if (audio_device != 0) {
            // TWO INDEPENDENT REASONS FOR SILENCE, and they are not the same
            // thing. Pausing stops the device but leaves the sampler producing;
            // muting stops both, so a muted session does no synthesis at all
            // rather than filling a ring nobody drains. That second part is what
            // keeps dropped() meaningful while muted.
            const nes_gui::AudioIntent intent = nes_gui::audio_intent(state.muted, state.running);
            if (console.audio_enabled != intent.sampler_enabled) {
                // The toggle just moved. Stop the consumer before touching the
                // ring, for the reason spelled out above.
                SDL_PauseAudioDevice(audio_device, 1);
                console.audio_enabled = intent.sampler_enabled;
                console.audio.clear();
            }

            const bool want_running = intent.device_running;
            if (!want_running || console.audio_reset_pending) {
                SDL_PauseAudioDevice(audio_device, 1);
                console.audio.clear();
                console.audio_reset_pending = false;
            }
            SDL_PauseAudioDevice(audio_device, want_running ? 0 : 1);
        }

        upload_framebuffer(screen, console.ppu, scratch);

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

        // The last free slot in the 1280x820 default layout: the palette ends
        // at y=612 and the window is 820 tall.
        place(546, 620, 726, 192);
        nes_gui::draw_controller_panel(console);

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

        // Printed after the first frame reaches the screen, so a screenshot
        // caller can wait for something real. A window EXISTS from creation
        // onwards and stays black through --pause-at's catch-up, so "the window
        // is up" does not mean there is anything on it.
        if (!announced_ready) {
            announced_ready = true;
            std::printf("ready: frame %llu on screen\n", static_cast<unsigned long long>(console.ppu.frame));
            std::fflush(stdout);
        }
    }

    // BEFORE the Bus goes out of scope, and before anything else is torn down.
    // The callback holds a pointer to it and runs on SDL's thread; closing the
    // device is what guarantees that thread is not inside the ring when the
    // ring is destroyed.
    if (audio_device != 0) {
        SDL_CloseAudioDevice(audio_device);
    }

    // The dump, while the PPU still exists. A failure is REPORTED rather than
    // ignored - write_ppm's own comment makes the case: "a dump that silently
    // did not happen is worse than no dump, because it is looked for".
    int exit_code = 0;
    if (!dump_path.empty()) {
        if (nes::write_ppm(dump_path, console.ppu.framebuffer, PPU::screen_width, PPU::screen_height, PPU::nes_palette,
                           64)) {
            std::printf("wrote %s at frame %llu\n", dump_path.c_str(),
                        static_cast<unsigned long long>(console.ppu.frame));
        } else {
            std::cerr << "could not write " << dump_path << "\n";
            exit_code = 1;
        }
    }

    // Before tearing anything down, and before the Bus goes out of scope: this
    // is the only point at which a game that has been saving all session gets
    // its work RAM onto disk. The loop above exits on window close, which is
    // how a player normally leaves - so a flush placed any later, or left to a
    // destructor, would be a flush that never ran.
    console.save_battery_ram();

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyTexture(screen);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}
