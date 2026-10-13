#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace btga {

constexpr std::size_t snapshot_chunk_payload_size = 1000U;
constexpr std::size_t snapshot_chunk_header_size = 14U;
constexpr std::size_t snapshot_max_chunks = 64U;

struct SnapshotChunk {
    std::uint32_t generation = 0;
    std::uint16_t index = 0;
    std::uint16_t count = 0;
    std::uint16_t size = 0;
    std::uint32_t checksum = 0;
    std::span<const std::uint8_t> data;
};

[[nodiscard]] std::uint32_t snapshot_fnv1a(std::span<const std::uint8_t> bytes);
// Serial-number comparison for frame-derived generations. This handles the
// uint32 wrap boundary as long as epochs are less than 2^31 frames apart.
[[nodiscard]] bool snapshot_generation_is_newer(std::uint32_t candidate,
                                                std::uint32_t current);
[[nodiscard]] bool encode_snapshot_chunk(std::span<std::uint8_t> output,
                                         const SnapshotChunk& chunk);
[[nodiscard]] bool decode_snapshot_chunk(SnapshotChunk& chunk,
                                         std::span<const std::uint8_t> input);

class SnapshotAssembler {
public:
    explicit SnapshotAssembler(std::size_t expected_size);
    void reset();
    [[nodiscard]] bool accept(const SnapshotChunk& chunk);
    [[nodiscard]] bool complete() const;
    [[nodiscard]] bool verified() const;
    [[nodiscard]] std::span<const std::uint8_t> bytes() const { return bytes_; }
    [[nodiscard]] std::uint32_t generation() const { return generation_; }
    [[nodiscard]] std::uint32_t checksum() const { return checksum_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::uint64_t received_ = 0;
    std::uint32_t generation_ = 0;
    std::uint32_t checksum_ = 0;
    std::uint16_t count_ = 0;
};

} // namespace btga
