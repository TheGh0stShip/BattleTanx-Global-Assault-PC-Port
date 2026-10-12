#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace btga {

struct StateFragment {
    std::uint32_t transfer_id = 0;
    std::uint32_t uncompressed_size = 0;
    std::uint32_t compressed_size = 0;
    std::uint64_t checksum = 0;
    std::uint16_t index = 0;
    std::uint16_t count = 0;
    std::span<const std::uint8_t> bytes{};
};

// Portable snapshot fragmentation and reassembly. The wire representation is
// explicitly big-endian and contains no native pointers or native structs.
class StateSynchronizer {
public:
    static constexpr std::size_t fragment_header_size = 29;
    static constexpr std::size_t max_fragment_bytes = 1100;
    static constexpr std::size_t max_snapshot_bytes = 4U * 1024U * 1024U;
    static constexpr std::size_t max_compressed_bytes =
        max_snapshot_bytes + max_snapshot_bytes / 128U + 1U;

    [[nodiscard]] static std::vector<std::uint8_t> compress(
        std::span<const std::uint8_t> source);
    [[nodiscard]] static std::optional<std::vector<std::uint8_t>> decompress(
        std::span<const std::uint8_t> source, std::size_t expected_size);
    [[nodiscard]] static std::uint64_t checksum(
        std::span<const std::uint8_t> source);

    [[nodiscard]] static std::size_t encode_fragment(
        std::span<std::uint8_t> destination, const StateFragment& fragment);
    [[nodiscard]] static std::optional<StateFragment> decode_fragment(
        std::span<const std::uint8_t> packet);

    void reset();
    [[nodiscard]] bool accept(std::span<const std::uint8_t> packet);
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> restore() const;
    [[nodiscard]] std::uint32_t transfer_id() const noexcept;

private:
    std::uint32_t transfer_id_ = 0;
    std::uint32_t uncompressed_size_ = 0;
    std::uint32_t compressed_size_ = 0;
    std::uint64_t checksum_ = 0;
    std::uint16_t fragment_count_ = 0;
    std::size_t received_count_ = 0;
    std::vector<std::uint8_t> compressed_;
    std::vector<bool> received_;
};

} // namespace btga
