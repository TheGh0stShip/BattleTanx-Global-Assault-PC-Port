#include "btga/texture_packs.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <system_error>

namespace btga {
namespace {

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

bool is_packaged_texture_pack(const std::filesystem::path& path) {
    return lowercase(path.extension().string()) == ".rtz";
}

std::vector<TexturePackInfo> discover_texture_packs(const std::filesystem::path& directory) {
    std::vector<TexturePackInfo> packs;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return packs;

    for (std::filesystem::directory_iterator it{directory, error}, end;
         !error && it != end; it.increment(error)) {
        const std::filesystem::directory_entry& entry = *it;
        const bool directory_pack = entry.is_directory(error) &&
            std::filesystem::is_regular_file(entry.path() / "rt64.json", error);
        error.clear();
        const bool packaged = entry.is_regular_file(error) && is_packaged_texture_pack(entry.path());
        error.clear();
        if (!directory_pack && !packaged) continue;

        std::string name = entry.path().stem().string();
        if (name.empty()) name = entry.path().filename().string();
        packs.push_back(TexturePackInfo{entry.path(), std::move(name), packaged});
    }

    std::sort(packs.begin(), packs.end(), [](const TexturePackInfo& left,
                                             const TexturePackInfo& right) {
        const std::string left_name = lowercase(left.display_name);
        const std::string right_name = lowercase(right.display_name);
        if (left_name != right_name) return left_name < right_name;
        return left.path.generic_string() < right.path.generic_string();
    });
    return packs;
}

std::filesystem::path install_texture_pack(const std::filesystem::path& source,
                                           const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error) ||
        !is_packaged_texture_pack(source)) {
        throw std::runtime_error("Texture pack must be a local .rtz file: " + source.string());
    }

    std::filesystem::create_directories(directory, error);
    if (error) {
        throw std::runtime_error("Could not create texture-pack directory: " + error.message());
    }

    const std::filesystem::path destination = directory / source.filename();
    if (std::filesystem::exists(destination, error) &&
        std::filesystem::equivalent(source, destination, error) && !error) {
        return destination;
    }
    error.clear();

    std::filesystem::path staging = destination;
    staging += ".installing";
    std::filesystem::copy_file(source, staging,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        throw std::runtime_error("Could not copy texture pack: " + error.message());
    }

    std::filesystem::remove(destination, error);
    error.clear();
    std::filesystem::rename(staging, destination, error);
    if (error) {
        std::filesystem::remove(staging);
        throw std::runtime_error("Could not finish texture-pack installation: " + error.message());
    }
    return destination;
}

} // namespace btga
