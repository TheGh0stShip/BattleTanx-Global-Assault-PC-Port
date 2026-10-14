#include "btga/input_sync.hpp"

extern "C" {
#include <btga/btga_gameplay.h>
#include <btga/btga_net.h>
}

#include <algorithm>
#include <array>
#include <cstring>

namespace btga {

std::uint32_t choose_authority_input_tick(
    std::uint32_t replica_tick, std::uint32_t authority_head,
    std::uint32_t replica_lead, std::uint32_t authority_lead) noexcept {
    const std::uint32_t replica_target = replica_tick + replica_lead;
    const std::uint32_t authority_target = authority_head + authority_lead;
    return static_cast<std::int32_t>(authority_target - replica_target) > 0
        ? authority_target : replica_target;
}

void TickInputHistory::reset() noexcept { samples_ = {}; }

bool TickInputHistory::record(
    std::uint32_t tick, OnlineControllerInput input) noexcept {
    Sample& sample = samples_[tick % capacity];
    if (sample.valid && sample.tick == tick) return sample.input == input;
    sample = {input, tick, true};
    return true;
}

TickInputHistory::Batch TickInputHistory::batch_ending_at(
    std::uint32_t tick) const noexcept {
    Batch batch{};
    std::size_t count = 0;
    std::uint32_t first = tick;
    std::uint32_t candidate = tick;
    for (;;) {
        const Sample& sample = samples_[candidate % capacity];
        if (!sample.valid || sample.tick != candidate) break;
        first = candidate;
        ++count;
        if (count == max_batch || candidate == 0U) break;
        --candidate;
    }
    if (count == 0U) return batch;
    batch.first_tick = first;
    batch.count = count;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint32_t sample_tick = first + static_cast<std::uint32_t>(index);
        batch.inputs[index] = samples_[sample_tick % capacity].input;
    }
    return batch;
}

std::size_t InputSynchronizer::encode(
    std::span<std::uint8_t> packet, std::uint64_t session_id,
    std::uint16_t peer_id, std::uint32_t sequence, std::uint32_t tick,
    std::uint8_t player_id, OnlineControllerInput input) const {
    const std::array inputs{input};
    return encode_batch(packet, session_id, peer_id, sequence, tick,
                        player_id, inputs);
}

std::size_t InputSynchronizer::encode_batch(
    std::span<std::uint8_t> packet, std::uint64_t session_id,
    std::uint16_t peer_id, std::uint32_t sequence, std::uint32_t first_tick,
    std::uint8_t player_id, std::span<const OnlineControllerInput> inputs,
    std::optional<OnlineSyncOrigin> sync_origin) const {
    if (player_id >= max_players || inputs.empty() ||
        inputs.size() > BTGA_NET_MAX_INPUT_FRAMES ||
        packet.size() < BTGA_NET_HEADER_SIZE) return 0;

    BtgaNetInputBatch batch{};
    batch.first_tick = first_tick;
    batch.player_id = player_id;
    batch.frame_count = static_cast<std::uint8_t>(inputs.size());
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        batch.frames[index].buttons = inputs[index].buttons;
        batch.frames[index].stick_x = inputs[index].stick_x;
        batch.frames[index].stick_y = inputs[index].stick_y;
    }

    std::array<std::uint8_t, BTGA_NET_MAX_PAYLOAD_SIZE> payload{};
    const std::size_t payload_size = btga_net_encode_input_batch(
        payload.data(), payload.size(), &batch);
    if (payload_size == 0 || BTGA_NET_HEADER_SIZE + payload_size > packet.size()) return 0;

    BtgaNetHeader header{};
    header.protocol_version = BTGA_NET_PROTOCOL_VERSION;
    header.message_type = BTGA_NET_MSG_INPUT;
    header.flags = static_cast<std::uint8_t>(BTGA_NET_FLAG_RELAYED |
        (sync_origin ? static_cast<unsigned>(BTGA_NET_FLAG_RELIABLE) : 0U));
    header.session_id = session_id;
    header.sequence = sequence;
    if (sync_origin) {
        header.acknowledgment = sync_origin->sender_tick;
        header.acknowledgment_bits = sync_origin->receiver_tick;
    }
    header.payload_size = static_cast<std::uint16_t>(payload_size);
    header.peer_id = peer_id;
    if (btga_net_encode_header(packet.data(), packet.size(), &header) !=
        BTGA_NET_HEADER_SIZE) return 0;
    std::memcpy(packet.data() + BTGA_NET_HEADER_SIZE, payload.data(), payload_size);
    return BTGA_NET_HEADER_SIZE + payload_size;
}

bool InputSynchronizer::accept(std::span<const std::uint8_t> packet,
                               std::uint64_t expected_session,
                               std::uint16_t expected_peer) {
    BtgaNetHeader header{};
    if (!btga_net_decode_header(&header, packet.data(), packet.size()) ||
        header.protocol_version != BTGA_NET_PROTOCOL_VERSION ||
        header.message_type != BTGA_NET_MSG_INPUT ||
        header.session_id != expected_session || header.peer_id != expected_peer ||
        header.payload_size != packet.size() - BTGA_NET_HEADER_SIZE) return false;

    BtgaNetInputBatch batch{};
    if (!btga_net_decode_input_batch(
            &batch, packet.data() + BTGA_NET_HEADER_SIZE, header.payload_size) ||
        batch.player_id != expected_peer) return false;

    PlayerHistory& history = histories_[batch.player_id];
    if (history.valid && header.sequence <= history.latest_sequence) return true;
    for (std::uint8_t index = 0; index < batch.frame_count; ++index) {
        const std::uint32_t tick = batch.first_tick + index;
        StoredInput& stored = history.inputs[tick % history_size];
        stored.input = {
            batch.frames[index].buttons,
            batch.frames[index].stick_x,
            batch.frames[index].stick_y,
        };
        stored.tick = tick;
        stored.sequence = header.sequence;
        stored.valid = true;
        if (!history.valid || tick >= history.latest_tick) {
            history.latest_input = stored.input;
            history.latest_tick = tick;
        }
    }
    history.latest_sequence = header.sequence;
    if ((header.flags & BTGA_NET_FLAG_RELIABLE) != 0U) {
        history.sync_origin = {header.acknowledgment, header.acknowledgment_bits};
        history.has_sync_origin = true;
    }
    history.valid = true;
    return true;
}

void InputSynchronizer::record_local(
    std::uint8_t player_id, std::uint32_t tick, std::uint32_t sequence,
    OnlineControllerInput input) {
    if (player_id >= max_players) return;
    PlayerHistory& history = histories_[player_id];
    StoredInput& stored = history.inputs[tick % history_size];
    stored = {input, tick, sequence, true};
    history.latest_input = input;
    history.latest_tick = tick;
    history.latest_sequence = sequence;
    history.valid = true;
}

std::optional<OnlineControllerInput> InputSynchronizer::input_for(
    std::uint8_t player_id, std::uint32_t tick) const {
    if (player_id >= max_players) return std::nullopt;
    const PlayerHistory& history = histories_[player_id];
    const StoredInput& stored = history.inputs[tick % history_size];
    if (!stored.valid || stored.tick != tick) return std::nullopt;
    return stored.input;
}

std::optional<OnlineControllerInput> InputSynchronizer::latest_input_for(
    std::uint8_t player_id) const {
    if (player_id >= max_players || !histories_[player_id].valid) return std::nullopt;
    return histories_[player_id].latest_input;
}

std::optional<std::uint32_t> InputSynchronizer::latest_tick_for(
    std::uint8_t player_id) const {
    if (player_id >= max_players || !histories_[player_id].valid) return std::nullopt;
    return histories_[player_id].latest_tick;
}

std::optional<OnlineSyncOrigin> InputSynchronizer::sync_origin_for(
    std::uint8_t player_id) const {
    if (player_id >= max_players || !histories_[player_id].has_sync_origin) {
        return std::nullopt;
    }
    return histories_[player_id].sync_origin;
}

void prime_frame_input_epoch(InputSynchronizer& synchronizer,
                             std::uint32_t authoritative_generation,
                             std::uint8_t player_count) {
    constexpr std::uint32_t frame_tag = UINT32_C(0x80000000);
    constexpr std::uint32_t input_delay = 4U;
    constexpr std::uint32_t redundancy = 8U;
    const std::uint32_t next_frame = authoritative_generation + 1U;
    const std::uint32_t next_publish = next_frame + input_delay;
    const std::uint32_t first = next_publish >= redundancy - 1U
        ? std::max(input_delay, next_publish - (redundancy - 1U))
        : input_delay;
    for (std::uint8_t player = 0;
         player < player_count && player < InputSynchronizer::max_players; ++player) {
        for (std::uint32_t frame = first; frame < next_publish; ++frame) {
            synchronizer.record_local(player, frame_tag | frame, 0, {});
        }
    }
}

} // namespace btga
