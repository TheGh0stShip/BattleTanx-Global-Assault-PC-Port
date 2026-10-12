#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <atomic>
#include <span>
#include <stdexcept>
#include <string_view>

#include "funcs.h"
#include "librecomp/game.hpp"
#include "btga/rom.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"
#include "xxHash/xxh3.h"
#ifdef BTGA_HAS_RT64
#include <SDL.h>
#if defined(_WIN32)
#include <SDL_syswm.h>
#endif
#include "rt64_context.hpp"
#endif

gpr get_entrypoint_address();
void btga_register_overlays();
extern RspUcodeFunc n_aspMain;

namespace {
constexpr std::u8string_view game_id = u8"battletanx-ga.n64.us.1.0";

std::atomic_uint32_t created_threads{0};
std::atomic_uint32_t display_lists{0};
std::atomic_uint32_t screen_updates{0};
std::atomic_uint32_t vi_events{0};
std::atomic_uint32_t rsp_requests{0};
#ifdef BTGA_HAS_RT64
SDL_Window* window = nullptr;
SDL_AudioDeviceID audio_device = 0;
SDL_GameController* gamepad = nullptr;
bool interactive = false;
#endif

class HeadlessRenderer final : public ultramodern::renderer::RendererContext {
public:
    bool valid() override { return true; }
    bool update_config(const ultramodern::renderer::GraphicsConfig&,
                       const ultramodern::renderer::GraphicsConfig&) override {
        return true;
    }
    void enable_instant_present() override {}
    void send_dl(const OSTask*) override { ++display_lists; }
    void send_dummy_workload(std::uint32_t) override {}
    void update_screen() override { ++screen_updates; }
    void shutdown() override {}
    std::uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1.0F; }
};

std::unique_ptr<ultramodern::renderer::RendererContext> create_headless_renderer(
    std::uint8_t*, ultramodern::renderer::WindowHandle, bool) {
    return std::make_unique<HeadlessRenderer>();
}

RspUcodeFunc* select_rsp_microcode(const OSTask* task) {
    ++rsp_requests;
    if (task->t.type == M_AUDTASK) {
        return n_aspMain;
    }
    return nullptr;
}

ultramodern::renderer::WindowHandle headless_window_handle() {
#if defined(_WIN32)
    return {.window = reinterpret_cast<HWND>(1), .thread_id = 0};
#elif defined(__APPLE__)
    return {.window = reinterpret_cast<void*>(1), .view = nullptr};
#else
    return reinterpret_cast<ultramodern::renderer::WindowHandle>(1);
#endif
}

#ifdef BTGA_HAS_RT64
ultramodern::renderer::WindowHandle interactive_window_handle() {
#if defined(_WIN32)
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info)) {
        throw std::runtime_error{"SDL could not expose the native Win32 window handle"};
    }
    return {.window = info.info.win.window, .thread_id = GetCurrentThreadId()};
#else
    return window;
#endif
}
#endif

void discard_audio(std::int16_t*, std::size_t) {}
std::size_t audio_frames_remaining() { return 0; }
void set_audio_frequency(std::uint32_t) {}

#ifdef BTGA_HAS_RT64
void queue_audio(std::int16_t* samples, std::size_t sample_count) {
    if (audio_device != 0) {
        SDL_QueueAudio(audio_device, samples,
                       static_cast<std::uint32_t>(sample_count * sizeof(*samples)));
    }
}
std::size_t queued_audio_frames() { return audio_device == 0 ? 0 : SDL_GetQueuedAudioSize(audio_device) / 4; }
void configure_audio(std::uint32_t frequency) {
    if (audio_device != 0) SDL_CloseAudioDevice(audio_device);
    SDL_AudioSpec wanted{};
    wanted.freq = static_cast<int>(frequency);
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = 1024;
    audio_device = SDL_OpenAudioDevice(nullptr, 0, &wanted, nullptr, 0);
    if (audio_device != 0) {
        SDL_PauseAudioDevice(audio_device, 0);
    } else {
        std::cerr << "SDL audio unavailable: " << SDL_GetError() << '\n';
    }
}
#endif

void poll_input() {
#ifdef BTGA_HAS_RT64
    if (!interactive) return;
    SDL_PumpEvents();
    if (gamepad && !SDL_GameControllerGetAttached(gamepad)) {
        SDL_GameControllerClose(gamepad);
        gamepad = nullptr;
    }
    if (!gamepad) {
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (SDL_IsGameController(i)) { gamepad = SDL_GameControllerOpen(i); break; }
        }
    }
#endif
}
bool get_input(int controller, std::uint16_t* buttons, float* x, float* y) {
    if (controller != 0) {
        return false;
    }
#ifdef BTGA_HAS_RT64
    if (interactive) {
        const auto* keys = SDL_GetKeyboardState(nullptr);
        *buttons = (keys[SDL_SCANCODE_X] ? 0x8000 : 0) | (keys[SDL_SCANCODE_Z] ? 0x4000 : 0) |
                   (keys[SDL_SCANCODE_LSHIFT] ? 0x2000 : 0) | (keys[SDL_SCANCODE_RETURN] ? 0x1000 : 0) |
                   (keys[SDL_SCANCODE_UP] ? 0x0800 : 0) | (keys[SDL_SCANCODE_DOWN] ? 0x0400 : 0) |
                   (keys[SDL_SCANCODE_LEFT] ? 0x0200 : 0) | (keys[SDL_SCANCODE_RIGHT] ? 0x0100 : 0) |
                   (keys[SDL_SCANCODE_Q] ? 0x20 : 0) | (keys[SDL_SCANCODE_E] ? 0x10 : 0) |
                   (keys[SDL_SCANCODE_I] ? 0x08 : 0) | (keys[SDL_SCANCODE_K] ? 0x04 : 0) |
                   (keys[SDL_SCANCODE_J] ? 0x02 : 0) | (keys[SDL_SCANCODE_L] ? 0x01 : 0);
        *x = float(keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A]);
        *y = float(keys[SDL_SCANCODE_W] - keys[SDL_SCANCODE_S]);
        if (gamepad) {
            auto pressed = [](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(gamepad, b) != 0; };
            *buttons |= (pressed(SDL_CONTROLLER_BUTTON_A) ? 0x8000 : 0) |
                        (pressed(SDL_CONTROLLER_BUTTON_X) ? 0x4000 : 0) |
                        (pressed(SDL_CONTROLLER_BUTTON_BACK) ? 0x2000 : 0) |
                        (pressed(SDL_CONTROLLER_BUTTON_START) ? 0x1000 : 0) |
                        (pressed(SDL_CONTROLLER_BUTTON_DPAD_UP) ? 0x0800 : 0) |
                        (pressed(SDL_CONTROLLER_BUTTON_DPAD_DOWN) ? 0x0400 : 0) |
                        (pressed(SDL_CONTROLLER_BUTTON_DPAD_LEFT) ? 0x0200 : 0) |
                        (pressed(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ? 0x0100 : 0) |
                        (pressed(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 0x20 : 0) |
                        (pressed(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 0x10 : 0) |
                        (pressed(SDL_CONTROLLER_BUTTON_Y) ? 0x08 : 0) |
                        (pressed(SDL_CONTROLLER_BUTTON_B) ? 0x04 : 0);
            constexpr float scale = 1.0F / 32767.0F;
            const float gx = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTX) * scale;
            const float gy = -SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTY) * scale;
            if (gx > 0.15F || gx < -0.15F) *x = gx;
            if (gy > 0.15F || gy < -0.15F) *y = gy;
            if (SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000) *buttons |= 0x2000;
        }
        return true;
    }
#endif
    *buttons = 0; *x = 0.0F; *y = 0.0F;
    return true;
}
void set_rumble(int controller, bool enabled) {
#ifdef BTGA_HAS_RT64
    if (interactive && controller == 0 && gamepad) {
        SDL_GameControllerRumble(gamepad, enabled ? 0xFFFF : 0, enabled ? 0xFFFF : 0, 250);
    }
#else
    (void)controller; (void)enabled;
#endif
}
ultramodern::input::connected_device_info_t connected_device(int controller) {
    if (controller == 0) {
        return {ultramodern::input::Device::Controller, ultramodern::input::Pak::None};
    }
    return {ultramodern::input::Device::None, ultramodern::input::Pak::None};
}

void on_vi() { ++vi_events; }

std::string game_thread_name(const OSThread* thread) {
    const std::uint32_t sequence = ++created_threads;
    std::cerr << "Created N64 thread " << sequence << ": id=" << thread->id
              << " priority=" << thread->priority << '\n';
    return "BTGA-" + std::to_string(thread->id);
}

void show_error(const char* message) {
    std::cerr << "Runtime error: " << message << '\n';
}

bool boot_requested = false;
std::chrono::steady_clock::time_point boot_started;

std::filesystem::path user_data_directory() {
#if defined(_WIN32)
    if (const char* local_app_data = std::getenv("LOCALAPPDATA")) {
        return std::filesystem::path{local_app_data} / "BattleTanxGA";
    }
    if (const char* app_data = std::getenv("APPDATA")) {
        return std::filesystem::path{app_data} / "BattleTanxGA";
    }
#else
    if (const char* xdg_data_home = std::getenv("XDG_DATA_HOME")) {
        return std::filesystem::path{xdg_data_home} / "BattleTanxGA";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / ".local" / "share" / "BattleTanxGA";
    }
#endif
    return std::filesystem::temp_directory_path() / "BattleTanxGA";
}

void drive_headless_runtime(void*) {
#ifdef BTGA_HAS_RT64
    if (interactive) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                ultramodern::quit();
                return;
            }
        }
        if (!boot_requested) {
            boot_requested = true;
            recomp::start_game(std::u8string{game_id}, {});
        }
        return;
    }
#endif
    if (!boot_requested) {
        boot_requested = true;
        boot_started = std::chrono::steady_clock::now();
        recomp::start_game(std::u8string{game_id}, {});
    } else if (std::chrono::steady_clock::now() - boot_started > std::chrono::seconds{5}) {
        std::cerr << "Headless boot window elapsed: threads=" << created_threads.load()
                  << " vi=" << vi_events.load()
                  << " display_lists=" << display_lists.load()
                  << " screens=" << screen_updates.load()
                  << " rsp_requests=" << rsp_requests.load() << '\n';
        // This is a bounded boot probe, not the application's eventual main loop.
        // N64 worker threads deliberately remain active at this point, so bypass
        // runtime teardown rather than unmapping RDRAM underneath them.
        std::_Exit(EXIT_SUCCESS);
    }
}

recomp::GameEntry make_game(std::uint64_t rom_hash) {
    return {
        .rom_hash = rom_hash,
        .internal_name = "BATTLETANX2",
        .display_name = "BattleTanx: Global Assault",
        .game_id = std::u8string{game_id},
        .mod_game_id = "battletanx-ga",
        .save_type = recomp::SaveType::Eep16k,
        .thumbnail_bytes = {},
        .is_enabled = true,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
    };
}
}

int main(int argc, char** argv) {
    bool boot = argc == 3 && std::string_view{argv[1]} == "--boot";
    bool play = argc == 3 && std::string_view{argv[1]} == "--play";
    if (argc != 1 && !boot && !play) {
        std::cerr << "Usage: " << argv[0] << " [--boot|--play /path/to/battletanx_ga.z64]\n";
        return 2;
    }

    std::uint64_t rom_hash = 0;
    std::filesystem::path rom_path;
    if (boot || play) {
        rom_path = argv[2];
        const btga::Rom rom = btga::Rom::load_verified(rom_path);
        rom_hash = XXH3_64bits(rom.bytes().data(), rom.bytes().size());
    }

    const recomp::GameEntry game = make_game(rom_hash);

    if (!recomp::register_game(game)) {
        std::cerr << "Failed to register BattleTanx: Global Assault with the runtime\n";
        return 1;
    }
    btga_register_overlays();

    std::cout << "Registered all translated game code at entrypoint 0x"
              << std::hex << static_cast<std::uint32_t>(game.entrypoint_address) << '\n';

    if (!boot && !play) {
        return 0;
    }

    const std::filesystem::path config_path = user_data_directory();
    std::filesystem::create_directories(config_path);
    recomp::register_config_path(config_path);
    if (recomp::select_rom(rom_path, game.game_id) != recomp::RomValidationError::Good) {
        std::cerr << "N64ModernRuntime rejected the already verified ROM\n";
        return 1;
    }

#ifdef BTGA_HAS_RT64
    if (play) {
        interactive = true;
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
            std::cerr << "SDL initialization failed: " << SDL_GetError() << '\n';
            return 1;
        }
        window = SDL_CreateWindow("BattleTanx: Global Assault", SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED, 960, 720,
                                  SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!window) { std::cerr << "Window creation failed: " << SDL_GetError() << '\n'; return 1; }
    }
#else
    if (play) { std::cerr << "This build does not include RT64\n"; return 1; }
#endif

    auto selected_window = headless_window_handle();
    auto* selected_queue_samples = discard_audio;
    auto* selected_frames_remaining = audio_frames_remaining;
    auto* selected_set_frequency = set_audio_frequency;
#ifdef BTGA_HAS_RT64
    if (play) {
        selected_window = interactive_window_handle();
        selected_queue_samples = queue_audio;
        selected_frames_remaining = queued_audio_frames;
        selected_set_frequency = configure_audio;
    }
#endif

    recomp::Configuration config{
        .argc = 0,
        .argv = nullptr,
        .project_version = {0, 1, 0},
        .window_handle = selected_window,
        .rsp_callbacks = {.get_rsp_microcode = select_rsp_microcode},
        .renderer_callbacks = {.create_render_context =
#ifdef BTGA_HAS_RT64
            play ? btga::renderer::create_rt64_context :
#endif
            create_headless_renderer},
        .audio_callbacks = {
            .queue_samples = selected_queue_samples,
            .get_frames_remaining = selected_frames_remaining,
            .set_frequency = selected_set_frequency,
        },
        .input_callbacks = {
            .poll_input = poll_input,
            .get_input = get_input,
            .set_rumble = set_rumble,
            .get_connected_device_info = connected_device,
        },
        .gfx_callbacks = {.update_gfx = drive_headless_runtime},
        .events_callbacks = {.vi_callback = on_vi},
        .error_handling_callbacks = {.message_box = show_error},
        .threads_callbacks = {.get_game_thread_name = game_thread_name},
    };
    recomp::start(config);
#ifdef BTGA_HAS_RT64
    if (gamepad != nullptr) {
        SDL_GameControllerClose(gamepad);
        gamepad = nullptr;
    }
    if (audio_device != 0) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }
    if (window != nullptr) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    if (play) {
        SDL_Quit();
    }
#endif
    return 0;
}
