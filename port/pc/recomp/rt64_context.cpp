#include "rt64_context.hpp"

#if defined(_WIN32)
#include <Windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <oleauto.h>
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <future>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "btga/texture_packs.hpp"
#include "btga/btga_gameplay.h"
#include "btga/dedicated_resume.hpp"
#include "btga/gameplay_digest.hpp"
#include "btga/gameplay_snapshot.hpp"
#include "btga/master_client.hpp"
#include "btga/input_sync.hpp"
#include "btga/online_multiplayer.hpp"
#include "btga/online_bootstrap.hpp"
#include "btga/snapshot_transfer.hpp"
#include "btga/udp_online_io.hpp"
#include "hle/rt64_application.h"
#include "imgui/imgui.h"
#include "render/rt64_texture_cache.h"
#include "ultramodern/ultramodern.hpp"

namespace {
constexpr std::uint8_t online_state_version = 1;
constexpr std::uint8_t online_state_ack_version = 2;
constexpr std::size_t online_state_packet_size = 17;
constexpr std::size_t random_seed_offset = 0x00114C80U;
constexpr std::size_t frame_counter_offset = 0x0021945CU;
constexpr std::uint32_t online_state_checksum_salt = UINT32_C(0x42544741);
constexpr std::size_t online_digest_history_size = 128U;
constexpr std::uint8_t gameplay_snapshot_version = 0x10;
constexpr std::uint8_t gameplay_snapshot_ack_version = 0x11;
constexpr std::size_t gameplay_snapshot_header_size =
    1U + btga::snapshot_chunk_header_size;
constexpr std::size_t gameplay_snapshot_chunk_size =
    btga::snapshot_chunk_payload_size;
constexpr std::size_t gameplay_snapshot_size = btga::gameplay_snapshot_size;
constexpr std::size_t gameplay_snapshot_chunk_count =
    (gameplay_snapshot_size + gameplay_snapshot_chunk_size - 1U) /
    gameplay_snapshot_chunk_size;
static_assert(gameplay_snapshot_header_size + gameplay_snapshot_chunk_size <=
              BTGA_NET_MAX_DATAGRAM_SIZE);
static_assert(gameplay_snapshot_chunk_count <= 64U);

void put_u32_be(std::uint8_t* output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 24);
    output[1] = static_cast<std::uint8_t>(value >> 16);
    output[2] = static_cast<std::uint8_t>(value >> 8);
    output[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t get_u32_be(const std::uint8_t* input) {
    return (static_cast<std::uint32_t>(input[0]) << 24) |
           (static_cast<std::uint32_t>(input[1]) << 16) |
           (static_cast<std::uint32_t>(input[2]) << 8) | input[3];
}

std::uint32_t load_rdram_word(const std::uint8_t* rdram, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

std::uint8_t load_rdram_byte(const std::uint8_t* rdram, std::size_t offset) {
    // N64Recomp keeps 32-bit guest words in native order; byte addresses use
    // the same XOR-3 lane mapping as MEM_BU.
    return rdram[offset ^ 3U];
}

void store_rdram_word(std::uint8_t* rdram, std::size_t offset, std::uint32_t value) {
    std::memcpy(rdram + offset, &value, sizeof(value));
}

std::uint32_t online_state_checksum(std::uint8_t version, std::uint32_t seed,
                                    std::uint32_t frame, std::uint32_t generation) {
    const std::uint32_t rotated_frame = (frame << 11) | (frame >> 21);
    return online_state_checksum_salt ^ version ^ seed ^ rotated_frame ^ generation;
}

btga::PcSettings pc_settings = btga::PcSettings::defaults();
std::filesystem::path pc_data_path;
std::mutex settings_update_mutex;
std::optional<btga::PcSettings> settings_update;

class Context;
std::atomic<Context*> active_context{nullptr};

bool environment_flag_enabled(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }

    const std::string_view text{value};
    return !text.empty() && text != "0" && text != "false" && text != "FALSE" &&
           text != "off" && text != "OFF";
}

void configure_aspect_ratio(RT64::UserConfiguration& config, btga::AspectRatioMode mode) {
    using Ratio = RT64::UserConfiguration::AspectRatio;
    if (mode == btga::AspectRatioMode::Original) {
        config.aspectRatio = Ratio::Original;
        config.extAspectRatio = Ratio::Original;
        return;
    }
    if (mode == btga::AspectRatioMode::MatchWindow) {
        config.aspectRatio = Ratio::Expand;
        config.extAspectRatio = Ratio::Expand;
        return;
    }

    double target = 16.0 / 9.0;
    switch (mode) {
    case btga::AspectRatioMode::Ratio16x10: target = 16.0 / 10.0; break;
    case btga::AspectRatioMode::Ratio21x9: target = 21.0 / 9.0; break;
    case btga::AspectRatioMode::Ratio32x9: target = 32.0 / 9.0; break;
    case btga::AspectRatioMode::Ratio16x9:
    default: break;
    }
    config.aspectRatio = Ratio::Manual;
    config.aspectTarget = target;
    config.extAspectRatio = Ratio::Manual;
    config.extAspectTarget = target;
}

void open_external_url(const char* url) {
    if (SDL_OpenURL(url) != 0) {
        std::cerr << "Could not open " << url << ": " << SDL_GetError() << '\n';
    }
}

void open_folder(const std::filesystem::path& folder_path) {
    std::filesystem::create_directories(folder_path);
#if defined(_WIN32)
    const std::wstring folder = folder_path.wstring();
    if (reinterpret_cast<std::intptr_t>(
            ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
        std::cerr << "Could not open folder: " << folder_path << '\n';
    }
#else
    std::string path = folder_path.generic_string();
    std::string encoded;
    encoded.reserve(path.size() + 16);
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded.push_back(static_cast<char>(c));
        }
        else {
            encoded.push_back('%');
            encoded.push_back(hex[c >> 4]);
            encoded.push_back(hex[c & 0xF]);
        }
    }
    open_external_url(("file://" + encoded).c_str());
#endif
}

void open_data_folder() {
    open_folder(pc_data_path);
}

std::string controller_input_name(std::string_view input) {
    if (input == "left_trigger") return "Left Trigger";
    if (input == "right_trigger") return "Right Trigger";
    if (input == "left_x_negative") return "Left Stick Left";
    if (input == "left_x_positive") return "Left Stick Right";
    if (input == "left_y_negative") return "Left Stick Up";
    if (input == "left_y_positive") return "Left Stick Down";
    if (input == "right_x_negative") return "Right Stick Left";
    if (input == "right_x_positive") return "Right Stick Right";
    if (input == "right_y_negative") return "Right Stick Up";
    if (input == "right_y_positive") return "Right Stick Down";
    const SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(std::string{input}.c_str());
    if (button != SDL_CONTROLLER_BUTTON_INVALID) {
        const char* name = SDL_GameControllerGetStringForButton(button);
        if (name != nullptr && *name != '\0') {
            std::string display{name};
            display.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(display.front())));
            return display;
        }
    }
    return std::string{input};
}

std::string controller_axis_binding(const SDL_ControllerAxisEvent& event) {
    constexpr Sint16 threshold = 16000;
    if (event.value > -threshold && event.value < threshold) return {};
    const bool positive = event.value > 0;
    switch (static_cast<SDL_GameControllerAxis>(event.axis)) {
    case SDL_CONTROLLER_AXIS_LEFTX: return positive ? "left_x_positive" : "left_x_negative";
    case SDL_CONTROLLER_AXIS_LEFTY: return positive ? "left_y_positive" : "left_y_negative";
    case SDL_CONTROLLER_AXIS_RIGHTX: return positive ? "right_x_positive" : "right_x_negative";
    case SDL_CONTROLLER_AXIS_RIGHTY: return positive ? "right_y_positive" : "right_y_negative";
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return positive ? "left_trigger" : std::string{};
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return positive ? "right_trigger" : std::string{};
    default: return {};
    }
}

SDL_Scancode imgui_key_to_sdl_scancode(ImGuiKey key) {
    switch (key) {
    case ImGuiKey_Tab: return SDL_SCANCODE_TAB;
    case ImGuiKey_LeftArrow: return SDL_SCANCODE_LEFT;
    case ImGuiKey_RightArrow: return SDL_SCANCODE_RIGHT;
    case ImGuiKey_UpArrow: return SDL_SCANCODE_UP;
    case ImGuiKey_DownArrow: return SDL_SCANCODE_DOWN;
    case ImGuiKey_PageUp: return SDL_SCANCODE_PAGEUP;
    case ImGuiKey_PageDown: return SDL_SCANCODE_PAGEDOWN;
    case ImGuiKey_Home: return SDL_SCANCODE_HOME;
    case ImGuiKey_End: return SDL_SCANCODE_END;
    case ImGuiKey_Insert: return SDL_SCANCODE_INSERT;
    case ImGuiKey_Delete: return SDL_SCANCODE_DELETE;
    case ImGuiKey_Backspace: return SDL_SCANCODE_BACKSPACE;
    case ImGuiKey_Space: return SDL_SCANCODE_SPACE;
    case ImGuiKey_Enter: return SDL_SCANCODE_RETURN;
    case ImGuiKey_Escape: return SDL_SCANCODE_ESCAPE;
    case ImGuiKey_LeftCtrl: return SDL_SCANCODE_LCTRL;
    case ImGuiKey_LeftShift: return SDL_SCANCODE_LSHIFT;
    case ImGuiKey_LeftAlt: return SDL_SCANCODE_LALT;
    case ImGuiKey_LeftSuper: return SDL_SCANCODE_LGUI;
    case ImGuiKey_RightCtrl: return SDL_SCANCODE_RCTRL;
    case ImGuiKey_RightShift: return SDL_SCANCODE_RSHIFT;
    case ImGuiKey_RightAlt: return SDL_SCANCODE_RALT;
    case ImGuiKey_RightSuper: return SDL_SCANCODE_RGUI;
    case ImGuiKey_Menu: return SDL_SCANCODE_APPLICATION;
    case ImGuiKey_Apostrophe: return SDL_SCANCODE_APOSTROPHE;
    case ImGuiKey_Comma: return SDL_SCANCODE_COMMA;
    case ImGuiKey_Minus: return SDL_SCANCODE_MINUS;
    case ImGuiKey_Period: return SDL_SCANCODE_PERIOD;
    case ImGuiKey_Slash: return SDL_SCANCODE_SLASH;
    case ImGuiKey_Semicolon: return SDL_SCANCODE_SEMICOLON;
    case ImGuiKey_Equal: return SDL_SCANCODE_EQUALS;
    case ImGuiKey_LeftBracket: return SDL_SCANCODE_LEFTBRACKET;
    case ImGuiKey_Backslash: return SDL_SCANCODE_BACKSLASH;
    case ImGuiKey_RightBracket: return SDL_SCANCODE_RIGHTBRACKET;
    case ImGuiKey_GraveAccent: return SDL_SCANCODE_GRAVE;
    case ImGuiKey_CapsLock: return SDL_SCANCODE_CAPSLOCK;
    case ImGuiKey_ScrollLock: return SDL_SCANCODE_SCROLLLOCK;
    case ImGuiKey_NumLock: return SDL_SCANCODE_NUMLOCKCLEAR;
    case ImGuiKey_PrintScreen: return SDL_SCANCODE_PRINTSCREEN;
    case ImGuiKey_Pause: return SDL_SCANCODE_PAUSE;
    case ImGuiKey_Keypad0: return SDL_SCANCODE_KP_0;
    case ImGuiKey_Keypad1: return SDL_SCANCODE_KP_1;
    case ImGuiKey_Keypad2: return SDL_SCANCODE_KP_2;
    case ImGuiKey_Keypad3: return SDL_SCANCODE_KP_3;
    case ImGuiKey_Keypad4: return SDL_SCANCODE_KP_4;
    case ImGuiKey_Keypad5: return SDL_SCANCODE_KP_5;
    case ImGuiKey_Keypad6: return SDL_SCANCODE_KP_6;
    case ImGuiKey_Keypad7: return SDL_SCANCODE_KP_7;
    case ImGuiKey_Keypad8: return SDL_SCANCODE_KP_8;
    case ImGuiKey_Keypad9: return SDL_SCANCODE_KP_9;
    case ImGuiKey_KeypadDecimal: return SDL_SCANCODE_KP_DECIMAL;
    case ImGuiKey_KeypadDivide: return SDL_SCANCODE_KP_DIVIDE;
    case ImGuiKey_KeypadMultiply: return SDL_SCANCODE_KP_MULTIPLY;
    case ImGuiKey_KeypadSubtract: return SDL_SCANCODE_KP_MINUS;
    case ImGuiKey_KeypadAdd: return SDL_SCANCODE_KP_PLUS;
    case ImGuiKey_KeypadEnter: return SDL_SCANCODE_KP_ENTER;
    case ImGuiKey_KeypadEqual: return SDL_SCANCODE_KP_EQUALS;
    case ImGuiKey_0: return SDL_SCANCODE_0;
    case ImGuiKey_1: return SDL_SCANCODE_1;
    case ImGuiKey_2: return SDL_SCANCODE_2;
    case ImGuiKey_3: return SDL_SCANCODE_3;
    case ImGuiKey_4: return SDL_SCANCODE_4;
    case ImGuiKey_5: return SDL_SCANCODE_5;
    case ImGuiKey_6: return SDL_SCANCODE_6;
    case ImGuiKey_7: return SDL_SCANCODE_7;
    case ImGuiKey_8: return SDL_SCANCODE_8;
    case ImGuiKey_9: return SDL_SCANCODE_9;
    default:
        break;
    }

    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
        return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (key - ImGuiKey_A));
    }
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) {
        return static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + (key - ImGuiKey_F1));
    }
    if (key >= ImGuiKey_F13 && key <= ImGuiKey_F24) {
        return static_cast<SDL_Scancode>(SDL_SCANCODE_F13 + (key - ImGuiKey_F13));
    }
    return SDL_SCANCODE_UNKNOWN;
}

std::array<std::uint8_t, 0x40> dummy_header{};
std::array<std::uint8_t, 0x1000> dmem{};
std::array<std::uint8_t, 0x1000> imem{};
bool rendererless_online_context = false;

unsigned int mi_intr = 0;
unsigned int dpc_start = 0;
unsigned int dpc_end = 0;
unsigned int dpc_current = 0;
unsigned int dpc_status = 0;
unsigned int dpc_clock = 0;
unsigned int dpc_bufbusy = 0;
unsigned int dpc_pipebusy = 0;
unsigned int dpc_tmem = 0;

void check_interrupts() {}

class Context final : public ultramodern::renderer::RendererContext {
public:
    Context(std::uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode)
        : rdram_(rdram), draft_settings_(pc_settings) {
        RT64::Application::Core core{};
#if defined(_WIN32)
        core.window = window.window;
#elif defined(__APPLE__)
        core.window.window = window.window;
        core.window.view = window.view;
#else
        core.window = window;
#endif
        core.checkInterrupts = check_interrupts;
        core.HEADER = dummy_header.data();
        core.RDRAM = rdram;
        core.DMEM = dmem.data();
        core.IMEM = imem.data();
        core.MI_INTR_REG = &mi_intr;
        core.DPC_START_REG = &dpc_start;
        core.DPC_END_REG = &dpc_end;
        core.DPC_CURRENT_REG = &dpc_current;
        core.DPC_STATUS_REG = &dpc_status;
        core.DPC_CLOCK_REG = &dpc_clock;
        core.DPC_BUFBUSY_REG = &dpc_bufbusy;
        core.DPC_PIPEBUSY_REG = &dpc_pipebusy;
        core.DPC_TMEM_REG = &dpc_tmem;

        auto* vi = ultramodern::renderer::get_vi_regs();
        core.VI_STATUS_REG = &vi->VI_STATUS_REG;
        core.VI_ORIGIN_REG = &vi->VI_ORIGIN_REG;
        core.VI_WIDTH_REG = &vi->VI_WIDTH_REG;
        core.VI_INTR_REG = &vi->VI_INTR_REG;
        core.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
        core.VI_TIMING_REG = &vi->VI_TIMING_REG;
        core.VI_V_SYNC_REG = &vi->VI_V_SYNC_REG;
        core.VI_H_SYNC_REG = &vi->VI_H_SYNC_REG;
        core.VI_LEAP_REG = &vi->VI_LEAP_REG;
        core.VI_H_START_REG = &vi->VI_H_START_REG;
        core.VI_V_START_REG = &vi->VI_V_START_REG;
        core.VI_V_BURST_REG = &vi->VI_V_BURST_REG;
        core.VI_X_SCALE_REG = &vi->VI_X_SCALE_REG;
        core.VI_Y_SCALE_REG = &vi->VI_Y_SCALE_REG;

        // Unattended multiplayer validation exercises the real recompiled
        // game and transport without presenting pixels. Keeping a live
        // RendererContext still owns the synchronized session, while avoiding
        // two unnecessary D3D/Vulkan devices and their intermittent setup
        // failures during rapid process-churn tests.
        if (rendererless_online_context) {
            rendererless_ = true;
            setup_result = ultramodern::renderer::SetupResult::Success;
            chosen_api = ultramodern::renderer::GraphicsApi::Auto;
            active_context.store(this, std::memory_order_release);
            adopt_preboot_session();
            return;
        }

        RT64::ApplicationConfiguration configuration{};
        configuration.appId = "BattleTanxGA";
        configuration.dataPath = pc_data_path;
        configuration.detectDataPath = pc_data_path.empty();
        configuration.useConfigurationFile = false;
        app_ = std::make_unique<RT64::Application>(core, configuration);
#if defined(_WIN32)
        app_->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
#else
        app_->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
#endif
        app_->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
        app_->userConfig.resolutionMultiplier = pc_settings.render_scale;
        configure_aspect_ratio(app_->userConfig, pc_settings.aspect_ratio);
        switch (pc_settings.antialiasing) {
        case 2:
            app_->userConfig.antialiasing = RT64::UserConfiguration::Antialiasing::MSAA2X;
            break;
        case 4:
            app_->userConfig.antialiasing = RT64::UserConfiguration::Antialiasing::MSAA4X;
            break;
        case 8:
            app_->userConfig.antialiasing = RT64::UserConfiguration::Antialiasing::MSAA8X;
            break;
        default:
            app_->userConfig.antialiasing = RT64::UserConfiguration::Antialiasing::None;
            break;
        }
        app_->userConfig.refreshRate = pc_settings.frame_limit > 0
            ? RT64::UserConfiguration::RefreshRate::Manual
            : RT64::UserConfiguration::RefreshRate::Original;
        app_->userConfig.refreshRateTarget = pc_settings.frame_limit > 0 ? pc_settings.frame_limit : 60;
        app_->userConfig.internalColorFormat = RT64::UserConfiguration::InternalColorFormat::Standard;
        app_->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
        const bool open_inspector = environment_flag_enabled("BTGA_RT64_INSPECTOR");
        app_->userConfig.developerMode = developer_mode || open_inspector ||
                                         environment_flag_enabled("BTGA_RT64_DEVELOPER");
        const RT64::Application::SetupResult setup_result = app_->setup(0);
        if (setup_result != RT64::Application::SetupResult::Success) {
            std::cerr << "RT64 renderer setup failed before online-session handoff (result="
                      << static_cast<int>(setup_result) << ").\n";
            app_.reset();
        } else if (open_inspector) {
            app_->processDeveloperShortcut(RT64::Application::DeveloperShortcut::Inspector);
        }
        if (app_) {
            if (app_->swapChain != nullptr) app_->swapChain->setVsyncEnabled(pc_settings.vsync);
            reload_texture_packs();
            app_->customOverlayCallback = [this]() { draw_settings_overlay(); };
            active_context.store(this, std::memory_order_release);
            if (environment_flag_enabled("BTGA_SETTINGS_OPEN")) app_->setCustomOverlayVisible(true);
        }
        adopt_preboot_session();
    }

    ~Context() override {
        if (online_task_.valid()) online_task_.wait();
        if (!online_restart_handoff_ && online_master_ &&
            !online_lobby_id_.empty() && !online_host_secret_.empty()) {
            online_master_->remove(online_lobby_id_, online_host_secret_);
        }
        if (online_connection_) online_connection_->leave_online();
        Context* expected = this;
        active_context.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
        if (app_) app_->customOverlayCallback = {};
    }

    bool valid() override { return static_cast<bool>(app_) || rendererless_; }
    bool update_config(const ultramodern::renderer::GraphicsConfig&,
                       const ultramodern::renderer::GraphicsConfig&) override { return false; }
    void enable_instant_present() override {}
    void send_dl(const OSTask* task) override {
        if (!app_) return;
        app_->state->rsp->reset();
        app_->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFFU,
                                       task->t.ucode_data & 0x3FFFFFFU, true);
        app_->processDisplayLists(app_->core.RDRAM, task->t.data_ptr & 0x3FFFFFFU, 0, true);
    }
    void send_dummy_workload(std::uint32_t address) override {
        if (!app_) return;
        // The renderer thread normally submits a harmless dummy display list while
        // the N64 thread starts.  During an online pre-boot rendezvous RDRAM has not
        // been initialized yet, so interpreting that address would dereference
        // uninitialized emulated memory.
        if (btga::preboot_online_waiting()) return;
        app_->processDisplayLists(app_->core.RDRAM, address, 0, true);
    }
    void update_screen() override {
        update_online();
        if (!app_) return;
        app_->updateScreen();
        if (apply_render_requested_.exchange(false, std::memory_order_acq_rel)) {
            apply_render_settings();
        }
    }
    void shutdown() override { if (app_) app_->end(); }
    std::uint32_t get_display_framerate() const override {
        return app_ ? app_->presentQueue->ext.sharedResources->swapChainRate : 60U;
    }
    float get_resolution_scale() const override { return 1.0F; }

    void toggle_settings() {
        if (!app_) return;
        if (app_->customOverlayVisible) {
            app_->customOverlayCloseFrames.store(2, std::memory_order_release);
            return;
        }
        draft_settings_ = pc_settings;
        app_->setCustomOverlayVisible(true);
    }

    bool settings_visible() const { return app_ && app_->customOverlayVisible; }

    bool process_event(SDL_Event* event) {
        if (!app_ || !app_->customOverlayVisible || event == nullptr) return false;
        // F10 belongs exclusively to the host loop. Forwarding the same press
        // into ImGui would open the overlay and immediately close it again.
        if ((event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) &&
            event->key.keysym.scancode == SDL_SCANCODE_F10) {
            return false;
        }
        const std::lock_guard capture_lock(controller_capture_mutex_);
        if (controller_capture_player_ >= 0) {
            if (event->type == SDL_CONTROLLERBUTTONDOWN) {
                const char* name = SDL_GameControllerGetStringForButton(
                    static_cast<SDL_GameControllerButton>(event->cbutton.button));
                if (name != nullptr && *name != '\0') pending_controller_binding_ = name;
                return true;
            }
            if (event->type == SDL_CONTROLLERAXISMOTION) {
                const std::string binding = controller_axis_binding(event->caxis);
                if (!binding.empty()) pending_controller_binding_ = binding;
                return !binding.empty();
            }
        }
        return app_->sdlEventFilter(event);
    }

    int online_local_player_slot() {
        const std::lock_guard lock(online_mutex_);
        if (!online_connection_ || online_connection_->state() != btga::OnlineState::Lobby) return -1;
        return static_cast<int>(online_connection_->relay_slot());
    }

    bool online_synchronized_input(int player, std::uint16_t& buttons, float& x, float& y) {
        const std::lock_guard lock(online_mutex_);
        if (!online_connection_ || online_connection_->state() != btga::OnlineState::Lobby ||
            player < 0 || player > 1) return false;
        pump_online_inputs_locked();
        if (online_frame_inputs_active_) {
            if (!online_frame_applied_inputs_valid_) return false;
            const auto& input = online_frame_applied_inputs_[static_cast<std::size_t>(player)];
            buttons = input.buttons;
            x = static_cast<float>(input.stick_x) / 127.0F;
            y = static_cast<float>(input.stick_y) / 127.0F;
            return true;
        }
        (void)prepare_applied_inputs_locked();
        if (!online_applied_inputs_valid_) return false;
        const auto& input = online_applied_inputs_[static_cast<std::size_t>(player)];
        buttons = input.buttons;
        x = static_cast<float>(input.stick_x) / 127.0F;
        y = static_cast<float>(input.stick_y) / 127.0F;
        return true;
    }

    void online_submit_local_input(std::uint16_t buttons, float x, float y) {
        std::unique_lock lock(online_mutex_);
        if (!online_connection_ || online_connection_->state() != btga::OnlineState::Lobby) return;
        const std::uint16_t local = online_connection_->relay_slot();
        if (local > 1) return; // Two-player synchronization milestone.
        const btga::OnlineControllerInput input{
            buttons,
            static_cast<std::int8_t>(std::clamp(x, -1.0F, 1.0F) * 127.0F),
            static_cast<std::int8_t>(std::clamp(y, -1.0F, 1.0F) * 127.0F),
        };
        online_latest_local_input_ = input;
        // Once the game clock is active, the simulation barrier publishes
        // this sample for a specific future frame. Controller polling can run
        // on a different native thread and must no longer define game timing.
        if (online_frame_inputs_active_) return;
        // Receive before assigning this poll's tick. Slot zero is the epoch
        // authority and binds its next tick to the latest client tick it has
        // actually observed; that exact pair is repeated in every host packet.
        pump_online_inputs_locked();
        if (local == 0 && online_state_synchronized_ && !online_tick_origins_valid_) {
            const auto remote_tick = online_inputs_.latest_tick_for(1);
            if (remote_tick) {
                online_local_tick_origin_ = online_tick_;
                online_remote_tick_origin_ = *remote_tick;
                online_tick_origins_valid_ = true;
                trace_online_origin_locked(local);
            }
        }
        std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
        const std::uint32_t tick = online_tick_++;
        const std::uint32_t sequence = online_sequence_++;
        online_inputs_.record_local(static_cast<std::uint8_t>(local), tick, sequence, input);
        constexpr std::uint32_t redundancy = 8;
        const std::uint32_t first_tick = tick >= redundancy - 1 ? tick - (redundancy - 1) : 0;
        std::array<btga::OnlineControllerInput, redundancy> frames{};
        std::size_t frame_count = 0;
        for (std::uint32_t frame_tick = first_tick; frame_tick <= tick; ++frame_tick) {
            const auto frame = online_inputs_.input_for(
                static_cast<std::uint8_t>(local), frame_tick);
            if (frame) frames[frame_count++] = *frame;
        }
        const std::size_t size = online_inputs_.encode_batch(
            packet, online_session_id_, local, sequence, first_tick,
            static_cast<std::uint8_t>(local),
            std::span<const btga::OnlineControllerInput>{frames.data(), frame_count},
            local == 0 && online_tick_origins_valid_
                ? std::optional<btga::OnlineSyncOrigin>{btga::OnlineSyncOrigin{
                      online_local_tick_origin_, online_remote_tick_origin_}}
                : std::nullopt);
        const std::uint16_t destination = local == 0 ? 1 : 0;
        if (size != 0) {
            const bool sent = online_connection_->send_gameplay(
                destination, {packet.data(), size});
            (void)sent;
        }
        pump_online_inputs_locked();
        const auto state_wait_started = std::chrono::steady_clock::now();
        while (!online_state_synchronized_) {
            if (!online_connection_ || online_connection_->state() != btga::OnlineState::Lobby) {
                return;
            }
            if (local == 0 && online_peer_seen_[1]) {
                if (!online_state_sent_) {
                    online_state_seed_ = load_rdram_word(rdram_, random_seed_offset);
                    online_state_frame_ = load_rdram_word(rdram_, frame_counter_offset);
                    online_state_generation_ = sequence;
                    online_state_sent_ = true;
                }
                send_online_state_locked(1, online_state_version);
            } else if (size != 0) {
                (void)online_connection_->send_gameplay(destination, {packet.data(), size});
            }
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            lock.lock();
            pump_online_inputs_locked();
            if (std::chrono::steady_clock::now() - state_wait_started >=
                std::chrono::milliseconds(250)) {
                online_status_ = local == 0
                    ? "Synchronizing authoritative game state..."
                    : "Waiting for authoritative game state...";
            }
            if (std::chrono::steady_clock::now() - state_wait_started >=
                std::chrono::seconds(10)) {
                online_status_ =
                    "Initial game-state synchronization timed out; continuing offline.";
                if (online_trace_) {
                    online_trace_ << "INITIAL_STATE_TIMEOUT tick=" << tick << '\n';
                    online_trace_.flush();
                }
                online_connection_->leave_online();
                return;
            }
        }
        if (local == 0 && !online_tick_origins_valid_) {
            const auto remote_tick = online_inputs_.latest_tick_for(1);
            if (remote_tick) {
                online_local_tick_origin_ = tick;
                online_remote_tick_origin_ = *remote_tick;
                online_tick_origins_valid_ = true;
                trace_online_origin_locked(local);
            }
        }
        if (!online_tick_origins_valid_) return;

        const auto wait_started = std::chrono::steady_clock::now();
        while (!prepare_applied_inputs_locked()) {
            // A deterministic simulation must never substitute a different
            // pair merely because one UDP datagram is late. Re-send the same
            // redundant batch while yielding the mutex so UI disconnect and
            // shutdown remain available. With the four-poll jitter buffer,
            // this loop is normally never entered.
            if (!online_connection_ || online_connection_->state() != btga::OnlineState::Lobby) {
                return;
            }
            const bool resent = online_connection_->send_gameplay(
                destination, {packet.data(), size});
            (void)resent;
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lock.lock();
            pump_online_inputs_locked();
            if (std::chrono::steady_clock::now() - wait_started >=
                std::chrono::milliseconds(250)) {
                online_status_ = "Waiting for a confirmed synchronized input frame...";
                if (!online_wait_traced_ && online_trace_) {
                    online_trace_ << "WAIT local_tick=" << tick << '\n';
                    online_trace_.flush();
                    online_wait_traced_ = true;
                }
            }
            if (std::chrono::steady_clock::now() - wait_started >=
                std::chrono::seconds(10)) {
                online_status_ = "Online input synchronization timed out; continuing offline.";
                if (online_trace_) {
                    online_trace_ << "POLL_INPUT_TIMEOUT tick=" << tick << '\n';
                    online_trace_.flush();
                }
                online_connection_->leave_online();
                return;
            }
        }
        online_wait_traced_ = false;
    }

    bool online_simulation_frame_barrier() {
        std::unique_lock lock(online_mutex_);
        // The game clock may execute before the renderer's first update. Adopt
        // the cold-boot handoff here so no peer can run an unpaired first tick.
        adopt_preboot_session();
        if (!online_connection_ || online_connection_->state() != btga::OnlineState::Lobby) {
            return true;
        }
        const std::uint16_t local = online_connection_->relay_slot();
        if (local > 1) return false;
        const std::uint16_t destination = local == 0 ? 1 : 0;
        const std::uint32_t frame = online_simulation_frame_++;
        // Front-end presentation remains poll-synchronized so menus retain
        // button edges. Native-frame timing there may legitimately differ;
        // enforce deterministic digests only in frame-synchronized gameplay.
        const bool frame_inputs_active = btga::gameplay_frame_input_active(
            {rdram_, 8U * 1024U * 1024U});
        std::uint32_t checksum = 0;
        if (frame_inputs_active) {
            const btga::GameplayStateDigests digests = online_gameplay_digests();
            checksum = digests.combined;
            record_online_digest(online_local_digests_, frame, digests);
            compare_online_digest(frame);
        }

        std::array<std::uint8_t, BTGA_NET_HEADER_SIZE + BTGA_NET_FRAME_SYNC_SIZE> packet{};
        BtgaNetHeader header{};
        header.protocol_version = BTGA_NET_PROTOCOL_VERSION;
        header.message_type = BTGA_NET_MSG_EVENT;
        header.flags = static_cast<std::uint8_t>(BTGA_NET_FLAG_RELIABLE |
                                                 BTGA_NET_FLAG_RELAYED);
        header.session_id = online_session_id_;
        header.sequence = online_sequence_++;
        header.payload_size = static_cast<std::uint16_t>(BTGA_NET_FRAME_SYNC_SIZE);
        header.peer_id = local;
        if (btga_net_encode_header(packet.data(), packet.size(), &header) !=
            BTGA_NET_HEADER_SIZE) return false;
        const BtgaNetFrameSync sync{frame, checksum};
        if (btga_net_encode_frame_sync(packet.data() + BTGA_NET_HEADER_SIZE,
                                       BTGA_NET_FRAME_SYNC_SIZE, &sync) !=
            BTGA_NET_FRAME_SYNC_SIZE) return false;

        const auto send_frame = [&]() {
            return online_connection_->send_gameplay(destination, packet);
        };
        (void)send_frame();
        pump_online_inputs_locked();
        // Frame zero is a strict common starting line. Afterwards permit the
        // same four-frame lead as the input jitter buffer, stalling only when
        // this peer would run farther ahead. This preserves bounded lockstep
        // without paying one full relay trip for every game update.
        constexpr std::uint32_t simulation_lead = 4;
        const auto peer_ready = [&]() {
            if (!online_remote_simulation_frame_) return false;
            if (frame == 0) return *online_remote_simulation_frame_ >= frame;
            return frame <= *online_remote_simulation_frame_ + simulation_lead;
        };
        const auto frame_wait_started = std::chrono::steady_clock::now();
        while (!peer_ready()) {
            if (!online_connection_ || online_connection_->state() != btga::OnlineState::Lobby) {
                return false;
            }
            (void)send_frame();
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lock.lock();
            pump_online_inputs_locked();
            if (std::chrono::steady_clock::now() - frame_wait_started >=
                std::chrono::seconds(10)) {
                online_status_ = "Peer frame synchronization timed out; continuing offline.";
                if (online_trace_) {
                    online_trace_ << "FRAME_BARRIER_TIMEOUT frame=" << frame << '\n';
                    online_trace_.flush();
                }
                online_connection_->leave_online();
                return false;
            }
        }
        // Advertise frame progress before waiting for delayed controller data.
        // If an earlier frame event was lost, blocking on input first can form
        // a circular wait: the leading peer waits for input while the lagging
        // peer waits for the leading peer's next frame event.
        if (!prepare_frame_inputs_locked(lock, frame, local, destination)) return false;
        // The global clock also runs during logos and the front end. Those
        // screens intentionally poll controllers independently of gameplay;
        // feeding them a once-per-frame latch makes presses disappear (the
        // intro can be skipped, then the main menu appears unresponsive).
        // Keep poll-indexed lockstep until a world with human players exists,
        // and use the frame-tagged latch only for that active match. This also
        // returns intermission/front-end screens to responsive polling after
        // the world has been torn down.
        constexpr std::size_t world_offset = 0x00219498U;
        constexpr std::size_t human_player_count_offset = 0x002194A4U;
        constexpr std::size_t game_update_state_offset = 0x002195CCU;
        const std::uint32_t world = load_rdram_word(rdram_, world_offset);
        const std::uint8_t human_players =
            load_rdram_byte(rdram_, human_player_count_offset);
        const std::uint32_t game_update_state =
            load_rdram_word(rdram_, game_update_state_offset);
        if (frame_inputs_active &&
            (!online_frame_inputs_active_ || online_gameplay_resync_requested_)) {
            // A match is a new deterministic epoch. The host's complete
            // simulation snapshot is applied and acknowledged before either
            // peer may execute the first frame-tagged gameplay update.
            if (!synchronize_gameplay_epoch_locked(lock, frame, local, destination)) {
                return false;
            }
            // The host generation is the authoritative runtime tick as well
            // as the guest-state epoch. Discard every predicted/queued frame
            // from the old timeline and restart both peers at generation+1.
            // The first recovered update is neutral on both machines; normal
            // delayed frame input resumes from the freshly rebased history.
            online_simulation_frame_ = online_gameplay_snapshot_generation_ + 1U;
            online_remote_simulation_frame_.reset();
            online_frame_inputs_ = {};
            btga::prime_frame_input_epoch(
                online_frame_inputs_, online_gameplay_snapshot_generation_);
            online_frame_applied_inputs_.fill({});
            online_frame_applied_inputs_valid_ = true;
            online_local_digests_.fill({});
            online_remote_digests_.fill({});
            online_first_desync_frame_.reset();
            online_last_digest_frame_.reset();
            online_digest_mismatch_run_ = 0;
            online_gameplay_resync_requested_ = false;
        } else if (!frame_inputs_active && online_frame_inputs_active_) {
            online_gameplay_snapshot_acknowledged_ = false;
            online_gameplay_snapshot_received_mask_ = 0;
        }
        if ((frame_inputs_active != online_frame_inputs_active_ || frame % 60U == 0U) &&
            online_trace_) {
            online_trace_ << "INPUT_MODE frame=" << frame
                          << " mode=" << (frame_inputs_active ? "frame" : "poll")
                          << " world=" << world
                          << " humans=" << static_cast<unsigned>(human_players)
                          << " state=" << game_update_state
                          << '\n';
            online_trace_.flush();
        }
        online_frame_inputs_active_ = frame_inputs_active;
        if (online_trace_ && (frame % 60U) == 0U) {
            online_trace_ << "SIM_FRAME frame=" << frame
                          << " gameplay_checksum=" << checksum << '\n';
            online_trace_.flush();
        }
        return true;
    }

    bool prepare_frame_inputs_locked(std::unique_lock<std::recursive_mutex>& lock,
                                     std::uint32_t frame, std::uint16_t local,
                                     std::uint16_t destination) {
        constexpr std::uint32_t frame_input_tag = UINT32_C(0x80000000);
        constexpr std::uint32_t input_delay = 4U;
        constexpr std::uint32_t redundancy = 8U;
        const std::uint32_t publish_frame = frame + input_delay;
        const std::uint32_t publish_tick = frame_input_tag | publish_frame;
        const std::uint32_t sequence = online_sequence_++;
        online_frame_inputs_.record_local(
            static_cast<std::uint8_t>(local), publish_tick, sequence,
            online_latest_local_input_);

        const std::uint32_t first_frame =
            publish_frame >= redundancy - 1U
                ? std::max(input_delay, publish_frame - (redundancy - 1U))
                : input_delay;
        std::array<btga::OnlineControllerInput, redundancy> frames{};
        std::size_t frame_count = 0;
        for (std::uint32_t input_frame = first_frame;
             input_frame <= publish_frame; ++input_frame) {
            const auto input = online_frame_inputs_.input_for(
                static_cast<std::uint8_t>(local), frame_input_tag | input_frame);
            if (input) frames[frame_count++] = *input;
        }
        std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
        const std::size_t packet_size = online_frame_inputs_.encode_batch(
            packet, online_session_id_, local, sequence,
            frame_input_tag | first_frame, static_cast<std::uint8_t>(local),
            {frames.data(), frame_count});
        if (packet_size == 0) return false;
        const auto send = [&]() {
            return online_connection_->send_gameplay(
                destination, {packet.data(), packet_size});
        };
        (void)send();
        pump_online_inputs_locked();

        if (frame < input_delay) {
            online_frame_applied_inputs_.fill({});
            online_frame_applied_inputs_valid_ = true;
            return true;
        }
        const std::uint32_t apply_tick = frame_input_tag | frame;
        const auto wait_started = std::chrono::steady_clock::now();
        for (;;) {
            const auto local_input = online_frame_inputs_.input_for(
                static_cast<std::uint8_t>(local), apply_tick);
            const auto remote_input = online_frame_inputs_.input_for(
                static_cast<std::uint8_t>(destination), apply_tick);
            if (local_input && remote_input) {
                online_frame_applied_inputs_[local] = *local_input;
                online_frame_applied_inputs_[destination] = *remote_input;
                online_frame_applied_inputs_valid_ = true;
                if (online_trace_ && (frame % 60U) == 0U) {
                    online_trace_ << "FRAME_INPUT frame=" << frame
                                  << " p0=" << online_frame_applied_inputs_[0].buttons
                                  << ',' << static_cast<int>(online_frame_applied_inputs_[0].stick_x)
                                  << ',' << static_cast<int>(online_frame_applied_inputs_[0].stick_y)
                                  << " p1=" << online_frame_applied_inputs_[1].buttons
                                  << ',' << static_cast<int>(online_frame_applied_inputs_[1].stick_x)
                                  << ',' << static_cast<int>(online_frame_applied_inputs_[1].stick_y)
                                  << '\n';
                    online_trace_.flush();
                }
                return true;
            }
            if (!online_connection_ || online_connection_->state() != btga::OnlineState::Lobby) {
                return false;
            }
            if (std::chrono::steady_clock::now() - wait_started >=
                std::chrono::milliseconds(250)) {
                online_status_ = "Waiting for frame-tagged multiplayer input...";
            }
            if (std::chrono::steady_clock::now() - wait_started >=
                std::chrono::seconds(10)) {
                online_status_ = "Frame input synchronization timed out; continuing offline.";
                if (online_trace_) {
                    online_trace_ << "FRAME_INPUT_TIMEOUT frame=" << frame << '\n';
                    online_trace_.flush();
                }
                online_connection_->leave_online();
                return false;
            }
            (void)send();
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lock.lock();
            pump_online_inputs_locked();
        }
    }

private:
    enum class OnlineTaskKind { Browse, Join, Heartbeat };
    struct OnlineTaskResult {
        OnlineTaskKind kind = OnlineTaskKind::Browse;
        std::vector<btga::OnlineLobby> lobbies;
        std::optional<btga::JoinRegistration> join;
        int error_status = 0;
        std::string error;
    };

    bool ensure_online_master() {
        try {
            if (!online_http_) {
                online_http_ = btga::make_platform_http_client(
                    btga::master_service_url());
                online_master_ = std::make_unique<btga::MasterClient>(*online_http_);
            }
            return true;
        } catch (const std::exception& error) {
            online_status_ = error.what();
            return false;
        }
    }

    template <typename Operation>
    void start_online_task(OnlineTaskKind kind, Operation operation) {
        if (online_task_.valid() || !ensure_online_master()) return;
        if (kind != OnlineTaskKind::Heartbeat) {
            online_status_ = "Contacting multiplayer service...";
        }
        online_task_ = std::async(std::launch::async,
            [kind, operation = std::move(operation)]() mutable {
                OnlineTaskResult result;
                result.kind = kind;
                try { operation(result); }
                catch (const btga::MasterServiceError& error) {
                    result.error_status = error.status();
                    result.error = error.what();
                }
                catch (const std::exception& error) { result.error = error.what(); }
                return result;
            });
    }

    void browse_online() {
        btga::MasterClient* master = online_master_.get();
        if (!master && !ensure_online_master()) return;
        master = online_master_.get();
        start_online_task(OnlineTaskKind::Browse, [master](OnlineTaskResult& result) {
            result.lobbies = master->list_lobbies();
        });
    }

    void join_online(std::string lobby_id) {
        if (!ensure_online_master()) return;
        btga::MasterClient* master = online_master_.get();
        online_pending_lobby_ = lobby_id;
        std::string resume_token;
        std::filesystem::path resume_path;
        try {
            resume_path = btga::dedicated_resume_path(lobby_id);
            resume_token = btga::load_dedicated_resume_token(
                resume_path);
        } catch (const std::exception& error) {
            online_status_ = error.what();
            return;
        }
        start_online_task(OnlineTaskKind::Join,
            [master, lobby_id = std::move(lobby_id),
             resume_token = std::move(resume_token),
             resume_path = std::move(resume_path)](OnlineTaskResult& result) {
                if (resume_token.empty()) {
                    result.join = master->join(lobby_id);
                    return;
                }
                try {
                    result.join = master->join(lobby_id, resume_token);
                } catch (const btga::MasterServiceError& error) {
                    // A rotated/expired credential must not permanently brick
                    // this server entry. Retire it and make one ordinary join;
                    // never loop on a credential the master rejected.
                    if (error.status() != 401 && error.status() != 403 &&
                        error.status() != 409) throw;
                    std::error_code remove_error;
                    std::filesystem::remove(resume_path, remove_error);
                    result.join = master->join(lobby_id);
                }
            });
    }

    bool restart_for_dedicated_join(
        const btga::JoinRegistration& registration) {
#if defined(_WIN32)
        if (pc_settings.last_rom_path.empty()) {
            online_status_ = "Select and validate a local ROM before joining a server.";
            return false;
        }
        wchar_t executable[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            online_status_ = "Could not locate the running executable.";
            return false;
        }
        const std::string session_text = std::to_string(registration.session_id);
        const std::string port_text = std::to_string(registration.relay.port);
        const auto set = [](const char* name, const std::string& value) {
            return SetEnvironmentVariableA(name, value.c_str()) != FALSE;
        };
        const auto clear = [](const char* name) {
            SetEnvironmentVariableA(name, nullptr);
        };
        const std::array<const char*, 5> names{
            "BTGA_DEDICATED_SESSION", "BTGA_DEDICATED_RELAY_HOST",
            "BTGA_DEDICATED_RELAY_PORT", "BTGA_DEDICATED_TOKEN",
            "BTGA_DEDICATED_LOBBY"};
        const bool environment_ready =
            set(names[0], session_text) &&
            set(names[1], registration.relay.address) &&
            set(names[2], port_text) && set(names[3], registration.join_token) &&
            set(names[4], online_pending_lobby_);
        if (!environment_ready) {
            for (const char* name : names) clear(name);
            online_status_ = "Could not prepare dedicated client credentials.";
            return false;
        }
        const std::filesystem::path rom =
            std::filesystem::u8path(pc_settings.last_rom_path);
        std::wstring command = L"\"";
        command += executable;
        command += L"\" --dedicated-client-direct \"";
        command += rom.wstring();
        command += L"\"";
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(
            executable, mutable_command.data(), nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &startup, &process);
        for (const char* name : names) clear(name);
        if (!created) {
            online_status_ = "Could not start the authoritative client.";
            return false;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        online_status_ = "Restarting into the selected dedicated server...";
        ultramodern::quit();
        return true;
#else
        (void)registration;
        online_status_ =
            "In-app dedicated-server restart is not implemented on this platform yet.";
        return false;
#endif
    }

    bool restart_for_online_preboot(bool host, std::uint64_t session,
                                    const btga::RelayEndpoint& relay,
                                    std::string_view token,
                                    std::string_view lobby_id,
                                    std::string_view host_secret) {
#if defined(_WIN32)
        if (pc_settings.last_rom_path.empty()) {
            online_status_ = "Select and validate a local ROM before starting online play.";
            return false;
        }
        wchar_t executable[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            online_status_ = "Could not locate the running executable for synchronized restart.";
            return false;
        }
        const std::string session_text = std::to_string(session);
        const std::string port_text = std::to_string(relay.port);
        const auto set = [](const char* name, const std::string& value) {
            return SetEnvironmentVariableA(name, value.c_str()) != FALSE;
        };
        const auto clear = [](const char* name) { SetEnvironmentVariableA(name, nullptr); };
        const std::array<const char*, 8> names{
            "BTGA_ONLINE_ROLE", "BTGA_ONLINE_SESSION", "BTGA_ONLINE_RELAY_HOST",
            "BTGA_ONLINE_RELAY_PORT", "BTGA_ONLINE_TOKEN", "BTGA_ONLINE_LOBBY",
            "BTGA_ONLINE_HOST_SECRET", "BTGA_ONLINE_LOCAL_VIEW"};
        const bool environment_ready =
            set(names[0], host ? "host" : "client") &&
            set(names[1], session_text) && set(names[2], relay.address) &&
            set(names[3], port_text) && set(names[4], std::string{token}) &&
            set(names[5], std::string{lobby_id}) &&
            set(names[6], std::string{host_secret}) &&
            set(names[7], host ? "host" : "client");
        if (!environment_ready) {
            for (const char* name : names) clear(name);
            online_status_ = "Could not prepare the synchronized restart environment.";
            return false;
        }
        const std::filesystem::path rom =
            std::filesystem::u8path(pc_settings.last_rom_path);
        std::wstring command = L"\"";
        command += executable;
        command += L"\" --online-resume \"";
        command += rom.wstring();
        command += L"\"";
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(
            executable, mutable_command.data(), nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &startup, &process);
        for (const char* name : names) clear(name);
        if (!created) {
            online_status_ = "Could not start the synchronized online process.";
            return false;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        // Ownership of lobby cleanup has moved to the replacement process.
        // The old Context destructor must not remove the lobby during exit.
        online_restart_handoff_ = true;
        online_status_ = "Restarting into a synchronized online session...";
        ultramodern::quit();
        return true;
#else
        (void)host; (void)session; (void)relay; (void)token;
        (void)lobby_id; (void)host_secret;
        online_status_ = "In-app synchronized restart is not implemented on this platform yet.";
        return false;
#endif
    }

    bool begin_online_relay(bool host, std::uint64_t session,
                            const btga::RelayEndpoint& relay,
                            std::string_view token) {
        online_udp_ = std::make_unique<btga::UdpOnlineIo>(relay.address, relay.port);
        online_connection_ = std::make_unique<btga::OnlineMultiplayer>(*online_udp_);
        const auto bytes = std::span{
            reinterpret_cast<const std::uint8_t*>(token.data()), token.size()};
        if (!online_connection_->enter_online() ||
            !online_connection_->begin_relay(host, session, bytes)) {
            online_connection_.reset();
            online_udp_.reset();
            online_status_ = "Could not begin relay binding.";
            return false;
        }
        online_status_ = "Authenticating with the gameplay relay...";
        online_session_id_ = session;
        online_sequence_ = 1;
        online_tick_ = 0;
        online_inputs_ = {};
        online_frame_inputs_ = {};
        online_latest_local_input_ = {};
        online_frame_applied_inputs_.fill({});
        online_frame_inputs_active_ = false;
        online_frame_applied_inputs_valid_ = false;
        online_peer_seen_.fill(false);
        online_tick_origins_valid_ = false;
        online_state_synchronized_ = false;
        online_state_sent_ = false;
        online_state_seed_ = 0;
        online_state_frame_ = 0;
        online_state_generation_ = 0;
        online_applied_inputs_valid_ = false;
        online_local_tick_origin_ = 0;
        online_remote_tick_origin_ = 0;
        online_last_traced_tick_.reset();
        online_wait_traced_ = false;
        online_boot_bridge_traced_ = false;
        online_simulation_frame_ = 0;
        online_remote_simulation_frame_.reset();
        online_local_digests_.fill({});
        online_remote_digests_.fill({});
        online_first_desync_frame_.reset();
        online_last_digest_frame_.reset();
        online_digest_mismatch_run_ = 0;
        online_gameplay_snapshot_.fill(0);
        online_gameplay_snapshot_generation_ = 0;
        online_gameplay_snapshot_checksum_ = 0;
        online_gameplay_snapshot_received_mask_ = 0;
        online_gameplay_snapshot_acknowledged_ = false;
        online_gameplay_applied_generation_ = 0;
        online_gameplay_snapshot_applied_ = false;
        online_gameplay_resync_requested_ = false;
        online_snapshot_assembler_.reset();
        online_trace_.close();
        online_trace_.open(pc_data_path / "btga-net-sync.log",
                           std::ios::out | std::ios::trunc);
        if (online_trace_) {
            online_trace_ << "BTGA net synchronization trace v1\n";
            online_trace_.flush();
        }
        return true;
    }

    void pump_online_inputs_locked() {
        if (!online_connection_) return;
        for (;;) {
            std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
            std::uint16_t source = 0;
            const std::size_t size = online_connection_->receive_gameplay(packet, source);
            if (size == 0) break;
            BtgaNetHeader header{};
            if (btga_net_decode_header(&header, packet.data(), size) &&
                header.protocol_version == BTGA_NET_PROTOCOL_VERSION &&
                header.message_type == BTGA_NET_MSG_EVENT &&
                header.session_id == online_session_id_ && header.peer_id == source &&
                header.payload_size == BTGA_NET_FRAME_SYNC_SIZE &&
                size == BTGA_NET_HEADER_SIZE + BTGA_NET_FRAME_SYNC_SIZE) {
                const std::uint8_t* payload = packet.data() + BTGA_NET_HEADER_SIZE;
                BtgaNetFrameSync sync{};
                if (btga_net_decode_frame_sync(&sync, payload, header.payload_size)) {
                    if (sync.state_checksum != 0U) {
                        btga::GameplayStateDigests remote_digests{};
                        remote_digests.combined = sync.state_checksum;
                        record_online_digest(online_remote_digests_, sync.frame,
                                             remote_digests);
                        compare_online_digest(sync.frame);
                    }
                    if (!online_remote_simulation_frame_ ||
                        sync.frame > *online_remote_simulation_frame_) {
                        online_remote_simulation_frame_ = sync.frame;
                    }
                }
                continue;
            }
            if (btga_net_decode_header(&header, packet.data(), size) &&
                header.protocol_version == BTGA_NET_PROTOCOL_VERSION &&
                header.message_type == BTGA_NET_MSG_SNAPSHOT &&
                header.session_id == online_session_id_ && header.peer_id == source &&
                header.payload_size >= 1U &&
                size == BTGA_NET_HEADER_SIZE + header.payload_size) {
                const std::uint8_t* payload = packet.data() + BTGA_NET_HEADER_SIZE;
                const std::uint16_t local = online_connection_->relay_slot();
                if (payload[0] == gameplay_snapshot_version && local == 1 && source == 0 &&
                    header.payload_size >= gameplay_snapshot_header_size) {
                    btga::SnapshotChunk chunk{};
                    const std::span<const std::uint8_t> encoded{
                        payload + 1, header.payload_size - 1U};
                    const bool decoded = btga::decode_snapshot_chunk(chunk, encoded);
                    if (decoded) {
                        // A completed snapshot is retransmitted until its ACK
                        // reaches the host. Those duplicate chunks belong to
                        // the epoch we already applied and must not open a new
                        // synchronization barrier. Only a strictly newer host
                        // generation requests a corrective rebase.
                        if (online_gameplay_snapshot_applied_ &&
                            btga::snapshot_generation_is_newer(
                                chunk.generation,
                                online_gameplay_applied_generation_)) {
                            online_gameplay_resync_requested_ = true;
                        }
                        if (online_gameplay_snapshot_acknowledged_ &&
                            chunk.generation != online_gameplay_snapshot_generation_) {
                            online_snapshot_assembler_.reset();
                            online_gameplay_snapshot_received_mask_ = 0;
                            online_gameplay_snapshot_acknowledged_ = false;
                        }
                    }
                    if (decoded && online_snapshot_assembler_.accept(chunk)) {
                        online_gameplay_snapshot_generation_ =
                            online_snapshot_assembler_.generation();
                        online_gameplay_snapshot_checksum_ =
                            online_snapshot_assembler_.checksum();
                        online_gameplay_snapshot_received_mask_ |= UINT64_C(1) << chunk.index;
                        if (online_snapshot_assembler_.verified()) {
                            std::memcpy(online_gameplay_snapshot_.data(),
                                        online_snapshot_assembler_.bytes().data(),
                                        gameplay_snapshot_size);
                            if (online_gameplay_snapshot_acknowledged_) {
                                // ACKs are datagrams too. Re-ACK a retransmitted
                                // completed snapshot so an isolated lost ACK can
                                // never strand the host at the epoch barrier.
                                send_gameplay_snapshot_ack_locked(0);
                            }
                        }
                    }
                    continue;
                }
                if (payload[0] == gameplay_snapshot_ack_version && local == 0 && source == 1 &&
                    header.payload_size == 9U &&
                    get_u32_be(payload + 1) == online_gameplay_snapshot_generation_ &&
                    get_u32_be(payload + 5) == online_gameplay_snapshot_checksum_) {
                    online_gameplay_snapshot_acknowledged_ = true;
                    continue;
                }
            }
            if (btga_net_decode_header(&header, packet.data(), size) &&
                header.protocol_version == BTGA_NET_PROTOCOL_VERSION &&
                header.message_type == BTGA_NET_MSG_SNAPSHOT &&
                header.session_id == online_session_id_ && header.peer_id == source &&
                header.payload_size == online_state_packet_size &&
                size == BTGA_NET_HEADER_SIZE + online_state_packet_size) {
                const std::uint8_t* payload = packet.data() + BTGA_NET_HEADER_SIZE;
                const std::uint8_t version = payload[0];
                const std::uint32_t seed = get_u32_be(payload + 1);
                const std::uint32_t frame = get_u32_be(payload + 5);
                const std::uint32_t generation = get_u32_be(payload + 9);
                const std::uint32_t checksum = get_u32_be(payload + 13);
                if (checksum != online_state_checksum(version, seed, frame, generation)) {
                    continue;
                }
                const std::uint16_t local = online_connection_->relay_slot();
                if (local == 1 && source == 0 && version == online_state_version) {
                    store_rdram_word(rdram_, random_seed_offset, seed);
                    store_rdram_word(rdram_, frame_counter_offset, frame);
                    online_state_seed_ = seed;
                    online_state_frame_ = frame;
                    online_state_generation_ = generation;
                    online_state_synchronized_ = true;
                    send_online_state_locked(0, online_state_ack_version);
                    if (online_trace_) {
                        online_trace_ << "STATE_RESTORED seed=" << seed
                                      << " frame=" << frame
                                      << " generation=" << generation << '\n';
                        online_trace_.flush();
                    }
                } else if (local == 0 && source == 1 &&
                           version == online_state_ack_version && online_state_sent_ &&
                           seed == online_state_seed_ && frame == online_state_frame_ &&
                           generation == online_state_generation_) {
                    online_state_synchronized_ = true;
                    online_status_ = "Authoritative state synchronized.";
                    if (online_trace_) {
                        online_trace_ << "STATE_ACK seed=" << seed
                                      << " frame=" << frame
                                      << " generation=" << generation << '\n';
                        online_trace_.flush();
                    }
                }
                continue;
            }
            bool frame_input = false;
            if (btga_net_decode_header(&header, packet.data(), size) &&
                header.message_type == BTGA_NET_MSG_INPUT &&
                header.payload_size == size - BTGA_NET_HEADER_SIZE) {
                BtgaNetInputBatch batch{};
                if (btga_net_decode_input_batch(
                        &batch, packet.data() + BTGA_NET_HEADER_SIZE,
                        header.payload_size)) {
                    frame_input = (batch.first_tick & UINT32_C(0x80000000)) != 0U;
                }
            }
            btga::InputSynchronizer& synchronizer =
                frame_input ? online_frame_inputs_ : online_inputs_;
            const bool accepted = synchronizer.accept(
                {packet.data(), size}, online_session_id_, source);
            if (accepted && source < online_peer_seen_.size()) {
                online_peer_seen_[source] = true;
                const std::uint16_t local = online_connection_->relay_slot();
                if (!frame_input && local == 1 && source == 0 && online_state_synchronized_ &&
                    !online_tick_origins_valid_) {
                    const auto origin = online_inputs_.sync_origin_for(0);
                    if (origin) {
                        // Host announces (host tick, client tick). Reverse the
                        // pair locally so both machines address identical input.
                        online_local_tick_origin_ = origin->receiver_tick;
                        online_remote_tick_origin_ = origin->sender_tick;
                        online_tick_origins_valid_ = true;
                        trace_online_origin_locked(local);
                    }
                }
            }
        }
    }

    void capture_gameplay_snapshot() {
        (void)btga::capture_gameplay_snapshot(
            {rdram_, 8U * 1024U * 1024U}, online_gameplay_snapshot_);
        online_gameplay_snapshot_checksum_ =
            btga::snapshot_fnv1a(online_gameplay_snapshot_);
    }

    bool apply_gameplay_snapshot() {
        if (btga::snapshot_fnv1a(online_gameplay_snapshot_) !=
            online_gameplay_snapshot_checksum_) return false;
        return btga::restore_gameplay_snapshot(
            {rdram_, 8U * 1024U * 1024U}, online_gameplay_snapshot_);
    }

    void send_gameplay_snapshot_chunks_locked(std::uint16_t destination) {
        if (!online_connection_) return;
        for (std::size_t chunk = 0; chunk < gameplay_snapshot_chunk_count; ++chunk) {
            const std::size_t offset = chunk * gameplay_snapshot_chunk_size;
            const std::size_t data_size = std::min(
                gameplay_snapshot_chunk_size, gameplay_snapshot_size - offset);
            const std::size_t payload_size = gameplay_snapshot_header_size + data_size;
            std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
            BtgaNetHeader header{};
            header.protocol_version = BTGA_NET_PROTOCOL_VERSION;
            header.message_type = BTGA_NET_MSG_SNAPSHOT;
            header.flags = static_cast<std::uint8_t>(BTGA_NET_FLAG_RELIABLE |
                                                     BTGA_NET_FLAG_RELAYED);
            header.session_id = online_session_id_;
            header.sequence = online_sequence_++;
            header.payload_size = static_cast<std::uint16_t>(payload_size);
            header.peer_id = online_connection_->relay_slot();
            if (btga_net_encode_header(packet.data(), packet.size(), &header) !=
                BTGA_NET_HEADER_SIZE) return;
            std::uint8_t* payload = packet.data() + BTGA_NET_HEADER_SIZE;
            payload[0] = gameplay_snapshot_version;
            const btga::SnapshotChunk descriptor{
                online_gameplay_snapshot_generation_,
                static_cast<std::uint16_t>(chunk),
                static_cast<std::uint16_t>(gameplay_snapshot_chunk_count),
                static_cast<std::uint16_t>(data_size),
                online_gameplay_snapshot_checksum_,
                {online_gameplay_snapshot_.data() + offset, data_size}};
            if (!btga::encode_snapshot_chunk(
                    {payload + 1, payload_size - 1U}, descriptor)) return;
            (void)online_connection_->send_gameplay(
                destination, {packet.data(), BTGA_NET_HEADER_SIZE + payload_size});
        }
    }

    void send_gameplay_snapshot_ack_locked(std::uint16_t destination) {
        if (!online_connection_) return;
        std::array<std::uint8_t, BTGA_NET_HEADER_SIZE + 9U> packet{};
        BtgaNetHeader header{};
        header.protocol_version = BTGA_NET_PROTOCOL_VERSION;
        header.message_type = BTGA_NET_MSG_SNAPSHOT;
        header.flags = static_cast<std::uint8_t>(BTGA_NET_FLAG_RELIABLE |
                                                 BTGA_NET_FLAG_RELAYED);
        header.session_id = online_session_id_;
        header.sequence = online_sequence_++;
        header.payload_size = 9;
        header.peer_id = online_connection_->relay_slot();
        if (btga_net_encode_header(packet.data(), packet.size(), &header) !=
            BTGA_NET_HEADER_SIZE) return;
        std::uint8_t* payload = packet.data() + BTGA_NET_HEADER_SIZE;
        payload[0] = gameplay_snapshot_ack_version;
        put_u32_be(payload + 1, online_gameplay_snapshot_generation_);
        put_u32_be(payload + 5, online_gameplay_snapshot_checksum_);
        (void)online_connection_->send_gameplay(destination, packet);
    }

    bool synchronize_gameplay_epoch_locked(std::unique_lock<std::recursive_mutex>& lock,
                                           std::uint32_t frame,
                                           std::uint16_t local,
                                           std::uint16_t destination) {
        online_gameplay_snapshot_acknowledged_ = false;
        if (local == 0) {
            online_gameplay_snapshot_generation_ = frame;
            capture_gameplay_snapshot();
            send_gameplay_snapshot_chunks_locked(destination);
        } else if (online_gameplay_snapshot_received_mask_ == 0) {
            online_gameplay_snapshot_generation_ = 0;
            online_gameplay_snapshot_checksum_ = 0;
            online_snapshot_assembler_.reset();
        }
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            pump_online_inputs_locked();
            if (local == 0 && online_gameplay_snapshot_acknowledged_) break;
            if (local == 1 && online_snapshot_assembler_.verified() &&
                (!online_gameplay_snapshot_applied_ ||
                 btga::snapshot_generation_is_newer(
                     online_snapshot_assembler_.generation(),
                     online_gameplay_applied_generation_))) {
                if (!apply_gameplay_snapshot()) return false;
                send_gameplay_snapshot_ack_locked(destination);
                online_gameplay_snapshot_acknowledged_ = true;
                online_gameplay_applied_generation_ =
                    online_gameplay_snapshot_generation_;
                online_gameplay_snapshot_applied_ = true;
                break;
            }
            if (!online_connection_ || online_connection_->state() != btga::OnlineState::Lobby)
                return false;
            if (local == 0) send_gameplay_snapshot_chunks_locked(destination);
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            lock.lock();
            if (std::chrono::steady_clock::now() - started > std::chrono::seconds(10)) {
                online_status_ = "Gameplay state synchronization timed out; continuing offline.";
                if (online_trace_) {
                    online_trace_ << "GAMEPLAY_EPOCH_TIMEOUT frame=" << frame << '\n';
                    online_trace_.flush();
                }
                online_connection_->leave_online();
                return false;
            }
        }
        if (online_trace_) {
            online_trace_ << "GAMEPLAY_EPOCH_SYNC frame=" << frame
                          << " role=" << (local == 0 ? "host" : "client")
                          << " checksum=" << online_gameplay_snapshot_checksum_ << '\n';
            online_trace_.flush();
        }
        if (local == 0) {
            online_gameplay_applied_generation_ =
                online_gameplay_snapshot_generation_;
            online_gameplay_snapshot_applied_ = true;
        }
        return true;
    }

    void send_online_state_locked(std::uint16_t destination, std::uint8_t version) {
        if (!online_connection_) return;
        std::array<std::uint8_t, BTGA_NET_HEADER_SIZE + online_state_packet_size> packet{};
        BtgaNetHeader header{};
        header.protocol_version = BTGA_NET_PROTOCOL_VERSION;
        header.message_type = BTGA_NET_MSG_SNAPSHOT;
        header.flags = static_cast<std::uint8_t>(BTGA_NET_FLAG_RELIABLE |
                                                 BTGA_NET_FLAG_RELAYED);
        header.session_id = online_session_id_;
        header.sequence = online_sequence_++;
        header.payload_size = static_cast<std::uint16_t>(online_state_packet_size);
        header.peer_id = online_connection_->relay_slot();
        if (btga_net_encode_header(packet.data(), packet.size(), &header) !=
            BTGA_NET_HEADER_SIZE) return;
        std::uint8_t* payload = packet.data() + BTGA_NET_HEADER_SIZE;
        payload[0] = version;
        put_u32_be(payload + 1, online_state_seed_);
        put_u32_be(payload + 5, online_state_frame_);
        put_u32_be(payload + 9, online_state_generation_);
        put_u32_be(payload + 13, online_state_checksum(
            version, online_state_seed_, online_state_frame_, online_state_generation_));
        (void)online_connection_->send_gameplay(destination, packet);
        if (version == online_state_version && online_trace_) {
            online_trace_ << "STATE_SEND seed=" << online_state_seed_
                          << " frame=" << online_state_frame_
                          << " generation=" << online_state_generation_ << '\n';
            online_trace_.flush();
        }
    }

    bool prepare_applied_inputs_locked() {
        if (!online_connection_ || !online_tick_origins_valid_ || online_tick_ == 0) return false;
        constexpr std::uint32_t input_delay = 4;
        const std::uint16_t local = online_connection_->relay_slot();
        if (local > 1) return false;
        const std::uint16_t remote = local == 0 ? 1 : 0;
        const std::uint32_t latest_local_tick = online_tick_ - 1;
        const std::uint32_t relative_tick = latest_local_tick - online_local_tick_origin_;
        if (relative_tick < input_delay) {
            online_applied_inputs_.fill({});
            online_applied_inputs_valid_ = true;
            return true;
        }
        const std::uint32_t delayed = relative_tick - input_delay;
        const auto local_input = online_inputs_.input_for(
            static_cast<std::uint8_t>(local), online_local_tick_origin_ + delayed);
        auto remote_input = online_inputs_.input_for(
            static_cast<std::uint8_t>(remote), online_remote_tick_origin_ + delayed);
        // The simulation clock and controller manager are separate emulated
        // threads. One peer can reach the first simulation barrier while the
        // other still needs a final boot/logo controller poll. Once frame 0
        // from that peer is confirmed, it cannot produce another poll-indexed
        // sample; repeat only its last confirmed sample to bridge the lagging
        // peer to the same barrier. Gameplay uses frame-tagged inputs after it.
        if (!remote_input && online_remote_simulation_frame_ &&
            *online_remote_simulation_frame_ == 0U) {
            remote_input = online_inputs_.latest_input_for(
                static_cast<std::uint8_t>(remote));
            if (remote_input && !online_boot_bridge_traced_ && online_trace_) {
                online_trace_ << "BOOT_INPUT_BRIDGE relative=" << relative_tick
                              << " remote_tick="
                              << online_remote_tick_origin_ + delayed << '\n';
                online_trace_.flush();
                online_boot_bridge_traced_ = true;
            }
        }
        if (!local_input || !remote_input) return false;
        online_applied_inputs_[local] = *local_input;
        online_applied_inputs_[remote] = *remote_input;
        online_applied_inputs_valid_ = true;
        trace_online_apply_locked(relative_tick, online_local_tick_origin_ + delayed,
                                  online_remote_tick_origin_ + delayed);
        return true;
    }

    struct OnlineFrameDigest {
        std::uint32_t frame = 0;
        btga::GameplayStateDigests digests{};
        bool valid = false;
    };

    btga::GameplayStateDigests online_gameplay_digests() const {
        // Hash simulation-owned guest state only. Do not include RDRAM wholesale:
        // libultra queues, thread stacks, display lists, camera/render state and
        // audio buffers legitimately differ between hosts and future per-device
        // camera modes. These ranges cover the 24 object slots, deterministic
        // player fields and team scores, RNG/time, game mode and match time.
        return btga::gameplay_state_digests({rdram_, 8U * 1024U * 1024U});
    }

    static void record_online_digest(
        std::array<OnlineFrameDigest, online_digest_history_size>& history,
        std::uint32_t frame, btga::GameplayStateDigests digests) {
        history[frame % history.size()] = {frame, digests, true};
    }

    void compare_online_digest(std::uint32_t frame) {
        const OnlineFrameDigest& local =
            online_local_digests_[frame % online_local_digests_.size()];
        const OnlineFrameDigest& remote =
            online_remote_digests_[frame % online_remote_digests_.size()];
        if (!local.valid || !remote.valid || local.frame != frame || remote.frame != frame) return;
        if (online_last_digest_frame_ && frame <= *online_last_digest_frame_) return;
        const bool consecutive = online_last_digest_frame_ &&
                                 frame == *online_last_digest_frame_ + 1U;
        online_last_digest_frame_ = frame;
        if (local.digests.combined == remote.digests.combined) {
            online_digest_mismatch_run_ = 0;
            return;
        }
        online_digest_mismatch_run_ = consecutive ? online_digest_mismatch_run_ + 1U : 1U;
        if (online_trace_) {
            const auto& d = local.digests;
            online_trace_ << "DIGEST_MISMATCH frame=" << frame
                          << " run=" << online_digest_mismatch_run_
                          << " local=" << d.combined
                          << " remote=" << remote.digests.combined
                          << " objects=" << d.objects
                          << " object_core=" << d.object_core
                          << " object_tail=" << d.object_tail
                          << " player_headers=" << d.player_headers
                          << " player_gameplay=" << d.player_gameplay
                          << " team_info=" << d.team_info
                          << " seed=" << d.random_seed
                          << " game_frame=" << d.frame_counter
                          << " mode=" << d.game_mode
                          << " match_time=" << d.match_time << '\n';
            if (online_digest_mismatch_run_ == 1U) {
                online_trace_ << "OBJECT_SLOTS frame=" << frame;
                for (std::size_t index = 0; index < d.object_slots.size(); ++index) {
                    online_trace_ << (index == 0 ? " " : ",")
                                  << d.object_slots[index];
                }
                online_trace_ << '\n';
            }
            online_trace_.flush();
        }
        if (online_digest_mismatch_run_ < 3U || online_first_desync_frame_) return;
        online_first_desync_frame_ = frame - 2U;
        online_status_ = "Persistent state divergence detected from simulation frame " +
                         std::to_string(*online_first_desync_frame_) +
                         "; online play stopped.";
        if (online_trace_) {
            online_trace_ << "DESYNC_ABORT frame=" << *online_first_desync_frame_ << '\n';
            online_trace_.flush();
        }
        // The selected-state snapshot is sufficient to establish a common
        // match epoch, but not a complete rollback save state: pointer-owned
        // world allocations and other simulation-driving state are outside it.
        // Stop cleanly instead of pretending a partial repair succeeded.
        if (online_connection_) online_connection_->leave_online();
    }

    void trace_online_origin_locked(std::uint16_t local) {
        if (!online_trace_) return;
        online_trace_ << "ORIGIN slot=" << local
                      << " local=" << online_local_tick_origin_
                      << " remote=" << online_remote_tick_origin_ << '\n';
        online_trace_.flush();
    }

    std::uint64_t rdram_hash() const {
        std::uint64_t hash = UINT64_C(1469598103934665603);
        for (std::size_t index = 0; index < 8U * 1024U * 1024U; ++index) {
            hash ^= rdram_[index];
            hash *= UINT64_C(1099511628211);
        }
        return hash;
    }

    void trace_online_apply_locked(std::uint32_t relative_tick,
                                   std::uint32_t local_tick,
                                   std::uint32_t remote_tick) {
        if (!online_trace_ || online_last_traced_tick_ == relative_tick) return;
        online_last_traced_tick_ = relative_tick;
        const auto& p0 = online_applied_inputs_[0];
        const auto& p1 = online_applied_inputs_[1];
        online_trace_ << "APPLY rel=" << relative_tick
                      << " local_tick=" << local_tick
                      << " remote_tick=" << remote_tick
                      << " p0=" << p0.buttons << ',' << static_cast<int>(p0.stick_x)
                      << ',' << static_cast<int>(p0.stick_y)
                      << " p1=" << p1.buttons << ',' << static_cast<int>(p1.stick_x)
                      << ',' << static_cast<int>(p1.stick_y);
        if ((relative_tick % 60U) == 0U) {
            online_trace_ << " rdram_fnv64=" << rdram_hash()
                          << " seed=" << load_rdram_word(rdram_, random_seed_offset)
                          << " frame=" << load_rdram_word(rdram_, frame_counter_offset);
        }
        online_trace_ << '\n';
        online_trace_.flush();
    }

    void disconnect_online() {
        const std::lock_guard lock(online_mutex_);
        if (online_connection_) online_connection_->leave_online();
        online_connection_.reset();
        online_udp_.reset();
        if (online_master_ && !online_lobby_id_.empty() && !online_host_secret_.empty() &&
            !online_task_.valid()) {
            const std::string lobby = online_lobby_id_;
            const std::string secret = online_host_secret_;
            btga::MasterClient* master = online_master_.get();
            start_online_task(OnlineTaskKind::Heartbeat,
                [master, lobby, secret](OnlineTaskResult&) { master->remove(lobby, secret); });
        }
        online_lobby_id_.clear();
        online_host_secret_.clear();
        online_is_host_ = false;
        online_status_ = "Offline. No multiplayer sockets are open.";
    }

    void update_online() {
        const std::lock_guard lock(online_mutex_);
        adopt_preboot_session();
        if (online_task_.valid() &&
            online_task_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            OnlineTaskResult result = online_task_.get();
            if (!result.error.empty()) {
                if (result.kind == OnlineTaskKind::Join && result.error_status == 404) {
                    const std::string stale_lobby = std::exchange(online_pending_lobby_, {});
                    std::erase_if(online_lobbies_, [&stale_lobby](const btga::OnlineLobby& lobby) {
                        return lobby.id == stale_lobby;
                    });
                    online_status_ = "That lobby expired. Refreshing the public lobby list...";
                    browse_online();
                } else {
                    online_status_ = result.error;
                }
            } else if (result.kind == OnlineTaskKind::Browse) {
                online_lobbies_ = std::move(result.lobbies);
                std::erase_if(online_lobbies_, [](const btga::OnlineLobby& lobby) {
                    return (lobby.capabilities &
                            BTGA_NET_CAP_DEDICATED_AUTHORITY) == 0U;
                });
                online_status_ = online_lobbies_.empty()
                    ? "No compatible dedicated servers are currently open."
                    : "Dedicated server list refreshed.";
            } else if (result.join) {
                online_lobby_id_ = online_pending_lobby_;
                online_is_host_ = false;
                try {
                    btga::save_dedicated_resume_token(
                        btga::dedicated_resume_path(online_pending_lobby_),
                        result.join->resume_token);
                    (void)restart_for_dedicated_join(*result.join);
                } catch (const std::exception& error) {
                    online_status_ = error.what();
                }
            } else {
                online_next_heartbeat_ = std::chrono::steady_clock::now() +
                    std::chrono::seconds(30);
            }
        }

        if (online_connection_ &&
            online_connection_->state() == btga::OnlineState::RelayBinding &&
            online_connection_->poll_relay_binding()) {
            online_status_ = online_is_host_
                ? "Lobby online. Waiting for players."
                : "Joined relay lobby. Waiting for synchronized match start.";
            online_next_heartbeat_ = std::chrono::steady_clock::now() +
                std::chrono::seconds(30);
        }
        if (online_is_host_ && online_connection_ &&
            online_connection_->state() == btga::OnlineState::Lobby &&
            !online_task_.valid() && std::chrono::steady_clock::now() >= online_next_heartbeat_ &&
            ensure_online_master()) {
            btga::MasterClient* master = online_master_.get();
            const std::string lobby = online_lobby_id_;
            const std::string secret = online_host_secret_;
            const std::uint16_t players = static_cast<std::uint16_t>(
                1 + std::count(online_peer_seen_.begin(), online_peer_seen_.end(), true));
            std::uint8_t seat_mask = 0U;
            // Relay slot zero is the peer host and has no join lease. Joined
            // relay slots 1..4 map to master-service lease seats 0..3.
            for (std::size_t slot = 1; slot < online_peer_seen_.size(); ++slot) {
                if (online_peer_seen_[slot]) {
                    seat_mask |= static_cast<std::uint8_t>(1U << (slot - 1U));
                }
            }
            start_online_task(OnlineTaskKind::Heartbeat,
                [master, lobby, secret, players, seat_mask](OnlineTaskResult&) {
                    master->heartbeat(lobby, secret, players,
                                      players > 1 ? "playing" : "lobby", seat_mask);
                });
        }
    }

    void adopt_preboot_session() {
        if (online_connection_) return;
        auto preboot = btga::take_preboot_online_session();
        if (!preboot) return;
        online_udp_ = std::move(preboot->udp);
        online_connection_ = std::move(preboot->connection);
        online_session_id_ = preboot->session_id;
        online_is_host_ = preboot->host;
        online_lobby_id_ = std::move(preboot->lobby_id);
        online_host_secret_ = std::move(preboot->host_secret);
        online_state_synchronized_ = true;
        // A pre-boot rendezvous has a stronger invariant than an in-game join:
        // neither runtime has entered N64 code yet.  Its controller epoch is
        // therefore exactly zero on both peers; discovering an origin later
        // would discard whichever polls the faster machine completed first.
        online_local_tick_origin_ = 0;
        online_remote_tick_origin_ = 0;
        online_tick_origins_valid_ = true;
        online_simulation_frame_ = 0;
        online_remote_simulation_frame_.reset();
        online_frame_inputs_ = {};
        online_latest_local_input_ = {};
        online_frame_applied_inputs_.fill({});
        online_frame_inputs_active_ = false;
        online_frame_applied_inputs_valid_ = false;
        online_local_digests_.fill({});
        online_remote_digests_.fill({});
        online_first_desync_frame_.reset();
        online_last_digest_frame_.reset();
        online_digest_mismatch_run_ = 0;
        online_boot_bridge_traced_ = false;
        online_peer_seen_[preboot->host ? 1U : 0U] = true;
        online_status_ = "Cold-boot multiplayer synchronized.";
        online_trace_.open(pc_data_path / "btga-net-sync.log",
                           std::ios::out | std::ios::trunc);
        if (online_trace_) {
            online_trace_ << "BTGA net synchronization trace v2\n"
                          << "PREBOOT slot=" << online_connection_->relay_slot()
                          << " seed=" << preboot->seed << '\n';
            online_trace_.flush();
        }
        trace_online_origin_locked(online_connection_->relay_slot());
        if (app_ && app_->appWindow && app_->appWindow->sdlWindow) {
            SDL_SetWindowTitle(app_->appWindow->sdlWindow, preboot->host
                ? "BattleTanx GA - HOST" : "BattleTanx GA - CLIENT");
        }
    }

    void draw_online_tab() {
        ImGui::SeparatorText("Online Multiplayer");
        ImGui::TextWrapped("Networking remains offline until Refresh or Join is selected. Matches use a persistent dedicated gameplay authority through the relay; no port forwarding or UPnP is required, and players never receive one another's address.");
        ImGui::TextWrapped("Status: %s", online_status_.c_str());
        const bool busy = online_task_.valid();
        const bool connected = online_connection_ &&
            online_connection_->state() != btga::OnlineState::Offline;
        if (connected) {
            const auto peers = std::count(
                online_peer_seen_.begin(), online_peer_seen_.end(), true);
            ImGui::Text("Relay slot: %u | Remote peers observed: %u",
                        static_cast<unsigned>(online_connection_->relay_slot()),
                        static_cast<unsigned>(peers));
            if (online_is_host_ && peers == 0) {
                ImGui::TextWrapped(
                    "Keep this overlay open until a remote peer is observed. "
                    "Then close F10 on every device before navigating the game menus.");
            }
        }
        ImGui::BeginDisabled(busy || connected);
        if (ImGui::Button("Refresh public lobbies")) browse_online();
        ImGui::SameLine();
        if (ImGui::Button("Open service dashboard")) {
            open_external_url("https://btga.thegh0stship.com/");
        }
        ImGui::EndDisabled();
        if (connected) {
            ImGui::SameLine();
            if (ImGui::Button("Disconnect")) disconnect_online();
        }
        ImGui::SeparatorText("Dedicated servers");
        if (online_lobbies_.empty()) ImGui::TextDisabled("Select Refresh public lobbies.");
        for (const btga::OnlineLobby& lobby : online_lobbies_) {
            ImGui::PushID(lobby.id.c_str());
            ImGui::Text("%s — %u/%u — %s — %s", lobby.name.c_str(),
                        static_cast<unsigned>(lobby.current_players),
                        static_cast<unsigned>(lobby.max_players),
                        lobby.region.c_str(), lobby.state.c_str());
            ImGui::SameLine();
            const bool joinable = !busy && !connected && lobby.state == "lobby" &&
                                  lobby.current_players < lobby.max_players;
            ImGui::BeginDisabled(!joinable);
            if (ImGui::Button("Join")) join_online(lobby.id);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::TextWrapped("The authority owns the canonical four-player world and controller timeline. Each device renders only its local view. A reconnect reclaims the same seat, installs the latest checkpoint, and replays the journal before gameplay resumes.");
    }

    static int window_mode_index(btga::WindowMode mode) {
        return static_cast<int>(mode);
    }

    static bool slider_percent(const char* label, float& value) {
        float percent = value * 100.0F;
        if (!ImGui::SliderFloat(label, &percent, 0.0F, 100.0F, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) return false;
        value = percent / 100.0F;
        return true;
    }

    void apply_settings() {
        draft_settings_.validate();
        draft_settings_.save(pc_data_path / "settings.ini");
        {
            const std::lock_guard lock(settings_update_mutex);
            settings_update = draft_settings_;
        }
        apply_render_requested_.store(true, std::memory_order_release);
    }

    void apply_render_settings() {
        const auto previous_aa = app_->userConfig.msaaSampleCount();
        app_->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
        app_->userConfig.resolutionMultiplier = draft_settings_.render_scale;
        configure_aspect_ratio(app_->userConfig, draft_settings_.aspect_ratio);
        switch (draft_settings_.antialiasing) {
        case 2: app_->userConfig.antialiasing = RT64::UserConfiguration::Antialiasing::MSAA2X; break;
        case 4: app_->userConfig.antialiasing = RT64::UserConfiguration::Antialiasing::MSAA4X; break;
        case 8: app_->userConfig.antialiasing = RT64::UserConfiguration::Antialiasing::MSAA8X; break;
        default: app_->userConfig.antialiasing = RT64::UserConfiguration::Antialiasing::None; break;
        }
        if (previous_aa != app_->userConfig.msaaSampleCount()) app_->updateMultisampling();
        if (app_->swapChain != nullptr) app_->swapChain->setVsyncEnabled(draft_settings_.vsync);
        app_->userConfig.refreshRate = draft_settings_.frame_limit > 0
            ? RT64::UserConfiguration::RefreshRate::Manual
            : RT64::UserConfiguration::RefreshRate::Original;
        if (draft_settings_.frame_limit > 0) {
            app_->userConfig.refreshRateTarget = draft_settings_.frame_limit;
        }
        app_->updateUserConfig(true);
        reload_texture_packs();

        SDL_Window* window = app_->appWindow->sdlWindow;
        if (window != nullptr) {
            std::uint32_t flags = 0;
            if (draft_settings_.window_mode == btga::WindowMode::Borderless) flags = SDL_WINDOW_FULLSCREEN_DESKTOP;
            else if (draft_settings_.window_mode == btga::WindowMode::Fullscreen) flags = SDL_WINDOW_FULLSCREEN;
            SDL_SetWindowFullscreen(window, flags);
            if (flags == 0) SDL_SetWindowSize(window, draft_settings_.window_width, draft_settings_.window_height);
        }

    }

    void reload_texture_packs() {
        texture_packs_ = btga::discover_texture_packs(pc_data_path / "texture-packs");
        texture_pack_status_.clear();
        if (app_ == nullptr || app_->textureCache == nullptr) return;
        if (!draft_settings_.texture_packs_enabled || texture_packs_.empty()) {
            app_->textureCache->clearReplacementDirectories();
            texture_pack_status_ = texture_packs_.empty() ? "No texture packs found." :
                "Texture packs are disabled.";
            return;
        }

        std::vector<RT64::ReplacementDirectory> directories;
        directories.reserve(texture_packs_.size());
        for (const btga::TexturePackInfo& pack : texture_packs_) {
            directories.emplace_back(pack.path);
        }
        if (app_->textureCache->loadReplacementDirectories(directories)) {
            texture_pack_status_ = "Loaded " + std::to_string(texture_packs_.size()) +
                (texture_packs_.size() == 1 ? " texture pack." : " texture packs.");
        }
        else {
            texture_pack_status_ = "One or more texture packs failed to load. See the log for details.";
        }
    }

    void draw_binding(const char* label, std::string& binding, int index) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(190.0F);
        const std::string button = (capture_binding_ == index ? "Press a key..." : binding) + "##" + std::to_string(index);
        if (ImGui::Button(button.c_str(), ImVec2(180.0F, 0.0F))) capture_binding_ = index;
        if (capture_binding_ != index) return;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            capture_binding_ = -1;
            return;
        }
        for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
            const ImGuiKey candidate = static_cast<ImGuiKey>(key);
            if (!ImGui::IsKeyPressed(candidate) || candidate == ImGuiKey_F10) continue;
            const SDL_Scancode scancode = imgui_key_to_sdl_scancode(candidate);
            const char* name = SDL_GetScancodeName(scancode);
            if (scancode != SDL_SCANCODE_UNKNOWN && name != nullptr && *name != '\0') {
                binding = name;
            }
            capture_binding_ = -1;
            break;
        }
    }

    void draw_controller_binding(const char* label, std::string& binding,
                                 int player, int index) {
        const std::lock_guard capture_lock(controller_capture_mutex_);
        ImGui::TextUnformatted(label);
        ImGui::SameLine(190.0F);
        const bool capturing = controller_capture_player_ == player &&
                               controller_capture_binding_ == index;
        const std::string button = (capturing ? "Press a controller input..." :
                                    controller_input_name(binding)) +
                                   "##controller-" + std::to_string(player) + '-' +
                                   std::to_string(index);
        if (ImGui::Button(button.c_str(), ImVec2(220.0F, 0.0F))) {
            controller_capture_player_ = player;
            controller_capture_binding_ = index;
            pending_controller_binding_.clear();
        }
        if (!capturing) return;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            controller_capture_player_ = -1;
            controller_capture_binding_ = -1;
            pending_controller_binding_.clear();
        }
        else if (!pending_controller_binding_.empty()) {
            binding = std::move(pending_controller_binding_);
            pending_controller_binding_.clear();
            controller_capture_player_ = -1;
            controller_capture_binding_ = -1;
        }
    }

    void draw_settings_overlay() {
        // Closing is completed by RT64 after this empty frame is submitted.
        // That clears the previous ImGui draw data instead of freezing the last
        // visible settings frame on screen.
        if (app_->customOverlayCloseFrames.load(std::memory_order_acquire) == 1) return;

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        const ImVec2 display = io.DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5F, display.y * 0.5F), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
        ImGui::SetNextWindowSize(ImVec2(std::min(760.0F, display.x - 40.0F), std::min(650.0F, display.y - 40.0F)), ImGuiCond_Always);
        bool open = true;
        if (capture_binding_ < 0 && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            open = false;
        }
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize;
        if (ImGui::Begin("BattleTanx: Global Assault Settings", &open, flags)) {
            ImGui::TextUnformatted("F10 closes settings. Changes apply when you select Apply & Save.");
            if (ImGui::BeginTabBar("SettingsTabs")) {
                if (ImGui::BeginTabItem("Video")) {
                    int mode = window_mode_index(draft_settings_.window_mode);
                    if (ImGui::Combo("Window mode", &mode, "Windowed\0Borderless\0Fullscreen\0")) {
                        draft_settings_.window_mode = static_cast<btga::WindowMode>(mode);
                    }
                    ImGui::SliderInt("Window width", &draft_settings_.window_width, 640, 7680);
                    ImGui::SliderInt("Window height", &draft_settings_.window_height, 480, 4320);
                    int aspect = static_cast<int>(draft_settings_.aspect_ratio);
                    if (ImGui::Combo("Aspect ratio", &aspect,
                                     "Original 4:3\0Match window\0" "16:9\0" "16:10\0"
                                     "21:9\0" "32:9\0")) {
                        draft_settings_.aspect_ratio = static_cast<btga::AspectRatioMode>(aspect);
                    }
                    ImGui::TextDisabled("Widescreen projection applies immediately; game-specific HUD validation is ongoing.");
                    ImGui::SliderFloat("Internal render scale", &draft_settings_.render_scale, 0.5F, 8.0F, "%.2fx");
                    int aa = draft_settings_.antialiasing == 8 ? 3 : draft_settings_.antialiasing / 2;
                    if (ImGui::Combo("Antialiasing", &aa, "Off\0MSAA 2x\0MSAA 4x\0MSAA 8x\0")) {
                        constexpr int values[] = {0, 2, 4, 8};
                        draft_settings_.antialiasing = values[aa];
                    }
                    ImGui::Checkbox("VSync", &draft_settings_.vsync);
                    bool custom_frame_limit = draft_settings_.frame_limit > 0;
                    if (ImGui::Checkbox("Custom presentation frame limit", &custom_frame_limit)) {
                        draft_settings_.frame_limit = custom_frame_limit ? 60 : 0;
                    }
                    if (custom_frame_limit) {
                        ImGui::SliderInt("Frame limit", &draft_settings_.frame_limit, 20, 360, "%d FPS");
                        ImGui::TextDisabled("Gameplay simulation remains at its original rate.");
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Mods")) {
                    ImGui::Checkbox("Extended visibility (remove distance fog)",
                                    &draft_settings_.mod_extended_visibility);
                    ImGui::TextWrapped("Extends the projection and world culling range. This can reveal level geometry that the original game never intended to show at once.");
                    ImGui::Separator();
                    ImGui::Checkbox("Independent dual-stick turret aiming",
                                    &draft_settings_.mod_independent_turret);
                    ImGui::TextWrapped("Turreted vehicles retain hull movement on the left stick while the right stick rotates the turret. Primary fire and turret-aimed weapons use the turret; deployables retain their original behavior.");
                    ImGui::TextWrapped("Mouse: WASD drives, left click fires, and holding right click captures the mouse for turret aiming.");
                    ImGui::BeginDisabled(!draft_settings_.mod_independent_turret);
                    ImGui::SliderFloat("Mouse turret sensitivity", &draft_settings_.mouse_turret_sensitivity,
                                       0.25F, 4.0F, "%.2fx");
                    ImGui::EndDisabled();
                    ImGui::TextDisabled("Both gameplay mods are disabled by default.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Textures")) {
                    ImGui::Checkbox("Enable texture packs", &draft_settings_.texture_packs_enabled);
                    ImGui::TextWrapped("Place RT64 .rtz packs or unpacked pack directories containing rt64.json in:");
                    ImGui::TextWrapped("%s", (pc_data_path / "texture-packs").string().c_str());
                    if (ImGui::Button("Open texture-packs folder")) {
                        open_folder(pc_data_path / "texture-packs");
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Rescan now")) reload_texture_packs();
                    ImGui::SeparatorText("Discovered packs");
                    if (texture_packs_.empty()) {
                        ImGui::TextDisabled("No .rtz files or unpacked RT64 packs found.");
                    }
                    else {
                        for (const btga::TexturePackInfo& pack : texture_packs_) {
                            ImGui::BulletText("%s (%s)", pack.display_name.c_str(),
                                              pack.packaged ? ".rtz" : "directory");
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("%s", pack.path.string().c_str());
                            }
                        }
                    }
                    if (!texture_pack_status_.empty()) {
                        ImGui::Separator();
                        ImGui::TextWrapped("%s", texture_pack_status_.c_str());
                    }
                    ImGui::TextDisabled("Apply & Save reloads all discovered packs. DDS packs with a low-mip cache provide the smoothest streaming.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Audio")) {
                    slider_percent("Master volume", draft_settings_.master_volume);
                    slider_percent("Music volume", draft_settings_.music_volume);
                    slider_percent("Effects volume", draft_settings_.effects_volume);
                    ImGui::Checkbox("Mono audio", &draft_settings_.mono_audio);
                    ImGui::TextDisabled("Master applies to the final mix; music and effects use the game's separate libmus buses. Mono averages both output channels.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Accessibility")) {
                    slider_percent("Screen shake", draft_settings_.screen_shake_intensity);
                    ImGui::TextDisabled("0%% removes random camera displacement while preserving gameplay recoil and timing.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Controllers")) {
                    for (std::size_t i = 0; i < draft_settings_.players.size(); ++i) {
                        auto& player = draft_settings_.players[i];
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::SeparatorText(("Player " + std::to_string(i + 1)).c_str());
                        int slot = player.controller_slot + 1;
                        if (ImGui::Combo("Controller", &slot, "Disabled\0Controller 1\0Controller 2\0Controller 3\0Controller 4\0")) player.controller_slot = slot - 1;
                        ImGui::SliderFloat("Deadzone", &player.deadzone, 0.0F, 0.95F, "%.2f");
                        ImGui::SliderFloat("Sensitivity", &player.sensitivity, 0.1F, 3.0F, "%.2fx");
                        ImGui::Checkbox("Invert horizontal", &player.invert_x); ImGui::SameLine();
                        ImGui::Checkbox("Invert vertical", &player.invert_y);
                        ImGui::SliderFloat("Turret sensitivity", &player.turret_sensitivity, 0.1F, 4.0F, "%.2fx");
                        ImGui::Checkbox("Invert turret horizontal", &player.invert_turret_x);
                        slider_percent("Rumble", player.rumble_strength);
                        if (ImGui::TreeNode("Controller bindings")) {
                            auto& b = draft_settings_.controllers[i];
                            std::array<std::pair<const char*, std::string*>, 14> bindings{{
                                {"A button", &b.button_a}, {"B button", &b.button_b},
                                {"Z trigger", &b.button_z}, {"Start", &b.button_start},
                                {"L shoulder", &b.button_l}, {"R shoulder", &b.button_r},
                                {"D-pad up", &b.dpad_up}, {"D-pad down", &b.dpad_down},
                                {"D-pad left", &b.dpad_left}, {"D-pad right", &b.dpad_right},
                                {"C up", &b.c_up}, {"C down", &b.c_down},
                                {"C left", &b.c_left}, {"C right", &b.c_right},
                            }};
                            for (std::size_t binding_index = 0; binding_index < bindings.size(); ++binding_index) {
                                draw_controller_binding(bindings[binding_index].first,
                                                        *bindings[binding_index].second,
                                                        static_cast<int>(i),
                                                        static_cast<int>(binding_index));
                            }
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Keyboard")) {
                    auto& k = draft_settings_.keyboard;
                    std::array<std::pair<const char*, std::string*>, 18> bindings{{
                        {"Stick up", &k.stick_up}, {"Stick down", &k.stick_down}, {"Stick left", &k.stick_left}, {"Stick right", &k.stick_right},
                        {"A button", &k.button_a}, {"B button", &k.button_b}, {"Z trigger", &k.button_z}, {"Start", &k.button_start},
                        {"L shoulder", &k.button_l}, {"R shoulder", &k.button_r}, {"D-pad up", &k.dpad_up}, {"D-pad down", &k.dpad_down},
                        {"D-pad left", &k.dpad_left}, {"D-pad right", &k.dpad_right}, {"C up", &k.c_up}, {"C down", &k.c_down},
                        {"C left", &k.c_left}, {"C right", &k.c_right},
                    }};
                    for (std::size_t i = 0; i < bindings.size(); ++i) draw_binding(bindings[i].first, *bindings[i].second, static_cast<int>(i));
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Online")) {
                    draw_online_tab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("System")) {
                    ImGui::SeparatorText("ROM startup");
                    if (draft_settings_.last_rom_path.empty()) {
                        ImGui::TextWrapped("The native ROM picker will open on the next launch.");
                    }
                    else {
                        ImGui::TextWrapped("Next launch: %s", draft_settings_.last_rom_path.c_str());
                    }
                    if (ImGui::Button("Open ROM picker next launch")) {
                        draft_settings_.last_rom_path.clear();
                    }
                    bool has_recent = false;
                    for (std::size_t i = 0; i < draft_settings_.recent_rom_paths.size(); ++i) {
                        const std::string& recent = draft_settings_.recent_rom_paths[i];
                        if (recent.empty()) continue;
                        has_recent = true;
                        ImGui::PushID(static_cast<int>(i));
                        const std::u8string encoded(
                            reinterpret_cast<const char8_t*>(recent.data()), recent.size());
                        const std::filesystem::path path{encoded};
                        const std::string filename = path.filename().string();
                        if (ImGui::Button(("Use " + filename + " next launch").c_str())) {
                            draft_settings_.last_rom_path = recent;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", recent.c_str());
                        ImGui::PopID();
                    }
                    if (!has_recent) ImGui::TextDisabled("No recently verified ROM paths yet.");
                    ImGui::TextDisabled("Changing ROMs takes effect after restarting the game.");
                    ImGui::SeparatorText("Support and files");
                    if (ImGui::Button("Open saves and settings folder")) open_data_folder();
                    ImGui::SameLine();
                    if (ImGui::Button("Open support page")) {
                        open_external_url("https://github.com/TheGh0stShip/BattleTanx-Global-Assault-PC-Port/issues");
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::Separator();
            if (ImGui::Button("Apply & Save")) apply_settings();
            ImGui::SameLine();
            if (ImGui::Button("Restore defaults")) draft_settings_ = btga::PcSettings::defaults();
            ImGui::SameLine();
            if (ImGui::Button("Close")) open = false;
        }
        ImGui::End();
        // This callback executes from inside Inspector::draw. Defer hiding until
        // RT64 has submitted one empty frame, otherwise the last settings draw data
        // remains frozen on screen and can no longer receive input.
        if (!open) app_->customOverlayCloseFrames.store(2, std::memory_order_release);
    }

    std::uint8_t* rdram_ = nullptr;
    std::unique_ptr<RT64::Application> app_;
    btga::PcSettings draft_settings_;
    std::vector<btga::TexturePackInfo> texture_packs_;
    std::string texture_pack_status_;
    int capture_binding_ = -1;
    int controller_capture_player_ = -1;
    int controller_capture_binding_ = -1;
    std::string pending_controller_binding_;
    std::mutex controller_capture_mutex_;
    std::atomic_bool apply_render_requested_{false};
    std::unique_ptr<btga::HttpClient> online_http_;
    std::unique_ptr<btga::MasterClient> online_master_;
    std::unique_ptr<btga::UdpOnlineIo> online_udp_;
    std::unique_ptr<btga::OnlineMultiplayer> online_connection_;
    std::future<OnlineTaskResult> online_task_;
    std::vector<btga::OnlineLobby> online_lobbies_;
    std::string online_status_ = "Offline. No multiplayer sockets are open.";
    std::string online_lobby_id_;
    std::string online_pending_lobby_;
    std::string online_host_secret_;
    bool online_is_host_ = false;
    bool online_restart_handoff_ = false;
    std::chrono::steady_clock::time_point online_next_heartbeat_{};
    std::recursive_mutex online_mutex_;
    btga::InputSynchronizer online_inputs_;
    btga::InputSynchronizer online_frame_inputs_;
    std::uint64_t online_session_id_ = 0;
    std::uint32_t online_sequence_ = 1;
    std::uint32_t online_tick_ = 0;
    std::uint32_t online_local_tick_origin_ = 0;
    std::uint32_t online_remote_tick_origin_ = 0;
    bool online_tick_origins_valid_ = false;
    bool online_state_synchronized_ = false;
    bool online_state_sent_ = false;
    std::uint32_t online_state_seed_ = 0;
    std::uint32_t online_state_frame_ = 0;
    std::uint32_t online_state_generation_ = 0;
    bool online_applied_inputs_valid_ = false;
    std::array<btga::OnlineControllerInput, 2> online_applied_inputs_{};
    btga::OnlineControllerInput online_latest_local_input_{};
    std::array<btga::OnlineControllerInput, 2> online_frame_applied_inputs_{};
    bool online_frame_inputs_active_ = false;
    bool online_frame_applied_inputs_valid_ = false;
    std::ofstream online_trace_;
    std::optional<std::uint32_t> online_last_traced_tick_;
    bool online_wait_traced_ = false;
    bool online_boot_bridge_traced_ = false;
    std::uint32_t online_simulation_frame_ = 0;
    std::optional<std::uint32_t> online_remote_simulation_frame_;
    std::array<OnlineFrameDigest, online_digest_history_size> online_local_digests_{};
    std::array<OnlineFrameDigest, online_digest_history_size> online_remote_digests_{};
    std::optional<std::uint32_t> online_first_desync_frame_;
    std::optional<std::uint32_t> online_last_digest_frame_;
    std::uint32_t online_digest_mismatch_run_ = 0;
    std::array<std::uint8_t, gameplay_snapshot_size> online_gameplay_snapshot_{};
    btga::SnapshotAssembler online_snapshot_assembler_{gameplay_snapshot_size};
    std::uint32_t online_gameplay_snapshot_generation_ = 0;
    std::uint32_t online_gameplay_snapshot_checksum_ = 0;
    std::uint64_t online_gameplay_snapshot_received_mask_ = 0;
    bool online_gameplay_snapshot_acknowledged_ = false;
    std::uint32_t online_gameplay_applied_generation_ = 0;
    bool online_gameplay_snapshot_applied_ = false;
    bool online_gameplay_resync_requested_ = false;
    bool rendererless_ = false;
    std::array<bool, btga::InputSynchronizer::max_players> online_peer_seen_{};
};
}

void btga::renderer::configure_rt64(const PcSettings& settings,
                                    const std::filesystem::path& data_path,
                                    bool rendererless_online) {
    pc_settings = settings;
    pc_data_path = data_path;
    rendererless_online_context = rendererless_online;
}

std::unique_ptr<ultramodern::renderer::RendererContext> btga::renderer::create_rt64_context(
    std::uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode) {
    return std::make_unique<Context>(rdram, window, developer_mode);
}

void btga::renderer::toggle_settings_overlay() {
    if (Context* context = active_context.load(std::memory_order_acquire)) context->toggle_settings();
}

bool btga::renderer::settings_overlay_visible() {
    Context* context = active_context.load(std::memory_order_acquire);
    return context != nullptr && context->settings_visible();
}

bool btga::renderer::process_settings_event(SDL_Event* event) {
    Context* context = active_context.load(std::memory_order_acquire);
    return context != nullptr && context->process_event(event);
}

bool btga::renderer::consume_settings_update(PcSettings& settings) {
    const std::lock_guard lock(settings_update_mutex);
    if (!settings_update) return false;
    settings = std::move(*settings_update);
    settings_update.reset();
    return true;
}

int btga::renderer::online_local_player_slot() {
    Context* context = active_context.load(std::memory_order_acquire);
    return context ? context->online_local_player_slot() : -1;
}

bool btga::renderer::online_synchronized_input(int player, std::uint16_t& buttons,
                                               float& x, float& y) {
    Context* context = active_context.load(std::memory_order_acquire);
    return context && context->online_synchronized_input(player, buttons, x, y);
}

void btga::renderer::online_submit_local_input(std::uint16_t buttons, float x, float y) {
    if (Context* context = active_context.load(std::memory_order_acquire)) {
        context->online_submit_local_input(buttons, x, y);
    }
}

bool btga::renderer::online_simulation_frame_barrier() {
    Context* context = active_context.load(std::memory_order_acquire);
    return context == nullptr || context->online_simulation_frame_barrier();
}
