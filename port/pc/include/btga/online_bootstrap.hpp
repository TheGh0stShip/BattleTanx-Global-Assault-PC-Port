#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "btga/master_client.hpp"
#include "btga/online_multiplayer.hpp"
#include "btga/udp_online_io.hpp"

namespace btga {

struct PrebootOnlineSession {
    std::unique_ptr<UdpOnlineIo> udp;
    std::unique_ptr<OnlineMultiplayer> connection;
    std::uint64_t session_id = 0;
    std::uint32_t seed = 0;
    bool host = false;
    std::string lobby_id;
    std::string host_secret;
};

bool begin_preboot_online(bool host, std::uint64_t session_id,
                          const RelayEndpoint& relay, std::string_view token,
                          std::string lobby_id = {}, std::string host_secret = {});
void poll_preboot_online();
[[nodiscard]] bool preboot_online_ready();
[[nodiscard]] std::string preboot_online_status();
[[nodiscard]] std::optional<std::uint32_t> preboot_online_seed();
[[nodiscard]] std::optional<PrebootOnlineSession> take_preboot_online_session();

} // namespace btga
