#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace btga {

// Bounded FIFO used only by explicitly enabled network-impairment tests.
// Keeping timing and storage separate from the socket makes its ordering and
// memory bound directly testable on every host platform.
class DelayedDatagramQueue {
public:
    using Clock = std::chrono::steady_clock;

    explicit DelayedDatagramQueue(std::size_t capacity = 4096U)
        : capacity_(capacity) {}

    [[nodiscard]] bool enqueue(std::span<const std::uint8_t> bytes,
                               Clock::time_point due) {
        if (bytes.empty() || queue_.size() >= capacity_) return false;
        queue_.push_back({{bytes.begin(), bytes.end()}, due});
        return true;
    }

    [[nodiscard]] std::span<const std::uint8_t> front_due(
        Clock::time_point now) const noexcept {
        if (queue_.empty() || queue_.front().due > now) return {};
        return queue_.front().bytes;
    }

    void pop_front() noexcept {
        if (!queue_.empty()) queue_.pop_front();
    }
    void clear() noexcept { queue_.clear(); }
    [[nodiscard]] std::size_t size() const noexcept { return queue_.size(); }

private:
    struct Datagram {
        std::vector<std::uint8_t> bytes;
        Clock::time_point due;
    };
    std::size_t capacity_;
    std::deque<Datagram> queue_;
};

} // namespace btga
