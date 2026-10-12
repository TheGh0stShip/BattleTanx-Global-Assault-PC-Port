#include "btga/input_sync.hpp"

extern "C" {
#include <btga/btga_gameplay.h>
#include <btga/btga_net.h>
}

#include <array>
#include <cstring>

namespace btga {

std::size_t InputSynchronizer::encode(
    std::span<std::uint8_t> packet, std::uint64_t session_id,
    std::uint16_t peer_id, std::uint32_t sequence, std::uint32_t tick,
    std::uint8_t player_id, OnlineControllerInput input) const {
    if (player_id >= max_players || packet.size() < BTGA_NET_HEADER_SIZE) return 0;

    BtgaNetInputBatch batch{};
    batch.first_tick = tick;
    batch.player_id = player_id;
    batch.frame_count = 1;
    batch.frames[0].buttons = input.buttons;
    batch.frames[0].stick_x = input.stick_x;
    batch.frames[0].stick_y = input.stick_y;

    std::array<std::uint8_t, BTGA_NET_MAX_PAYLOAD_SIZE> payload{};
    const std::size_t payload_size = btga_net_encode_input_batch(
        payload.data(), payload.size(), &batch);
    if (payload_size == 0 || BTGA_NET_HEADER_SIZE + payload_size > packet.size()) return 0;

    BtgaNetHeader header{};
    header.protocol_version = BTGA_NET_PROTOCOL_VERSION;
    header.message_type = BTGA_NET_MSG_INPUT;
    header.flags = BTGA_NET_FLAG_RELAYED;
    header.session_id = session_id;
    header.sequence = sequence;
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

    StoredInput& stored = inputs_[batch.player_id];
    for (std::uint8_t index = 0; index < batch.frame_count; ++index) {
        const std::uint32_t tick = batch.first_tick + index;
        if (stored.valid && (tick < stored.tick ||
            (tick == stored.tick && header.sequence <= stored.sequence))) continue;
        stored.input = {
            batch.frames[index].buttons,
            batch.frames[index].stick_x,
            batch.frames[index].stick_y,
        };
        stored.tick = tick;
        stored.sequence = header.sequence;
        stored.valid = true;
    }
    return true;
}

std::optional<OnlineControllerInput> InputSynchronizer::input_for(
    std::uint8_t player_id, std::uint32_t tick) const {
    if (player_id >= max_players) return std::nullopt;
    const StoredInput& stored = inputs_[player_id];
    if (!stored.valid || stored.tick != tick) return std::nullopt;
    return stored.input;
}

std::optional<OnlineControllerInput> InputSynchronizer::latest_input_for(
    std::uint8_t player_id) const {
    if (player_id >= max_players || !inputs_[player_id].valid) return std::nullopt;
    return inputs_[player_id].input;
}

} // namespace btga
