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
    [[nodiscard]] bool accept_relay_datagram(
        std::span<const std::uint8_t> datagram);
    [[nodiscard]] std::uint16_t relay_slot() const noexcept;

private:
    OnlineIo& io_;
    OnlineState state_ = OnlineState::Offline;
    bool io_open_ = false;
    BtgaRelayClient relay_{};
};

} // namespace btga
