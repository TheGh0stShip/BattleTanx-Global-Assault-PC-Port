#include <algorithm>
#include <charconv>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <atomic>
#include <span>
#include <stdexcept>
#include <string_view>

#include "funcs.h"
#include "librecomp/game.hpp"
#include "btga/rom.hpp"
#include "btga/pc_settings.hpp"
#include "btga/texture_packs.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"
#include "xxHash/xxh3.h"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif
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
constexpr int max_controllers = 4;

std::atomic_uint32_t created_threads{0};
std::atomic_uint32_t display_lists{0};
std::atomic_uint32_t screen_updates{0};
std::atomic_uint32_t vi_events{0};
std::atomic_uint32_t rsp_requests{0};
std::array<std::atomic<float>, max_controllers> turret_stick_x{};
std::atomic_bool mouse_turret_aiming{false};
btga::PcSettings pc_settings = btga::PcSettings::defaults();
std::mutex input_settings_mutex;
#ifdef BTGA_HAS_RT64
SDL_Window* window = nullptr;
SDL_AudioDeviceID audio_device = 0;
std::mutex audio_mutex;
std::uint32_t audio_frequency = 44100;
std::array<SDL_GameController*, max_controllers> gamepads{};
bool interactive = false;

struct KeyboardScancodes {
    SDL_Scancode stick_up = SDL_SCANCODE_W;
    SDL_Scancode stick_down = SDL_SCANCODE_S;
    SDL_Scancode stick_left = SDL_SCANCODE_A;
    SDL_Scancode stick_right = SDL_SCANCODE_D;
    SDL_Scancode button_a = SDL_SCANCODE_X;
    SDL_Scancode button_b = SDL_SCANCODE_Z;
    SDL_Scancode button_z = SDL_SCANCODE_LSHIFT;
    SDL_Scancode button_start = SDL_SCANCODE_RETURN;
    SDL_Scancode button_l = SDL_SCANCODE_Q;
    SDL_Scancode button_r = SDL_SCANCODE_E;
    SDL_Scancode dpad_up = SDL_SCANCODE_UP;
    SDL_Scancode dpad_down = SDL_SCANCODE_DOWN;
    SDL_Scancode dpad_left = SDL_SCANCODE_LEFT;
    SDL_Scancode dpad_right = SDL_SCANCODE_RIGHT;
    SDL_Scancode c_up = SDL_SCANCODE_I;
    SDL_Scancode c_down = SDL_SCANCODE_K;
    SDL_Scancode c_left = SDL_SCANCODE_J;
    SDL_Scancode c_right = SDL_SCANCODE_L;
} keyboard;
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
    std::lock_guard<std::mutex> lock{audio_mutex};
    if (audio_device == 0) {
        return;
    }

    constexpr std::size_t channels = 2;
    constexpr std::size_t soft_cap_ms = 200;
    constexpr std::size_t hard_cap_ms = 500;
    const std::size_t bytes_per_ms =
        static_cast<std::size_t>(audio_frequency) * channels * sizeof(*samples) / 1000;
    const std::size_t queued_bytes = SDL_GetQueuedAudioSize(audio_device);
    if (queued_bytes > hard_cap_ms * bytes_per_ms) {
        std::cerr << "Audio queue reached " << queued_bytes / bytes_per_ms
                  << " ms; resynchronizing\n";
        SDL_ClearQueuedAudio(audio_device);
    } else if (queued_bytes > soft_cap_ms * bytes_per_ms) {
        return;
    }

    float master_volume = 1.0F;
    {
        const std::lock_guard settings_lock(input_settings_mutex);
        master_volume = pc_settings.master_volume;
    }
    if (master_volume < 1.0F) {
        for (std::size_t i = 0; i < sample_count; ++i) {
            const float scaled = static_cast<float>(samples[i]) * master_volume;
            samples[i] = static_cast<std::int16_t>(std::clamp(
                scaled, static_cast<float>((std::numeric_limits<std::int16_t>::min)()),
                static_cast<float>((std::numeric_limits<std::int16_t>::max)())));
        }
    }
    SDL_QueueAudio(audio_device, samples,
                   static_cast<std::uint32_t>(sample_count * sizeof(*samples)));
}
std::size_t queued_audio_frames() {
    std::lock_guard<std::mutex> lock{audio_mutex};
    if (audio_device == 0) {
        return 0;
    }
    const std::size_t queued = SDL_GetQueuedAudioSize(audio_device) / 4;
    const std::size_t scheduling_headroom = audio_frequency / 30;
    return queued > scheduling_headroom ? queued - scheduling_headroom : 0;
}
void configure_audio(std::uint32_t frequency) {
    std::lock_guard<std::mutex> lock{audio_mutex};
    if (audio_device != 0) SDL_CloseAudioDevice(audio_device);
    SDL_AudioSpec wanted{};
    wanted.freq = static_cast<int>(frequency);
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = 1024;
    audio_device = SDL_OpenAudioDevice(nullptr, 0, &wanted, nullptr, 0);
    audio_frequency = frequency;
    if (audio_device != 0) {
        SDL_PauseAudioDevice(audio_device, 0);
    } else {
        std::cerr << "SDL audio unavailable: " << SDL_GetError() << '\n';
    }
}

SDL_Scancode configured_scancode(const std::string& name, SDL_Scancode fallback) {
    const SDL_Scancode result = SDL_GetScancodeFromName(name.c_str());
    return result == SDL_SCANCODE_UNKNOWN ? fallback : result;
}

void configure_keyboard() {
    const btga::KeyboardBindings& source = pc_settings.keyboard;
    keyboard.stick_up = configured_scancode(source.stick_up, SDL_SCANCODE_W);
    keyboard.stick_down = configured_scancode(source.stick_down, SDL_SCANCODE_S);
    keyboard.stick_left = configured_scancode(source.stick_left, SDL_SCANCODE_A);
    keyboard.stick_right = configured_scancode(source.stick_right, SDL_SCANCODE_D);
    keyboard.button_a = configured_scancode(source.button_a, SDL_SCANCODE_X);
    keyboard.button_b = configured_scancode(source.button_b, SDL_SCANCODE_Z);
    keyboard.button_z = configured_scancode(source.button_z, SDL_SCANCODE_LSHIFT);
    keyboard.button_start = configured_scancode(source.button_start, SDL_SCANCODE_RETURN);
    keyboard.button_l = configured_scancode(source.button_l, SDL_SCANCODE_Q);
    keyboard.button_r = configured_scancode(source.button_r, SDL_SCANCODE_E);
    keyboard.dpad_up = configured_scancode(source.dpad_up, SDL_SCANCODE_UP);
    keyboard.dpad_down = configured_scancode(source.dpad_down, SDL_SCANCODE_DOWN);
    keyboard.dpad_left = configured_scancode(source.dpad_left, SDL_SCANCODE_LEFT);
    keyboard.dpad_right = configured_scancode(source.dpad_right, SDL_SCANCODE_RIGHT);
    keyboard.c_up = configured_scancode(source.c_up, SDL_SCANCODE_I);
    keyboard.c_down = configured_scancode(source.c_down, SDL_SCANCODE_K);
    keyboard.c_left = configured_scancode(source.c_left, SDL_SCANCODE_J);
    keyboard.c_right = configured_scancode(source.c_right, SDL_SCANCODE_L);
}

SDL_GameController* configured_gamepad(int controller) {
    if (controller < 0 || controller >= max_controllers) return nullptr;
    const int slot = pc_settings.players[static_cast<std::size_t>(controller)].controller_slot;
    if (slot < 0 || slot >= max_controllers) return nullptr;
    return gamepads[static_cast<std::size_t>(slot)];
}

bool controller_input_pressed(SDL_GameController* gamepad, const std::string& binding) {
    constexpr Sint16 threshold = 16000;
    const SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(binding.c_str());
    if (button != SDL_CONTROLLER_BUTTON_INVALID) {
        return SDL_GameControllerGetButton(gamepad, button) != 0;
    }
    if (binding == "left_trigger") return SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > threshold;
    if (binding == "right_trigger") return SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > threshold;
    if (binding == "left_x_negative") return SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTX) < -threshold;
    if (binding == "left_x_positive") return SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTX) > threshold;
    if (binding == "left_y_negative") return SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTY) < -threshold;
    if (binding == "left_y_positive") return SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTY) > threshold;
    if (binding == "right_x_negative") return SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTX) < -threshold;
    if (binding == "right_x_positive") return SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTX) > threshold;
    if (binding == "right_y_negative") return SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTY) < -threshold;
    if (binding == "right_y_positive") return SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTY) > threshold;
    return false;
}
#endif

void poll_input() {
#ifdef BTGA_HAS_RT64
    if (!interactive) return;
    const std::lock_guard lock(input_settings_mutex);
    if (btga::renderer::consume_settings_update(pc_settings)) configure_keyboard();
    SDL_PumpEvents();
    for (auto& gamepad : gamepads) {
        if (gamepad && !SDL_GameControllerGetAttached(gamepad)) {
            SDL_GameControllerClose(gamepad);
            gamepad = nullptr;
        }
    }
    for (int device = 0; device < SDL_NumJoysticks(); ++device) {
        if (!SDL_IsGameController(device)) continue;
        const SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(device);
        bool already_open = false;
        for (SDL_GameController* gamepad : gamepads) {
            if (gamepad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepad)) == instance) {
                already_open = true;
                break;
            }
        }
        if (already_open) continue;
        for (auto& gamepad : gamepads) {
            if (!gamepad) {
                gamepad = SDL_GameControllerOpen(device);
                break;
            }
        }
    }
#endif
}
bool get_input(int controller, std::uint16_t* buttons, float* x, float* y) {
    if (controller < 0 || controller >= max_controllers) {
        return false;
    }
#ifdef BTGA_HAS_RT64
    if (interactive) {
        const std::lock_guard lock(input_settings_mutex);
        *buttons = 0;
        *x = 0.0F;
        *y = 0.0F;
        if (btga::renderer::settings_overlay_visible()) {
            if (controller == 0) {
                SDL_SetRelativeMouseMode(SDL_FALSE);
                mouse_turret_aiming.store(false, std::memory_order_relaxed);
                turret_stick_x[0].store(0.0F, std::memory_order_relaxed);
            }
            return true;
        }
        if (controller == 0) {
            const auto* keys = SDL_GetKeyboardState(nullptr);
            *buttons = (keys[keyboard.button_a] ? 0x8000 : 0) | (keys[keyboard.button_b] ? 0x4000 : 0) |
                       (keys[keyboard.button_z] ? 0x2000 : 0) | (keys[keyboard.button_start] ? 0x1000 : 0) |
                       (keys[keyboard.dpad_up] ? 0x0800 : 0) | (keys[keyboard.dpad_down] ? 0x0400 : 0) |
                       (keys[keyboard.dpad_left] ? 0x0200 : 0) | (keys[keyboard.dpad_right] ? 0x0100 : 0) |
                       (keys[keyboard.button_l] ? 0x20 : 0) | (keys[keyboard.button_r] ? 0x10 : 0) |
                       (keys[keyboard.c_up] ? 0x08 : 0) | (keys[keyboard.c_down] ? 0x04 : 0) |
                       (keys[keyboard.c_left] ? 0x02 : 0) | (keys[keyboard.c_right] ? 0x01 : 0);
            *x = float(keys[keyboard.stick_right] - keys[keyboard.stick_left]);
            *y = float(keys[keyboard.stick_up] - keys[keyboard.stick_down]);
            if (pc_settings.mod_independent_turret) {
                const std::uint32_t mouse = SDL_GetMouseState(nullptr, nullptr);
                const bool aiming = (mouse & SDL_BUTTON_RMASK) != 0;
                const bool was_aiming = mouse_turret_aiming.exchange(aiming, std::memory_order_relaxed);
                if (aiming != was_aiming) {
                    SDL_SetRelativeMouseMode(aiming ? SDL_TRUE : SDL_FALSE);
                }
                int mouse_x = 0;
                SDL_GetRelativeMouseState(&mouse_x, nullptr);
                if ((mouse & SDL_BUTTON_LMASK) != 0) *buttons |= 0x8000;
                if (aiming && was_aiming) {
                    const float sensitivity = pc_settings.mouse_turret_sensitivity;
                    turret_stick_x[0].store(std::clamp(mouse_x * 0.025F * sensitivity, -1.0F, 1.0F),
                                            std::memory_order_relaxed);
                }
                else {
                    turret_stick_x[0].store(0.0F, std::memory_order_relaxed);
                }
            }
            else if (mouse_turret_aiming.exchange(false, std::memory_order_relaxed)) {
                SDL_SetRelativeMouseMode(SDL_FALSE);
                turret_stick_x[0].store(0.0F, std::memory_order_relaxed);
            }
        }
        SDL_GameController* gamepad = configured_gamepad(controller);
        if (gamepad) {
            const btga::ControllerBindings& binding = pc_settings.controllers[static_cast<std::size_t>(controller)];
            auto pressed = [gamepad](const std::string& input) { return controller_input_pressed(gamepad, input); };
            const bool independent_turret = pc_settings.mod_independent_turret;
            *buttons |= (pressed(binding.button_a) ? 0x8000 : 0) |
                        (pressed(binding.button_b) ? 0x4000 : 0) |
                        (pressed(binding.button_z) ? 0x2000 : 0) |
                        (pressed(binding.button_start) ? 0x1000 : 0) |
                        (pressed(binding.dpad_up) ? 0x0800 : 0) |
                        (pressed(binding.dpad_down) ? 0x0400 : 0) |
                        (pressed(binding.dpad_left) ? 0x0200 : 0) |
                        (pressed(binding.dpad_right) ? 0x0100 : 0) |
                        (pressed(binding.button_l) ? 0x20 : 0) |
                        (pressed(binding.button_r) ? 0x10 : 0) |
                        (pressed(binding.c_up) ? 0x08 : 0) |
                        (pressed(binding.c_down) ? 0x04 : 0) |
                        (!independent_turret && pressed(binding.c_left) ? 0x02 : 0) |
                        (!independent_turret && pressed(binding.c_right) ? 0x01 : 0);
            constexpr float scale = 1.0F / 32767.0F;
            const float gx = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTX) * scale;
            const float gy = -SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTY) * scale;
            const float rx = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTX) * scale;
            const btga::PlayerSettings& player = pc_settings.players[static_cast<std::size_t>(controller)];
            const float configured_x = gx * player.sensitivity * (player.invert_x ? -1.0F : 1.0F);
            const float configured_y = gy * player.sensitivity * (player.invert_y ? -1.0F : 1.0F);
            if (configured_x > player.deadzone || configured_x < -player.deadzone) {
                *x = std::clamp(configured_x, -1.0F, 1.0F);
            }
            if (configured_y > player.deadzone || configured_y < -player.deadzone) {
                *y = std::clamp(configured_y, -1.0F, 1.0F);
            }
            if (controller != 0 || !mouse_turret_aiming.load(std::memory_order_relaxed)) {
                turret_stick_x[static_cast<std::size_t>(controller)].store(
                    std::abs(rx) > player.deadzone ? std::clamp(rx, -1.0F, 1.0F) : 0.0F,
                    std::memory_order_relaxed);
            }
        }
        return controller == 0 || gamepad != nullptr;
    }
#endif
    if (controller != 0) return false;
    *buttons = 0; *x = 0.0F; *y = 0.0F;
    return true;
}

extern "C" bool btga_extended_visibility_enabled() {
    const std::lock_guard lock(input_settings_mutex);
    return pc_settings.mod_extended_visibility;
}

extern "C" bool btga_independent_turret_enabled() {
    const std::lock_guard lock(input_settings_mutex);
    return pc_settings.mod_independent_turret;
}

extern "C" float btga_music_volume_setting() {
    const std::lock_guard lock(input_settings_mutex);
    return pc_settings.music_volume;
}

extern "C" float btga_effects_volume_setting() {
    const std::lock_guard lock(input_settings_mutex);
    return pc_settings.effects_volume;
}

extern "C" float btga_turret_stick_x(int player) {
    if (player < 0 || player >= max_controllers) return 0.0F;
    return turret_stick_x[static_cast<std::size_t>(player)].load(std::memory_order_relaxed);
}

void set_rumble(int controller, bool enabled) {
#ifdef BTGA_HAS_RT64
    const std::lock_guard lock(input_settings_mutex);
    if (interactive && controller >= 0 && controller < max_controllers &&
        configured_gamepad(controller)) {
        SDL_GameController* gamepad = configured_gamepad(controller);
        const float strength = pc_settings.players[static_cast<std::size_t>(controller)].rumble_strength;
        const auto magnitude = static_cast<std::uint16_t>(strength * 65535.0F);
        SDL_GameControllerRumble(gamepad, enabled ? magnitude : 0, enabled ? magnitude : 0, 250);
    }
#else
    (void)controller; (void)enabled;
#endif
}
ultramodern::input::connected_device_info_t connected_device(int controller) {
    if (controller == 0) {
        // The runtime enum has no ControllerPak value. Reporting a generic
        // attached pak makes libultra's status probe succeed; osPfsInitPak HLE
        // determines that this is a memory pak rather than a rumble pak.
        return {ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak};
    }
#ifdef BTGA_HAS_RT64
    const std::lock_guard lock(input_settings_mutex);
    if (interactive && controller > 0 && controller < max_controllers &&
        configured_gamepad(controller)) {
        return {ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak};
    }
#endif
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

std::filesystem::path write_failure_log(const char* message) {
    const std::filesystem::path directory = btga::pc_user_data_directory();
    const std::filesystem::path path = directory / "crash.log";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::ofstream log{path, std::ios::app};
    if (log) {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        log << "\n[" << now << "] BattleTanxGA runtime failure\n" << message << '\n';
    }
    return path;
}

#if defined(_WIN32) && defined(BTGA_HAS_RT64)
std::optional<std::filesystem::path> choose_rom() {
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"Nintendo 64 ROM (*.z64)\0*.z64\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = L"Select BattleTanx: Global Assault (USA Rev 0)";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return std::nullopt;
    return std::filesystem::path{file};
}

void show_startup_error(const char* message) {
    MessageBoxA(nullptr, message, "BattleTanx: Global Assault could not start",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}
#else
void show_startup_error(const char*) {}
#endif

bool boot_requested = false;
std::chrono::steady_clock::time_point boot_started;
std::chrono::seconds headless_boot_duration{5};

void drive_headless_runtime(void*) {
#ifdef BTGA_HAS_RT64
    if (interactive) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            btga::renderer::process_settings_event(&event);
            if (event.type == SDL_QUIT) {
                ultramodern::quit();
                return;
            }
            if (event.type == SDL_KEYDOWN && !event.key.repeat &&
                event.key.keysym.scancode == SDL_SCANCODE_F10) {
                btga::renderer::toggle_settings_overlay();
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
    } else if (std::chrono::steady_clock::now() - boot_started > headless_boot_duration) {
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

void patch_screen_edge_scissors(std::uint8_t*, recomp_context*);

recomp::GameEntry make_game(std::uint64_t rom_hash) {
    return {
        .rom_hash = rom_hash,
        .internal_name = "BATTLETANX2",
        .display_name = "BattleTanx: Global Assault",
        .game_id = std::u8string{game_id},
        .mod_game_id = "battletanx-ga",
        // BattleTanx GA stores profiles on a Controller Pak, not cartridge
        // EEPROM. AllowAll keeps unrelated runtime probes benign while the
        // actual persistence is handled by controller_pak_hle.cpp.
        .save_type = recomp::SaveType::AllowAll,
        .thumbnail_bytes = {},
        .is_enabled = true,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
        .on_init_callback = patch_screen_edge_scissors,
    };
}

void patch_screen_edge_scissors(std::uint8_t*, recomp_context*) {
    // Patch the runtime's private ROM copy before any N64 threads start. The
    // game later DMAs this segment-1 block into its live display-list buffer,
    // avoiding a race with RT64 reading that buffer. The user's ROM file is
    // never modified. All six replacements are exact-value guarded.
    struct Fix { std::size_t offset; std::uint32_t original; std::uint32_t replacement; };
    constexpr Fix fixes[] = {
        {0xB7F74, 0x004FC3BCU, 0x005003C0U},
        {0xB7F8C, 0x004FC1DCU, 0x005001DCU},
        {0xB7FA4, 0x004FC3BCU, 0x005003C0U},
        {0xB7FD4, 0x004FC1DCU, 0x005001DCU},
        {0xB7FEC, 0x0027C3BCU, 0x0027C3C0U},
        {0xB8004, 0x004FC3BCU, 0x005003C0U},
    };
    const auto source = recomp::get_rom();
    std::vector<std::uint8_t> patched(source.begin(), source.end());
    for (const Fix& fix : fixes) {
        if (fix.offset + 4 > patched.size()) {
            std::cerr << "Runtime ROM is too small for the viewport compatibility patch\n";
            std::abort();
        }
        const std::uint32_t word = (static_cast<std::uint32_t>(patched[fix.offset]) << 24) |
                                   (static_cast<std::uint32_t>(patched[fix.offset + 1]) << 16) |
                                   (static_cast<std::uint32_t>(patched[fix.offset + 2]) << 8) |
                                   patched[fix.offset + 3];
        if (word != fix.original) {
            std::cerr << "Verified ROM did not contain the expected viewport display lists\n";
            std::abort();
        }
        patched[fix.offset] = static_cast<std::uint8_t>(fix.replacement >> 24);
        patched[fix.offset + 1] = static_cast<std::uint8_t>(fix.replacement >> 16);
        patched[fix.offset + 2] = static_cast<std::uint8_t>(fix.replacement >> 8);
        patched[fix.offset + 3] = static_cast<std::uint8_t>(fix.replacement);
    }
    recomp::set_rom_contents(std::move(patched));
}
}

int runtime_main(int argc, char** argv) {
    if (argc > 0 && argv != nullptr && argv[0] != nullptr) {
        btga::configure_pc_user_data_directory(std::filesystem::path{argv[0]});
    }
    const std::filesystem::path config_path = btga::pc_user_data_directory();
    std::filesystem::create_directories(config_path);
    const std::filesystem::path settings_path = config_path / "settings.ini";
    pc_settings = btga::PcSettings::load(settings_path);
    if (!std::filesystem::exists(settings_path)) pc_settings.save(settings_path);

    if (argc == 2 && btga::is_packaged_texture_pack(std::filesystem::path{argv[1]})) {
        const std::filesystem::path installed =
            btga::install_texture_pack(std::filesystem::path{argv[1]}, config_path / "texture-packs");
        std::cout << "Installed texture pack: " << installed << '\n';
        // Continue as a normal launch so drag-and-drop both installs the pack
        // and starts the remembered game without requiring a second action.
        argc = 1;
    }

    std::optional<std::filesystem::path> picked_rom;
#if defined(_WIN32) && defined(BTGA_HAS_RT64)
    const bool choose_requested = argc == 2 && std::string_view{argv[1]} == "--choose-rom";
    if (argc == 1 && !pc_settings.last_rom_path.empty()) {
        const std::filesystem::path remembered = std::filesystem::u8path(pc_settings.last_rom_path);
        if (std::filesystem::is_regular_file(remembered)) {
            try {
                (void)btga::Rom::load_verified(remembered);
                picked_rom = remembered;
            } catch (const std::exception& error) {
                std::cerr << "Remembered ROM is no longer usable: " << error.what() << '\n';
            }
        }
    }
    if ((argc == 1 || choose_requested) && !picked_rom) {
        picked_rom = choose_rom();
        if (!picked_rom) return 0;
    }
#else
    const bool choose_requested = false;
#endif
    bool boot = argc == 3 && std::string_view{argv[1]} == "--boot";
    bool timed_boot = argc == 4 && std::string_view{argv[1]} == "--boot-seconds";
    bool play = picked_rom.has_value() ||
                (argc == 3 && std::string_view{argv[1]} == "--play") ||
                (argc == 2 && std::string_view{argv[1]}.starts_with("-") == false);
    if (argc != 1 && !choose_requested && !boot && !timed_boot && !play) {
        std::cerr << "Usage: " << argv[0]
                  << " [ROM | --choose-rom | --boot ROM | --boot-seconds SECONDS ROM | --play ROM]\n";
        return 2;
    }

    if (timed_boot) {
        unsigned int seconds = 0;
        const std::string_view text{argv[2]};
        const auto [end, error] =
            std::from_chars(text.data(), text.data() + text.size(), seconds);
        if (error != std::errc{} || end != text.data() + text.size() ||
            seconds == 0 || seconds > 3600) {
            std::cerr << "--boot-seconds must be an integer from 1 through 3600\n";
            return 2;
        }
        headless_boot_duration = std::chrono::seconds{seconds};
    }

    std::uint64_t rom_hash = 0;
    std::filesystem::path rom_path;
    if (boot || timed_boot || play) {
        if (picked_rom) rom_path = *picked_rom;
        else if (timed_boot) rom_path = argv[3];
        else if (argc == 2) rom_path = argv[1];
        else rom_path = argv[2];
        const btga::Rom rom = btga::Rom::load_verified(rom_path);
        rom_hash = XXH3_64bits(rom.bytes().data(), rom.bytes().size());
        if (play) {
            std::error_code path_error;
            const std::filesystem::path absolute = std::filesystem::absolute(rom_path, path_error);
            pc_settings.remember_rom(path_error ? rom_path : absolute);
            pc_settings.save(settings_path);
        }
    }

    const recomp::GameEntry game = make_game(rom_hash);

    if (!recomp::register_game(game)) {
        std::cerr << "Failed to register BattleTanx: Global Assault with the runtime\n";
        return 1;
    }
    btga_register_overlays();

    std::cout << "Registered all translated game code at entrypoint 0x"
              << std::hex << static_cast<std::uint32_t>(game.entrypoint_address) << '\n';

    if (!boot && !timed_boot && !play) {
        return 0;
    }

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
        configure_keyboard();
        std::uint32_t window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
        if (pc_settings.window_mode == btga::WindowMode::Borderless) {
            window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        } else if (pc_settings.window_mode == btga::WindowMode::Fullscreen) {
            window_flags |= SDL_WINDOW_FULLSCREEN;
        }
        window = SDL_CreateWindow("BattleTanx: Global Assault", SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED, pc_settings.window_width,
                                  pc_settings.window_height, window_flags);
        if (!window) { std::cerr << "Window creation failed: " << SDL_GetError() << '\n'; return 1; }
        // Populate ports before libultra's initial controller query. Hot-plug
        // changes are picked up by the same routine during normal polling.
        poll_input();
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
        btga::renderer::configure_rt64(pc_settings, config_path);
        selected_window = interactive_window_handle();
        selected_queue_samples = queue_audio;
        selected_frames_remaining = queued_audio_frames;
        selected_set_frequency = configure_audio;
    }
#endif

    recomp::Configuration config{
        .argc = 0,
        .argv = nullptr,
        .project_version = {0, 2, 1},
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
    for (auto& gamepad : gamepads) {
        if (gamepad != nullptr) {
            SDL_GameControllerClose(gamepad);
            gamepad = nullptr;
        }
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

int main(int argc, char** argv) {
    try {
        return runtime_main(argc, argv);
    } catch (const std::exception& error) {
        const std::filesystem::path log = write_failure_log(error.what());
        const std::string detail = std::string{error.what()} + "\n\nDiagnostic log:\n" + log.string();
        std::cerr << "BattleTanx: Global Assault could not start: " << detail << '\n';
        show_startup_error(detail.c_str());
        return 1;
    }
}
