#include "btga/translated_authority.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

extern "C" {
#include <btga/btga_gameplay.h>
#include <btga/btga_net.h>
}

namespace btga {
namespace {
constexpr std::array<std::uint8_t, 8> journal_magic{
    'B', 'T', 'G', 'A', 'J', 'N', 'L', '3'};
constexpr std::array<std::uint8_t, 8> snapshot_magic{
    'B', 'T', 'G', 'A', 'S', 'N', 'P', '3'};
constexpr std::size_t journal_header_size = 16U;
constexpr std::size_t journal_record_payload_size = 36U;
constexpr std::size_t journal_record_size = 40U;
constexpr std::size_t snapshot_header_size = 32U;
static_assert(journal_record_payload_size ==
    sizeof(std::uint32_t) + online_player_limit * 4U +
    sizeof(std::uint64_t) + 4U + sizeof(std::uint32_t));
static_assert(journal_record_size ==
    journal_record_payload_size + sizeof(std::uint32_t));

void put_u32_be(std::uint8_t* output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 24U);
    output[1] = static_cast<std::uint8_t>(value >> 16U);
    output[2] = static_cast<std::uint8_t>(value >> 8U);
    output[3] = static_cast<std::uint8_t>(value);
}

void put_u64_be(std::uint8_t* output, std::uint64_t value) {
    put_u32_be(output, static_cast<std::uint32_t>(value >> 32U));
    put_u32_be(output + 4U, static_cast<std::uint32_t>(value));
}

std::uint32_t get_u32_be(const std::uint8_t* input) {
    return (static_cast<std::uint32_t>(input[0]) << 24U) |
           (static_cast<std::uint32_t>(input[1]) << 16U) |
           (static_cast<std::uint32_t>(input[2]) << 8U) |
           static_cast<std::uint32_t>(input[3]);
}

std::uint64_t get_u64_be(const std::uint8_t* input) {
    return (static_cast<std::uint64_t>(get_u32_be(input)) << 32U) |
           get_u32_be(input + 4U);
}

std::uint32_t record_checksum(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t value = UINT32_C(2166136261);
    for (const auto byte : bytes) {
        value ^= byte;
        value *= UINT32_C(16777619);
    }
    return value;
}

std::array<std::uint8_t, journal_record_size> encode_record(
    const AuthoritativeFrame& frame) {
    std::array<std::uint8_t, journal_record_size> record{};
    put_u32_be(record.data(), frame.tick);
    std::size_t cursor = 4U;
    for (const auto& input : frame.inputs) {
        record[cursor++] = static_cast<std::uint8_t>(input.buttons >> 8U);
        record[cursor++] = static_cast<std::uint8_t>(input.buttons);
        record[cursor++] = static_cast<std::uint8_t>(input.stick_x);
        record[cursor++] = static_cast<std::uint8_t>(input.stick_y);
    }
    put_u64_be(record.data() + cursor, frame.state_digest);
    cursor += 8U;
    record[cursor++] = frame.received_mask;
    cursor += 3U; // reserved
    put_u32_be(record.data() + cursor, frame.random_seed);
    put_u32_be(record.data() + journal_record_payload_size,
               record_checksum(std::span{record}.first(
                   journal_record_payload_size)));
    return record;
}

bool valid_record(
    const std::array<std::uint8_t, journal_record_size>& record) noexcept {
    return record[29] == 0U && record[30] == 0U && record[31] == 0U &&
        get_u32_be(record.data() + journal_record_payload_size) ==
            record_checksum(std::span{record}.first(
                journal_record_payload_size));
}

AuthoritativeFrame decode_record(
    const std::array<std::uint8_t, journal_record_size>& record) {
    AuthoritativeFrame frame{};
    frame.tick = get_u32_be(record.data());
    std::size_t cursor = 4U;
    for (auto& input : frame.inputs) {
        input.buttons = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(record[cursor]) << 8U) |
            record[cursor + 1U]);
        input.stick_x = static_cast<std::int8_t>(record[cursor + 2U]);
        input.stick_y = static_cast<std::int8_t>(record[cursor + 3U]);
        cursor += 4U;
    }
    frame.state_digest = get_u64_be(record.data() + cursor);
    cursor += 8U;
    frame.received_mask = record[cursor++];
    cursor += 3U; // reserved
    frame.random_seed = get_u32_be(record.data() + cursor);
    return frame;
}

} // namespace

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

TranslatedAuthority::~TranslatedAuthority()
{
    if (journal_file_.is_open()) journal_file_.close();
    if (remove_journal_on_close_ && !journal_path_.empty()) {
        std::error_code error;
        std::filesystem::remove(journal_path_, error);
        std::filesystem::remove(journal_path_.string() + ".snapshot.0", error);
        std::filesystem::remove(journal_path_.string() + ".snapshot.1", error);
    }
}

bool TranslatedAuthority::checkpoint_now(
    std::span<const std::uint8_t> checkpoint_state) {
    if (tick_active_ || checkpoint_state.empty()) return false;
    snapshot_ = AuthoritativeSnapshot{
        next_tick_, digest(checkpoint_state),
        std::vector<std::uint8_t>{checkpoint_state.begin(), checkpoint_state.end()}};
    if (persistent_journal_ && !persist_snapshot(*snapshot_)) {
        persistent_storage_healthy_ = false;
        return false;
    }
    return true;
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

bool TranslatedAuthority::begin_connection_epoch(std::uint8_t seat) noexcept
{
    if (seat >= online_player_limit || (occupied_mask_ & (1U << seat)) == 0U) {
        return false;
    }
    have_sequence_[seat] = false;
    latest_sequences_[seat] = 0U;
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
    // Inputs queued before a seat timed out remain useful only if that seat is
    // still connected when this tick is committed. Advertising those stale
    // presence bits would violate the wire invariant that received_mask is a
    // subset of connected_mask and would make the canonical frame impossible
    // to encode.
    received_mask_ = static_cast<std::uint8_t>(
        entry.present_mask & occupied_mask_ & connected_mask_);
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
    std::uint32_t tick, std::span<const std::uint8_t> canonical_state,
    std::span<const std::uint8_t> checkpoint_state)
{
    if (!tick_active_ || tick != active_tick_ || canonical_state.empty() ||
        (!disk_journal_enabled_ && canonical_journal_.size() >= journal_limit)) {
        return std::nullopt;
    }
    const std::uint64_t state_digest = digest(canonical_state);
    AuthoritativeFrame frame{tick, current_inputs_, state_digest, received_mask_};
    if (canonical_state.size() >= sizeof(frame.random_seed)) {
        std::memcpy(&frame.random_seed, canonical_state.data(),
                    sizeof(frame.random_seed));
    }
    if (tick != next_tick_ || !append_journal(frame)) return std::nullopt;
    ++next_tick_;
    tick_active_ = false;
    history_[tick % history_size] = {};
    if (next_tick_ % snapshot_interval_ == 0U) {
        const auto state = checkpoint_state.empty()
            ? canonical_state : checkpoint_state;
        snapshot_ = AuthoritativeSnapshot{
            next_tick_, digest(state),
            std::vector<std::uint8_t>{state.begin(), state.end()}};
        if (persistent_journal_ && !persist_snapshot(*snapshot_)) {
            // The prior alternating checkpoint remains valid. The journal is
            // still canonical and can replay forward from it, so gameplay may
            // continue while the operator observes the missing new checkpoint.
            persistent_storage_healthy_ = false;
        }
    }
    return frame;
}

const std::optional<AuthoritativeSnapshot>&
TranslatedAuthority::latest_snapshot() const noexcept { return snapshot_; }

std::uint32_t TranslatedAuthority::next_tick() const noexcept { return next_tick_; }
std::uint8_t TranslatedAuthority::occupied_mask() const noexcept { return occupied_mask_; }
std::uint8_t TranslatedAuthority::connected_mask() const noexcept { return connected_mask_; }

std::size_t TranslatedAuthority::journal_size() const noexcept {
    return next_tick_;
}

std::size_t TranslatedAuthority::copy_journal_from(
    std::uint32_t first_tick, std::span<AuthoritativeFrame> destination) const
{
    if (first_tick >= next_tick_ || destination.empty()) return 0U;
    std::size_t copied = 0U;
    if (first_tick < journal_base_tick_ &&
        !read_disk_journal(first_tick, destination, copied)) return 0U;
    const std::uint32_t next = first_tick + static_cast<std::uint32_t>(copied);
    if (copied < destination.size() && next >= journal_base_tick_) {
        const std::size_t offset = static_cast<std::size_t>(next - journal_base_tick_);
        if (offset < canonical_journal_.size()) {
            const std::size_t count = (std::min)(
                destination.size() - copied, canonical_journal_.size() - offset);
            std::copy_n(canonical_journal_.begin() + offset, count,
                        destination.begin() + copied);
            copied += count;
        }
    }
    return copied;
}

bool TranslatedAuthority::copy_complete_journal(
    std::vector<AuthoritativeFrame>& destination) const
{
    destination.resize(next_tick_);
    if (destination.empty()) return true;
    if (copy_journal_from(0U, destination) != destination.size()) {
        destination.clear();
        return false;
    }
    for (std::size_t index = 0; index < destination.size(); ++index) {
        if (destination[index].tick != index) {
            destination.clear();
            return false;
        }
    }
    return true;
}

bool TranslatedAuthority::enable_disk_journal(
    const std::filesystem::path& path, std::size_t memory_frames)
{
    return create_disk_journal(path, memory_frames, true);
}

bool TranslatedAuthority::enable_persistent_journal(
    const std::filesystem::path& path, std::size_t memory_frames)
{
    return create_disk_journal(path, memory_frames, false);
}

bool TranslatedAuthority::create_disk_journal(
    const std::filesystem::path& path, std::size_t memory_frames,
    bool remove_on_close)
{
    if (disk_journal_enabled_ || next_tick_ != 0U || tick_active_ || path.empty() ||
        memory_frames == 0U) {
        return false;
    }
    journal_file_.open(path, std::ios::binary | std::ios::in | std::ios::out |
        std::ios::trunc);
    if (!journal_file_) return false;
    std::array<std::uint8_t, journal_header_size> header{};
    std::copy(journal_magic.begin(), journal_magic.end(), header.begin());
    put_u32_be(header.data() + 8U, 2U);
    put_u32_be(header.data() + 12U, static_cast<std::uint32_t>(journal_record_size));
    journal_file_.write(reinterpret_cast<const char*>(header.data()), header.size());
    journal_file_.flush();
    if (!journal_file_) {
        journal_file_.close();
        return false;
    }
    journal_path_ = path;
    journal_memory_limit_ = memory_frames;
    disk_journal_enabled_ = true;
    remove_journal_on_close_ = remove_on_close;
    persistent_journal_ = !remove_on_close;
    return true;
}

bool TranslatedAuthority::open_persistent_journal(
    const std::filesystem::path& path, std::size_t memory_frames)
{
    if (disk_journal_enabled_ || next_tick_ != 0U || tick_active_ || path.empty() ||
        memory_frames == 0U) return false;
    journal_file_.open(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!journal_file_) return false;
    std::array<std::uint8_t, journal_header_size> header{};
    journal_file_.read(reinterpret_cast<char*>(header.data()), header.size());
    if (!journal_file_ || !std::equal(journal_magic.begin(), journal_magic.end(), header.begin()) ||
        get_u32_be(header.data() + 8U) != 2U ||
        get_u32_be(header.data() + 12U) != journal_record_size) {
        journal_file_.close();
        return false;
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size < journal_header_size) {
        journal_file_.close();
        return false;
    }
    const auto payload = size - journal_header_size;
    const auto complete = payload / journal_record_size;
    if (complete > (std::numeric_limits<std::uint32_t>::max)()) {
        journal_file_.close();
        return false;
    }
    if (payload % journal_record_size != 0U) {
        journal_file_.close();
        std::filesystem::resize_file(
            path, journal_header_size + complete * journal_record_size, error);
        if (error) return false;
        journal_file_.open(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!journal_file_) return false;
    }
    std::deque<AuthoritativeFrame> loaded_journal;
    journal_file_.clear();
    journal_file_.seekg(journal_header_size, std::ios::beg);
    for (std::uint32_t tick = 0; tick < complete; ++tick) {
        std::array<std::uint8_t, journal_record_size> record{};
        journal_file_.read(reinterpret_cast<char*>(record.data()), record.size());
        if (!journal_file_) {
            journal_file_.close();
            return false;
        }
        const auto frame = decode_record(record);
        if (!valid_record(record) || frame.tick != tick) {
            journal_file_.close();
            return false;
        }
        loaded_journal.push_back(frame);
        if (loaded_journal.size() > memory_frames) loaded_journal.pop_front();
    }
    canonical_journal_ = std::move(loaded_journal);
    next_tick_ = static_cast<std::uint32_t>(complete);
    journal_base_tick_ = next_tick_ - static_cast<std::uint32_t>(canonical_journal_.size());
    if (!canonical_journal_.empty()) last_inputs_ = canonical_journal_.back().inputs;
    journal_path_ = path;
    journal_memory_limit_ = memory_frames;
    disk_journal_enabled_ = true;
    remove_journal_on_close_ = false;
    persistent_journal_ = true;
    journal_file_.clear();
    journal_file_.seekp(0, std::ios::end);
    return load_persistent_snapshot();
}

std::uint32_t TranslatedAuthority::journal_base_tick() const noexcept {
    return journal_base_tick_;
}

bool TranslatedAuthority::persistent_storage_healthy() const noexcept {
    return persistent_storage_healthy_;
}

bool TranslatedAuthority::append_journal(const AuthoritativeFrame& frame)
{
    if (disk_journal_enabled_) {
        const auto record = encode_record(frame);
        journal_file_.clear();
        journal_file_.seekp(0, std::ios::end);
        journal_file_.write(
            reinterpret_cast<const char*>(record.data()), record.size());
        journal_file_.flush();
        if (!journal_file_) return false;
    }
    canonical_journal_.push_back(frame);
    if (canonical_journal_.size() > journal_memory_limit_) {
        canonical_journal_.pop_front();
        ++journal_base_tick_;
    }
    return true;
}

bool TranslatedAuthority::persist_snapshot(const AuthoritativeSnapshot& snapshot)
{
    if (journal_path_.empty() || snapshot.state.empty() ||
        digest(snapshot.state) != snapshot.state_digest ||
        snapshot.state.size() > (std::numeric_limits<std::uint32_t>::max)()) return false;
    const unsigned slot = (snapshot.next_tick / snapshot_interval_) & 1U;
    const auto destination = std::filesystem::path{
        journal_path_.string() + ".snapshot." + std::to_string(slot)};
    const auto temporary = std::filesystem::path{destination.string() + ".tmp"};
    std::array<std::uint8_t, snapshot_header_size> header{};
    std::copy(snapshot_magic.begin(), snapshot_magic.end(), header.begin());
    put_u32_be(header.data() + 8U, 1U);
    put_u32_be(header.data() + 12U, snapshot.next_tick);
    put_u64_be(header.data() + 16U, snapshot.state_digest);
    put_u32_be(header.data() + 24U, static_cast<std::uint32_t>(snapshot.state.size()));
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output) return false;
        output.write(reinterpret_cast<const char*>(header.data()), header.size());
        output.write(reinterpret_cast<const char*>(snapshot.state.data()),
                     static_cast<std::streamsize>(snapshot.state.size()));
        output.flush();
        if (!output) return false;
    }
    std::error_code error;
#if defined(_WIN32)
    // Keep POSIX checkpoint replacement atomic. Windows requires the existing
    // destination to be removed before rename; the alternating slot retains a
    // separately validated prior checkpoint across that short window.
    std::filesystem::remove(destination, error);
    error.clear();
#endif
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

bool TranslatedAuthority::load_persistent_snapshot()
{
    std::optional<AuthoritativeSnapshot> newest;
    for (unsigned slot = 0; slot < 2U; ++slot) {
        const auto path = std::filesystem::path{
            journal_path_.string() + ".snapshot." + std::to_string(slot)};
        std::ifstream input{path, std::ios::binary};
        if (!input) continue;
        std::array<std::uint8_t, snapshot_header_size> header{};
        input.read(reinterpret_cast<char*>(header.data()), header.size());
        if (!input || !std::equal(snapshot_magic.begin(), snapshot_magic.end(), header.begin()) ||
            get_u32_be(header.data() + 8U) != 1U) continue;
        const auto tick = get_u32_be(header.data() + 12U);
        const auto state_digest = get_u64_be(header.data() + 16U);
        const auto state_size = get_u32_be(header.data() + 24U);
        if (tick > next_tick_ || state_size == 0U || state_size > 16U * 1024U * 1024U) continue;
        std::vector<std::uint8_t> state(state_size);
        input.read(reinterpret_cast<char*>(state.data()), state.size());
        if (!input || input.peek() != std::ifstream::traits_type::eof() ||
            digest(state) != state_digest) continue;
        if (!newest || tick > newest->next_tick) {
            newest = AuthoritativeSnapshot{tick, state_digest, std::move(state)};
        }
    }
    snapshot_ = std::move(newest);
    return snapshot_.has_value() || next_tick_ == 0U;
}

bool TranslatedAuthority::read_disk_journal(
    std::uint32_t first_tick, std::span<AuthoritativeFrame> destination,
    std::size_t& copied) const
{
    copied = 0U;
    if (!disk_journal_enabled_ || first_tick >= journal_base_tick_) return false;
    journal_file_.flush();
    journal_file_.clear();
    const auto offset = static_cast<std::streamoff>(journal_header_size) +
        static_cast<std::streamoff>(first_tick) *
            static_cast<std::streamoff>(journal_record_size);
    journal_file_.seekg(offset, std::ios::beg);
    const std::size_t wanted = (std::min)(
        destination.size(), static_cast<std::size_t>(journal_base_tick_ - first_tick));
    for (; copied < wanted; ++copied) {
        std::array<std::uint8_t, journal_record_size> record{};
        journal_file_.read(reinterpret_cast<char*>(record.data()), record.size());
        if (!journal_file_ || !valid_record(record)) return false;
        destination[copied] = decode_record(record);
        if (destination[copied].tick != first_tick + copied) return false;
    }
    return true;
}

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
