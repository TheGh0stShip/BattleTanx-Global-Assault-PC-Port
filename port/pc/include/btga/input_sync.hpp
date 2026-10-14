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

struct OnlineSyncOrigin {
    std::uint32_t sender_tick = 0;
    std::uint32_t receiver_tick = 0;

    friend bool operator==(const OnlineSyncOrigin&, const OnlineSyncOrigin&) = default;
};

// Assign a newly observed controller sample to a tick that is still useful to
// the authority. A recovering replica may intentionally render several ticks
// behind the live head; stamping from that presentation tick would make every
// input late even though the client remains healthy.
[[nodiscard]] std::uint32_t choose_authority_input_tick(
    std::uint32_t replica_tick, std::uint32_t authority_head,
    std::uint32_t replica_lead = 2U, std::uint32_t authority_lead = 4U) noexcept;

// Stores inputs that were actually sampled for a simulation tick. Re-sending
// a batch provides loss redundancy, but never invents commands for future
// ticks and never changes a tick after its first sample was committed locally.
class TickInputHistory {
public:
    static constexpr std::size_t capacity = 64;
    static constexpr std::size_t max_batch = 8;

    struct Batch {
        std::uint32_t first_tick = 0;
        std::array<OnlineControllerInput, max_batch> inputs{};
        std::size_t count = 0;
    };

    void reset() noexcept;
    [[nodiscard]] bool record(std::uint32_t tick,
                              OnlineControllerInput input) noexcept;
    [[nodiscard]] Batch batch_ending_at(std::uint32_t tick) const noexcept;

private:
    struct Sample {
        OnlineControllerInput input{};
        std::uint32_t tick = 0;
        bool valid = false;
    };
    std::array<Sample, capacity> samples_{};
};

// Encodes and validates the portable gameplay datagrams carried by the relay.
// This class deliberately contains no sockets, clocks, host pointers, or native
// struct serialization so the same bytes can be used by every target platform.
class InputSynchronizer {
public:
    static constexpr std::size_t max_players = 4;
    static constexpr std::size_t history_size = 64;

    [[nodiscard]] std::size_t encode(std::span<std::uint8_t> packet,
                                     std::uint64_t session_id,
                                     std::uint16_t peer_id,
                                     std::uint32_t sequence,
                                     std::uint32_t tick,
                                     std::uint8_t player_id,
                                     OnlineControllerInput input) const;

    [[nodiscard]] std::size_t encode_batch(
        std::span<std::uint8_t> packet, std::uint64_t session_id,
        std::uint16_t peer_id, std::uint32_t sequence,
        std::uint32_t first_tick, std::uint8_t player_id,
        std::span<const OnlineControllerInput> inputs,
        std::optional<OnlineSyncOrigin> sync_origin = std::nullopt) const;

    void record_local(std::uint8_t player_id, std::uint32_t tick,
                      std::uint32_t sequence, OnlineControllerInput input);

    [[nodiscard]] bool accept(std::span<const std::uint8_t> packet,
                              std::uint64_t expected_session,
                              std::uint16_t expected_peer);

    [[nodiscard]] std::optional<OnlineControllerInput> input_for(
        std::uint8_t player_id, std::uint32_t tick) const;
    [[nodiscard]] std::optional<OnlineControllerInput> latest_input_for(
        std::uint8_t player_id) const;
    [[nodiscard]] std::optional<std::uint32_t> latest_tick_for(
        std::uint8_t player_id) const;
    [[nodiscard]] std::optional<OnlineSyncOrigin> sync_origin_for(
        std::uint8_t player_id) const;

private:
    struct StoredInput {
        OnlineControllerInput input{};
        std::uint32_t tick = 0;
        std::uint32_t sequence = 0;
        bool valid = false;
    };
    struct PlayerHistory {
        std::array<StoredInput, history_size> inputs{};
        OnlineControllerInput latest_input{};
        std::uint32_t latest_tick = 0;
        std::uint32_t latest_sequence = 0;
        OnlineSyncOrigin sync_origin{};
        bool has_sync_origin = false;
        bool valid = false;
    };
    std::array<PlayerHistory, max_players> histories_{};
};

// Reconstructs the complete delayed-input window after an authoritative
// timeline rebase. Without this, the first post-snapshot frame waits forever
// for pre-rebase ticks that no peer will retransmit.
void prime_frame_input_epoch(InputSynchronizer& synchronizer,
                             std::uint32_t authoritative_generation,
                             std::uint8_t player_count = 2);

} // namespace btga
