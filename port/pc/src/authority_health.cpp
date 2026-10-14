#include "btga/authority_health.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace btga {

bool save_authority_health(
    const std::filesystem::path& path, const AuthorityHealth& health) {
    if (path.empty() || health.connected_mask > 0x0fU ||
        health.recovery_mask > 0x0fU) return false;
    const auto temporary = std::filesystem::path{path.string() + ".tmp"};
    const nlohmann::json document{
        {"version", 1}, {"loop_sequence", health.loop_sequence},
        {"next_tick", health.next_tick},
        {"connected_mask", health.connected_mask},
        {"recovery_mask", health.recovery_mask},
        {"gameplay_tick_active", health.gameplay_tick_active}
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
    // std::filesystem::rename cannot replace an existing file on Windows.
    // Supervisors retain the last observed write time across this short gap.
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

std::optional<AuthorityHealth> load_authority_health(
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
        AuthorityHealth health;
        health.loop_sequence = document.at("loop_sequence").get<std::uint64_t>();
        health.next_tick = document.at("next_tick").get<std::uint32_t>();
        const auto connected = document.at("connected_mask").get<std::uint64_t>();
        const auto recovery = document.at("recovery_mask").get<std::uint64_t>();
        if (connected > 0x0fU || recovery > 0x0fU) return std::nullopt;
        health.connected_mask = static_cast<std::uint8_t>(connected);
        health.recovery_mask = static_cast<std::uint8_t>(recovery);
        health.gameplay_tick_active =
            document.at("gameplay_tick_active").get<bool>();
        return health;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace btga
