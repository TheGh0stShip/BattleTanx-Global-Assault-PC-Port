#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <vector>

#include "btga/authoritative_session.hpp"

namespace btga {

// Authority adapter for the translated N64 runtime. The translated game owns
// the call stack and invokes the two boundaries around its real gameplay tick;
// this object owns input selection and canonical state publication.
class TranslatedAuthority {
public:
    explicit TranslatedAuthority(std::uint8_t occupied_mask = 0x0fU,
                                 std::uint32_t snapshot_interval = 30U,
                                 std::uint32_t jitter_buffer_ticks = 3U);
    ~TranslatedAuthority();

    [[nodiscard]] bool submit(std::uint8_t seat, std::uint32_t tick,
                              OnlineControllerInput input);
    [[nodiscard]] bool ingest_client_packet(
        std::span<const std::uint8_t> packet, std::uint64_t session_id,
        std::uint16_t relay_source_slot);
    [[nodiscard]] bool set_connected(std::uint8_t seat, bool connected);

    // Called once at the entry of the translated gameplay update. Returns the
    // immutable controller inputs that get_input must expose for this tick.
    [[nodiscard]] bool ready(std::uint32_t server_tick) const noexcept;
    [[nodiscard]] bool begin_tick(std::uint32_t server_tick);
    [[nodiscard]] std::span<const OnlineControllerInput, online_player_limit>
    inputs() const noexcept;

    // Called once at the matching exit after the translated game has mutated
    // RDRAM. A repeated or out-of-order completion is rejected.
    [[nodiscard]] std::optional<AuthoritativeFrame> finish_tick(
        std::uint32_t tick, std::span<const std::uint8_t> rdram);

    [[nodiscard]] const std::optional<AuthoritativeSnapshot>&
    latest_snapshot() const noexcept;
    [[nodiscard]] std::uint32_t next_tick() const noexcept;
    [[nodiscard]] std::uint8_t occupied_mask() const noexcept;
    [[nodiscard]] std::uint8_t connected_mask() const noexcept;
    [[nodiscard]] std::size_t journal_size() const noexcept;
    [[nodiscard]] std::size_t copy_journal_from(
        std::uint32_t first_tick, std::span<AuthoritativeFrame> destination) const;
    // Spool the complete canonical input history to an endian-stable file
    // while retaining only a bounded replay tail in memory. This must be
    // enabled before the first gameplay tick.
    [[nodiscard]] bool enable_disk_journal(
        const std::filesystem::path& path, std::size_t memory_frames = 4096U);
    // Persistent journals survive process shutdown. Two alternating,
    // checksummed portable-state checkpoints sit beside the input journal so
    // a replacement authority can restore the newest complete checkpoint and
    // replay the remaining canonical frames. `open_persistent_journal` never
    // trusts a partial record or checkpoint.
    [[nodiscard]] bool enable_persistent_journal(
        const std::filesystem::path& path, std::size_t memory_frames = 4096U);
    [[nodiscard]] bool open_persistent_journal(
        const std::filesystem::path& path, std::size_t memory_frames = 4096U);
    [[nodiscard]] std::uint32_t journal_base_tick() const noexcept;
    [[nodiscard]] bool persistent_storage_healthy() const noexcept;

private:
    struct TickInputs {
        std::uint32_t tick = 0;
        std::uint8_t present_mask = 0;
        std::array<OnlineControllerInput, online_player_limit> inputs{};
    };

    static constexpr std::size_t history_size = 256;
    // In-memory-only authorities retain the historical safety bound. Runtime
    // authorities enable the disk journal, removing this match-duration cliff
    // without allowing resident memory to grow with match age.
    static constexpr std::size_t journal_limit = 1'000'000;
    [[nodiscard]] static std::uint64_t digest(
        std::span<const std::uint8_t> bytes) noexcept;

    std::array<TickInputs, history_size> history_{};
    std::array<OnlineControllerInput, online_player_limit> current_inputs_{};
    std::array<OnlineControllerInput, online_player_limit> last_inputs_{};
    std::array<std::uint32_t, online_player_limit> latest_sequences_{};
    std::array<bool, online_player_limit> have_sequence_{};
    [[nodiscard]] bool append_journal(const AuthoritativeFrame& frame);
    [[nodiscard]] bool read_disk_journal(
        std::uint32_t first_tick, std::span<AuthoritativeFrame> destination,
        std::size_t& copied) const;
    [[nodiscard]] bool create_disk_journal(
        const std::filesystem::path& path, std::size_t memory_frames,
        bool remove_on_close);
    [[nodiscard]] bool persist_snapshot(const AuthoritativeSnapshot& snapshot);
    [[nodiscard]] bool load_persistent_snapshot();

    std::deque<AuthoritativeFrame> canonical_journal_;
    std::filesystem::path journal_path_;
    mutable std::fstream journal_file_;
    std::size_t journal_memory_limit_ = journal_limit;
    std::uint32_t journal_base_tick_ = 0;
    bool disk_journal_enabled_ = false;
    bool remove_journal_on_close_ = true;
    bool persistent_journal_ = false;
    bool persistent_storage_healthy_ = true;
    std::optional<AuthoritativeSnapshot> snapshot_;
    std::uint32_t next_tick_ = 0;
    std::uint32_t active_tick_ = 0;
    std::uint32_t snapshot_interval_ = 30;
    std::uint32_t jitter_buffer_ticks_ = 3;
    std::uint8_t occupied_mask_ = 0x0fU;
    std::uint8_t connected_mask_ = 0x0fU;
    std::uint8_t received_mask_ = 0;
    bool tick_active_ = false;
};

} // namespace btga
