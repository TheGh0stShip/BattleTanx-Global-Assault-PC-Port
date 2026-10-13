#pragma once

#include <memory>
#include <filesystem>

#include "btga/pc_settings.hpp"
#include "ultramodern/renderer_context.hpp"

union SDL_Event;

namespace btga::renderer {
void configure_rt64(const PcSettings& settings, const std::filesystem::path& data_path,
                    bool rendererless_online = false);
void toggle_settings_overlay();
bool settings_overlay_visible();
bool process_settings_event(SDL_Event* event);
bool consume_settings_update(PcSettings& settings);
int online_local_player_slot();
bool online_synchronized_input(int player, std::uint16_t& buttons, float& x, float& y);
void online_submit_local_input(std::uint16_t buttons, float x, float y);
bool online_simulation_frame_barrier();
std::unique_ptr<ultramodern::renderer::RendererContext> create_rt64_context(
    std::uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode);
}
