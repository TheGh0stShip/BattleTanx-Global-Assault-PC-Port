#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace btga {

enum class ClientResyncDecision {
    request_checkpoint,
    quarantine,
};

// A divergent replica must stop immediately and ask the authority for a fresh
// checkpoint. Repeated failures in a short window indicate a non-portable
// state or incompatible build; quarantine that replica rather than allowing it
// to repeatedly acquire the shared recovery barrier and pause healthy players.
class ClientResyncPolicy {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t attempt_limit = 3U;
    static constexpr auto attempt_window = std::chrono::minutes{5};

    [[nodiscard]] ClientResyncDecision mismatch(Clock::time_point now) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::size_t recent_attempts(Clock::time_point now) const noexcept;

private:
    void prune(Clock::time_point now) noexcept;

    std::array<Clock::time_point, attempt_limit> attempts_{};
    std::size_t begin_ = 0U;
    std::size_t size_ = 0U;
};

// Separates reachability (subscriptions/keepalives) from advancing controller
// production. Only a strictly newer input horizon refreshes input liveness;
// duplicated UDP packets and a wedged client repeating one batch cannot keep
// its last held controls active.
class ClientInputLiveness {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t player_limit = 4U;

    void reset() noexcept;
    void observe_transport(std::uint8_t seat, Clock::time_point now) noexcept;
    [[nodiscard]] bool observe_input(
        std::uint8_t seat, std::uint32_t horizon,
        Clock::time_point now) noexcept;
    void clear(std::uint8_t seat) noexcept;

    [[nodiscard]] bool seen(std::uint8_t seat) const noexcept;
    [[nodiscard]] bool input_stale(
        std::uint8_t seat, Clock::time_point now,
        Clock::duration timeout) const noexcept;
    [[nodiscard]] bool transport_stale(
        std::uint8_t seat, Clock::time_point now,
        Clock::duration timeout) const noexcept;

private:
    struct Seat {
        Clock::time_point transport{};
        Clock::time_point input{};
        std::uint32_t horizon = 0U;
        bool seen = false;
        bool have_horizon = false;
    };
    std::array<Seat, player_limit> seats_{};
};

} // namespace btga
