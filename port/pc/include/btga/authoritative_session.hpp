#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "btga/input_sync.hpp"

namespace btga {

constexpr std::size_t online_player_limit = 4;

struct AuthoritativeFrame {
    std::uint32_t tick = 0;
    std::array<OnlineControllerInput, online_player_limit> inputs{};
    std::uint64_t state_digest = 0;
    std::uint8_t received_mask = 0;
};

struct AuthoritativeSnapshot {
    std::uint32_t next_tick = 0;
    std::uint64_t state_digest = 0;
    std::vector<std::uint8_t> state;
};

// The dedicated server owns this interface.  A translated-game backend and a
// future fully native backend implement the same four-player simulation
// contract; transport, rendering and client prediction are deliberately not
// part of it.
class AuthoritativeSimulation {
public:
    virtual ~AuthoritativeSimulation() = default;
    virtual bool run_tick(
        std::uint32_t tick,
        std::span<const OnlineControllerInput, online_player_limit> inputs) = 0;
    [[nodiscard]] virtual std::uint64_t state_digest() const = 0;
    virtual bool save_state(std::vector<std::uint8_t>& destination) const = 0;
};

class AuthoritativeSession {
public:
    static constexpr std::size_t input_history = 256;

    explicit AuthoritativeSession(std::uint8_t occupied_mask = 0x0fU,
                                  std::uint32_t snapshot_interval = 30U,
                                  std::uint32_t jitter_buffer_ticks = 3U);

    [[nodiscard]] std::uint32_t next_tick() const noexcept;
    [[nodiscard]] std::uint8_t occupied_mask() const noexcept;
    [[nodiscard]] std::uint8_t connected_mask() const noexcept;
    [[nodiscard]] bool set_connected(std::uint8_t seat, bool connected);
    [[nodiscard]] bool submit(std::uint8_t seat, std::uint32_t tick,
                              OnlineControllerInput input);
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool due(std::uint32_t server_tick) const noexcept;
    [[nodiscard]] std::optional<AuthoritativeFrame> advance(
        AuthoritativeSimulation& simulation);
    [[nodiscard]] std::optional<AuthoritativeFrame> advance_due(
        AuthoritativeSimulation& simulation, std::uint32_t server_tick);
    [[nodiscard]] const std::optional<AuthoritativeSnapshot>& latest_snapshot() const noexcept;

private:
    struct TickInputs {
        std::uint32_t tick = 0;
        std::uint8_t present_mask = 0;
        std::array<OnlineControllerInput, online_player_limit> inputs{};
    };

    std::array<TickInputs, input_history> history_{};
    std::array<OnlineControllerInput, online_player_limit> last_inputs_{};
    std::optional<AuthoritativeSnapshot> snapshot_;
    std::uint32_t next_tick_ = 0;
    std::uint32_t snapshot_interval_ = 30;
    std::uint32_t jitter_buffer_ticks_ = 3;
    std::uint8_t occupied_mask_ = 0x0fU;
    std::uint8_t connected_mask_ = 0x0fU;
};

} // namespace btga
