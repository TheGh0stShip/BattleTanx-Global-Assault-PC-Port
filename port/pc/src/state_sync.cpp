#include "btga/state_sync.hpp"

#include <algorithm>
#include <limits>

namespace btga {
namespace {
constexpr std::uint8_t wire_version = 1;

void put_u16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value);
}
void put_u32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}
void put_u64(std::uint8_t* out, std::uint64_t value) {
    put_u32(out, static_cast<std::uint32_t>(value >> 32));
    put_u32(out + 4, static_cast<std::uint32_t>(value));
}
std::uint16_t get_u16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) | in[1]);
}
std::uint32_t get_u32(const std::uint8_t* in) {
    return (static_cast<std::uint32_t>(in[0]) << 24) |
           (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | in[3];
}
std::uint64_t get_u64(const std::uint8_t* in) {
    return (static_cast<std::uint64_t>(get_u32(in)) << 32) | get_u32(in + 4);
}
}

std::vector<std::uint8_t> StateSynchronizer::compress(
    std::span<const std::uint8_t> source) {
    // PackBits-style encoding. High-bit control bytes encode repeated runs;
    // low-bit controls encode literal runs. It is intentionally tiny and
    // portable to every target rather than relying on a platform compressor.
    std::vector<std::uint8_t> output;
    output.reserve(source.size());
    std::size_t at = 0;
    while (at < source.size()) {
        std::size_t run = 1;
        while (at + run < source.size() && source[at + run] == source[at] && run < 128) ++run;
        if (run >= 4) {
            output.push_back(static_cast<std::uint8_t>(0x80U | (run - 1U)));
            output.push_back(source[at]);
            at += run;
            continue;
        }
        const std::size_t literal_begin = at;
        at += run;
        while (at < source.size() && at - literal_begin < 128) {
            run = 1;
            while (at + run < source.size() && source[at + run] == source[at] && run < 128) ++run;
            if (run >= 4) break;
            at += std::min(run, 128U - (at - literal_begin));
        }
        const std::size_t literal_size = at - literal_begin;
        output.push_back(static_cast<std::uint8_t>(literal_size - 1U));
        output.insert(output.end(), source.begin() + static_cast<std::ptrdiff_t>(literal_begin),
                      source.begin() + static_cast<std::ptrdiff_t>(at));
    }
    return output;
}

std::optional<std::vector<std::uint8_t>> StateSynchronizer::decompress(
    std::span<const std::uint8_t> source, std::size_t expected_size) {
    if (expected_size > max_snapshot_bytes) return std::nullopt;
    std::vector<std::uint8_t> output;
    output.reserve(expected_size);
    std::size_t at = 0;
    while (at < source.size() && output.size() < expected_size) {
        const std::uint8_t control = source[at++];
        const std::size_t count = (control & 0x7FU) + 1U;
        if ((control & 0x80U) != 0U) {
            if (at == source.size() || count > expected_size - output.size()) return std::nullopt;
            output.insert(output.end(), count, source[at++]);
        } else {
            if (count > source.size() - at || count > expected_size - output.size()) return std::nullopt;
            output.insert(output.end(), source.begin() + static_cast<std::ptrdiff_t>(at),
                          source.begin() + static_cast<std::ptrdiff_t>(at + count));
            at += count;
        }
    }
    if (at != source.size() || output.size() != expected_size) return std::nullopt;
    return output;
}

std::uint64_t StateSynchronizer::checksum(std::span<const std::uint8_t> source) {
    std::uint64_t hash = UINT64_C(1469598103934665603);
    for (const std::uint8_t byte : source) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

std::size_t StateSynchronizer::encode_fragment(
    std::span<std::uint8_t> destination, const StateFragment& fragment) {
    if (fragment.bytes.empty() || fragment.bytes.size() > max_fragment_bytes ||
        fragment.uncompressed_size == 0 || fragment.uncompressed_size > max_snapshot_bytes ||
        fragment.compressed_size == 0 || fragment.count == 0 || fragment.index >= fragment.count ||
        destination.size() < fragment_header_size + fragment.bytes.size()) return 0;
    destination[0] = wire_version;
    put_u32(destination.data() + 1, fragment.transfer_id);
    put_u32(destination.data() + 5, fragment.uncompressed_size);
    put_u32(destination.data() + 9, fragment.compressed_size);
    put_u64(destination.data() + 13, fragment.checksum);
    put_u16(destination.data() + 21, fragment.index);
    put_u16(destination.data() + 23, fragment.count);
    put_u32(destination.data() + 25, static_cast<std::uint32_t>(fragment.bytes.size()));
    std::copy(fragment.bytes.begin(), fragment.bytes.end(), destination.begin() + fragment_header_size);
    return fragment_header_size + fragment.bytes.size();
}

std::optional<StateFragment> StateSynchronizer::decode_fragment(
    std::span<const std::uint8_t> packet) {
    if (packet.size() < fragment_header_size || packet[0] != wire_version) return std::nullopt;
    StateFragment result{};
    result.transfer_id = get_u32(packet.data() + 1);
    result.uncompressed_size = get_u32(packet.data() + 5);
    result.compressed_size = get_u32(packet.data() + 9);
    result.checksum = get_u64(packet.data() + 13);
    result.index = get_u16(packet.data() + 21);
    result.count = get_u16(packet.data() + 23);
    const std::uint32_t bytes = get_u32(packet.data() + 25);
    if (result.uncompressed_size == 0 || result.uncompressed_size > max_snapshot_bytes ||
        result.compressed_size == 0 || result.compressed_size > max_compressed_bytes ||
        result.count == 0 || result.index >= result.count ||
        bytes == 0 || bytes > max_fragment_bytes || packet.size() != fragment_header_size + bytes ||
        result.count != (result.compressed_size + max_fragment_bytes - 1U) /
                            max_fragment_bytes) {
        return std::nullopt;
    }
    result.bytes = packet.subspan(fragment_header_size);
    return result;
}

void StateSynchronizer::reset() {
    *this = {};
}

bool StateSynchronizer::accept(std::span<const std::uint8_t> packet) {
    const auto fragment = decode_fragment(packet);
    if (!fragment) return false;
    if (fragment_count_ == 0) {
        transfer_id_ = fragment->transfer_id;
        uncompressed_size_ = fragment->uncompressed_size;
        compressed_size_ = fragment->compressed_size;
        checksum_ = fragment->checksum;
        fragment_count_ = fragment->count;
        compressed_.resize(compressed_size_);
        received_.assign(fragment_count_, false);
    } else if (transfer_id_ != fragment->transfer_id ||
               uncompressed_size_ != fragment->uncompressed_size ||
               compressed_size_ != fragment->compressed_size || checksum_ != fragment->checksum ||
               fragment_count_ != fragment->count) {
        return false;
    }
    const std::size_t offset = static_cast<std::size_t>(fragment->index) * max_fragment_bytes;
    if (offset >= compressed_.size() || fragment->bytes.size() > compressed_.size() - offset ||
        (fragment->index + 1U < fragment_count_ && fragment->bytes.size() != max_fragment_bytes)) {
        return false;
    }
    if (!received_[fragment->index]) {
        std::copy(fragment->bytes.begin(), fragment->bytes.end(), compressed_.begin() + offset);
        received_[fragment->index] = true;
        ++received_count_;
    }
    return true;
}

bool StateSynchronizer::complete() const noexcept {
    return fragment_count_ != 0 && received_count_ == fragment_count_;
}

std::optional<std::vector<std::uint8_t>> StateSynchronizer::restore() const {
    if (!complete()) return std::nullopt;
    auto restored = decompress(compressed_, uncompressed_size_);
    if (!restored || checksum(*restored) != checksum_) return std::nullopt;
    return restored;
}

std::uint32_t StateSynchronizer::transfer_id() const noexcept { return transfer_id_; }

} // namespace btga
