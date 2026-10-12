#include "rt64_context.hpp"

#if defined(_WIN32)
#include <Windows.h>
#include <objidl.h>
#include <oleauto.h>
#endif

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string_view>

#include "hle/rt64_application.h"
#include "ultramodern/ultramodern.hpp"

namespace {
btga::PcSettings pc_settings = btga::PcSettings::defaults();
std::filesystem::path pc_data_path;

bool environment_flag_enabled(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }

    const std::string_view text{value};
    return !text.empty() && text != "0" && text != "false" && text != "FALSE" &&
           text != "off" && text != "OFF";
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
    Context(std::uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode) {
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
        app_->userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Original;
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
    }
    void shutdown() override { if (app_) app_->end(); }
    std::uint32_t get_display_framerate() const override { return app_->presentQueue->ext.sharedResources->swapChainRate; }
    float get_resolution_scale() const override { return 1.0F; }

private:
    std::unique_ptr<RT64::Application> app_;
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
