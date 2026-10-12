#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "btga/online_multiplayer.hpp"

namespace btga {

// Concrete relay transport. Construction is inert: DNS resolution and socket
// creation happen only when OnlineMultiplayer::enter_online calls open().
class UdpOnlineIo final : public OnlineIo {
public:
    UdpOnlineIo(std::string relay_host, std::uint16_t relay_port);
    ~UdpOnlineIo() override;

    UdpOnlineIo(const UdpOnlineIo&) = delete;
    UdpOnlineIo& operator=(const UdpOnlineIo&) = delete;

    bool open() override;
    void close() override;
    bool send_relay(std::span<const std::uint8_t> datagram) override;
    std::size_t receive_relay(std::span<std::uint8_t> datagram) override;

    [[nodiscard]] bool is_open() const noexcept;

private:
    static constexpr std::uintptr_t invalid_socket = ~std::uintptr_t{0};

    std::string relay_host_;
    std::uint16_t relay_port_;
    std::uintptr_t socket_ = invalid_socket;
#if defined(_WIN32)
    bool winsock_started_ = false;
#endif
};

} // namespace btga
