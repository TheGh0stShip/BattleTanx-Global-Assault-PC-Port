#include "btga/udp_online_io.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

extern "C" {
#include <btga/btga_relay.h>
}

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace btga {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket invalid_native_socket = INVALID_SOCKET;

void close_native_socket(NativeSocket socket) {
    closesocket(socket);
}

bool set_nonblocking(NativeSocket socket) {
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

bool would_block() {
    return WSAGetLastError() == WSAEWOULDBLOCK;
}
#else
using NativeSocket = int;
constexpr NativeSocket invalid_native_socket = -1;

void close_native_socket(NativeSocket socket) {
    ::close(socket);
}

bool set_nonblocking(NativeSocket socket) {
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool would_block() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}
#endif

NativeSocket from_storage(std::uintptr_t socket) {
    return static_cast<NativeSocket>(socket);
}

std::optional<std::string> environment_value(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0U;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result{value};
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::nullopt : std::optional<std::string>{value};
#endif
}

std::uint32_t fault_interval(const char* name) {
    const auto text = environment_value(name);
    if (!text) return 0U;
    std::uint32_t value = 0U;
    const auto* begin = text->data();
    const auto* finish = begin + text->size();
    const auto [end, error] = std::from_chars(begin, finish, value);
    return error == std::errc{} && end == finish && value >= 2U &&
            value <= 1'000'000U
        ? value : 0U;
}

} // namespace

UdpFaultProfile udp_test_fault_profile_from_environment() noexcept {
    try {
        const auto enabled = environment_value("BTGA_NET_TEST_FAULTS");
        if (!enabled || *enabled != "1") return {};
        return {
            fault_interval("BTGA_NET_TEST_DROP_OUTGOING_EVERY"),
            fault_interval("BTGA_NET_TEST_DROP_INCOMING_EVERY")
        };
    } catch (...) {
        return {};
    }
}

UdpOnlineIo::UdpOnlineIo(std::string relay_host, std::uint16_t relay_port,
                         UdpFaultProfile fault_profile)
    : relay_host_(std::move(relay_host)), relay_port_(relay_port),
      fault_profile_(fault_profile) {
    if (fault_profile_.drop_outgoing_every == 1U) {
        fault_profile_.drop_outgoing_every = 0U;
    }
    if (fault_profile_.drop_incoming_every == 1U) {
        fault_profile_.drop_incoming_every = 0U;
    }
}

UdpOnlineIo::~UdpOnlineIo() {
    close();
}

bool UdpOnlineIo::open() {
    if (is_open() || relay_host_.empty() || relay_port_ == 0) return false;

#if defined(_WIN32)
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    winsock_started_ = true;
#endif

    std::array<char, 6> port{};
    const int port_length = std::snprintf(port.data(), port.size(), "%u",
                                          static_cast<unsigned>(relay_port_));
    if (port_length <= 0 || static_cast<std::size_t>(port_length) >= port.size()) {
        close();
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(relay_host_.c_str(), port.data(), &hints, &addresses) != 0) {
        close();
        return false;
    }

    for (const addrinfo* address = addresses; address != nullptr;
         address = address->ai_next) {
        const NativeSocket candidate = socket(address->ai_family, address->ai_socktype,
                                               address->ai_protocol);
        if (candidate == invalid_native_socket) continue;
        if (::connect(candidate, address->ai_addr,
                      static_cast<int>(address->ai_addrlen)) == 0 &&
            set_nonblocking(candidate)) {
            socket_ = static_cast<std::uintptr_t>(candidate);
            break;
        }
        close_native_socket(candidate);
    }
    freeaddrinfo(addresses);
    if (!is_open()) close();
    return is_open();
}

void UdpOnlineIo::close() {
    if (is_open()) {
        close_native_socket(from_storage(socket_));
        socket_ = invalid_socket;
    }
#if defined(_WIN32)
    if (winsock_started_) {
        WSACleanup();
        winsock_started_ = false;
    }
#endif
}

bool UdpOnlineIo::send_relay(std::span<const std::uint8_t> datagram) {
    if (!is_open() || datagram.empty() ||
        datagram.size() > BTGA_RELAY_MAX_DATAGRAM_SIZE ||
        datagram.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    ++outgoing_datagrams_;
    if (fault_profile_.drop_outgoing_every != 0U &&
        outgoing_datagrams_ % fault_profile_.drop_outgoing_every == 0U) {
        return true;
    }
#if defined(_WIN32)
    const int sent = send(from_storage(socket_),
                          reinterpret_cast<const char*>(datagram.data()),
                          static_cast<int>(datagram.size()), 0);
#else
    const ssize_t sent = send(from_storage(socket_), datagram.data(), datagram.size(), 0);
#endif
    return sent >= 0 && static_cast<std::size_t>(sent) == datagram.size();
}

std::size_t UdpOnlineIo::receive_relay(std::span<std::uint8_t> datagram) {
    if (!is_open() || datagram.empty()) return 0;
    const std::size_t capacity = (std::min)(
        datagram.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)()));
    for (;;) {
#if defined(_WIN32)
        const int received = recv(from_storage(socket_),
                                  reinterpret_cast<char*>(datagram.data()),
                                  static_cast<int>(capacity), 0);
#else
        const ssize_t received = recv(from_storage(socket_), datagram.data(), capacity, 0);
#endif
        if (received < 0 && would_block()) return 0;
        if (received <= 0) return 0;
        ++incoming_datagrams_;
        if (fault_profile_.drop_incoming_every == 0U ||
            incoming_datagrams_ % fault_profile_.drop_incoming_every != 0U) {
            return static_cast<std::size_t>(received);
        }
    }
}

bool UdpOnlineIo::is_open() const noexcept {
    return socket_ != invalid_socket;
}

void UdpOnlineIo::set_fault_profile(UdpFaultProfile fault_profile) noexcept {
    fault_profile_ = fault_profile;
    if (fault_profile_.drop_outgoing_every == 1U) {
        fault_profile_.drop_outgoing_every = 0U;
    }
    if (fault_profile_.drop_incoming_every == 1U) {
        fault_profile_.drop_incoming_every = 0U;
    }
    outgoing_datagrams_ = 0U;
    incoming_datagrams_ = 0U;
}

} // namespace btga
