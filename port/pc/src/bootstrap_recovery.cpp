#include "btga/bootstrap_recovery.hpp"

#include <limits>

namespace btga {

void BootstrapRecovery::reset() noexcept {
    seats_ = {};
    barrier_.reset();
}

BootstrapRequest BootstrapRecovery::request_cold(
    std::uint8_t seat, std::uint32_t latest_checkpoint_tick) noexcept {
    if (seat >= player_limit) return BootstrapRequest::continue_generation;
    auto& state = seats_[seat];
    if (!state.pending) {
        state = {.pending = true};
        return BootstrapRequest::start_generation;
    }
    if (state.expected_ack == 1U && state.snapshot_id != 0U &&
        latest_checkpoint_tick != 0U) {
        state.expected_ack = 0U;
        state.snapshot_id = 0U;
        return BootstrapRequest::supersede_initial_generation;
    }
    return BootstrapRequest::continue_generation;
}

void BootstrapRecovery::snapshot_sent(
    std::uint8_t seat, std::uint32_t snapshot_id,
    std::uint32_t authority_head) noexcept {
    if (seat >= player_limit || snapshot_id == 0U) return;
    auto& state = seats_[seat];
    // A transport send may complete after its requester disconnected. Do not
    // resurrect that seat or manufacture a generation without request_cold().
    if (!state.pending) return;
    state.snapshot_id = snapshot_id;
    state.expected_ack = authority_head + 1U;
}

BootstrapAck BootstrapRecovery::acknowledge(
    std::uint8_t seat, std::uint32_t encoded_next_tick,
    std::uint32_t snapshot_id, std::uint32_t authority_head,
    bool first_subscription) noexcept {
    if (seat >= player_limit || encoded_next_tick == 0U) return {};

    // The wire value is next_tick + 1 so that zero can mean "no bootstrap
    // installed".  Never allow a peer to claim progress beyond the
    // authority's canonical journal head: doing so would clear the recovery
    // barrier without proving that the missing ticks were replayed.  At the
    // terminal uint32 tick the +1 encoding is exhausted; rejecting all
    // impossible future values still leaves the last representable tick safe.
    const std::uint32_t maximum_ack = authority_head ==
            std::numeric_limits<std::uint32_t>::max()
        ? std::numeric_limits<std::uint32_t>::max()
        : authority_head + 1U;
    if (encoded_next_tick > maximum_ack) return {};

    auto& state = seats_[seat];
    if (state.pending && (state.expected_ack == 0U || state.snapshot_id == 0U ||
                          snapshot_id != state.snapshot_id)) return {};

    const std::uint32_t next_tick = encoded_next_tick - 1U;
    const bool was_pending = state.pending;
    if (was_pending && encoded_next_tick < state.expected_ack) {
        // Snapshot receipt is the proof that this endpoint can participate in
        // recovery. Only now freeze the authority at a stable replay head; an
        // endpoint that never receives its snapshot cannot stall the match.
        barrier_.begin(seat, authority_head);
        barrier_.acknowledge(seat, next_tick, authority_head);
        return {BootstrapAckKind::replay, next_tick};
    }

    state = {};
    if ((was_pending || first_subscription) && next_tick < authority_head) {
        barrier_.begin(seat, authority_head);
    }
    barrier_.acknowledge(seat, next_tick, authority_head);
    return {next_tick < authority_head ? BootstrapAckKind::replay
                                      : BootstrapAckKind::ready,
            next_tick};
}

void BootstrapRecovery::disconnect(std::uint8_t seat) noexcept {
    if (seat >= player_limit) return;
    seats_[seat] = {};
    barrier_.disconnect(seat);
}

bool BootstrapRecovery::pending(std::uint8_t seat) const noexcept {
    return seat < player_limit && seats_[seat].pending;
}
std::uint32_t BootstrapRecovery::expected_ack(std::uint8_t seat) const noexcept {
    return seat < player_limit ? seats_[seat].expected_ack : 0U;
}
std::uint32_t BootstrapRecovery::snapshot_id(std::uint8_t seat) const noexcept {
    return seat < player_limit ? seats_[seat].snapshot_id : 0U;
}
std::uint8_t BootstrapRecovery::pending_mask() const noexcept {
    std::uint8_t mask = 0U;
    for (std::uint8_t seat = 0U; seat < player_limit; ++seat) {
        if (seats_[seat].pending) {
            mask = static_cast<std::uint8_t>(mask | (1U << seat));
        }
    }
    return mask;
}
bool BootstrapRecovery::paused() const noexcept { return barrier_.paused(); }
std::uint8_t BootstrapRecovery::mask() const noexcept { return barrier_.mask(); }

void RecoveryProgressWatchdog::reset() noexcept { seats_ = {}; }

void RecoveryProgressWatchdog::observe(
    std::uint8_t seat, std::uint32_t next_tick,
    Clock::time_point now) noexcept {
    if (seat >= player_limit) return;
    auto& state = seats_[seat];
    if (!state.observed ||
        static_cast<std::int32_t>(next_tick - state.next_tick) > 0) {
        state = {.last_progress = now, .next_tick = next_tick,
                 .observed = true};
    }
}

void RecoveryProgressWatchdog::clear(std::uint8_t seat) noexcept {
    if (seat < player_limit) seats_[seat] = {};
}

bool RecoveryProgressWatchdog::stalled(
    std::uint8_t seat, Clock::time_point now,
    Clock::duration timeout) const noexcept {
    if (seat >= player_limit || timeout <= Clock::duration::zero()) return false;
    const auto& state = seats_[seat];
    return state.observed && now - state.last_progress > timeout;
}

std::uint32_t RecoveryProgressWatchdog::next_tick(
    std::uint8_t seat) const noexcept {
    return seat < player_limit && seats_[seat].observed
        ? seats_[seat].next_tick : 0U;
}

} // namespace btga
