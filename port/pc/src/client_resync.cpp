#include "btga/client_resync.hpp"

namespace btga {

void ClientResyncPolicy::prune(Clock::time_point now) noexcept {
    while (size_ != 0U && now - attempts_[begin_] >= attempt_window) {
        begin_ = (begin_ + 1U) % attempts_.size();
        --size_;
    }
}

ClientResyncDecision ClientResyncPolicy::mismatch(Clock::time_point now) noexcept {
    prune(now);
    if (size_ == attempts_.size()) return ClientResyncDecision::quarantine;
    attempts_[(begin_ + size_) % attempts_.size()] = now;
    ++size_;
    return ClientResyncDecision::request_checkpoint;
}

void ClientResyncPolicy::reset() noexcept {
    attempts_ = {};
    begin_ = 0U;
    size_ = 0U;
}

std::size_t ClientResyncPolicy::recent_attempts(Clock::time_point now) const noexcept {
    auto copy = *this;
    copy.prune(now);
    return copy.size_;
}

void ClientInputLiveness::reset() noexcept { seats_ = {}; }

void ClientInputLiveness::observe_transport(
    std::uint8_t seat, Clock::time_point now) noexcept {
    if (seat >= player_limit) return;
    auto& state = seats_[seat];
    if (!state.seen) {
        state.input = now;
        state.seen = true;
    }
    state.transport = now;
}

bool ClientInputLiveness::observe_input(
    std::uint8_t seat, std::uint32_t horizon,
    Clock::time_point now) noexcept {
    if (seat >= player_limit) return false;
    observe_transport(seat, now);
    auto& state = seats_[seat];
    if (state.have_horizon &&
        static_cast<std::int32_t>(horizon - state.horizon) <= 0) return false;
    state.horizon = horizon;
    state.have_horizon = true;
    state.input = now;
    return true;
}

void ClientInputLiveness::clear(std::uint8_t seat) noexcept {
    if (seat < player_limit) seats_[seat] = {};
}

bool ClientInputLiveness::seen(std::uint8_t seat) const noexcept {
    return seat < player_limit && seats_[seat].seen;
}

bool ClientInputLiveness::input_stale(
    std::uint8_t seat, Clock::time_point now,
    Clock::duration timeout) const noexcept {
    return seat < player_limit && timeout > Clock::duration::zero() &&
        seats_[seat].seen && now - seats_[seat].input > timeout;
}

bool ClientInputLiveness::transport_stale(
    std::uint8_t seat, Clock::time_point now,
    Clock::duration timeout) const noexcept {
    return seat < player_limit && timeout > Clock::duration::zero() &&
        seats_[seat].seen && now - seats_[seat].transport > timeout;
}

} // namespace btga
