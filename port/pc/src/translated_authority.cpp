#include "btga/translated_authority.hpp"

#include <algorithm>

extern "C" {
#include <btga/btga_gameplay.h>
#include <btga/btga_net.h>
}

namespace btga {

TranslatedAuthority::TranslatedAuthority(
    std::uint8_t occupied_mask, std::uint32_t snapshot_interval,
    std::uint32_t jitter_buffer_ticks)
    : snapshot_interval_(snapshot_interval),
      jitter_buffer_ticks_(jitter_buffer_ticks),
      occupied_mask_(occupied_mask & 0x0fU),
      connected_mask_(occupied_mask_)
{
    if (snapshot_interval_ == 0U) snapshot_interval_ = 1U;
}

bool TranslatedAuthority::submit(
    std::uint8_t seat, std::uint32_t tick, OnlineControllerInput input)
{
    if (seat >= online_player_limit || (occupied_mask_ & (1U << seat)) == 0U ||
        tick < next_tick_ || tick - next_tick_ >= history_size) {
        return false;
    }
    TickInputs& entry = history_[tick % history_size];
    if (entry.tick != tick) entry = {.tick = tick};
    const auto bit = static_cast<std::uint8_t>(1U << seat);
    if ((entry.present_mask & bit) != 0U) return entry.inputs[seat] == input;
    entry.inputs[seat] = input;
    entry.present_mask |= bit;
    return true;
}

bool TranslatedAuthority::ingest_client_packet(
    std::span<const std::uint8_t> packet, std::uint64_t session_id,
    std::uint16_t relay_source_slot)
{
    if (relay_source_slot == 0U || relay_source_slot > online_player_limit) return false;
    const auto seat = static_cast<std::uint8_t>(relay_source_slot - 1U);
    BtgaNetHeader header{};
    if (!btga_net_decode_header(&header, packet.data(), packet.size()) ||
        header.protocol_version != BTGA_NET_PROTOCOL_VERSION ||
        header.message_type != BTGA_NET_MSG_INPUT ||
        header.session_id != session_id || header.peer_id != relay_source_slot ||
        header.payload_size != packet.size() - BTGA_NET_HEADER_SIZE) return false;
    if (have_sequence_[seat] &&
        static_cast<std::int32_t>(header.sequence - latest_sequences_[seat]) <= 0) {
        return true;
    }
    BtgaNetInputBatch batch{};
    const auto payload = packet.subspan(BTGA_NET_HEADER_SIZE);
    if (!btga_net_decode_input_batch(&batch, payload.data(), payload.size()) ||
        batch.player_id != seat) return false;
    for (std::uint8_t index = 0; index < batch.frame_count; ++index) {
        const std::uint32_t tick = batch.first_tick + index;
        if (tick < batch.first_tick) return false;
        if (tick < next_tick_) continue;
        const auto& input = batch.frames[index];
        if (!submit(seat, tick, {input.buttons, input.stick_x, input.stick_y})) return false;
    }
    latest_sequences_[seat] = header.sequence;
    have_sequence_[seat] = true;
    return true;
}

bool TranslatedAuthority::set_connected(std::uint8_t seat, bool connected)
{
    if (seat >= online_player_limit || (occupied_mask_ & (1U << seat)) == 0U) {
        return false;
    }
    const auto bit = static_cast<std::uint8_t>(1U << seat);
    if (connected) connected_mask_ |= bit;
    else {
        connected_mask_ &= static_cast<std::uint8_t>(~bit);
        last_inputs_[seat] = {};
        // A relay resume uses a newly authenticated endpoint and starts a new
        // transport sequence epoch. Only reset replay state after the server
        // has timed the old endpoint out; packets from an active endpoint must
        // remain strictly monotonic.
        have_sequence_[seat] = false;
        latest_sequences_[seat] = 0U;
    }
    return true;
}

bool TranslatedAuthority::ready(std::uint32_t server_tick) const noexcept
{
    if (tick_active_) return false;
    const TickInputs& entry = history_[next_tick_ % history_size];
    const std::uint8_t required = occupied_mask_ & connected_mask_;
    const std::uint8_t present = entry.tick == next_tick_ ? entry.present_mask : 0U;
    return (present & required) == required ||
           server_tick >= next_tick_ + jitter_buffer_ticks_;
}

bool TranslatedAuthority::begin_tick(std::uint32_t server_tick)
{
    if (!ready(server_tick)) return false;
    active_tick_ = next_tick_;
    TickInputs& entry = history_[active_tick_ % history_size];
    if (entry.tick != active_tick_) entry = {.tick = active_tick_};
    received_mask_ = entry.present_mask;
    for (std::size_t seat = 0; seat < online_player_limit; ++seat) {
        const auto bit = static_cast<std::uint8_t>(1U << seat);
        if ((occupied_mask_ & bit) == 0U || (connected_mask_ & bit) == 0U) {
            current_inputs_[seat] = {};
        } else if ((entry.present_mask & bit) != 0U) {
            current_inputs_[seat] = entry.inputs[seat];
            last_inputs_[seat] = current_inputs_[seat];
        } else {
            current_inputs_[seat] = last_inputs_[seat];
        }
    }
    tick_active_ = true;
    return true;
}

std::span<const OnlineControllerInput, online_player_limit>
TranslatedAuthority::inputs() const noexcept { return current_inputs_; }

std::optional<AuthoritativeFrame> TranslatedAuthority::finish_tick(
    std::uint32_t tick, std::span<const std::uint8_t> rdram)
{
    if (!tick_active_ || tick != active_tick_ || rdram.empty()) return std::nullopt;
    const std::uint64_t state_digest = digest(rdram);
    AuthoritativeFrame frame{tick, current_inputs_, state_digest, received_mask_};
    ++next_tick_;
    tick_active_ = false;
    history_[tick % history_size] = {};
    if (next_tick_ % snapshot_interval_ == 0U) {
        snapshot_ = AuthoritativeSnapshot{
            next_tick_, state_digest,
            std::vector<std::uint8_t>{rdram.begin(), rdram.end()}};
    }
    return frame;
}

const std::optional<AuthoritativeSnapshot>&
TranslatedAuthority::latest_snapshot() const noexcept { return snapshot_; }

std::uint32_t TranslatedAuthority::next_tick() const noexcept { return next_tick_; }
std::uint8_t TranslatedAuthority::occupied_mask() const noexcept { return occupied_mask_; }
std::uint8_t TranslatedAuthority::connected_mask() const noexcept { return connected_mask_; }

std::uint64_t TranslatedAuthority::digest(
    std::span<const std::uint8_t> bytes) noexcept
{
    std::uint64_t value = UINT64_C(1469598103934665603);
    for (const std::uint8_t byte : bytes) {
        value ^= byte;
        value *= UINT64_C(1099511628211);
    }
    return value;
}

} // namespace btga
