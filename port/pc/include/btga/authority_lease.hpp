#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "btga/master_client.hpp"

namespace btga {

// The minimum control-plane identity a replacement dedicated process needs to
// authenticate to the existing lobby and obtain a fresh authority relay token.
// Admission tokens themselves are deliberately never persisted.
struct PersistentAuthorityLease {
    std::string lobby_id;
    std::string host_secret;
    std::uint64_t session_id = 0;
    RelayEndpoint relay;
    std::uint32_t packet_sequence = 0;
};

[[nodiscard]] bool save_authority_lease(
    const std::filesystem::path& path, const PersistentAuthorityLease& lease);
[[nodiscard]] std::optional<PersistentAuthorityLease> load_authority_lease(
    const std::filesystem::path& path);

} // namespace btga
