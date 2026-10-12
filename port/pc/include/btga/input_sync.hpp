#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace btga {

struct OnlineControllerInput {
    std::uint16_t buttons = 0;
    std::int8_t stick_x = 0;
    std::int8_t stick_y = 0;

    friend bool operator==(const OnlineControllerInput&, const OnlineControllerInput&) = default;
};

// Encodes and validates the portable gameplay datagrams carried by the relay.
// This class deliberately contains no sockets, clocks, host pointers, or native
// struct serialization so the same bytes can be used by every target platform.
class InputSynchronizer {
public:
    static constexpr std::size_t max_players = 16;

    [[nodiscard]] std::size_t encode(std::span<std::uint8_t> packet,
                                     std::uint64_t session_id,
                                     std::uint16_t peer_id,
                                     std::uint32_t sequence,
                                     std::uint32_t tick,
                                     std::uint8_t player_id,
                                     OnlineControllerInput input) const;

    [[nodiscard]] bool accept(std::span<const std::uint8_t> packet,
                              std::uint64_t expected_session,
                              std::uint16_t expected_peer);

    [[nodiscard]] std::optional<OnlineControllerInput> input_for(
        std::uint8_t player_id, std::uint32_t tick) const;
    [[nodiscard]] std::optional<OnlineControllerInput> latest_input_for(
        std::uint8_t player_id) const;

private:
    struct StoredInput {
        OnlineControllerInput input{};
        std::uint32_t tick = 0;
        std::uint32_t sequence = 0;
        bool valid = false;
    };
    std::array<StoredInput, max_players> inputs_{};
};

} // namespace btga
