#include "btga/authoritative_replica.hpp"

#include <algorithm>
#include <cstring>

extern "C" {
#include <btga/btga_gameplay.h>
}

namespace btga {
namespace {

bool serial_newer(std::uint32_t candidate, std::uint32_t current) {
    const std::uint32_t distance = candidate - current;
    return distance != 0U && distance < UINT32_C(0x80000000);
}

} // namespace

bool AuthoritativeReplica::accept_frame(std::span<const std::uint8_t> payload) {
    BtgaNetAuthoritativeFrame decoded{};
    if (!btga_net_decode_authoritative_frame(
            &decoded, payload.data(), payload.size())) return false;
    if (frame_ && !serial_newer(decoded.tick, frame_->tick)) {
        return decoded.tick == frame_->tick &&
               decoded.state_digest == frame_->state_digest;
    }
    CanonicalFrame frame;
    frame.tick = decoded.tick;
    frame.state_digest = decoded.state_digest;
    frame.occupied_mask = decoded.occupied_mask;
    frame.connected_mask = decoded.connected_mask;
    frame.received_mask = decoded.received_mask;
    for (std::size_t seat = 0; seat < frame.inputs.size(); ++seat) {
        frame.inputs[seat] = {decoded.inputs[seat].buttons,
                              decoded.inputs[seat].stick_x,
                              decoded.inputs[seat].stick_y};
    }
    frame_ = frame;
    return true;
}

void AuthoritativeReplica::reset_snapshot(
    std::uint32_t snapshot_id, std::uint32_t tick, std::uint64_t digest,
    std::uint32_t total_size) {
    snapshot_ = CanonicalSnapshot{snapshot_id, tick, digest,
                                  std::vector<std::uint8_t>(total_size)};
    const std::size_t count =
        (total_size + BTGA_NET_SNAPSHOT_CHUNK_DATA_LIMIT - 1U) /
        BTGA_NET_SNAPSHOT_CHUNK_DATA_LIMIT;
    received_chunks_.assign(count, 0U);
    received_bytes_ = 0U;
    newest_snapshot_id_ = snapshot_id;
    have_snapshot_id_ = true;
}

bool AuthoritativeReplica::accept_snapshot_chunk(
    std::span<const std::uint8_t> payload) {
    BtgaNetSnapshotChunk chunk{};
    if (!btga_net_decode_snapshot_chunk(&chunk, payload.data(), payload.size()) ||
        chunk.total_size > canonical_state_limit ||
        chunk.offset % BTGA_NET_SNAPSHOT_CHUNK_DATA_LIMIT != 0U) return false;
    const std::size_t index = chunk.offset / BTGA_NET_SNAPSHOT_CHUNK_DATA_LIMIT;
    const std::size_t expected_size = (std::min)(
        static_cast<std::size_t>(BTGA_NET_SNAPSHOT_CHUNK_DATA_LIMIT),
        static_cast<std::size_t>(chunk.total_size - chunk.offset));
    if (chunk.data_size != expected_size) return false;

    if (!snapshot_ || chunk.snapshot_id != snapshot_->snapshot_id) {
        if (have_snapshot_id_ &&
            !serial_newer(chunk.snapshot_id, newest_snapshot_id_)) return false;
        reset_snapshot(chunk.snapshot_id, chunk.tick, chunk.state_digest,
                       chunk.total_size);
    }
    if (chunk.tick != snapshot_->tick ||
        chunk.state_digest != snapshot_->state_digest ||
        chunk.total_size != snapshot_->state.size() ||
        index >= received_chunks_.size()) return false;
    auto destination = snapshot_->state.data() + chunk.offset;
    if (received_chunks_[index] != 0U) {
        return std::memcmp(destination, chunk.data, chunk.data_size) == 0;
    }
    std::memcpy(destination, chunk.data, chunk.data_size);
    received_chunks_[index] = 1U;
    received_bytes_ += chunk.data_size;
    return true;
}

const std::optional<CanonicalFrame>&
AuthoritativeReplica::latest_frame() const noexcept {
    return frame_;
}

bool AuthoritativeReplica::snapshot_ready() const noexcept {
    return snapshot_ && received_bytes_ == snapshot_->state.size() &&
           std::all_of(received_chunks_.begin(), received_chunks_.end(),
                       [](std::uint8_t received) { return received != 0U; });
}

std::optional<CanonicalSnapshot> AuthoritativeReplica::take_snapshot() {
    if (!snapshot_ready()) return std::nullopt;
    auto completed = std::move(snapshot_);
    snapshot_.reset();
    received_chunks_.clear();
    received_bytes_ = 0U;
    return completed;
}

} // namespace btga
