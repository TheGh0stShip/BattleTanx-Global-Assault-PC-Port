#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" {
#include <btga/btga_relay_client.h>
}

namespace btga {

enum class OnlineState {
    Offline,
    Browsing,
    RelayBinding,
    Lobby,
    Error,
};

class OnlineIo {
public:
    virtual ~OnlineIo() = default;
    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool send_relay(std::span<const std::uint8_t> datagram) = 0;
    virtual std::size_t receive_relay(std::span<std::uint8_t> datagram) = 0;
};

class OnlineMultiplayer {
public:
    explicit OnlineMultiplayer(OnlineIo& io);
    ~OnlineMultiplayer();

    OnlineMultiplayer(const OnlineMultiplayer&) = delete;
    OnlineMultiplayer& operator=(const OnlineMultiplayer&) = delete;

    [[nodiscard]] OnlineState state() const noexcept;
    [[nodiscard]] bool enter_online();
    void leave_online() noexcept;
    [[nodiscard]] bool begin_relay(bool host, std::uint64_t session_id,
                                   std::span<const std::uint8_t> admission_token);
    // Reuses the open UDP socket with a freshly signed admission token. This
    // refreshes NAT mappings and recreates relay state after a data-plane
    // restart without reopening the game session.
    [[nodiscard]] bool rebind_relay(bool host, std::uint64_t session_id,
                                    std::span<const std::uint8_t> admission_token);
    [[nodiscard]] bool accept_relay_datagram(
        std::span<const std::uint8_t> datagram);
    [[nodiscard]] bool poll_relay_binding();
    [[nodiscard]] bool send_gameplay(
        std::uint16_t destination_slot,
        std::span<const std::uint8_t> inner_datagram);
    [[nodiscard]] std::size_t receive_gameplay(
        std::span<std::uint8_t> destination, std::uint16_t& source_slot);
    [[nodiscard]] std::uint16_t relay_slot() const noexcept;

private:
    OnlineIo& io_;
    OnlineState state_ = OnlineState::Offline;
    bool io_open_ = false;
    BtgaRelayClient relay_{};
    bool gameplay_send_failure_reported_ = false;
};

} // namespace btga
