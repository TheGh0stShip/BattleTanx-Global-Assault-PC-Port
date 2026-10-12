#include "btga/online_multiplayer.hpp"
#include "btga/udp_online_io.hpp"

extern "C" {
#include <btga/btga_net.h>
}

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace {
constexpr std::string_view marker = "pc-port-live-smoke";

bool wait_for_binding(btga::OnlineMultiplayer& online) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (online.poll_relay_binding()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::size_t make_ping(std::span<std::uint8_t> packet, std::uint64_t session,
                      std::uint16_t peer) {
    BtgaNetHeader header{};
    header.protocol_version = BTGA_NET_PROTOCOL_VERSION;
    header.message_type = BTGA_NET_MSG_PING;
    header.flags = BTGA_NET_FLAG_RELAYED;
    header.session_id = session;
    header.payload_size = static_cast<std::uint16_t>(marker.size());
    header.peer_id = peer;
    const std::size_t header_size = btga_net_encode_header(
        packet.data(), packet.size(), &header);
    if (header_size == 0 || header_size + marker.size() > packet.size()) return 0;
    std::memcpy(packet.data() + header_size, marker.data(), marker.size());
    return header_size + marker.size();
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 6 || (std::string_view(argv[1]) != "host" &&
                      std::string_view(argv[1]) != "client")) {
        std::cerr << "usage: btga_pc_live_multiplayer_test host|client RELAY PORT SESSION_HEX TOKEN\n";
        return 2;
    }
    const bool host = std::string_view(argv[1]) == "host";
    const unsigned long port_value = std::stoul(argv[3]);
    std::uint64_t session = 0;
    if (port_value == 0 || port_value > 65535 ||
        !btga_net_parse_session_id(&session, argv[4], std::strlen(argv[4]))) {
        std::cerr << "invalid relay port or session id\n";
        return 2;
    }

    btga::UdpOnlineIo io{argv[2], static_cast<std::uint16_t>(port_value)};
    btga::OnlineMultiplayer online{io};
    const auto token = std::span{
        reinterpret_cast<const std::uint8_t*>(argv[5]), std::strlen(argv[5])};
    if (!online.enter_online() || !online.begin_relay(host, session, token) ||
        !wait_for_binding(online)) {
        std::cerr << "relay binding failed\n";
        return 1;
    }
    std::cout << "BOUND " << online.relay_slot() << '\n' << std::flush;

    if (!host) {
        std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
        const std::size_t size = make_ping(packet, session, online.relay_slot());
        if (size == 0 || !online.send_gameplay(0, {packet.data(), size})) {
            std::cerr << "client gameplay send failed\n";
            return 1;
        }
        return 0;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
        std::uint16_t source = 0;
        const std::size_t size = online.receive_gameplay(packet, source);
        if (size != 0) {
            BtgaNetHeader header{};
            if (!btga_net_decode_header(&header, packet.data(), size) || source == 0 ||
                header.message_type != BTGA_NET_MSG_PING ||
                header.payload_size != marker.size() ||
                std::memcmp(packet.data() + BTGA_NET_HEADER_SIZE,
                            marker.data(), marker.size()) != 0) {
                std::cerr << "host received invalid gameplay datagram\n";
                return 1;
            }
            std::cout << "RECEIVED " << source << '\n';
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cerr << "host gameplay receive timed out\n";
    return 1;
}
