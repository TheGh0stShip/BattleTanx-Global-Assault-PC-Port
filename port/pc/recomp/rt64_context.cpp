#include "rt64_context.hpp"

#if defined(_WIN32)
#include <Windows.h>
#include <objidl.h>
#include <oleauto.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>

#include "hle/rt64_application.h"
#include "ultramodern/ultramodern.hpp"

namespace {
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
        configuration.useConfigurationFile = false;
        app_ = std::make_unique<RT64::Application>(core, configuration);
#if defined(_WIN32)
        app_->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
#else
        app_->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
#endif
        app_->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
        app_->userConfig.aspectRatio = RT64::UserConfiguration::AspectRatio::Original;
        app_->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
        app_->userConfig.antialiasing = RT64::UserConfiguration::Antialiasing::None;
        app_->userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Original;
        app_->userConfig.internalColorFormat = RT64::UserConfiguration::InternalColorFormat::Standard;
        app_->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
        app_->userConfig.developerMode = developer_mode;
        if (app_->setup(0) != RT64::Application::SetupResult::Success) {
            app_.reset();
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

std::unique_ptr<ultramodern::renderer::RendererContext> btga::renderer::create_rt64_context(
    std::uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode) {
    return std::make_unique<Context>(rdram, window, developer_mode);
}
