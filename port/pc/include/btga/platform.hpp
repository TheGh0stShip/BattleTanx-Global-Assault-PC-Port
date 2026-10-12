#pragma once

#include "btga/game.hpp"
#include <string_view>

namespace btga {

class Platform {
  public:
    virtual ~Platform() = default;
    virtual bool pump(Input& input) = 0;
    virtual void present(const Framebuffer& framebuffer) = 0;
    static Platform* create(std::string_view title);
};

} // namespace btga
