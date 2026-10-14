#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>

#include "btga/delayed_datagram_queue.hpp"
#include "btga/online_multiplayer.hpp"

namespace btga {

struct UdpFaultProfile {
    // Zero disables a direction. Values >= 2 discard each Nth datagram while
    // reporting a successful send/empty receive, modelling network loss rather
    // than an application-visible socket error.
    std::uint32_t drop_outgoing_every = 0;
    std::uint32_t drop_incoming_every = 0;
    // Test-only one-way delay. Fifty milliseconds in each endpoint direction
    // produces approximately 100 ms RTT without changing the host network.
    std::uint32_t delay_milliseconds = 0;
};

// Test-only environment adapter. It returns an empty profile unless
// BTGA_NET_TEST_FAULTS=1. Production callers that do not opt in retain the
// normal transport exactly.
[[nodiscard]] UdpFaultProfile udp_test_fault_profile_from_environment() noexcept;

// Concrete relay transport. Construction is inert: DNS resolution and socket
// creation happen only when OnlineMultiplayer::enter_online calls open().
class UdpOnlineIo final : public OnlineIo {
public:
    UdpOnlineIo(std::string relay_host, std::uint16_t relay_port,
                UdpFaultProfile fault_profile = {});
    ~UdpOnlineIo() override;

    UdpOnlineIo(const UdpOnlineIo&) = delete;
    UdpOnlineIo& operator=(const UdpOnlineIo&) = delete;

    bool open() override;
    void close() override;
    bool send_relay(std::span<const std::uint8_t> datagram) override;
    std::size_t receive_relay(std::span<std::uint8_t> datagram) override;

    [[nodiscard]] bool is_open() const noexcept;
    void set_fault_profile(UdpFaultProfile fault_profile) noexcept;

private:
    [[nodiscard]] bool send_now(std::span<const std::uint8_t> datagram);
    void flush_delayed_outgoing();

    static constexpr std::uintptr_t invalid_socket = ~std::uintptr_t{0};

    std::string relay_host_;
    std::uint16_t relay_port_;
    UdpFaultProfile fault_profile_;
    std::uint64_t outgoing_datagrams_ = 0;
    std::uint64_t incoming_datagrams_ = 0;
    bool send_failure_reported_ = false;
    bool delay_queue_full_reported_ = false;
    DelayedDatagramQueue delayed_outgoing_;
    std::uintptr_t socket_ = invalid_socket;
#if defined(_WIN32)
    bool winsock_started_ = false;
#endif
};

} // namespace btga
