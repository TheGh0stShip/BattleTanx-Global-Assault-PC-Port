#include "btga/authoritative_server.hpp"

#include <algorithm>
#include <array>
#include <limits>

extern "C" {
#include <btga/btga_gameplay.h>
#include <btga/btga_net.h>
}

namespace btga {

AuthoritativeServerCore::AuthoritativeServerCore(
    std::uint8_t occupied_mask, std::uint32_t snapshot_interval,
    std::uint32_t jitter_buffer_ticks)
    : session_(occupied_mask, snapshot_interval, jitter_buffer_ticks) {}

bool AuthoritativeServerCore::ingest_input_payload(
    std::span<const std::uint8_t> payload) {
    BtgaNetInputBatch batch{};
    if (!btga_net_decode_input_batch(&batch, payload.data(), payload.size()) ||
        batch.player_id >= online_player_limit) return false;
    for (std::uint8_t index = 0; index < batch.frame_count; ++index) {
        const BtgaNetInputFrame& input = batch.frames[index];
        const std::uint32_t tick = batch.first_tick + index;
        if (tick < batch.first_tick ||
            !session_.submit(batch.player_id, tick,
                             {input.buttons, input.stick_x, input.stick_y})) {
            return false;
        }
    }
    return true;
}

bool AuthoritativeServerCore::ingest_client_packet(
    std::span<const std::uint8_t> packet, std::uint64_t session_id,
    std::uint16_t relay_source_slot) {
    if (relay_source_slot == 0U || relay_source_slot > online_player_limit) {
        return false;
    }
    const auto seat = static_cast<std::uint8_t>(relay_source_slot - 1U);
    BtgaNetHeader header{};
    if (!btga_net_decode_header(&header, packet.data(), packet.size()) ||
        header.protocol_version != BTGA_NET_PROTOCOL_VERSION ||
        header.message_type != BTGA_NET_MSG_INPUT ||
        header.session_id != session_id || header.peer_id != relay_source_slot ||
        header.payload_size != packet.size() - BTGA_NET_HEADER_SIZE) {
        return false;
    }
    BtgaNetInputBatch batch{};
    const auto payload = packet.subspan(BTGA_NET_HEADER_SIZE);
    if (!btga_net_decode_input_batch(&batch, payload.data(), payload.size()) ||
        batch.player_id != seat) return false;
    if (have_sequence_[seat] &&
        static_cast<std::int32_t>(header.sequence - latest_sequences_[seat]) <= 0) {
        return true;
    }
    for (std::uint8_t index = 0; index < batch.frame_count; ++index) {
        const std::uint32_t tick = batch.first_tick + index;
        if (tick < batch.first_tick) return false;
        if (tick < session_.next_tick()) continue;
        const auto& input = batch.frames[index];
        if (!session_.submit(seat, tick,
                             {input.buttons, input.stick_x, input.stick_y})) {
            return false;
        }
    }
    latest_sequences_[seat] = header.sequence;
    have_sequence_[seat] = true;
    return true;
}

bool AuthoritativeServerCore::set_connected(std::uint8_t seat, bool connected) {
    if (!session_.set_connected(seat, connected)) return false;
    if (!connected) {
        have_sequence_[seat] = false;
        latest_sequences_[seat] = 0U;
    }
    return true;
}

std::optional<AuthorityEmission> AuthoritativeServerCore::step(
    AuthoritativeSimulation& simulation, std::uint32_t server_tick) {
    const auto frame = session_.advance_due(simulation, server_tick);
    if (!frame) return std::nullopt;

    BtgaNetAuthoritativeFrame wire{};
    wire.tick = frame->tick;
    wire.state_digest = frame->state_digest;
    wire.occupied_mask = session_.occupied_mask();
    wire.connected_mask = session_.connected_mask();
    wire.received_mask = frame->received_mask;
    for (std::size_t seat = 0; seat < online_player_limit; ++seat) {
        wire.inputs[seat].buttons = frame->inputs[seat].buttons;
        wire.inputs[seat].stick_x = frame->inputs[seat].stick_x;
        wire.inputs[seat].stick_y = frame->inputs[seat].stick_y;
    }

    AuthorityEmission emission;
    emission.frame_payload.resize(BTGA_NET_AUTHORITY_FRAME_SIZE);
    const std::size_t frame_size = btga_net_encode_authoritative_frame(
        emission.frame_payload.data(), emission.frame_payload.size(), &wire);
    if (frame_size == 0U) return std::nullopt;
    emission.frame_payload.resize(frame_size);

    const auto& snapshot = session_.latest_snapshot();
    if (snapshot && snapshot->next_tick != emitted_snapshot_tick_) {
        auto payloads = encode_snapshot(snapshot->next_tick, snapshot->state_digest,
                                        snapshot->state);
        if (!payloads) return std::nullopt;
        emission.snapshot_payloads = std::move(*payloads);
        emitted_snapshot_tick_ = snapshot->next_tick;
    }
    return emission;
}

std::optional<std::vector<std::vector<std::uint8_t>>>
AuthoritativeServerCore::make_resync(
    const AuthoritativeSimulation& simulation) {
    std::vector<std::uint8_t> state;
    if (!simulation.save_state(state)) return std::nullopt;
    return encode_snapshot(session_.next_tick(), simulation.state_digest(), state);
}

std::optional<std::vector<std::vector<std::uint8_t>>>
AuthoritativeServerCore::encode_snapshot(
    std::uint32_t tick, std::uint64_t digest,
    std::span<const std::uint8_t> state) {
    if (state.empty() || state.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    std::vector<std::vector<std::uint8_t>> payloads;
    ++snapshot_id_;
    const auto total_size = static_cast<std::uint32_t>(state.size());
    for (std::uint32_t offset = 0; offset < total_size;) {
        const auto data_size = static_cast<std::uint16_t>((std::min)(
            static_cast<std::uint32_t>(BTGA_NET_SNAPSHOT_CHUNK_DATA_LIMIT),
            total_size - offset));
        BtgaNetSnapshotChunk chunk{snapshot_id_, tick, digest, total_size,
                                   offset, data_size, state.data() + offset};
        std::vector<std::uint8_t> encoded(
            BTGA_NET_SNAPSHOT_CHUNK_HEADER_SIZE + data_size);
        const std::size_t encoded_size = btga_net_encode_snapshot_chunk(
            encoded.data(), encoded.size(), &chunk);
        if (encoded_size == 0U) return std::nullopt;
        encoded.resize(encoded_size);
        payloads.push_back(std::move(encoded));
        offset += data_size;
    }
    return payloads;
}

const AuthoritativeSession& AuthoritativeServerCore::session() const noexcept {
    return session_;
}

} // namespace btga
