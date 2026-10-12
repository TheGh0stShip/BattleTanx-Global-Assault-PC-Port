#pragma once

#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace btga::renderer {
std::unique_ptr<ultramodern::renderer::RendererContext> create_rt64_context(
    std::uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode);
}
