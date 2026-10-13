#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace btga {

constexpr std::size_t gameplay_snapshot_size = 32'340U;

// Dedicated authorities need enough canonical state to resume a cold process
// at a live tick.  The smaller gameplay snapshot remains the wire format used
// by the legacy peer-to-peer bootstrap path.
constexpr std::size_t dedicated_checkpoint_base_size = 123'225U;

[[nodiscard]] bool gameplay_snapshot_random_seed(
    std::span<const std::uint8_t> snapshot,
    std::uint32_t& random_seed);

[[nodiscard]] bool restore_gameplay_random_seed(
    std::span<std::uint8_t> rdram,
    std::uint32_t random_seed);

[[nodiscard]] bool capture_gameplay_snapshot(
    std::span<const std::uint8_t> rdram,
    std::span<std::uint8_t> snapshot);

[[nodiscard]] bool restore_gameplay_snapshot(
    std::span<std::uint8_t> rdram,
    std::span<const std::uint8_t> snapshot);

[[nodiscard]] bool capture_dedicated_checkpoint(
    std::span<const std::uint8_t> rdram,
    std::vector<std::uint8_t>& checkpoint);

[[nodiscard]] bool restore_dedicated_checkpoint(
    std::span<std::uint8_t> rdram,
    std::span<const std::uint8_t> checkpoint);

[[nodiscard]] bool is_dedicated_checkpoint(
    std::span<const std::uint8_t> checkpoint);

} // namespace btga
