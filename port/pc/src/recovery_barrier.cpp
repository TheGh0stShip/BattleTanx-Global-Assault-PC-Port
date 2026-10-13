#include "btga/recovery_barrier.hpp"

namespace btga {
namespace {
constexpr std::uint8_t seat_bit(std::uint8_t seat) noexcept {
    return static_cast<std::uint8_t>(1U << seat);
}
} // namespace

void RecoveryBarrier::reset() noexcept { recovering_mask_ = 0U; }

void RecoveryBarrier::begin(
    std::uint8_t seat, std::uint32_t authority_head) noexcept {
    if (seat >= player_limit || authority_head == 0U) return;
    recovering_mask_ |= seat_bit(seat);
}

void RecoveryBarrier::acknowledge(
    std::uint8_t seat, std::uint32_t client_next_tick,
    std::uint32_t authority_head) noexcept {
    if (seat >= player_limit) return;
    const auto bit = seat_bit(seat);
    if ((recovering_mask_ & bit) != 0U && client_next_tick >= authority_head) {
        recovering_mask_ &= static_cast<std::uint8_t>(~bit);
    }
}

void RecoveryBarrier::disconnect(std::uint8_t seat) noexcept {
    if (seat >= player_limit) return;
    recovering_mask_ &= static_cast<std::uint8_t>(~seat_bit(seat));
}

bool RecoveryBarrier::paused() const noexcept { return recovering_mask_ != 0U; }

bool RecoveryBarrier::recovering(std::uint8_t seat) const noexcept {
    return seat < player_limit &&
        (recovering_mask_ & seat_bit(seat)) != 0U;
}

std::uint8_t RecoveryBarrier::mask() const noexcept { return recovering_mask_; }

} // namespace btga
