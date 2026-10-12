#include "btga/master_client.hpp"
#include "btga/input_sync.hpp"
#include "btga/online_multiplayer.hpp"
#include "btga/udp_online_io.hpp"

extern "C" {
#include <btga/btga_net.h>
}

#include <array>
#include <chrono>
#include <iostream>
#include <span>
#include <string_view>
#include <thread>

namespace {
bool bind(btga::OnlineMultiplayer& online, bool host, std::uint64_t session,
          std::string_view token) {
    const auto bytes = std::span{
        reinterpret_cast<const std::uint8_t*>(token.data()), token.size()};
    if (!online.enter_online() || !online.begin_relay(host, session, bytes)) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (online.poll_relay_binding()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

int main() {
    try {
        auto http = btga::make_platform_http_client("https://btga.thegh0stship.com/api/v1");
        btga::MasterClient master{*http, "pc-native-e2e-v1"};
        btga::HostRegistration hosted = master.host("Native PC end-to-end test", 4);
        struct Cleanup {
            btga::MasterClient& master;
            btga::HostRegistration& hosted;
            ~Cleanup() { master.remove(hosted.lobby.id, hosted.host_secret); }
        } cleanup{master, hosted};

        const auto listed = master.list_lobbies();
        bool found = false;
        for (const auto& lobby : listed) found |= lobby.id == hosted.lobby.id;
        if (!found) throw std::runtime_error("hosted lobby was not discoverable");
        const btga::JoinRegistration joined = master.join(hosted.lobby.id);
        if (joined.session_id != hosted.session_id || joined.relay.address != hosted.relay.address ||
            joined.relay.port != hosted.relay.port) {
            throw std::runtime_error("host and join relay assignments disagree");
        }

        btga::UdpOnlineIo host_io{hosted.relay.address, hosted.relay.port};
        btga::UdpOnlineIo client_io{joined.relay.address, joined.relay.port};
        btga::OnlineMultiplayer host{host_io};
        btga::OnlineMultiplayer client{client_io};
        if (!bind(host, true, hosted.session_id, hosted.relay_token) ||
            !bind(client, false, joined.session_id, joined.join_token)) {
            throw std::runtime_error("native relay binding failed");
        }
        std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> gameplay{};
        btga::InputSynchronizer client_inputs;
        btga::InputSynchronizer host_inputs;
        const btga::OnlineControllerInput sent_input{0xA000, -64, 100};
        const std::size_t gameplay_size = client_inputs.encode(
            gameplay, hosted.session_id, client.relay_slot(), 1, 120, 1,
            sent_input);
        if (gameplay_size == 0 || !client.send_gameplay(0, {gameplay.data(), gameplay_size})) {
            throw std::runtime_error("native gameplay send failed");
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool received = false;
        while (std::chrono::steady_clock::now() < deadline) {
            std::uint16_t source = 0;
            const std::size_t size = host.receive_gameplay(gameplay, source);
            if (size != 0) {
                received = source == client.relay_slot() &&
                    host_inputs.accept({gameplay.data(), size}, hosted.session_id, source) &&
                    host_inputs.input_for(1, 120) == sent_input;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!received) throw std::runtime_error("native gameplay receive timed out");
        master.heartbeat(hosted.lobby.id, hosted.host_secret, 2, "playing");
        std::cout << "native HTTPS + public relay multiplayer test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
