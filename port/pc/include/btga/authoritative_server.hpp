#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "btga/authoritative_session.hpp"

namespace btga {

struct AuthorityEmission {
    std::vector<std::uint8_t> frame_payload;
    std::vector<std::vector<std::uint8_t>> snapshot_payloads;
};

// Transport-neutral heart of the dedicated server. UDP/relay code supplies
// decoded client payloads and sends the returned canonical payloads; it never
// participates in simulation decisions.
class AuthoritativeServerCore {
public:
    explicit AuthoritativeServerCore(std::uint8_t occupied_mask = 0x0fU,
                                     std::uint32_t snapshot_interval = 30U,
                                     std::uint32_t jitter_buffer_ticks = 3U);

    [[nodiscard]] bool ingest_input_payload(std::span<const std::uint8_t> payload);
    [[nodiscard]] bool ingest_client_packet(
        std::span<const std::uint8_t> packet, std::uint64_t session_id,
        std::uint16_t relay_source_slot);
    [[nodiscard]] bool set_connected(std::uint8_t seat, bool connected);
    [[nodiscard]] std::optional<AuthorityEmission> step(
        AuthoritativeSimulation& simulation, std::uint32_t server_tick);
    [[nodiscard]] std::optional<std::vector<std::vector<std::uint8_t>>> make_resync(
        const AuthoritativeSimulation& simulation);
    [[nodiscard]] const AuthoritativeSession& session() const noexcept;

private:
    [[nodiscard]] std::optional<std::vector<std::vector<std::uint8_t>>>
    encode_snapshot(std::uint32_t tick, std::uint64_t digest,
                    std::span<const std::uint8_t> state);

    AuthoritativeSession session_;
    std::uint32_t snapshot_id_ = 0;
    std::uint32_t emitted_snapshot_tick_ = 0;
    std::array<std::uint32_t, online_player_limit> latest_sequences_{};
    std::array<bool, online_player_limit> have_sequence_{};
};

} // namespace btga
