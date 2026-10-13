#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace btga {

struct HttpResponse {
    int status = 0;
    std::string body;
};

class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpResponse request(std::string_view method, std::string_view path,
                                 std::string_view body = {},
                                 std::string_view bearer = {}) = 0;
};

std::unique_ptr<HttpClient> make_platform_http_client(std::string base_url);

struct OnlineLobby {
    std::string id;
    std::string name;
    std::string build;
    std::string platform;
    std::string region;
    std::string state;
    std::uint32_t capabilities = 0;
    std::uint16_t max_players = 0;
    std::uint16_t current_players = 0;
};

struct RelayEndpoint {
    std::string address;
    std::uint16_t port = 0;
};

struct HostRegistration {
    OnlineLobby lobby;
    std::string host_secret;
    std::uint64_t session_id = 0;
    RelayEndpoint relay;
    std::string relay_token;
};

struct JoinRegistration {
    std::uint64_t session_id = 0;
    std::uint16_t seat = 0;
    RelayEndpoint relay;
    std::string join_token;
    std::string resume_token;
};

class MasterClient {
public:
    static constexpr std::uint16_t protocol_version = 1;
    explicit MasterClient(HttpClient& http, std::string build = "pc-port-v1");

    std::vector<OnlineLobby> list_lobbies();
    HostRegistration host(std::string_view name, std::uint16_t max_players,
                          std::string_view region = "eastus");
    JoinRegistration join(std::string_view lobby_id,
                          std::string_view resume_token = {});
    void heartbeat(std::string_view lobby_id, std::string_view host_secret,
                   std::uint16_t players, std::string_view state,
                   std::uint8_t seat_mask);
    void remove(std::string_view lobby_id, std::string_view host_secret) noexcept;

    [[nodiscard]] const std::string& build() const noexcept { return build_; }

private:
    HttpClient& http_;
    std::string build_;
};

} // namespace btga
