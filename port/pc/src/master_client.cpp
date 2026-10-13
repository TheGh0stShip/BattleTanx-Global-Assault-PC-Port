#include "btga/master_client.hpp"

extern "C" {
#include <btga/btga_handshake.h>
#include <btga/btga_net.h>
}

#include <nlohmann/json.hpp>

#include <limits>
#include <stdexcept>

namespace btga {
namespace {
using Json = nlohmann::json;

constexpr std::size_t max_response_size = 64 * 1024;

std::string escape_path(std::string_view input) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    for (const unsigned char value : input) {
        if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '-' || value == '_' ||
            value == '.' || value == '~') {
            output.push_back(static_cast<char>(value));
        } else {
            output.push_back('%');
            output.push_back(hex[value >> 4]);
            output.push_back(hex[value & 15]);
        }
    }
    return output;
}

Json parse_response(const HttpResponse& response, int expected) {
    if (response.status != expected) {
        std::string detail;
        try {
            const Json error = Json::parse(response.body);
            detail = error.value("message", "");
        } catch (...) {}
        throw std::runtime_error(
            "master service returned HTTP " + std::to_string(response.status) +
            (detail.empty() ? "" : ": " + detail));
    }
    if (response.body.size() > max_response_size) {
        throw std::runtime_error("master response exceeds 64 KiB");
    }
    try {
        return Json::parse(response.body);
    } catch (const std::exception&) {
        throw std::runtime_error("master returned malformed JSON");
    }
}

std::string bounded_string(const Json& object, const char* key, std::size_t maximum) {
    if (!object.contains(key) || !object[key].is_string()) {
        throw std::runtime_error(std::string("master response is missing ") + key);
    }
    std::string value = object[key].get<std::string>();
    if (value.empty() || value.size() > maximum) {
        throw std::runtime_error(std::string("master response has invalid ") + key);
    }
    return value;
}

std::uint16_t bounded_u16(const Json& object, const char* key, std::uint16_t minimum,
                          std::uint16_t maximum) {
    if (!object.contains(key) || !object[key].is_number_unsigned()) {
        throw std::runtime_error(std::string("master response is missing ") + key);
    }
    const auto value = object[key].get<std::uint64_t>();
    if (value < minimum || value > maximum) {
        throw std::runtime_error(std::string("master response has invalid ") + key);
    }
    return static_cast<std::uint16_t>(value);
}

OnlineLobby decode_lobby(const Json& source) {
    OnlineLobby lobby;
    lobby.id = bounded_string(source, "id", 64);
    lobby.name = bounded_string(source, "name", 48);
    lobby.build = bounded_string(source, "build", 64);
    lobby.platform = bounded_string(source, "platform", 16);
    lobby.region = bounded_string(source, "region", 24);
    lobby.state = source.value("state", "lobby");
    if (lobby.state != "lobby" && lobby.state != "playing") {
        throw std::runtime_error("master response has invalid state");
    }
    lobby.max_players = bounded_u16(source, "max_players", 1, 16);
    lobby.current_players = bounded_u16(source, "current_players", 1, lobby.max_players);
    if (!source.contains("capabilities") || !source["capabilities"].is_number_unsigned()) {
        throw std::runtime_error("master response is missing capabilities");
    }
    const auto capabilities = source["capabilities"].get<std::uint64_t>();
    if (capabilities > (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::runtime_error("master response has invalid capabilities");
    }
    lobby.capabilities = static_cast<std::uint32_t>(capabilities);
    return lobby;
}

RelayEndpoint decode_relay(const Json& source) {
    RelayEndpoint relay;
    relay.address = bounded_string(source, "address", 253);
    relay.port = bounded_u16(source, "udp_port", 1, 65535);
    return relay;
}

std::uint64_t decode_session(const Json& source) {
    const std::string text = bounded_string(source, "session_id", BTGA_NET_SESSION_ID_TEXT_SIZE);
    std::uint64_t session = 0;
    if (!btga_net_parse_session_id(&session, text.data(), text.size())) {
        throw std::runtime_error("master response has invalid session_id");
    }
    return session;
}
} // namespace

MasterClient::MasterClient(HttpClient& http, std::string build)
    : http_(http), build_(std::move(build)) {
    if (build_.empty() || build_.size() > 64) {
        throw std::invalid_argument("online build identifier is invalid");
    }
}

std::vector<OnlineLobby> MasterClient::list_lobbies() {
    const Json body = parse_response(http_.request(
        "GET", "/lobbies?protocol=1&build=" + escape_path(build_)), 200);
    if (!body.contains("lobbies") || !body["lobbies"].is_array() ||
        body["lobbies"].size() > 64) {
        throw std::runtime_error("master response has invalid lobby list");
    }
    std::vector<OnlineLobby> result;
    result.reserve(body["lobbies"].size());
    for (const Json& lobby : body["lobbies"]) result.push_back(decode_lobby(lobby));
    return result;
}

HostRegistration MasterClient::host(std::string_view name, std::uint16_t max_players,
                                    std::string_view region) {
    const Json request = {
        {"name", name}, {"protocol", protocol_version}, {"build", build_},
        {"platform", "pc"}, {"capabilities", BTGA_NET_CAP_RELAY_UDP},
        {"region", region}, {"max_players", max_players}, {"current_players", 1},
        {"state", "lobby"}
    };
    const Json body = parse_response(http_.request("POST", "/lobbies", request.dump()), 201);
    HostRegistration result;
    result.lobby = decode_lobby(body.at("lobby"));
    result.host_secret = bounded_string(body, "host_secret", 128);
    result.session_id = decode_session(body);
    result.relay = decode_relay(body.at("relay"));
    result.relay_token = bounded_string(body, "relay_token", BTGA_NET_MAX_JOIN_TOKEN_SIZE);
    return result;
}

JoinRegistration MasterClient::join(std::string_view lobby_id,
                                    std::string_view resume_token) {
    Json request = {
        {"protocol", protocol_version}, {"build", build_}, {"platform", "pc"}
    };
    if (!resume_token.empty()) request["resume_token"] = resume_token;
    const Json body = parse_response(http_.request(
        "POST", "/lobbies/" + escape_path(lobby_id) + "/join", request.dump()), 200);
    JoinRegistration result;
    result.session_id = decode_session(body);
    if (body.contains("seat")) result.seat = bounded_u16(body, "seat", 0, 3);
    result.relay = decode_relay(body.at("relay"));
    result.join_token = bounded_string(body, "join_token", BTGA_NET_MAX_JOIN_TOKEN_SIZE);
    if (body.contains("resume_token")) {
        result.resume_token = bounded_string(
            body, "resume_token", BTGA_NET_MAX_JOIN_TOKEN_SIZE);
    }
    return result;
}

void MasterClient::heartbeat(std::string_view lobby_id, std::string_view host_secret,
                             std::uint16_t players, std::string_view state,
                             std::uint8_t seat_mask) {
    if ((seat_mask & 0xf0U) != 0U) {
        throw std::invalid_argument("seat mask exceeds four-player limit");
    }
    const Json request = {
        {"current_players", players}, {"state", state}, {"seat_mask", seat_mask}
    };
    (void)parse_response(http_.request(
        "PUT", "/lobbies/" + escape_path(lobby_id) + "/heartbeat",
        request.dump(), host_secret), 200);
}

void MasterClient::remove(std::string_view lobby_id,
                          std::string_view host_secret) noexcept {
    try {
        const HttpResponse response = http_.request(
            "DELETE", "/lobbies/" + escape_path(lobby_id), {}, host_secret);
        (void)response;
    } catch (...) {}
}

} // namespace btga
