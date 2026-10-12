#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace btga {
std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> input);
std::string hex_digest(const std::array<std::uint8_t, 32>& digest);
} // namespace btga
