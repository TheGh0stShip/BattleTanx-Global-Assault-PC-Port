#include "btga/authority_lease.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace btga {
namespace {
bool valid(const PersistentAuthorityLease& lease) {
    return !lease.lobby_id.empty() && lease.lobby_id.size() <= 64U &&
           !lease.host_secret.empty() && lease.host_secret.size() <= 128U &&
           lease.session_id != 0U && !lease.relay.address.empty() &&
           lease.relay.address.size() <= 253U && lease.relay.port != 0U;
}
} // namespace

bool save_authority_lease(const std::filesystem::path& path,
                          const PersistentAuthorityLease& lease) {
    if (path.empty() || !valid(lease)) return false;
    const auto temporary = std::filesystem::path{path.string() + ".tmp"};
    const nlohmann::json document{
        {"version", 1}, {"lobby_id", lease.lobby_id},
        {"host_secret", lease.host_secret}, {"session_id", lease.session_id},
        {"relay_address", lease.relay.address}, {"relay_port", lease.relay.port},
        {"packet_sequence", lease.packet_sequence}
    };
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output) return false;
        output << document.dump() << '\n';
        output.flush();
        if (!output) return false;
    }
    std::error_code error;
    std::filesystem::permissions(
        temporary, std::filesystem::perms::owner_read |
                   std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
#if defined(_WIN32)
    // Windows rename does not replace an existing file. POSIX rename does and
    // is atomic, so preserve the old control record until the replacement is
    // ready everywhere that supports it.
    std::filesystem::remove(path, error);
    error.clear();
#endif
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

std::optional<PersistentAuthorityLease> load_authority_lease(
    const std::filesystem::path& path) {
    std::error_code error;
    if (path.empty() || std::filesystem::file_size(path, error) > 4096U || error) {
        return std::nullopt;
    }
    try {
        std::ifstream input{path, std::ios::binary};
        if (!input) return std::nullopt;
        const auto document = nlohmann::json::parse(input);
        if (!document.is_object() || document.value("version", 0) != 1) {
            return std::nullopt;
        }
        PersistentAuthorityLease lease;
        lease.lobby_id = document.at("lobby_id").get<std::string>();
        lease.host_secret = document.at("host_secret").get<std::string>();
        lease.session_id = document.at("session_id").get<std::uint64_t>();
        lease.relay.address = document.at("relay_address").get<std::string>();
        const auto port = document.at("relay_port").get<std::uint64_t>();
        if (port == 0U || port > 65535U) return std::nullopt;
        lease.relay.port = static_cast<std::uint16_t>(port);
        const auto sequence = document.value("packet_sequence", UINT64_C(0));
        if (sequence > UINT32_MAX) return std::nullopt;
        lease.packet_sequence = static_cast<std::uint32_t>(sequence);
        return valid(lease) ? std::optional{std::move(lease)} : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace btga
