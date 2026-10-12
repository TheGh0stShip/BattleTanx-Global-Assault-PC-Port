#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace btga {

struct TexturePackInfo {
    std::filesystem::path path;
    std::string display_name;
    bool packaged = false;
};

std::vector<TexturePackInfo> discover_texture_packs(const std::filesystem::path& directory);

} // namespace btga
