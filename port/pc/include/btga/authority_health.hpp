#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace btga {

// Secret-free process health written for an external restart supervisor. The
// loop sequence must be advanced by the translated authority's main thread;
// network activity alone is deliberately insufficient to claim health.
struct AuthorityHealth {
    std::uint64_t loop_sequence = 0U;
    std::uint32_t next_tick = 0U;
    std::uint8_t connected_mask = 0U;
    std::uint8_t recovery_mask = 0U;
    bool gameplay_tick_active = false;
};

[[nodiscard]] bool save_authority_health(
    const std::filesystem::path& path, const AuthorityHealth& health);
[[nodiscard]] std::optional<AuthorityHealth> load_authority_health(
    const std::filesystem::path& path);

} // namespace btga
