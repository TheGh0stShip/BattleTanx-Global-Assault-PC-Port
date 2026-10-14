#pragma once

#include "btga/recovery_barrier.hpp"

#include <array>
#include <chrono>
#include <cstdint>

namespace btga {

enum class BootstrapRequest : std::uint8_t {
    continue_generation,
    start_generation,
    supersede_initial_generation,
};

enum class BootstrapAckKind : std::uint8_t {
    ignore,
    replay,
    ready,
};

struct BootstrapAck {
    BootstrapAckKind kind = BootstrapAckKind::ignore;
    std::uint32_t next_tick = 0;
};

// Owns the recovery protocol state for all authenticated seats. Transport
// retries remain outside this class; generation identity, installation proof,
// replay position and barrier release do not. A request does not acquire the
// shared barrier: the matching-generation acknowledgment is the proof that the
// endpoint can receive recovery traffic, and only then may it pause simulation.
class BootstrapRecovery {
public:
    static constexpr std::uint8_t player_limit = RecoveryBarrier::player_limit;

    void reset() noexcept;
    [[nodiscard]] BootstrapRequest request_cold(
        std::uint8_t seat, std::uint32_t latest_checkpoint_tick) noexcept;
    void snapshot_sent(std::uint8_t seat, std::uint32_t snapshot_id,
                       std::uint32_t authority_head) noexcept;
    [[nodiscard]] BootstrapAck acknowledge(
        std::uint8_t seat, std::uint32_t encoded_next_tick,
        std::uint32_t snapshot_id, std::uint32_t authority_head,
        bool first_subscription) noexcept;
    void disconnect(std::uint8_t seat) noexcept;

    [[nodiscard]] bool pending(std::uint8_t seat) const noexcept;
    [[nodiscard]] std::uint32_t expected_ack(std::uint8_t seat) const noexcept;
    [[nodiscard]] std::uint32_t snapshot_id(std::uint8_t seat) const noexcept;
    [[nodiscard]] std::uint8_t pending_mask() const noexcept;
    [[nodiscard]] bool paused() const noexcept;
    [[nodiscard]] std::uint8_t mask() const noexcept;

private:
    struct Seat {
        bool pending = false;
        std::uint32_t expected_ack = 0;
        std::uint32_t snapshot_id = 0;
    };
    std::array<Seat, player_limit> seats_{};
    RecoveryBarrier barrier_;
};

// Bounds the time a proven endpoint may hold the shared replay barrier without
// advancing its acknowledged cursor. Packet liveness is intentionally separate:
// a peer can remain reachable while losing every journal datagram.
class RecoveryProgressWatchdog {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::uint8_t player_limit = BootstrapRecovery::player_limit;

    void reset() noexcept;
    void observe(std::uint8_t seat, std::uint32_t next_tick,
                 Clock::time_point now) noexcept;
    void clear(std::uint8_t seat) noexcept;
    [[nodiscard]] bool stalled(std::uint8_t seat, Clock::time_point now,
                               Clock::duration timeout) const noexcept;
    [[nodiscard]] std::uint32_t next_tick(std::uint8_t seat) const noexcept;

private:
    struct Seat {
        Clock::time_point last_progress{};
        std::uint32_t next_tick = 0U;
        bool observed = false;
    };
    std::array<Seat, player_limit> seats_{};
};

} // namespace btga
