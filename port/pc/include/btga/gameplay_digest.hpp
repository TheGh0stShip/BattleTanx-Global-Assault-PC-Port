#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace btga {

struct GameplayStateDigests {
    std::uint32_t objects = 0;
    std::uint32_t object_core = 0;
    std::uint32_t object_tail = 0;
    std::array<std::uint32_t, 24> object_slots{};
    std::uint32_t player_headers = 0;
    std::uint32_t player_gameplay = 0;
    std::uint32_t team_info = 0;
    std::uint32_t random_seed = 0;
    std::uint32_t frame_counter = 0;
    std::uint32_t game_mode = 0;
    std::uint32_t match_time = 0;
    std::uint32_t combined = 0;
};

// Returns zero when the supplied memory is smaller than the original 8 MiB
// N64 RDRAM image. The digest deliberately excludes presentation, libultra,
// audio and host-runtime state that may differ without changing simulation.
[[nodiscard]] std::uint32_t gameplay_state_digest(
    std::span<const std::uint8_t> rdram);

[[nodiscard]] GameplayStateDigests gameplay_state_digests(
    std::span<const std::uint8_t> rdram);

// True only while the retail game state is actively simulating a multiplayer
// world. Front-end, attract, loading, pause, results, and teardown states must
// continue using controller-poll synchronization.
[[nodiscard]] bool gameplay_frame_input_active(
    std::span<const std::uint8_t> rdram);

} // namespace btga
