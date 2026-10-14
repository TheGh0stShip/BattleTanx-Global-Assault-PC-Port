#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace btga {

// Secret-free process health written for an external restart supervisor. The
// loop sequence is advanced by the translated authority's main thread. An
// idle frontend may park after publishing its initial sequence, so the
// supervisor also considers fresh health writes while gameplay is inactive;
// active gameplay still requires the sequence itself to advance.
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
