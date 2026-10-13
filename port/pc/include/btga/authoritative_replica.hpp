#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "btga/input_sync.hpp"

namespace btga {

constexpr std::size_t canonical_state_limit = 8U * 1024U * 1024U;

struct CanonicalFrame {
    std::uint32_t tick = 0;
    std::uint64_t state_digest = 0;
    std::uint32_t random_seed = 0;
    std::uint8_t occupied_mask = 0;
    std::uint8_t connected_mask = 0;
    std::uint8_t received_mask = 0;
    std::array<OnlineControllerInput, 4> inputs{};
};

struct CanonicalSnapshot {
    std::uint32_t snapshot_id = 0;
    std::uint32_t tick = 0;
    std::uint64_t state_digest = 0;
    std::vector<std::uint8_t> state;
};

// Decode one authoritative frame without imposing delivery order. Historical
// journal replay intentionally arrives behind the current live broadcast, so
// bounded replay consumers must validate and place frames by tick themselves.
[[nodiscard]] bool decode_authoritative_frame(
    std::span<const std::uint8_t> payload, CanonicalFrame& frame);

// Minimal portable client state receiver. It does not simulate or adjudicate
// gameplay; it accepts monotonic server frames and reconstructs reliable full
// snapshots, including out-of-order and duplicate chunks.
class AuthoritativeReplica {
public:
    [[nodiscard]] bool accept_frame(std::span<const std::uint8_t> payload);
    [[nodiscard]] bool accept_snapshot_chunk(std::span<const std::uint8_t> payload);
    [[nodiscard]] const std::optional<CanonicalFrame>& latest_frame() const noexcept;
    [[nodiscard]] bool snapshot_ready() const noexcept;
    [[nodiscard]] std::optional<CanonicalSnapshot> take_snapshot();

private:
    void reset_snapshot(std::uint32_t snapshot_id, std::uint32_t tick,
                        std::uint64_t digest, std::uint32_t total_size);

    std::optional<CanonicalFrame> frame_;
    std::optional<CanonicalSnapshot> snapshot_;
    std::vector<std::uint8_t> received_chunks_;
    std::size_t received_bytes_ = 0;
    std::uint32_t newest_snapshot_id_ = 0;
    bool have_snapshot_id_ = false;
};

} // namespace btga
