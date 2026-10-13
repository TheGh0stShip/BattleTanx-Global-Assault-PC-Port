#include "btga/snapshot_transfer.hpp"

#include <algorithm>
#include <cstring>

namespace btga {
namespace {
void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}
std::uint16_t get16(const std::uint8_t* p) { return (p[0] << 8) | p[1]; }
std::uint32_t get32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
           (std::uint32_t{p[2]} << 8) | p[3];
}
}

std::uint32_t snapshot_fnv1a(std::span<const std::uint8_t> bytes) {
    std::uint32_t hash = UINT32_C(2166136261);
    for (const auto byte : bytes) { hash ^= byte; hash *= UINT32_C(16777619); }
    return hash;
}

bool snapshot_generation_is_newer(std::uint32_t candidate,
                                  std::uint32_t current) {
    const std::uint32_t distance = candidate - current;
    return distance != 0U && distance < UINT32_C(0x80000000);
}

bool encode_snapshot_chunk(std::span<std::uint8_t> output,
                           const SnapshotChunk& chunk) {
    if (chunk.count == 0 || chunk.count > snapshot_max_chunks ||
        chunk.index >= chunk.count || chunk.data.size() > snapshot_chunk_payload_size ||
        chunk.size != chunk.data.size() ||
        output.size() != snapshot_chunk_header_size + chunk.data.size()) return false;
    put32(output.data(), chunk.generation);
    put16(output.data() + 4, chunk.index);
    put16(output.data() + 6, chunk.count);
    put16(output.data() + 8, chunk.size);
    put32(output.data() + 10, chunk.checksum);
    std::memcpy(output.data() + snapshot_chunk_header_size,
                chunk.data.data(), chunk.data.size());
    return true;
}

bool decode_snapshot_chunk(SnapshotChunk& chunk,
                           std::span<const std::uint8_t> input) {
    if (input.size() < snapshot_chunk_header_size) return false;
    chunk.generation = get32(input.data());
    chunk.index = get16(input.data() + 4);
    chunk.count = get16(input.data() + 6);
    chunk.size = get16(input.data() + 8);
    chunk.checksum = get32(input.data() + 10);
    if (chunk.count == 0 || chunk.count > snapshot_max_chunks ||
        chunk.index >= chunk.count || chunk.size > snapshot_chunk_payload_size ||
        input.size() != snapshot_chunk_header_size + chunk.size) return false;
    chunk.data = input.subspan(snapshot_chunk_header_size);
    return true;
}

SnapshotAssembler::SnapshotAssembler(std::size_t expected_size) : bytes_(expected_size) {}
void SnapshotAssembler::reset() {
    std::fill(bytes_.begin(), bytes_.end(), std::uint8_t{0}); received_ = 0;
    generation_ = 0; checksum_ = 0; count_ = 0;
}
bool SnapshotAssembler::accept(const SnapshotChunk& chunk) {
    if (received_ != 0 && chunk.generation != generation_) {
        // A verified epoch may roll forward without reconstructing the
        // receiver object. Never roll backward, and never abandon a partial
        // or corrupt epoch based on an unauthenticated generation number.
        if (!verified() || chunk.generation <= generation_) return false;
        reset();
    }
    const std::size_t expected_count =
        (bytes_.size() + snapshot_chunk_payload_size - 1U) / snapshot_chunk_payload_size;
    if (chunk.count != expected_count || chunk.index >= chunk.count) return false;
    const std::size_t offset = chunk.index * snapshot_chunk_payload_size;
    const std::size_t expected = std::min(snapshot_chunk_payload_size, bytes_.size() - offset);
    if (chunk.size != expected || chunk.data.size() != expected) return false;
    if (received_ == 0) {
        generation_ = chunk.generation; checksum_ = chunk.checksum; count_ = chunk.count;
    }
    if (chunk.generation != generation_ || chunk.checksum != checksum_ ||
        chunk.count != count_) return false;
    std::memcpy(bytes_.data() + offset, chunk.data.data(), expected);
    received_ |= UINT64_C(1) << chunk.index;
    return true;
}
bool SnapshotAssembler::complete() const {
    if (count_ == 0) return false;
    const std::uint64_t complete_mask = count_ == 64U
        ? UINT64_MAX
        : (UINT64_C(1) << count_) - 1U;
    return received_ == complete_mask;
}
bool SnapshotAssembler::verified() const { return complete() && snapshot_fnv1a(bytes_) == checksum_; }

} // namespace btga
