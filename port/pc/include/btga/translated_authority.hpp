#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "btga/authoritative_session.hpp"

namespace btga {

// Authority adapter for the translated N64 runtime. The translated game owns
// the call stack and invokes the two boundaries around its real gameplay tick;
// this object owns input selection and canonical state publication.
class TranslatedAuthority {
public:
    explicit TranslatedAuthority(std::uint8_t occupied_mask = 0x0fU,
                                 std::uint32_t snapshot_interval = 30U,
                                 std::uint32_t jitter_buffer_ticks = 3U);

    [[nodiscard]] bool submit(std::uint8_t seat, std::uint32_t tick,
                              OnlineControllerInput input);
    [[nodiscard]] bool ingest_client_packet(
        std::span<const std::uint8_t> packet, std::uint64_t session_id,
        std::uint16_t relay_source_slot);
    [[nodiscard]] bool set_connected(std::uint8_t seat, bool connected);

    // Called once at the entry of the translated gameplay update. Returns the
    // immutable controller inputs that get_input must expose for this tick.
    [[nodiscard]] bool ready(std::uint32_t server_tick) const noexcept;
    [[nodiscard]] bool begin_tick(std::uint32_t server_tick);
    [[nodiscard]] std::span<const OnlineControllerInput, online_player_limit>
    inputs() const noexcept;

    // Called once at the matching exit after the translated game has mutated
    // RDRAM. A repeated or out-of-order completion is rejected.
    [[nodiscard]] std::optional<AuthoritativeFrame> finish_tick(
        std::uint32_t tick, std::span<const std::uint8_t> rdram);

    [[nodiscard]] const std::optional<AuthoritativeSnapshot>&
    latest_snapshot() const noexcept;
    [[nodiscard]] std::uint32_t next_tick() const noexcept;
    [[nodiscard]] std::uint8_t occupied_mask() const noexcept;
    [[nodiscard]] std::uint8_t connected_mask() const noexcept;

private:
    struct TickInputs {
        std::uint32_t tick = 0;
        std::uint8_t present_mask = 0;
        std::array<OnlineControllerInput, online_player_limit> inputs{};
    };

    static constexpr std::size_t history_size = 256;
    [[nodiscard]] static std::uint64_t digest(
        std::span<const std::uint8_t> bytes) noexcept;

    std::array<TickInputs, history_size> history_{};
    std::array<OnlineControllerInput, online_player_limit> current_inputs_{};
    std::array<OnlineControllerInput, online_player_limit> last_inputs_{};
    std::array<std::uint32_t, online_player_limit> latest_sequences_{};
    std::array<bool, online_player_limit> have_sequence_{};
    std::optional<AuthoritativeSnapshot> snapshot_;
    std::uint32_t next_tick_ = 0;
    std::uint32_t active_tick_ = 0;
    std::uint32_t snapshot_interval_ = 30;
    std::uint32_t jitter_buffer_ticks_ = 3;
    std::uint8_t occupied_mask_ = 0x0fU;
    std::uint8_t connected_mask_ = 0x0fU;
    std::uint8_t received_mask_ = 0;
    bool tick_active_ = false;
};

} // namespace btga
