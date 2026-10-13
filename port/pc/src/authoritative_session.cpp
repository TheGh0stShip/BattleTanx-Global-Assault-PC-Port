#include "btga/authoritative_session.hpp"

namespace btga {

AuthoritativeSession::AuthoritativeSession(std::uint8_t occupied_mask,
                                           std::uint32_t snapshot_interval,
                                           std::uint32_t jitter_buffer_ticks)
    : snapshot_interval_(snapshot_interval),
      jitter_buffer_ticks_(jitter_buffer_ticks),
      occupied_mask_(static_cast<std::uint8_t>(occupied_mask & 0x0fU)),
      connected_mask_(occupied_mask_) {}

std::uint32_t AuthoritativeSession::next_tick() const noexcept {
    return next_tick_;
}

std::uint8_t AuthoritativeSession::occupied_mask() const noexcept {
    return occupied_mask_;
}

std::uint8_t AuthoritativeSession::connected_mask() const noexcept {
    return connected_mask_;
}

bool AuthoritativeSession::set_connected(std::uint8_t seat, bool connected) {
    if (seat >= online_player_limit || (occupied_mask_ & (1U << seat)) == 0U) {
        return false;
    }
    const auto bit = static_cast<std::uint8_t>(1U << seat);
    if (connected) {
        connected_mask_ = static_cast<std::uint8_t>(connected_mask_ | bit);
    } else {
        connected_mask_ = static_cast<std::uint8_t>(connected_mask_ & ~bit);
        last_inputs_[seat] = {};
        for (auto& entry : history_) {
            entry.present_mask = static_cast<std::uint8_t>(entry.present_mask & ~bit);
            entry.inputs[seat] = {};
        }
    }
    return true;
}

bool AuthoritativeSession::submit(std::uint8_t seat, std::uint32_t tick,
                                  OnlineControllerInput input) {
    if (seat >= online_player_limit || (connected_mask_ & (1U << seat)) == 0U ||
        tick < next_tick_ || tick - next_tick_ >= input_history) {
        return false;
    }
    TickInputs& entry = history_[tick % history_.size()];
    if (entry.tick != tick) entry = {.tick = tick};
    const std::uint8_t bit = static_cast<std::uint8_t>(1U << seat);
    if ((entry.present_mask & bit) != 0U) return entry.inputs[seat] == input;
    entry.inputs[seat] = input;
    entry.present_mask = static_cast<std::uint8_t>(entry.present_mask | bit);
    return true;
}

bool AuthoritativeSession::ready() const noexcept {
    if (occupied_mask_ == 0U) return false;
    const TickInputs& entry = history_[next_tick_ % history_.size()];
    return entry.tick == next_tick_ &&
           (entry.present_mask & connected_mask_) == connected_mask_;
}

bool AuthoritativeSession::due(std::uint32_t server_tick) const noexcept {
    return occupied_mask_ != 0U &&
           static_cast<std::int32_t>(server_tick - next_tick_) >=
               static_cast<std::int32_t>(jitter_buffer_ticks_);
}

std::optional<AuthoritativeFrame> AuthoritativeSession::advance(
    AuthoritativeSimulation& simulation) {
    if (!ready()) return std::nullopt;
    return advance_due(simulation, next_tick_);
}

std::optional<AuthoritativeFrame> AuthoritativeSession::advance_due(
    AuthoritativeSimulation& simulation, std::uint32_t server_tick) {
    if (!ready() && !due(server_tick)) return std::nullopt;
    TickInputs& entry = history_[next_tick_ % history_.size()];
    if (entry.tick != next_tick_) entry = {.tick = next_tick_};
    const std::uint8_t received_mask =
        static_cast<std::uint8_t>(entry.present_mask & occupied_mask_);
    for (std::size_t seat = 0; seat < online_player_limit; ++seat) {
        const std::uint8_t bit = static_cast<std::uint8_t>(1U << seat);
        if ((occupied_mask_ & bit) == 0U) {
            entry.inputs[seat] = {};
        } else if ((connected_mask_ & bit) == 0U) {
            entry.inputs[seat] = {};
        } else if ((received_mask & bit) == 0U) {
            entry.inputs[seat] = last_inputs_[seat];
        } else {
            last_inputs_[seat] = entry.inputs[seat];
        }
    }
    if (!simulation.run_tick(next_tick_, entry.inputs)) return std::nullopt;

    AuthoritativeFrame frame{
        next_tick_, entry.inputs, simulation.state_digest(), received_mask};
    ++next_tick_;
    entry = {};
    if (snapshot_interval_ != 0U && next_tick_ % snapshot_interval_ == 0U) {
        AuthoritativeSnapshot snapshot;
        snapshot.next_tick = next_tick_;
        snapshot.state_digest = frame.state_digest;
        if (!simulation.save_state(snapshot.state)) return std::nullopt;
        snapshot_ = std::move(snapshot);
    }
    return frame;
}

const std::optional<AuthoritativeSnapshot>&
AuthoritativeSession::latest_snapshot() const noexcept {
    return snapshot_;
}

} // namespace btga
