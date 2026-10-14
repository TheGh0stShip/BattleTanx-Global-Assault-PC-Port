#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace btga {

// Pure scheduling state for incrementally transmitting a checkpoint. The
// caller owns packet encoding and nonblocking I/O; this cursor guarantees that
// work can be bounded per pump without changing the existing chunk format.
class SnapshotSendCursor {
public:
    using Clock = std::chrono::steady_clock;
    // Reliability comes from retaining a generation at the receiver and
    // retrying that generation from a rotated first chunk. Unconditionally
    // tripling every datagram delays live canonical frames and can fill the
    // socket queue when four clients bootstrap together.
    static constexpr unsigned copies_per_chunk = 1U;
    // A full-size chunk every 7 ms consumed approximately the relay's entire
    // 160 KiB/s global egress allowance once its envelope was included. That
    // let bulk bootstrap traffic starve canonical frames and journal repair.
    // Twelve milliseconds keeps checkpoint traffic near 95 KiB/s and reserves
    // capacity for live four-player traffic without blocking simulation.
    static constexpr auto packet_period = std::chrono::milliseconds{12};

    void start(std::uint32_t chunk_count, std::uint32_t first_chunk,
               Clock::time_point now) noexcept {
        chunk_count_ = chunk_count;
        first_chunk_ = chunk_count == 0U ? 0U : first_chunk % chunk_count;
        ordinal_ = 0U;
        copy_ = 0U;
        next_send_ = now;
        active_ = chunk_count != 0U;
    }

    [[nodiscard]] std::optional<std::uint32_t> next(
        Clock::time_point now) const noexcept {
        if (!active_ || now < next_send_) return std::nullopt;
        return (first_chunk_ + ordinal_) % chunk_count_;
    }

    void mark_sent() noexcept {
        if (!active_) return;
        next_send_ += packet_period;
        if (++copy_ == copies_per_chunk) {
            copy_ = 0U;
            if (++ordinal_ == chunk_count_) active_ = false;
        }
    }

    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    std::uint32_t chunk_count_ = 0U;
    std::uint32_t first_chunk_ = 0U;
    std::uint32_t ordinal_ = 0U;
    unsigned copy_ = 0U;
    Clock::time_point next_send_{};
    bool active_ = false;
};

} // namespace btga
