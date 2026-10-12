#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <atomic>
#include <span>
#include <string_view>

#include "funcs.h"
#include "librecomp/game.hpp"
#include "btga/rom.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"
#include "xxHash/xxh3.h"

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

void discard_audio(std::int16_t*, std::size_t) {}
std::size_t audio_frames_remaining() { return 0; }
void set_audio_frequency(std::uint32_t) {}

void poll_input() {}
bool get_input(int controller, std::uint16_t* buttons, float* x, float* y) {
    if (controller != 0) {
        return false;
    }
    *buttons = 0;
    *x = 0.0F;
    *y = 0.0F;
    return true;
}
void set_rumble(int, bool) {}
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

void drive_headless_runtime(void*) {
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
    if (argc != 1 && !boot) {
        std::cerr << "Usage: " << argv[0] << " [--boot /path/to/battletanx_ga.z64]\n";
        return 2;
    }

    std::uint64_t rom_hash = 0;
    std::filesystem::path rom_path;
    if (boot) {
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

    if (!boot) {
        return 0;
    }

    const std::filesystem::path config_path =
        std::filesystem::temp_directory_path() / "battletanx-ga-pc-runtime";
    std::filesystem::create_directories(config_path);
    recomp::register_config_path(config_path);
    if (recomp::select_rom(rom_path, game.game_id) != recomp::RomValidationError::Good) {
        std::cerr << "N64ModernRuntime rejected the already verified ROM\n";
        return 1;
    }

    recomp::Configuration config{
        .argc = 0,
        .argv = nullptr,
        .project_version = {0, 1, 0},
        .window_handle = headless_window_handle(),
        .rsp_callbacks = {.get_rsp_microcode = select_rsp_microcode},
        .renderer_callbacks = {.create_render_context = create_headless_renderer},
        .audio_callbacks = {
            .queue_samples = discard_audio,
            .get_frames_remaining = audio_frames_remaining,
            .set_frequency = set_audio_frequency,
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
    return 0;
}
