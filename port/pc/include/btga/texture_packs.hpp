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

bool is_packaged_texture_pack(const std::filesystem::path& path);
std::vector<TexturePackInfo> discover_texture_packs(const std::filesystem::path& directory);
std::filesystem::path install_texture_pack(const std::filesystem::path& source,
                                           const std::filesystem::path& directory);

} // namespace btga
