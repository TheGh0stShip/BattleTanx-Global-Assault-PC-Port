#include "rt64_context.hpp"

#if defined(_WIN32)
#include <Windows.h>
#include <objidl.h>
#include <oleauto.h>
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <string_view>

#include "hle/rt64_application.h"
#include "imgui/imgui.h"
#include "ultramodern/ultramodern.hpp"

namespace {
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
        : draft_settings_(pc_settings) {
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
        app_->userConfig.aspectRatio = RT64::UserConfiguration::AspectRatio::Original;
        app_->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
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
        if (app_->setup(0) != RT64::Application::SetupResult::Success) {
            app_.reset();
        } else if (open_inspector) {
            app_->processDeveloperShortcut(RT64::Application::DeveloperShortcut::Inspector);
        }
        if (app_) {
            if (app_->swapChain != nullptr) app_->swapChain->setVsyncEnabled(pc_settings.vsync);
            app_->customOverlayCallback = [this]() { draw_settings_overlay(); };
            active_context.store(this, std::memory_order_release);
            if (environment_flag_enabled("BTGA_SETTINGS_OPEN")) app_->setCustomOverlayVisible(true);
        }
    }

    ~Context() override {
        Context* expected = this;
        active_context.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
        if (app_) app_->customOverlayCallback = {};
    }

    bool valid() override { return static_cast<bool>(app_); }
    bool update_config(const ultramodern::renderer::GraphicsConfig&,
                       const ultramodern::renderer::GraphicsConfig&) override { return false; }
    void enable_instant_present() override {}
    void send_dl(const OSTask* task) override {
        app_->state->rsp->reset();
        app_->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFFU,
                                       task->t.ucode_data & 0x3FFFFFFU, true);
        app_->processDisplayLists(app_->core.RDRAM, task->t.data_ptr & 0x3FFFFFFU, 0, true);
    }
    void send_dummy_workload(std::uint32_t address) override { app_->processDisplayLists(app_->core.RDRAM, address, 0, true); }
    void update_screen() override {
        app_->updateScreen();
        if (apply_render_requested_.exchange(false, std::memory_order_acq_rel)) {
            apply_render_settings();
        }
    }
    void shutdown() override { if (app_) app_->end(); }
    std::uint32_t get_display_framerate() const override { return app_->presentQueue->ext.sharedResources->swapChainRate; }
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

private:
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

        SDL_Window* window = app_->appWindow->sdlWindow;
        if (window != nullptr) {
            std::uint32_t flags = 0;
            if (draft_settings_.window_mode == btga::WindowMode::Borderless) flags = SDL_WINDOW_FULLSCREEN_DESKTOP;
            else if (draft_settings_.window_mode == btga::WindowMode::Fullscreen) flags = SDL_WINDOW_FULLSCREEN;
            SDL_SetWindowFullscreen(window, flags);
            if (flags == 0) SDL_SetWindowSize(window, draft_settings_.window_width, draft_settings_.window_height);
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
                if (ImGui::BeginTabItem("Audio")) {
                    slider_percent("Master volume", draft_settings_.master_volume);
                    slider_percent("Music volume", draft_settings_.music_volume);
                    slider_percent("Effects volume", draft_settings_.effects_volume);
                    ImGui::TextDisabled("Music/effects separation requires game-audio channel classification; master volume applies now.");
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
                if (ImGui::BeginTabItem("Mods")) {
                    ImGui::Checkbox("Extended visibility (remove distance fog)",
                                    &draft_settings_.mod_extended_visibility);
                    ImGui::TextWrapped("Extends the projection and world culling range. This can reveal level geometry that the original game never intended to show at once.");
                    ImGui::Separator();
                    ImGui::Checkbox("Independent dual-stick turret aiming",
                                    &draft_settings_.mod_independent_turret);
                    ImGui::TextWrapped("Turreted vehicles retain hull movement on the left stick while the right stick rotates the turret. Primary fire and turret-aimed weapons use the turret; deployables retain their original behavior.");
                    ImGui::TextWrapped("Mouse: WASD drives, left click fires, and holding right click captures the mouse for turret aiming.");
                    ImGui::TextDisabled("Both gameplay mods are disabled by default.");
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

    std::unique_ptr<RT64::Application> app_;
    btga::PcSettings draft_settings_;
    int capture_binding_ = -1;
    int controller_capture_player_ = -1;
    int controller_capture_binding_ = -1;
    std::string pending_controller_binding_;
    std::mutex controller_capture_mutex_;
    std::atomic_bool apply_render_requested_{false};
};
}

void btga::renderer::configure_rt64(const PcSettings& settings,
                                    const std::filesystem::path& data_path) {
    pc_settings = settings;
    pc_data_path = data_path;
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
