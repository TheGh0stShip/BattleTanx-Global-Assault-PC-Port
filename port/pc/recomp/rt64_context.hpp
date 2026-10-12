#pragma once

#include <memory>
#include <filesystem>

#include "btga/pc_settings.hpp"
#include "ultramodern/renderer_context.hpp"

union SDL_Event;

namespace btga::renderer {
void configure_rt64(const PcSettings& settings, const std::filesystem::path& data_path);
void toggle_settings_overlay();
bool settings_overlay_visible();
bool process_settings_event(SDL_Event* event);
bool consume_settings_update(PcSettings& settings);
std::unique_ptr<ultramodern::renderer::RendererContext> create_rt64_context(
    std::uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode);
}
