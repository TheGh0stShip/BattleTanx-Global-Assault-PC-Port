#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace btga {

class Rom {
  public:
    static Rom load_verified(const std::filesystem::path& path);
    [[nodiscard]] std::span<const std::uint8_t> bytes() const { return bytes_; }

  private:
    std::vector<std::uint8_t> bytes_;
};

} // namespace btga
