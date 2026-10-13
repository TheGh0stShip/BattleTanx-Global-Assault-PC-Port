#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace btga {

constexpr std::size_t gameplay_snapshot_size = 32'316U;

[[nodiscard]] bool capture_gameplay_snapshot(
    std::span<const std::uint8_t> rdram,
    std::span<std::uint8_t> snapshot);

[[nodiscard]] bool restore_gameplay_snapshot(
    std::span<std::uint8_t> rdram,
    std::span<const std::uint8_t> snapshot);

} // namespace btga
