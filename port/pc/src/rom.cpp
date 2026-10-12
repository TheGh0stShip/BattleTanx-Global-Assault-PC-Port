#include "btga/rom.hpp"
#include "btga/sha256.hpp"

#include <fstream>
#include <stdexcept>

namespace btga {
Rom Rom::load_verified(const std::filesystem::path& path) {
    if (path.extension() != ".z64") throw std::runtime_error("ROM must be a big-endian .z64 file");
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("could not open ROM: " + path.string());
    const auto length = stream.tellg();
    if (length != 8 * 1024 * 1024) throw std::runtime_error("ROM must be exactly 8 MiB");
    Rom rom;
    rom.bytes_.resize(static_cast<std::size_t>(length));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(rom.bytes_.data()), length);
    constexpr auto expected = "88268ce770ff39f2ca839848b842c87866df6c5ddaf775f180b2522c18ff6221";
    const std::string actual = hex_digest(sha256(rom.bytes_));
    if (actual != expected) throw std::runtime_error("unsupported ROM SHA-256: " + actual);
    return rom;
}
} // namespace btga
