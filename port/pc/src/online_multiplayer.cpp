#include "btga/online_multiplayer.hpp"

#include <array>
#include <cstring>

namespace btga {

OnlineMultiplayer::OnlineMultiplayer(OnlineIo& io) : io_(io) {
    btga_relay_client_init(&relay_);
}

OnlineMultiplayer::~OnlineMultiplayer() {
    leave_online();
}

OnlineState OnlineMultiplayer::state() const noexcept {
    return state_;
}

bool OnlineMultiplayer::enter_online() {
    if (state_ != OnlineState::Offline) return false;
    if (!io_.open()) {
        state_ = OnlineState::Error;
        return false;
    }
    io_open_ = true;
    state_ = OnlineState::Browsing;
    return true;
}

void OnlineMultiplayer::leave_online() noexcept {
    btga_relay_client_disconnect(&relay_);
    if (io_open_) {
        io_.close();
        io_open_ = false;
    }
    state_ = OnlineState::Offline;
}

bool OnlineMultiplayer::begin_relay(
    bool host, std::uint64_t session_id,
    std::span<const std::uint8_t> admission_token) {
    std::array<std::uint8_t, BTGA_RELAY_MAX_DATAGRAM_SIZE> datagram{};
    if (state_ != OnlineState::Browsing || admission_token.empty() ||
        !btga_relay_client_begin(
            &relay_, host ? BTGA_RELAY_ROLE_HOST : BTGA_RELAY_ROLE_CLIENT,
            session_id, admission_token.data(), admission_token.size())) {
        return false;
    }
    const std::size_t size = btga_relay_client_make_bind(
        &relay_, datagram.data(), datagram.size());
    if (size == 0 || !io_.send_relay({datagram.data(), size})) {
        btga_relay_client_disconnect(&relay_);
        state_ = OnlineState::Error;
        return false;
    }
    state_ = OnlineState::RelayBinding;
    return true;
}

bool OnlineMultiplayer::accept_relay_datagram(
    std::span<const std::uint8_t> datagram) {
    if (state_ != OnlineState::RelayBinding ||
        !btga_relay_client_accept_bound(&relay_, datagram.data(),
                                        datagram.size())) {
        return false;
    }
    state_ = OnlineState::Lobby;
    return true;
}

bool OnlineMultiplayer::poll_relay_binding() {
    std::array<std::uint8_t, BTGA_RELAY_MAX_DATAGRAM_SIZE> datagram{};
    if (state_ != OnlineState::RelayBinding) return false;
    const std::size_t size = io_.receive_relay(datagram);
    return size != 0 && accept_relay_datagram({datagram.data(), size});
}

bool OnlineMultiplayer::send_gameplay(
    std::uint16_t destination_slot,
    std::span<const std::uint8_t> inner_datagram) {
    std::array<std::uint8_t, BTGA_RELAY_MAX_DATAGRAM_SIZE> relay_datagram{};
    if (state_ != OnlineState::Lobby) return false;
    const std::size_t size = btga_relay_client_wrap(
        &relay_, destination_slot, inner_datagram.data(), inner_datagram.size(),
        relay_datagram.data(), relay_datagram.size());
    return size != 0 && io_.send_relay({relay_datagram.data(), size});
}

std::size_t OnlineMultiplayer::receive_gameplay(
    std::span<std::uint8_t> destination, std::uint16_t& source_slot) {
    std::array<std::uint8_t, BTGA_RELAY_MAX_DATAGRAM_SIZE> relay_datagram{};
    const std::uint8_t* inner = nullptr;
    std::size_t inner_size = 0;
    if (state_ != OnlineState::Lobby) return 0;
    const std::size_t size = io_.receive_relay(relay_datagram);
    if (size == 0 || !btga_relay_client_unwrap(
            &relay_, relay_datagram.data(), size, &inner, &inner_size, &source_slot) ||
        inner_size > destination.size()) {
        return 0;
    }
    std::memcpy(destination.data(), inner, inner_size);
    return inner_size;
}

std::uint16_t OnlineMultiplayer::relay_slot() const noexcept {
    return relay_.relay_slot;
}

} // namespace btga
