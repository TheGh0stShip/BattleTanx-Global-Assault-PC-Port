#include "btga/online_bootstrap.hpp"

extern "C" {
#include <btga/btga_net.h>
}

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

namespace btga {
namespace {

constexpr std::uint8_t bootstrap_version = 1;
constexpr std::size_t bootstrap_payload_size = 14;
constexpr std::uint32_t bootstrap_seed = UINT32_C(0x4A6F7921);
constexpr std::uint32_t checksum_salt = UINT32_C(0x42544741);

enum class BootstrapMessage : std::uint8_t { Hello = 1, Start = 2, Ack = 3, Go = 4 };

struct BootstrapState {
    std::unique_ptr<UdpOnlineIo> udp;
    std::unique_ptr<OnlineMultiplayer> connection;
    std::uint64_t session_id = 0;
    std::uint32_t seed = bootstrap_seed;
    std::uint32_t nonce = 0;
    std::uint32_t sequence = 1;
    bool host = false;
    bool ready = false;
    bool peer_seen = false;
    std::string lobby_id;
    std::string host_secret;
    std::string status = "Offline";
    std::chrono::steady_clock::time_point next_send{};
};

std::mutex bootstrap_mutex;
std::optional<BootstrapState> bootstrap;
std::optional<std::uint32_t> negotiated_seed;
std::atomic_bool boot_released{false};

void put_u32(std::uint8_t* output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 24);
    output[1] = static_cast<std::uint8_t>(value >> 16);
    output[2] = static_cast<std::uint8_t>(value >> 8);
    output[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t get_u32(const std::uint8_t* input) {
    return (static_cast<std::uint32_t>(input[0]) << 24) |
           (static_cast<std::uint32_t>(input[1]) << 16) |
           (static_cast<std::uint32_t>(input[2]) << 8) | input[3];
}

std::uint32_t checksum(BootstrapMessage message, std::uint32_t seed, std::uint32_t nonce) {
    return checksum_salt ^ bootstrap_version ^ static_cast<std::uint8_t>(message) ^
           seed ^ ((nonce << 7) | (nonce >> 25));
}

void send(BootstrapState& state, std::uint16_t destination, BootstrapMessage message) {
    std::array<std::uint8_t, BTGA_NET_HEADER_SIZE + bootstrap_payload_size> packet{};
    BtgaNetHeader header{};
    header.protocol_version = BTGA_NET_PROTOCOL_VERSION;
    header.message_type = BTGA_NET_MSG_EVENT;
    header.flags = static_cast<std::uint8_t>(BTGA_NET_FLAG_RELIABLE | BTGA_NET_FLAG_RELAYED);
    header.session_id = state.session_id;
    header.sequence = state.sequence++;
    header.payload_size = static_cast<std::uint16_t>(bootstrap_payload_size);
    header.peer_id = state.connection->relay_slot();
    if (btga_net_encode_header(packet.data(), packet.size(), &header) != BTGA_NET_HEADER_SIZE) {
        return;
    }
    std::uint8_t* payload = packet.data() + BTGA_NET_HEADER_SIZE;
    payload[0] = bootstrap_version;
    payload[1] = static_cast<std::uint8_t>(message);
    put_u32(payload + 2, state.seed);
    put_u32(payload + 6, state.nonce);
    put_u32(payload + 10, checksum(message, state.seed, state.nonce));
    (void)state.connection->send_gameplay(destination, packet);
}

bool decode(const BootstrapState& state, std::span<const std::uint8_t> packet,
            std::uint16_t source, BootstrapMessage& message,
            std::uint32_t& seed, std::uint32_t& nonce) {
    BtgaNetHeader header{};
    if (!btga_net_decode_header(&header, packet.data(), packet.size()) ||
        header.protocol_version != BTGA_NET_PROTOCOL_VERSION ||
        header.message_type != BTGA_NET_MSG_EVENT || header.session_id != state.session_id ||
        header.peer_id != source || header.payload_size != bootstrap_payload_size ||
        packet.size() != BTGA_NET_HEADER_SIZE + bootstrap_payload_size) {
        return false;
    }
    const std::uint8_t* payload = packet.data() + BTGA_NET_HEADER_SIZE;
    if (payload[0] != bootstrap_version || payload[1] < 1 || payload[1] > 4) return false;
    message = static_cast<BootstrapMessage>(payload[1]);
    seed = get_u32(payload + 2);
    nonce = get_u32(payload + 6);
    return get_u32(payload + 10) == checksum(message, seed, nonce);
}

} // namespace

bool begin_preboot_online(bool host, std::uint64_t session_id,
                          const RelayEndpoint& relay, std::string_view token,
                          std::string lobby_id, std::string host_secret) {
    std::lock_guard lock(bootstrap_mutex);
    if (bootstrap || session_id == 0 || token.empty()) return false;
    BootstrapState state;
    state.udp = std::make_unique<UdpOnlineIo>(relay.address, relay.port);
    state.connection = std::make_unique<OnlineMultiplayer>(*state.udp);
    state.host = host;
    state.session_id = session_id;
    state.nonce = static_cast<std::uint32_t>(session_id) ^
                  static_cast<std::uint32_t>(session_id >> 32) ^ checksum_salt;
    state.lobby_id = std::move(lobby_id);
    state.host_secret = std::move(host_secret);
    state.status = "Binding to gameplay relay before boot...";
    if (!state.connection->enter_online() ||
        !state.connection->begin_relay(host, session_id, {
            reinterpret_cast<const std::uint8_t*>(token.data()), token.size()})) {
        return false;
    }
    bootstrap.emplace(std::move(state));
    boot_released.store(false, std::memory_order_release);
    negotiated_seed = bootstrap_seed;
    return true;
}

void poll_preboot_online() {
    std::lock_guard lock(bootstrap_mutex);
    if (!bootstrap || bootstrap->ready) return;
    BootstrapState& state = *bootstrap;
    if (state.connection->state() == OnlineState::RelayBinding) {
        if (!state.connection->poll_relay_binding()) return;
        state.status = state.host ? "Relay bound; waiting for joining player before boot..."
                                  : "Relay bound; negotiating synchronized cold boot...";
    }
    if (state.connection->state() != OnlineState::Lobby) return;

    for (;;) {
        std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
        std::uint16_t source = 0;
        const std::size_t size = state.connection->receive_gameplay(packet, source);
        if (size == 0) break;
        BootstrapMessage message{};
        std::uint32_t seed = 0;
        std::uint32_t nonce = 0;
        if (!decode(state, {packet.data(), size}, source, message, seed, nonce) ||
            nonce != state.nonce) continue;
        if (state.host && source == 1 && message == BootstrapMessage::Hello) {
            state.peer_seen = true;
            state.status = "Player joined; agreeing on cold-boot seed...";
        } else if (!state.host && source == 0 && message == BootstrapMessage::Start) {
            state.seed = seed;
            negotiated_seed = seed;
            state.peer_seen = true;
            send(state, 0, BootstrapMessage::Ack);
            state.status = "Seed accepted; waiting for synchronized start...";
        } else if (state.host && source == 1 && message == BootstrapMessage::Ack &&
                   state.peer_seen && seed == state.seed) {
            for (int copy = 0; copy < 16; ++copy) send(state, 1, BootstrapMessage::Go);
            state.ready = true;
            state.status = "Synchronized cold boot starting.";
        } else if (!state.host && source == 0 && message == BootstrapMessage::Go &&
                   state.peer_seen && seed == state.seed) {
            state.ready = true;
            state.status = "Synchronized cold boot starting.";
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < state.next_send) return;
    state.next_send = now + std::chrono::milliseconds(50);
    if (state.host) {
        if (state.peer_seen) send(state, 1, BootstrapMessage::Start);
    } else {
        send(state, 0, state.peer_seen ? BootstrapMessage::Ack : BootstrapMessage::Hello);
    }
}

bool preboot_online_ready() {
    std::lock_guard lock(bootstrap_mutex);
    return bootstrap && bootstrap->ready;
}

bool preboot_online_waiting() {
    std::lock_guard lock(bootstrap_mutex);
    return negotiated_seed.has_value() && !boot_released.load(std::memory_order_acquire);
}

void release_preboot_online_boot() {
    boot_released.store(true, std::memory_order_release);
}

std::string preboot_online_status() {
    std::lock_guard lock(bootstrap_mutex);
    return bootstrap ? bootstrap->status : "Offline";
}

std::optional<std::uint32_t> preboot_online_seed() {
    std::lock_guard lock(bootstrap_mutex);
    return negotiated_seed;
}

std::optional<PrebootOnlineSession> take_preboot_online_session() {
    std::lock_guard lock(bootstrap_mutex);
    if (!bootstrap || !bootstrap->ready) return std::nullopt;
    PrebootOnlineSession result;
    result.udp = std::move(bootstrap->udp);
    result.connection = std::move(bootstrap->connection);
    result.session_id = bootstrap->session_id;
    result.seed = bootstrap->seed;
    result.host = bootstrap->host;
    result.lobby_id = std::move(bootstrap->lobby_id);
    result.host_secret = std::move(bootstrap->host_secret);
    bootstrap.reset();
    return result;
}

} // namespace btga
