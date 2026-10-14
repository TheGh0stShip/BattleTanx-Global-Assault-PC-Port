#include "dedicated_runtime_bridge.hpp"
#include "dedicated_client_bridge.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <chrono>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "btga/authoritative_server.hpp"
#include "btga/authority_health.hpp"
#include "btga/authority_lease.hpp"
#include "btga/bootstrap_recovery.hpp"
#include "btga/client_resync.hpp"
#include "btga/gameplay_digest.hpp"
#include "btga/fixed_tick_clock.hpp"
#include "btga/snapshot_send_cursor.hpp"
#include "btga/gameplay_snapshot.hpp"
#include "btga/master_client.hpp"
#include "btga/online_multiplayer.hpp"
#include "btga/translated_authority.hpp"
#include "btga/udp_online_io.hpp"
#include "recomp.h"

extern "C" void func_8009C524(std::uint8_t* rdram, recomp_context* ctx);
extern "C" void func_80079FF0(std::uint8_t* rdram, recomp_context* ctx);
extern "C" void func_800CF0CC(std::uint8_t* rdram, recomp_context* ctx);

extern "C" {
#include <btga/btga_gameplay.h>
#include <btga/btga_net.h>
#include <btga/btga_relay_client.h>
}

namespace {
constexpr std::size_t rdram_size = 8U * 1024U * 1024U;
constexpr auto bootstrap_retry_delay = std::chrono::seconds{3};
constexpr auto stale_input_timeout = std::chrono::seconds{2};
constexpr auto recovery_abandon_timeout = std::chrono::seconds{10};
constexpr auto recovery_stall_timeout = std::chrono::seconds{15};
static_assert(recovery_abandon_timeout > bootstrap_retry_delay,
              "recovery state must survive at least one snapshot retry");
static_assert(btga::BootstrapRecovery::player_limit == btga::online_player_limit,
              "recovery and authoritative seat capacities must match");
std::mutex authority_mutex;
std::mutex master_mutex;
btga::TranslatedAuthority authority{0x0fU, 30U, 3U};
btga::BootstrapRecovery bootstrap_recovery;
btga::RecoveryProgressWatchdog recovery_watchdog;
std::array<btga::OnlineControllerInput, btga::online_player_limit> active_inputs{};
btga::ClientInputLiveness client_liveness;
std::chrono::steady_clock::time_point authority_started;
std::atomic_uint64_t authority_loop_sequence{0U};
bool enabled = false;
bool gameplay_tick_active = false;
bool recovery_replay_running = false;
bool recovery_pending = false;
bool recovery_pause_announced = false;
bool match_started = false;
bool transition_checkpoint_pending = false;
thread_local bool dedicated_loop_running = false;
std::uint32_t active_tick = 0;
std::uint32_t inactive_tick_probes = 0;
std::uint32_t network_poll_count = 0;
std::uint32_t received_datagrams = 0;
std::array<std::uint32_t, btga::online_player_limit> latest_input_horizons{};
std::uint64_t simulation_stalls = 0U;
std::uint64_t maximum_tick_lag_microseconds = 0U;
std::uint64_t session_id = 0;
std::uint32_t outgoing_sequence = 0;
std::uint32_t snapshot_id = 0;
std::uint8_t snapshot_subscribers = 0;
std::uint8_t snapshot_requests = 0;
std::uint8_t bootstrap_requests = 0;
std::uint8_t replay_requests = 0;
std::uint8_t disk_replay_announced = 0;
std::uint8_t replay_send_failure_announced = 0;
std::uint8_t snapshot_cursor = 0;
std::array<std::chrono::steady_clock::time_point, btga::online_player_limit>
    snapshot_last_sent{};
std::array<std::uint32_t, btga::online_player_limit> replay_next_tick{};
std::array<std::uint32_t, btga::online_player_limit> snapshot_retry_rotation{};
std::vector<std::uint8_t> initial_snapshot_state;
bool initial_snapshot_available = false;
struct SnapshotTransfer {
    std::vector<std::uint8_t> state;
    std::uint64_t canonical_digest = 0U;
    std::uint32_t tick = 0U;
    std::uint32_t id = 0U;
    std::uint16_t destination_slot = 0U;
    btga::SnapshotSendCursor cursor;
};
SnapshotTransfer snapshot_transfer;
unsigned int configured_players = 4;
std::unique_ptr<btga::UdpOnlineIo> udp;
std::unique_ptr<btga::OnlineMultiplayer> multiplayer;
std::jthread network_thread;
std::unique_ptr<btga::HttpClient> master_http;
std::unique_ptr<btga::MasterClient> master;
std::string public_lobby_id;
std::string public_host_secret;
std::string pending_authority_relay_token;
std::chrono::steady_clock::time_point next_heartbeat{};
std::chrono::steady_clock::time_point next_recovery_status{};
std::optional<std::filesystem::path> persistent_lease_path;
btga::PersistentAuthorityLease persistent_lease;
std::optional<std::filesystem::path> authority_health_path;
std::chrono::steady_clock::time_point next_health_write{};
std::uint64_t last_health_sequence = 0U;

std::uint64_t canonical_state_digest(std::span<const std::uint8_t> state) {
    std::uint64_t value = UINT64_C(1469598103934665603);
    for (const auto byte : state) {
        value ^= byte;
        value *= UINT64_C(1099511628211);
    }
    return value;
}

void poll_authority_health() {
    const auto now = std::chrono::steady_clock::now();
    if (!authority_health_path || now < next_health_write) return;
    next_health_write = now + std::chrono::seconds{1};
    const auto sequence = authority_loop_sequence.load(std::memory_order_acquire);

    btga::AuthorityHealth health;
    std::filesystem::path path;
    {
        const std::unique_lock lock{authority_mutex, std::try_to_lock};
        if (!lock.owns_lock()) return;
        if (!authority_health_path) return;
        path = *authority_health_path;
        health = {sequence, authority.next_tick(), authority.connected_mask(),
                  bootstrap_recovery.mask(), gameplay_tick_active};
    }
    // An empty lobby parks the translated frontend after one inactive pass,
    // while this network pump remains responsible for admission and master
    // heartbeats. Refresh idle health from the pump, but require translated
    // loop progress once gameplay is active so match hangs remain detectable.
    if (health.gameplay_tick_active && sequence == last_health_sequence) return;
    if (btga::save_authority_health(path, health)) {
        last_health_sequence = sequence;
    } else {
        std::fputs("BTGA_AUTHORITY could not update process health\n", stderr);
    }
}

void poll_master_heartbeat() {
    const auto now = std::chrono::steady_clock::now();
    unsigned players = 0;
    std::uint8_t seat_mask = 0U;
    std::uint32_t packet_sequence = 0U;
    bool playing = false;
    {
        const std::unique_lock lock{authority_mutex, std::try_to_lock};
        if (!lock.owns_lock()) return;
        seat_mask = authority.connected_mask();
        for (unsigned seat = 0; seat < 4; ++seat) {
            players += (seat_mask >> seat) & 1U;
        }
        packet_sequence = outgoing_sequence;
        playing = match_started;
    }
    std::string refreshed_relay_token;
    {
        const std::lock_guard lock{master_mutex};
        if (!master || now < next_heartbeat) return;
        try {
            refreshed_relay_token = master->heartbeat(
                public_lobby_id, public_host_secret,
                static_cast<std::uint16_t>(players),
                playing ? "playing" : "lobby", seat_mask);
            if (persistent_lease_path) {
                persistent_lease.packet_sequence = packet_sequence;
                if (!btga::save_authority_lease(
                        *persistent_lease_path, persistent_lease)) {
                    std::fputs(
                        "BTGA_AUTHORITY could not persist packet sequence\n", stderr);
                }
            }
        } catch (const std::exception& exception) {
            std::fprintf(
                stderr, "BTGA_AUTHORITY heartbeat failed: %s\n", exception.what());
        }
        next_heartbeat = now + std::chrono::seconds{15};
    }
    if (!refreshed_relay_token.empty()) {
        const std::lock_guard lock{authority_mutex};
        pending_authority_relay_token = std::move(refreshed_relay_token);
    }
}

std::uint32_t server_clock_tick() {
    const auto elapsed = std::chrono::steady_clock::now() - authority_started;
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    if (nanoseconds <= 0) return 0U;
    return static_cast<std::uint32_t>(btga::fixed_rate_tick(
        static_cast<std::uint64_t>(nanoseconds), 30U));
}

extern "C" void btga_advance_retail_mode_counters(std::uint8_t* rdram) {
    constexpr gpr inner_previous = static_cast<gpr>(static_cast<std::int32_t>(0x802195C0U));
    constexpr gpr inner_counter = static_cast<gpr>(static_cast<std::int32_t>(0x802195C4U));
    constexpr gpr inner_target = static_cast<gpr>(static_cast<std::int32_t>(0x802195C8U));
    constexpr gpr inner_current = static_cast<gpr>(static_cast<std::int32_t>(0x802195CCU));
    constexpr gpr outer_previous = static_cast<gpr>(static_cast<std::int32_t>(0x802195D0U));
    constexpr gpr outer_counter = static_cast<gpr>(static_cast<std::int32_t>(0x802195D4U));
    constexpr gpr outer_target = static_cast<gpr>(static_cast<std::int32_t>(0x802195D8U));
    constexpr gpr outer_current = static_cast<gpr>(static_cast<std::int32_t>(0x802195DCU));

    if (MEM_W(0, outer_current) != MEM_W(0, outer_target)) {
        MEM_W(0, outer_previous) = MEM_W(0, outer_current);
        MEM_W(0, outer_current) = MEM_W(0, outer_target);
        MEM_W(0, outer_counter) = 0;
    } else {
        ++MEM_W(0, outer_counter);
    }
    if (MEM_W(0, inner_current) != MEM_W(0, inner_target)) {
        MEM_W(0, inner_previous) = MEM_W(0, inner_current);
        MEM_W(0, inner_current) = MEM_W(0, inner_target);
        MEM_W(0, inner_counter) = 0;
    } else {
        ++MEM_W(0, inner_counter);
    }
}

bool enable_relay(std::uint64_t requested_session, const btga::RelayEndpoint& relay,
                  std::string_view token, std::string& error) {
    if (requested_session == 0U || relay.address.empty() || relay.port == 0U || token.empty()) {
        error = "dedicated session, relay endpoint, or authority token is invalid";
        return false;
    }
    authority_health_path.reset();
    session_id = requested_session;
    const auto fault_profile = btga::udp_test_fault_profile_from_environment();
    udp = std::make_unique<btga::UdpOnlineIo>(relay.address, relay.port);
    multiplayer = std::make_unique<btga::OnlineMultiplayer>(*udp);
    if (!multiplayer->enter_online() || !multiplayer->begin_relay(
            true, session_id,
            {reinterpret_cast<const std::uint8_t*>(token.data()), token.size()})) {
        multiplayer.reset();
        udp.reset();
        error = "dedicated authority could not begin relay admission";
        return false;
    }
    const auto bind_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < bind_deadline &&
           !multiplayer->poll_relay_binding()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    if (multiplayer->state() != btga::OnlineState::Lobby) {
        multiplayer.reset();
        udp.reset();
        error = "dedicated authority relay binding timed out";
        return false;
    }
    udp->set_fault_profile(fault_profile);
    if (fault_profile.drop_outgoing_every != 0U ||
        fault_profile.drop_incoming_every != 0U ||
        fault_profile.delay_milliseconds != 0U) {
        std::fprintf(stderr,
                     "BTGA_AUTHORITY test UDP loss outgoing=%u incoming=%u delay_ms=%u\n",
                     fault_profile.drop_outgoing_every,
                     fault_profile.drop_incoming_every,
                     fault_profile.delay_milliseconds);
    }
    bool journal_ready = false;
    try {
        std::size_t journal_memory_frames = 4096U;
        if (const char* configured = std::getenv(
                "BTGA_DEDICATED_JOURNAL_MEMORY_FRAMES")) {
            const std::string_view text{configured};
            std::size_t parsed = 0U;
            const auto result = std::from_chars(
                text.data(), text.data() + text.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
                parsed < 32U || parsed > 1'000'000U) {
                error = "BTGA_DEDICATED_JOURNAL_MEMORY_FRAMES must be 32..1000000";
                multiplayer.reset();
                udp.reset();
                return false;
            }
            journal_memory_frames = parsed;
        }
        if (const char* configured_directory = std::getenv("BTGA_DEDICATED_STATE_DIR");
            configured_directory != nullptr && *configured_directory != '\0') {
            const std::filesystem::path directory{configured_directory};
            std::error_code filesystem_error;
            std::filesystem::create_directories(directory, filesystem_error);
            if (filesystem_error) {
                error = "could not create BTGA_DEDICATED_STATE_DIR";
            } else {
                authority_health_path = directory / "authority-health.json";
                std::array<char, 64> name{};
                std::snprintf(name.data(), name.size(), "authority-%016llx.journal",
                              static_cast<unsigned long long>(requested_session));
                const auto path = directory / name.data();
                if (std::filesystem::exists(path)) {
                    journal_ready = authority.open_persistent_journal(
                        path, journal_memory_frames);
                    recovery_pending = journal_ready && authority.next_tick() != 0U;
                    if (!journal_ready) {
                        error = "persistent authority journal or checkpoint is invalid";
                    }
                } else {
                    journal_ready = authority.enable_persistent_journal(
                        path, journal_memory_frames);
                }
            }
        } else {
            const auto directory = std::filesystem::temp_directory_path();
            std::random_device random;
            for (unsigned attempt = 0; attempt < 16U && !journal_ready; ++attempt) {
                const auto nonce = (static_cast<std::uint64_t>(random()) << 32U) |
                    static_cast<std::uint64_t>(random());
                std::array<char, 64> name{};
                std::snprintf(name.data(), name.size(),
                              "btga-authority-%016llx-%016llx.journal",
                              static_cast<unsigned long long>(requested_session),
                              static_cast<unsigned long long>(nonce));
                const auto path = directory / name.data();
                if (!std::filesystem::exists(path)) {
                    journal_ready = authority.enable_disk_journal(
                        path, journal_memory_frames);
                }
            }
        }
    } catch (const std::exception& exception) {
        error = std::string{"could not create canonical journal: "} + exception.what();
    }
    if (!journal_ready) {
        multiplayer.reset();
        udp.reset();
        if (error.empty()) error = "could not create canonical disk journal";
        return false;
    }
    const auto recovered_elapsed = std::chrono::nanoseconds{
        static_cast<std::int64_t>(
            (static_cast<std::uint64_t>(authority.next_tick()) *
             UINT64_C(1000000000)) / 30U)};
    authority_started = std::chrono::steady_clock::now() - recovered_elapsed;
    client_liveness.reset();
    latest_input_horizons.fill(0U);
    simulation_stalls = 0U;
    maximum_tick_lag_microseconds = 0U;
    authority_loop_sequence.store(0U, std::memory_order_release);
    next_health_write = {};
    last_health_sequence = 0U;
    snapshot_subscribers = 0U;
    snapshot_requests = 0U;
    bootstrap_requests = 0U;
    replay_requests = 0U;
    disk_replay_announced = 0U;
    replay_send_failure_announced = 0U;
    bootstrap_recovery.reset();
    recovery_watchdog.reset();
    recovery_pause_announced = false;
    // A recovered non-empty journal is an existing match even while its seats
    // reconnect. Do not rewrite the dashboard phase back to an open lobby.
    match_started = recovery_pending;
    transition_checkpoint_pending = false;
    next_recovery_status = {};
    snapshot_cursor = 0U;
    snapshot_last_sent.fill({});
    replay_next_tick.fill(0U);
    snapshot_retry_rotation.fill(0U);
    initial_snapshot_state.clear();
    snapshot_transfer = {};
    // A restarted authority already owns a validated portable checkpoint.
    // Make it available before translated gameplay starts so a genuinely new
    // client (acknowledgment zero) can bootstrap while the authority waits at
    // the recovery barrier. The compact tick-zero array is intentionally not
    // used on this path; send_pending_bootstrap_snapshot_locked selects the
    // persisted dedicated checkpoint below.
    initial_snapshot_available = recovery_pending &&
        authority.latest_snapshot() &&
        btga::is_dedicated_checkpoint(authority.latest_snapshot()->state);
    for (std::uint8_t seat = 0; seat < btga::online_player_limit; ++seat) {
        (void)authority.set_connected(seat, false);
    }
    enabled = true;
    network_thread = std::jthread([](std::stop_token stop) {
        while (!stop.stop_requested()) {
            btga_dedicated_network_poll();
            poll_authority_health();
            poll_master_heartbeat();
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    });
    return true;
}

bool load_configured_players(std::string& error) {
    configured_players = 4U;
    const char* configured = std::getenv("BTGA_DEDICATED_PLAYERS");
    if (configured == nullptr || *configured == '\0') return true;
    const std::string_view text{configured};
    const auto [end, parse_error] = std::from_chars(
        text.data(), text.data() + text.size(), configured_players);
    if (parse_error != std::errc{} || end != text.data() + text.size() ||
        configured_players < 1U || configured_players > 4U) {
        error = "BTGA_DEDICATED_PLAYERS must be from 1 through 4";
        return false;
    }
    return true;
}
}

bool encode_and_send_frame(
    const btga::AuthoritativeFrame& frame,
    std::uint16_t destination_slot = BTGA_RELAY_BROADCAST_SLOT)
{
    BtgaNetAuthoritativeFrame wire{};
    wire.tick = frame.tick;
    wire.state_digest = frame.state_digest;
    wire.random_seed = frame.random_seed;
    wire.occupied_mask = authority.occupied_mask();
    // A replayed historical frame may contain an input from a seat that has
    // since timed out. Preserve the wire invariant that every received seat
    // was connected for that frame; otherwise the portable encoder correctly
    // rejects the repair packet. Live frames are unchanged because their
    // received mask is already a subset of the current connected mask.
    wire.connected_mask = static_cast<std::uint8_t>(
        authority.connected_mask() | frame.received_mask);
    wire.received_mask = frame.received_mask;
    for (std::size_t seat = 0; seat < frame.inputs.size(); ++seat) {
        wire.inputs[seat] = {frame.inputs[seat].buttons,
                             frame.inputs[seat].stick_x,
                             frame.inputs[seat].stick_y};
    }
    std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
    const std::size_t payload_size = btga_net_encode_authoritative_frame(
        packet.data() + BTGA_NET_HEADER_SIZE,
        packet.size() - BTGA_NET_HEADER_SIZE, &wire);
    if (payload_size == 0U) {
        std::fputs("BTGA_AUTHORITY frame encode payload failed\n", stderr);
        return false;
    }
    BtgaNetHeader header{};
    header.protocol_version = BTGA_NET_PROTOCOL_VERSION;
    header.message_type = BTGA_NET_MSG_EVENT;
    header.flags = BTGA_NET_FLAG_RELAYED;
    header.session_id = session_id;
    header.sequence = ++outgoing_sequence;
    // Advertise the journal head on every live or replayed frame. Clients use
    // it to decide whether they need accelerated catch-up without trusting a
    // host-native clock or memory image.
    header.acknowledgment = authority.next_tick();
    header.acknowledgment_bits = 0;
    header.payload_size = static_cast<std::uint16_t>(payload_size);
    header.peer_id = 0;
    if (btga_net_encode_header(packet.data(), packet.size(), &header) == 0U) {
        std::fputs("BTGA_AUTHORITY frame encode header failed\n", stderr);
        return false;
    }
    if (!multiplayer) {
        std::fputs("BTGA_AUTHORITY frame send missing multiplayer\n", stderr);
        return false;
    }
    const bool sent = multiplayer->send_gameplay(
        destination_slot,
        {packet.data(), BTGA_NET_HEADER_SIZE + payload_size});
    if (!sent && std::getenv("BTGA_NET_TEST_FAULTS") != nullptr) {
        std::fprintf(stderr,
                     "BTGA_AUTHORITY frame send failed tick=%u destination=%u\n",
                     frame.tick, static_cast<unsigned>(destination_slot));
    }
    return sent;
}

bool send_recovery_status_locked() {
    std::array<std::uint8_t, BTGA_NET_HEADER_SIZE> packet{};
    BtgaNetHeader header{};
    header.protocol_version = BTGA_NET_PROTOCOL_VERSION;
    header.message_type = BTGA_NET_MSG_PING;
    header.flags = BTGA_NET_FLAG_RELAYED;
    header.session_id = session_id;
    header.sequence = ++outgoing_sequence;
    header.acknowledgment = authority.next_tick();
    header.acknowledgment_bits = bootstrap_recovery.mask();
    header.payload_size = 0U;
    header.peer_id = 0U;
    return btga_net_encode_header(packet.data(), packet.size(), &header) != 0U &&
        multiplayer && multiplayer->send_gameplay(BTGA_RELAY_BROADCAST_SLOT, packet);
}

std::uint32_t next_snapshot_generation() noexcept {
    ++snapshot_id;
    if (snapshot_id == 0U) ++snapshot_id; // zero means "no generation"
    return snapshot_id;
}

std::uint32_t start_snapshot_transfer_locked(
    std::uint32_t tick, std::uint16_t destination_slot,
    std::span<const std::uint8_t> state, std::uint64_t canonical_digest = 0U,
    std::uint32_t retransmit_id = 0U, std::uint32_t first_chunk = 0U) {
    if (snapshot_transfer.cursor.active() || state.empty() ||
        state.size() > UINT32_MAX) return 0U;
    if (canonical_digest == 0U) {
        canonical_digest = UINT64_C(1469598103934665603);
        for (const std::uint8_t byte : state) {
            canonical_digest ^= byte;
            canonical_digest *= UINT64_C(1099511628211);
        }
    }
    const auto total = static_cast<std::uint32_t>(state.size());
    // Keep one generation ID while retransmitting a bootstrap. The replica
    // retains already received chunks for that ID, so UDP loss can be repaired
    // over several sends instead of every retry discarding partial progress.
    const std::uint32_t id = retransmit_id != 0U
        ? retransmit_id : next_snapshot_generation();
    snapshot_transfer.state.assign(state.begin(), state.end());
    snapshot_transfer.canonical_digest = canonical_digest;
    snapshot_transfer.tick = tick;
    snapshot_transfer.id = id;
    snapshot_transfer.destination_slot = destination_slot;
    const std::uint32_t chunk_count =
        (total + BTGA_NET_SNAPSHOT_CHUNK_DATA_LIMIT - 1U) /
        BTGA_NET_SNAPSHOT_CHUNK_DATA_LIMIT;
    snapshot_transfer.cursor.start(
        chunk_count, first_chunk, std::chrono::steady_clock::now());
    return id;
}

void pump_snapshot_transfer_locked() {
    // At most eight nonblocking datagrams are emitted per network pump. The
    // paced schedule reserves relay capacity for live authority and repair
    // traffic, while a delayed pump may catch up without sleeping in the
    // simulation path.
    constexpr unsigned packet_budget = 8U;
    for (unsigned sent = 0U;
         sent < packet_budget && snapshot_transfer.cursor.active();) {
        const auto now = std::chrono::steady_clock::now();
        const auto due_chunk = snapshot_transfer.cursor.next(now);
        if (!due_chunk) break;
        const auto total = static_cast<std::uint32_t>(snapshot_transfer.state.size());
        const std::uint32_t chunk_index = *due_chunk;
        const std::uint32_t offset =
            chunk_index * BTGA_NET_SNAPSHOT_CHUNK_DATA_LIMIT;
        const auto data_size = static_cast<std::uint16_t>(std::min<std::uint32_t>(
            BTGA_NET_SNAPSHOT_CHUNK_DATA_LIMIT, total - offset));
        std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
        BtgaNetSnapshotChunk chunk{
            snapshot_transfer.id, snapshot_transfer.tick,
            snapshot_transfer.canonical_digest, total, offset, data_size,
            snapshot_transfer.state.data() + offset};
        const std::size_t payload_size = btga_net_encode_snapshot_chunk(
            packet.data() + BTGA_NET_HEADER_SIZE,
            packet.size() - BTGA_NET_HEADER_SIZE, &chunk);
        if (payload_size == 0U) {
            snapshot_transfer = {};
            return;
        }
        BtgaNetHeader header{};
        header.protocol_version = BTGA_NET_PROTOCOL_VERSION;
        header.message_type = BTGA_NET_MSG_SNAPSHOT;
        header.flags = static_cast<std::uint8_t>(BTGA_NET_FLAG_RELIABLE |
                                                 BTGA_NET_FLAG_RELAYED);
        header.session_id = session_id;
        header.sequence = ++outgoing_sequence;
        header.payload_size = static_cast<std::uint16_t>(payload_size);
        header.peer_id = 0;
        if (btga_net_encode_header(packet.data(), packet.size(), &header) == 0U ||
            !multiplayer || !multiplayer->send_gameplay(
                snapshot_transfer.destination_slot,
                {packet.data(), BTGA_NET_HEADER_SIZE + payload_size})) {
            // Retain the same chunk for the next pump when the nonblocking
            // socket reports backpressure.
            return;
        }
        ++sent;
        snapshot_transfer.cursor.mark_sent();
        if (!snapshot_transfer.cursor.active()) snapshot_transfer = {};
    }
}

// Must be called with authority_mutex held. Bootstrap transport remains live
// while the recovery barrier has simulation paused; otherwise a cold client
// could never receive the tick-zero state needed to begin journal replay.
void send_pending_bootstrap_snapshot_locked() {
    if (!initial_snapshot_available || snapshot_requests == 0U ||
        snapshot_transfer.cursor.active()) return;
    std::uint8_t target = UINT8_MAX;
    for (std::uint8_t offset = 0; offset < btga::online_player_limit; ++offset) {
        const auto seat = static_cast<std::uint8_t>(
            (snapshot_cursor + offset) % btga::online_player_limit);
        const auto bit = static_cast<std::uint8_t>(1U << seat);
        if ((snapshot_requests & bit) != 0U &&
            bootstrap_recovery.pending(seat)) {
            target = seat;
            break;
        }
    }
    if (target == UINT8_MAX) return;
    const auto bit = static_cast<std::uint8_t>(1U << target);
    std::uint32_t checkpoint_tick = 0U;
    std::span<const std::uint8_t> checkpoint_state{initial_snapshot_state};
    std::uint64_t checkpoint_digest = 0U;
    const std::uint32_t retransmit_id = bootstrap_recovery.snapshot_id(target);
    const bool retrying_initial_generation = retransmit_id != 0U &&
        bootstrap_recovery.expected_ack(target) == 1U;
    if (!retrying_initial_generation) {
        if (const auto& latest = authority.latest_snapshot(); latest &&
            latest->next_tick <= authority.next_tick() &&
            btga::is_dedicated_checkpoint(latest->state)) {
            checkpoint_tick = latest->next_tick;
            checkpoint_state = latest->state;
            checkpoint_digest = latest->state_digest;
        }
    }
    const std::uint32_t first_chunk = retransmit_id == 0U
        ? 0U : ++snapshot_retry_rotation[target];
    const std::uint32_t queued_id = start_snapshot_transfer_locked(
        checkpoint_tick, static_cast<std::uint16_t>(target + 1U),
        checkpoint_state, checkpoint_digest, retransmit_id, first_chunk);
    if (queued_id != 0U) {
        snapshot_last_sent[target] = std::chrono::steady_clock::now();
        // Keep the authority frozen until the replacement has installed this
        // checkpoint *and* replayed through the head at which the recovery
        // barrier began. Releasing at checkpoint_tick strands the client a
        // fixed number of ticks behind because both sides then run at 30 Hz.
        bootstrap_recovery.snapshot_sent(
            target, queued_id,
            authority.next_tick());
        std::fprintf(stderr,
                     "BTGA_AUTHORITY bootstrap snapshot seat=%u id=%u tick=%u retry=%u\n",
                     static_cast<unsigned>(target),
                     bootstrap_recovery.snapshot_id(target), checkpoint_tick,
                     retransmit_id != 0U ? 1U : 0U);
        snapshot_requests &= static_cast<std::uint8_t>(~bit);
        bootstrap_requests &= static_cast<std::uint8_t>(~bit);
        snapshot_cursor = static_cast<std::uint8_t>(
            (target + 1U) % btga::online_player_limit);
    } else {
        std::fputs("BTGA_AUTHORITY bootstrap snapshot queue failed\n", stderr);
    }
}

bool btga_dedicated_enable_from_environment(std::string& error)
{
    const std::lock_guard lock{authority_mutex};
    const auto variable = [](const char* name) -> std::string {
        const char* value = std::getenv(name);
        return value == nullptr ? std::string{} : std::string{value};
    };
    const std::string session_text = variable("BTGA_DEDICATED_SESSION");
    const std::string relay_host = variable("BTGA_DEDICATED_RELAY_HOST");
    const std::string relay_port_text = variable("BTGA_DEDICATED_RELAY_PORT");
    const std::string token = variable("BTGA_DEDICATED_TOKEN");
    unsigned int relay_port = 0;
    const auto [session_end, session_error] = std::from_chars(
        session_text.data(), session_text.data() + session_text.size(), session_id);
    const auto [port_end, port_error] = std::from_chars(
        relay_port_text.data(), relay_port_text.data() + relay_port_text.size(), relay_port);
    if (session_error != std::errc{} || session_end != session_text.data() + session_text.size() ||
        port_error != std::errc{} || port_end != relay_port_text.data() + relay_port_text.size() ||
        session_id == 0U || relay_port == 0U || relay_port > 65535U ||
        relay_host.empty() || token.empty()) {
        error = "dedicated session, relay endpoint, or authority token is missing or invalid";
        return false;
    }
    if (!load_configured_players(error)) return false;
    return enable_relay(session_id,
                        {relay_host, static_cast<std::uint16_t>(relay_port)}, token, error);
}

bool btga_dedicated_enable_public(std::string_view name, std::string& error) {
    if (!load_configured_players(error)) return false;
    try {
        auto http = btga::make_platform_http_client(btga::master_service_url());
        auto client = std::make_unique<btga::MasterClient>(*http);
        std::optional<std::filesystem::path> lease_path;
        if (const char* state_directory = std::getenv("BTGA_DEDICATED_STATE_DIR");
            state_directory != nullptr && *state_directory != '\0') {
            const std::filesystem::path directory{state_directory};
            std::error_code create_error;
            std::filesystem::create_directories(directory, create_error);
            if (create_error) {
                error = "could not create BTGA_DEDICATED_STATE_DIR";
                return false;
            }
            lease_path = directory / "authority-control.json";
        }
        if (lease_path && std::filesystem::exists(*lease_path)) {
            auto lease = btga::load_authority_lease(*lease_path);
            if (!lease) {
                error = "persistent authority control record is invalid";
                return false;
            }
            std::string relay_token;
            try {
                relay_token = client->heartbeat(
                    lease->lobby_id, lease->host_secret, 1U, "playing", 0U);
            } catch (const btga::MasterServiceError& service_error) {
                if (service_error.status() != 404) throw;
                std::error_code remove_error;
                std::filesystem::remove(*lease_path, remove_error);
                if (remove_error) {
                    error = "expired lobby is gone, but its persistent authority record "
                            "could not be removed";
                    return false;
                }
                std::fprintf(stderr,
                             "BTGA_AUTHORITY saved lobby %s expired; creating a replacement\n",
                             lease->lobby_id.c_str());
                lease.reset();
            }
            if (lease) {
                // A heartbeat persists the exact sequence at least every 15
                // seconds. Reserve a gap larger than the maximum packets that
                // can be emitted in that interval, and persist it before relay bind.
                lease->packet_sequence += UINT32_C(65536);
                if (!btga::save_authority_lease(*lease_path, *lease)) {
                    error = "could not reserve replacement authority packet sequence";
                    return false;
                }
                {
                    const std::lock_guard lock{authority_mutex};
                    outgoing_sequence = lease->packet_sequence;
                    if (!enable_relay(lease->session_id, lease->relay, relay_token, error)) {
                        return false;
                    }
                    const std::lock_guard master_lock{master_mutex};
                    public_lobby_id = lease->lobby_id;
                    public_host_secret = lease->host_secret;
                    persistent_lease_path = lease_path;
                    persistent_lease = *lease;
                    next_heartbeat = std::chrono::steady_clock::now();
                    master_http = std::move(http);
                    master = std::move(client);
                }
                std::printf("ONLINE_LOBBY_ID=%s\n", public_lobby_id.c_str());
                std::fputs("BTGA_AUTHORITY reclaimed persistent lobby authority\n", stderr);
                std::fflush(stdout);
                std::fflush(stderr);
                return true;
            }
        }
        // The public lobby advertises the service's four-seat capacity even
        // when a test authority starts with fewer required participants. A
        // dedicated authority is not itself a player; its seat-mask heartbeat
        // reports the exact number of authenticated gameplay seats.
        const btga::HostRegistration registration = client->host(
            name, btga::online_player_limit, "eastus",
            BTGA_NET_CAP_RELAY_UDP | BTGA_NET_CAP_DEDICATED_AUTHORITY);
        const btga::PersistentAuthorityLease new_lease{
            registration.lobby.id, registration.host_secret,
            registration.session_id, registration.relay, 0U};
        if (lease_path && !btga::save_authority_lease(*lease_path, new_lease)) {
            client->remove(registration.lobby.id, registration.host_secret);
            error = "could not persist authority control record";
            return false;
        }
        {
            const std::lock_guard lock{authority_mutex};
            if (!enable_relay(registration.session_id, registration.relay,
                              registration.relay_token, error)) {
                client->remove(registration.lobby.id, registration.host_secret);
                if (lease_path) {
                    std::error_code remove_error;
                    std::filesystem::remove(*lease_path, remove_error);
                }
                return false;
            }
            {
                const std::lock_guard master_lock{master_mutex};
                public_lobby_id = registration.lobby.id;
                public_host_secret = registration.host_secret;
                persistent_lease_path = lease_path;
                persistent_lease = new_lease;
                next_heartbeat = std::chrono::steady_clock::now();
                master_http = std::move(http);
                master = std::move(client);
            }
        }
        std::printf("ONLINE_LOBBY_ID=%s\n", public_lobby_id.c_str());
        std::fflush(stdout);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

extern "C" int btga_dedicated_enabled()
{
    const std::lock_guard lock{authority_mutex};
    return enabled ? 1 : 0;
}

void btga_dedicated_network_poll()
{
    // Network service is independent of the translated simulation. If a tick
    // boundary briefly owns authority state, skip this 2 ms poll rather than
    // ever making either thread wait on the other.
    const std::unique_lock lock{authority_mutex, std::try_to_lock};
    if (!lock.owns_lock()) return;
    if (!enabled || !multiplayer) return;
    if (++network_poll_count == 1U) {
        std::fputs("BTGA_AUTHORITY network pump active\n", stderr);
    }
    if (!pending_authority_relay_token.empty()) {
        const auto token = std::span{
            reinterpret_cast<const std::uint8_t*>(
                pending_authority_relay_token.data()),
            pending_authority_relay_token.size()};
        if (multiplayer->rebind_relay(true, session_id, token)) {
            std::fputs("BTGA_AUTHORITY refreshing relay binding\n", stderr);
        } else {
            std::fputs("BTGA_AUTHORITY could not refresh relay binding\n", stderr);
        }
        pending_authority_relay_token.clear();
    }
    if (multiplayer->state() == btga::OnlineState::RelayBinding) {
        if (multiplayer->poll_relay_binding()) {
            std::fputs("BTGA_AUTHORITY relay binding restored\n", stderr);
        }
        return;
    }
    if (multiplayer->state() != btga::OnlineState::Lobby) return;
    pump_snapshot_transfer_locked();
    std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
    const auto now = std::chrono::steady_clock::now();
    for (;;) {
        std::uint16_t source_slot = 0;
        const std::size_t size = multiplayer->receive_gameplay(packet, source_slot);
        if (size == 0U) break;
        BtgaNetHeader incoming{};
        const bool header_valid = btga_net_decode_header(
            &incoming, packet.data(), size) != 0 &&
            incoming.protocol_version == BTGA_NET_PROTOCOL_VERSION &&
            incoming.session_id == session_id && incoming.peer_id == source_slot &&
            incoming.payload_size == size - BTGA_NET_HEADER_SIZE;
        const bool subscription = header_valid && source_slot >= 1U &&
            source_slot <= btga::online_player_limit &&
            incoming.message_type == BTGA_NET_MSG_PING && incoming.payload_size == 0U;
        const bool accepted = subscription || authority.ingest_client_packet(
                std::span<const std::uint8_t>{packet.data(), size},
                session_id, source_slot);
        bool current_input = false;
        std::uint32_t input_horizon = 0U;
        if (accepted && !subscription && source_slot >= 1U &&
            source_slot <= btga::online_player_limit &&
            incoming.message_type == BTGA_NET_MSG_INPUT) {
            BtgaNetInputBatch batch{};
            const auto payload = std::span<const std::uint8_t>{
                packet.data() + BTGA_NET_HEADER_SIZE, incoming.payload_size};
            if (btga_net_decode_input_batch(
                    &batch, payload.data(), payload.size()) != 0U &&
                batch.frame_count != 0U) {
                const std::uint32_t last_tick = batch.first_tick +
                    static_cast<std::uint32_t>(batch.frame_count - 1U);
                current_input = last_tick >= batch.first_tick &&
                    last_tick >= authority.next_tick();
                input_horizon = last_tick;
                latest_input_horizons[static_cast<std::size_t>(source_slot - 1U)] =
                    last_tick;
            }
        }
        if (received_datagrams++ < 12U) {
            std::fprintf(stderr, "BTGA_AUTHORITY input source=%u accepted=%d bytes=%zu\n",
                         static_cast<unsigned>(source_slot), accepted ? 1 : 0, size);
        }
        if (accepted) {
            const auto seat = static_cast<std::size_t>(source_slot - 1U);
            if (subscription) {
                const auto bit = static_cast<std::uint8_t>(1U << seat);
                const bool first_subscription = (snapshot_subscribers & bit) == 0U;
                const bool bootstrap_missing = incoming.acknowledgment == 0U;
                const bool retry_due = snapshot_last_sent[seat] ==
                        std::chrono::steady_clock::time_point{} ||
                    now - snapshot_last_sent[seat] >= bootstrap_retry_delay;
                if (bootstrap_missing) {
                    const auto& latest = authority.latest_snapshot();
                    const std::uint32_t latest_tick = latest &&
                            btga::is_dedicated_checkpoint(latest->state)
                        ? latest->next_tick : 0U;
                    const auto request = bootstrap_recovery.request_cold(
                        static_cast<std::uint8_t>(seat), latest_tick);
                    if (request != btga::BootstrapRequest::continue_generation) {
                        // The relay authenticated this bound endpoint and the
                        // recovery state machine accepted a new connection
                        // generation. Permit its packet sequence to restart
                        // immediately, without waiting for the old endpoint's
                        // liveness timeout. Repeated keepalives for the same
                        // generation cannot reach this branch.
                        (void)authority.begin_connection_epoch(
                            static_cast<std::uint8_t>(seat));
                        snapshot_retry_rotation[seat] = 0U;
                        snapshot_last_sent[seat] = {};
                        recovery_watchdog.clear(
                            static_cast<std::uint8_t>(seat));
                        bootstrap_requests |= bit;
                        snapshot_requests |= bit;
                        replay_requests &= static_cast<std::uint8_t>(~bit);
                    }
                    if (first_subscription || retry_due) {
                        snapshot_requests |= bit;
                        if (!first_subscription && retry_due) {
                            std::fprintf(stderr,
                                         "BTGA_AUTHORITY bootstrap retry requested "
                                         "seat=%zu id=%u\n",
                                         seat,
                                         bootstrap_recovery.snapshot_id(
                                             static_cast<std::uint8_t>(seat)));
                        }
                    }
                } else {
                    const auto ack = bootstrap_recovery.acknowledge(
                        static_cast<std::uint8_t>(seat), incoming.acknowledgment,
                        incoming.acknowledgment_bits, authority.next_tick(),
                        first_subscription);
                    if (ack.kind != btga::BootstrapAckKind::ignore) {
                        replay_next_tick[seat] = ack.next_tick;
                    }
                    if (ack.kind == btga::BootstrapAckKind::replay) {
                        replay_requests |= bit;
                        if ((bootstrap_recovery.mask() & bit) != 0U) {
                            recovery_watchdog.observe(
                                static_cast<std::uint8_t>(seat), ack.next_tick,
                                now);
                        }
                    } else if (ack.kind == btga::BootstrapAckKind::ready) {
                        bootstrap_requests &= static_cast<std::uint8_t>(~bit);
                        snapshot_requests &= static_cast<std::uint8_t>(~bit);
                        replay_requests &= static_cast<std::uint8_t>(~bit);
                        if (replay_next_tick[seat] >= authority.journal_base_tick()) {
                            disk_replay_announced &= static_cast<std::uint8_t>(~bit);
                        }
                        recovery_watchdog.clear(
                            static_cast<std::uint8_t>(seat));
                    }
                    // The same next-tick acknowledgment also repairs an
                    // ordinary lost canonical frame after bootstrap. This is
                    // a bounded journal replay only; it does not reopen the
                    // global recovery barrier or alter checkpoint state.
                    const std::uint32_t client_next_tick =
                        incoming.acknowledgment - 1U;
                    if (client_next_tick >= authority.journal_base_tick() &&
                        static_cast<std::int32_t>(
                            client_next_tick - authority.next_tick()) < 0) {
                        replay_next_tick[seat] = client_next_tick;
                        replay_requests |= bit;
                    }
                }
                snapshot_subscribers |= bit;
            }
            // Recovery traffic proves that the endpoint is reachable, but it
            // does not prove that its gameplay producer is advancing. Track
            // those independently so a wedged client cannot hold steering by
            // sending only subscriptions or stale input batches.
            client_liveness.observe_transport(
                static_cast<std::uint8_t>(seat), now);
            const bool input_advanced = current_input &&
                client_liveness.observe_input(
                    static_cast<std::uint8_t>(seat), input_horizon, now);
            if (input_advanced) {
                (void)authority.set_connected(
                    static_cast<std::uint8_t>(seat), true);
            }
        } else {
            BtgaNetHeader rejected{};
            const bool decoded = btga_net_decode_header(&rejected, packet.data(), size) != 0;
            std::fprintf(stderr,
                         "BTGA_AUTHORITY rejected decoded=%d type=%u session=%016llx expected=%016llx peer=%u expected_peer=%u payload=%u bytes=%zu\n",
                         decoded ? 1 : 0, static_cast<unsigned>(rejected.message_type),
                         static_cast<unsigned long long>(rejected.session_id),
                         static_cast<unsigned long long>(session_id),
                         static_cast<unsigned>(rejected.peer_id),
                         static_cast<unsigned>(source_slot),
                         static_cast<unsigned>(rejected.payload_size), size);
        }
    }
    // One bounded journal window is attempted per client acknowledgment. A
    // client cannot make the authority burst an unbounded match history,
    // while a recovering client can advance the window by acknowledging
    // contiguous consumed ticks. A failed datagram must not stop the rest of
    // the window: periodic loss can otherwise phase-lock the first replayed
    // tick to the fault profile indefinitely. Clients buffer out-of-order
    // frames and the missing tick is retried with the next acknowledgment.
    // Clients acknowledge about four times per second and consume 30 ticks per
    // second. Eight distinct frames per acknowledgment supplies 32 ticks per
    // second, enough to converge while the recovery barrier is closed without
    // overflowing either endpoint's nonblocking UDP socket. The oldest missing
    // frame is repeated once after the window so deterministic periodic loss
    // cannot phase-lock that same packet forever; this remains nonblocking and
    // does not duplicate the bulk of the replay.
    constexpr std::size_t replay_window = 8U;
    for (std::uint8_t seat = 0; seat < btga::online_player_limit; ++seat) {
        const auto bit = static_cast<std::uint8_t>(1U << seat);
        if ((replay_requests & bit) == 0U) continue;
        replay_requests &= static_cast<std::uint8_t>(~bit);
        std::array<btga::AuthoritativeFrame, replay_window> frames{};
        if (replay_next_tick[seat] < authority.journal_base_tick() &&
            (disk_replay_announced & bit) == 0U) {
            std::fprintf(stderr,
                         "BTGA_AUTHORITY disk journal replay seat=%u tick=%u base=%u\n",
                         static_cast<unsigned>(seat), replay_next_tick[seat],
                         authority.journal_base_tick());
            disk_replay_announced |= bit;
        }
        const std::size_t count = authority.copy_journal_from(
            replay_next_tick[seat], frames);
        if (std::getenv("BTGA_NET_TEST_FAULTS") != nullptr && count == 0U) {
            std::fprintf(stderr,
                         "BTGA_AUTHORITY live repair empty seat=%u next=%u head=%u base=%u\n",
                         static_cast<unsigned>(seat), replay_next_tick[seat],
                         authority.next_tick(), authority.journal_base_tick());
        }
        for (std::size_t index = 0; index < count; ++index) {
            // Replay is opportunistic and nonblocking. Missing frames remain
            // behind the client's acknowledgment cursor and are offered again
            // on its next subscription; live simulation never sleeps here.
            if (!encode_and_send_frame(
                    frames[index], static_cast<std::uint16_t>(seat + 1U))) {
                if ((replay_send_failure_announced & bit) == 0U) {
                    std::fprintf(stderr,
                                 "BTGA_AUTHORITY journal replay backpressure "
                                 "seat=%u tick=%u\n",
                                 static_cast<unsigned>(seat), frames[index].tick);
                    replay_send_failure_announced |= bit;
                }
                // Keep offering the remainder of this bounded window. The
                // client buffers later frames, while its unchanged next-tick
                // acknowledgment causes the missing frame to be retried on
                // the next keepalive. Stopping here can phase-lock one loss
                // to the front of every repair window indefinitely.
                continue;
            }
            if (index + 1U == count) {
                replay_send_failure_announced &= static_cast<std::uint8_t>(~bit);
            }
        }
        if (count != 0U) {
            (void)encode_and_send_frame(
                frames[0], static_cast<std::uint16_t>(seat + 1U));
        }
    }
    for (std::uint8_t seat = 0; seat < btga::online_player_limit; ++seat) {
        const auto bit = static_cast<std::uint8_t>(1U << seat);
        if ((bootstrap_recovery.mask() & bit) != 0U &&
            recovery_watchdog.stalled(seat, now, recovery_stall_timeout)) {
            // A proven endpoint acquired the barrier but stopped consuming
            // replay. Release healthy players after a bounded pause. Keep its
            // replay cursor active so it may still catch up in the background;
            // subsequent ordinary acknowledgments cannot reacquire a barrier.
            std::fprintf(stderr,
                         "BTGA_AUTHORITY recovery stalled seat=%u next=%u; "
                         "releasing barrier\n",
                         static_cast<unsigned>(seat),
                         recovery_watchdog.next_tick(seat));
            bootstrap_recovery.disconnect(seat);
            bootstrap_requests &= static_cast<std::uint8_t>(~bit);
            snapshot_requests &= static_cast<std::uint8_t>(~bit);
            replay_requests |= bit;
            recovery_watchdog.clear(seat);
            (void)authority.set_connected(seat, false);
        }

        if (!client_liveness.seen(seat)) continue;

        // Stop applying a vanished player's last held controls quickly, but
        // retain its authenticated subscription and bootstrap generation long
        // enough for the normal retransmit path to survive internet jitter.
        // An accepted packet below marks it connected again immediately.
        if (client_liveness.input_stale(seat, now, stale_input_timeout) &&
            (authority.connected_mask() & bit) != 0U) {
            (void)authority.set_connected(seat, false);
            std::fprintf(stderr,
                         "BTGA_AUTHORITY seat=%u controls neutralized; awaiting reconnect\n",
                         static_cast<unsigned>(seat));
        }

        if (client_liveness.transport_stale(
                seat, now, recovery_abandon_timeout)) {
            client_liveness.clear(seat);
            snapshot_subscribers &= static_cast<std::uint8_t>(~bit);
            snapshot_requests &= static_cast<std::uint8_t>(~bit);
            bootstrap_requests &= static_cast<std::uint8_t>(~bit);
            replay_requests &= static_cast<std::uint8_t>(~bit);
            disk_replay_announced &= static_cast<std::uint8_t>(~bit);
            replay_send_failure_announced &= static_cast<std::uint8_t>(~bit);
            bootstrap_recovery.disconnect(seat);
            recovery_watchdog.clear(seat);
            (void)authority.set_connected(seat, false);
        }
    }
}

bool btga_dedicated_controller_input(
    int controller, std::uint16_t& buttons, float& x, float& y)
{
    const std::lock_guard lock{authority_mutex};
    if (!enabled || controller < 0 ||
        controller >= static_cast<int>(active_inputs.size())) return false;
    const auto& input = active_inputs[static_cast<std::size_t>(controller)];
    buttons = input.buttons;
    x = static_cast<float>(input.stick_x) / 127.0F;
    y = static_cast<float>(input.stick_y) / 127.0F;
    return true;
}

extern "C" void btga_dedicated_tick_begin(
    std::uint8_t* rdram, recomp_context*)
{
    if (recovery_replay_running || recovery_pending) return;
    {
        const std::lock_guard lock{authority_mutex};
        if (!enabled || gameplay_tick_active) return;
    }
    // This hook is also the dedicated process's main-loop liveness signal.
    // Empty public lobbies remain in the frontend until their first player
    // connects, so count those inactive passes as progress too. Otherwise the
    // supervisor mistakes a healthy, idle authority for a hung process and
    // restarts it after the startup-health deadline.
    authority_loop_sequence.fetch_add(1U, std::memory_order_release);
    if (!btga::gameplay_frame_input_active({rdram, rdram_size})) {
        const std::lock_guard lock{authority_mutex};
        if (inactive_tick_probes++ < 5U) {
            constexpr gpr world = static_cast<gpr>(static_cast<std::int32_t>(0x80219498U));
            constexpr gpr humans = static_cast<gpr>(static_cast<std::int32_t>(0x802194A4U));
            constexpr gpr update = static_cast<gpr>(static_cast<std::int32_t>(0x802195CCU));
            std::fprintf(stderr,
                         "BTGA_AUTHORITY waiting world=%08x humans=%u update=%d\n",
                         static_cast<unsigned>(MEM_W(0, world)),
                         static_cast<unsigned>(MEM_BU(0, humans)),
                         static_cast<int>(MEM_W(0, update)));
        }
        return;
    }
    inactive_tick_probes = 0U;
    std::uint32_t wait_iterations = 0;
    for (;;) {
        {
            const std::lock_guard lock{authority_mutex};
            if (!enabled) return;
            if (authority.begin_tick(server_clock_tick())) {
                active_tick = authority.next_tick();
                std::ranges::copy(authority.inputs(), active_inputs.begin());
                gameplay_tick_active = true;
                return;
            }
            if (++wait_iterations % 1000U == 0U) {
                std::fprintf(stderr,
                             "BTGA_AUTHORITY waiting for tick=%u clock=%u connected=%02x\n",
                             authority.next_tick(), server_clock_tick(),
                             authority.connected_mask());
                std::fflush(stderr);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
}

extern "C" void btga_dedicated_match_bootstrap(
    std::uint8_t* rdram, recomp_context*)
{
    const std::lock_guard lock{authority_mutex};
    if (!enabled && !btga_dedicated_client_enabled()) return;
    // Client processes do not execute the authority-registration path that
    // normally parses this setting. Read it here as well so reduced-player
    // gates configure identical retail state on both sides.
    std::string player_error;
    (void)load_configured_players(player_error);

    // Native dedicated-session orchestration. Skip the retail menu state
    // machine and configure a four-human deathmatch before the main loop's
    // first mode transition. The normal translated level initializer still
    // owns world creation, entities, AI, collision and mission rules.
    constexpr gpr game_kind = static_cast<gpr>(static_cast<std::int32_t>(0x80117EB4U));
    constexpr gpr human_count = static_cast<gpr>(static_cast<std::int32_t>(0x80117EB0U));
    constexpr gpr selected_level = static_cast<gpr>(static_cast<std::int32_t>(0x80117F44U));
    constexpr gpr player_modes = static_cast<gpr>(static_cast<std::int32_t>(0x80117F04U));
    constexpr gpr player_teams = static_cast<gpr>(static_cast<std::int32_t>(0x80117ED4U));
    constexpr gpr player_tanks = static_cast<gpr>(static_cast<std::int32_t>(0x80117F34U));
    constexpr gpr outer_mode = static_cast<gpr>(static_cast<std::int32_t>(0x802195D8U));
    constexpr gpr inner_mode = static_cast<gpr>(static_cast<std::int32_t>(0x802195C8U));
    constexpr gpr random_seed = static_cast<gpr>(static_cast<std::int32_t>(0x80114C80U));

    MEM_W(0, game_kind) = 2;       // deathmatch
    MEM_B(0, human_count) = static_cast<std::uint8_t>(configured_players);
    // First entry in retail mode-2's multiplayer arena table (D_8011B758).
    // Level zero is not valid for this mode and corrupts the spatial grid.
    MEM_W(0, selected_level) = 20;
    for (int seat = 0; seat < 4; ++seat) {
        MEM_W(seat * 4, player_modes) =
            seat < static_cast<int>(configured_players) ? 2 : 0;
        MEM_W(seat * 4, player_teams) = seat + 1;
        MEM_W(seat * 4, player_tanks) = seat;
    }
    MEM_W(0, outer_mode) = 1;
    MEM_W(0, inner_mode) = 3;
    MEM_W(0, random_seed) = static_cast<std::int32_t>(UINT32_C(0x4A6F7921));
    std::fprintf(stderr, "BTGA_AUTHORITY configured native %u-player match\n",
                 configured_players);
    std::fflush(stderr);
}

extern "C" void btga_dedicated_test_force_match_end(
    std::uint8_t* rdram, std::uint32_t next_tick) {
    static bool forced = false;
    const char* enabled_flag = std::getenv("BTGA_NET_TEST_FORCE_MATCH_TRANSITION");
    if (forced || enabled_flag == nullptr ||
        std::string_view{enabled_flag} != "1" || next_tick != 901U) return;

    constexpr gpr game_mode = static_cast<gpr>(
        static_cast<std::int32_t>(0x802194A0U));
    constexpr gpr match_limit = static_cast<gpr>(
        static_cast<std::int32_t>(0x802194B0U));
    constexpr gpr frame_counter = static_cast<gpr>(
        static_cast<std::int32_t>(0x8021945CU));
    constexpr gpr unit_zero = static_cast<gpr>(
        static_cast<std::int32_t>(0x80235F00U));
    const auto mode = static_cast<std::uint32_t>(MEM_W(0, game_mode));
    const auto limit = static_cast<std::uint32_t>(MEM_W(0, match_limit));
    const gpr info = static_cast<gpr>(MEM_W(0x10, unit_zero));
    if (info == 0) return;

    switch (mode) {
    case 5U:
        MEM_W(0, frame_counter) = static_cast<std::int32_t>(limit + 1U);
        break;
    case 0U:
    case 6U:
        MEM_H(0, info) = static_cast<std::uint16_t>(limit);
        break;
    case 3U:
        MEM_W(8, info) = static_cast<std::int32_t>(
            std::bit_cast<std::uint32_t>(static_cast<float>(limit)));
        break;
    case 4U:
        MEM_W(0x74, unit_zero) = 1;
        break;
    default:
        MEM_H(4, info) = static_cast<std::uint16_t>(limit);
        break;
    }
    forced = true;
    std::fprintf(stderr,
                 "BTGA_AUTHORITY test match end forced tick=%u mode=%u limit=%u\n",
                 next_tick - 1U, mode, limit);
    std::fflush(stderr);
}

extern "C" void btga_dedicated_advance_results(
    std::uint8_t* rdram, recomp_context* context) {
    constexpr gpr current_mode = static_cast<gpr>(
        static_cast<std::int32_t>(0x802195CCU));
    constexpr gpr mode_frames = static_cast<gpr>(
        static_cast<std::int32_t>(0x802195C4U));
    if (MEM_W(0, current_mode) != 6 || MEM_W(0, mode_frames) != 120) return;
    // A rendered local match waits for the user on the results screen. A
    // dedicated session has no shared menu owner, so invoke the exact retail
    // results-continue callback after a deterministic two-second display.
    func_800CF0CC(rdram, context);
    if (enabled) transition_checkpoint_pending = true;
    std::fputs("BTGA_AUTHORITY dedicated results continued\n", stderr);
    std::fflush(stderr);
}

extern "C" void btga_dedicated_run_loop(
    std::uint8_t* rdram, recomp_context* context)
{
    if (!btga_dedicated_enabled()) return;
    // func_8009C524 contains the hook that enters this loop, and the loop must
    // call func_8009C524 once per server tick.  Some retail state transitions
    // reach that hook again from inside the tick.  Do not recursively start a
    // second infinite server loop on the same translated thread.
    if (dedicated_loop_running) return;
    dedicated_loop_running = true;

    bool restored_persistent_session = false;
    if (recovery_pending) {
        std::optional<btga::AuthoritativeSnapshot> checkpoint;
        std::vector<btga::AuthoritativeFrame> replay;
        {
            const std::lock_guard lock{authority_mutex};
            if (authority.latest_snapshot()) checkpoint = *authority.latest_snapshot();
            if (!checkpoint || checkpoint->next_tick > authority.next_tick()) {
                std::fputs("BTGA_AUTHORITY persistent checkpoint unavailable\n", stderr);
                std::abort();
            }
            replay.resize(authority.next_tick() - checkpoint->next_tick);
            if (!replay.empty() && authority.copy_journal_from(
                    checkpoint->next_tick, replay) != replay.size()) {
                std::fputs("BTGA_AUTHORITY persistent replay gap\n", stderr);
                std::abort();
            }
        }
        if (!btga::restore_dedicated_checkpoint(
                {rdram, rdram_size}, checkpoint->state)) {
            std::fputs("BTGA_AUTHORITY persistent checkpoint restore failed\n", stderr);
            std::abort();
        }
        std::array<std::uint8_t, btga::gameplay_snapshot_size> restored_state{};
        if (!btga::capture_gameplay_snapshot(
                {rdram, rdram_size}, restored_state) ||
            checkpoint->state.size() < restored_state.size() ||
            !std::ranges::equal(
                restored_state,
                std::span<const std::uint8_t>{checkpoint->state}.first(
                    restored_state.size()))) {
            std::size_t first_difference = restored_state.size();
            if (checkpoint->state.size() >= restored_state.size()) {
                for (std::size_t index = 0; index < restored_state.size(); ++index) {
                    if (restored_state[index] != checkpoint->state[index]) {
                        first_difference = index;
                        break;
                    }
                }
            }
            std::fprintf(stderr,
                         "BTGA_AUTHORITY restored checkpoint differs offset=%zu\n",
                         first_difference);
            std::abort();
        }
        recovery_replay_running = true;
        for (const auto& frame : replay) {
            active_inputs = frame.inputs;
            btga_advance_retail_mode_counters(rdram);
            func_80079FF0(rdram, context);
            func_8009C524(rdram, context);
            // Bootstrap and presentation code can consume the retail RNG
            // without changing canonical gameplay. Re-anchor it to the
            // authoritative end-of-tick value before validating the replay.
            if (!btga::restore_gameplay_random_seed(
                    {rdram, rdram_size}, frame.random_seed)) {
                std::fputs("BTGA_AUTHORITY could not restore replay RNG state\n", stderr);
                std::abort();
            }
            std::array<std::uint8_t, btga::gameplay_snapshot_size> state{};
            const bool captured = btga::capture_gameplay_snapshot(
                {rdram, rdram_size}, state);
            const auto actual_digest = captured ? canonical_state_digest(state) : 0U;
            if (!captured || actual_digest != frame.state_digest) {
                std::uint32_t actual_seed = 0U;
                (void)btga::gameplay_snapshot_random_seed(state, actual_seed);
                std::fprintf(stderr,
                             "BTGA_AUTHORITY persistent replay diverged at tick=%u "
                             "expected=%016llx actual=%016llx "
                             "expected_rng=%08x actual_rng=%08x\n",
                             frame.tick,
                             static_cast<unsigned long long>(frame.state_digest),
                             static_cast<unsigned long long>(actual_digest),
                             frame.random_seed, actual_seed);
                std::abort();
            }
        }
        recovery_replay_running = false;
        recovery_pending = false;
        restored_persistent_session = true;
        std::fprintf(stderr,
                     "BTGA_AUTHORITY recovered checkpoint=%u journal_head=%u\n",
                     checkpoint->next_tick, authority.next_tick());
        std::fflush(stderr);
    }

    // The retail top-level loop is coupled to graphics-task completion and VI
    // pacing. A server has neither. Own the server clock here and invoke the
    // translated gameplay update directly; that update still runs the game's
    // world, collision, AI, damage, objectives and transition code.
    constexpr auto tick_period = std::chrono::microseconds{33333};
    auto deadline = std::chrono::steady_clock::now();
    bool announced_ready = false;
    std::int32_t observed_inner_mode = MEM_W(0, static_cast<gpr>(
        static_cast<std::int32_t>(0x802195CCU)));
    // Never relabel a recovered live state as tick zero. Fresh clients are
    // bootstrapped from the persisted portable checkpoint and journal tail.
    bool initial_snapshot_sent = restored_persistent_session;
    for (;;) {
        bool wait_for_players = false;
        bool wait_for_recovery = false;
        std::uint8_t recovery_mask = 0U;
        std::uint32_t recovery_head = 0U;
        {
            const std::lock_guard lock{authority_mutex};
            send_pending_bootstrap_snapshot_locked();
            const std::uint8_t required = static_cast<std::uint8_t>(
                (1U << configured_players) - 1U);
            if (!announced_ready &&
                (authority.connected_mask() & required) != required) {
                announced_ready = false;
                wait_for_players = true;
            }
            if (!wait_for_players && !announced_ready) {
                std::fprintf(stderr, "BTGA_AUTHORITY all %u seats ready\n",
                             configured_players);
                announced_ready = true;
                match_started = true;
            }
            const bool initial_bootstrap_pending = initial_snapshot_sent &&
                (bootstrap_recovery.pending_mask() & required) != 0U;
            if (!wait_for_players &&
                (initial_bootstrap_pending || bootstrap_recovery.paused())) {
                wait_for_recovery = true;
                recovery_mask = static_cast<std::uint8_t>(
                    bootstrap_recovery.mask() |
                    (initial_bootstrap_pending
                        ? bootstrap_recovery.pending_mask() & required : 0U));
                recovery_head = authority.next_tick();
                if (!recovery_pause_announced) {
                    std::fprintf(stderr,
                                 "BTGA_AUTHORITY recovery wait mask=%02x head=%u\n",
                                 recovery_mask, recovery_head);
                    std::fflush(stderr);
                    recovery_pause_announced = true;
                }
            } else if (recovery_pause_announced && !bootstrap_recovery.paused()) {
                std::fprintf(stderr,
                             "BTGA_AUTHORITY recovery complete head=%u\n",
                             authority.next_tick());
                std::fflush(stderr);
                if (!send_recovery_status_locked()) {
                    std::fputs("BTGA_AUTHORITY recovery status send failed\n", stderr);
                }
                recovery_pause_announced = false;
            }
            // Canonical gameplay frames stop while retail advances match and
            // level-transition states. Keep the existing PING/status channel
            // alive throughout that interval so clients distinguish a healthy
            // authority at a stationary gameplay head from a lost server.
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_recovery_status) {
                if (!send_recovery_status_locked()) {
                    std::fputs("BTGA_AUTHORITY status heartbeat send failed\n", stderr);
                }
                next_recovery_status = now + std::chrono::milliseconds{250};
            }
        }
        if (wait_for_players || wait_for_recovery) {
            deadline = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
            continue;
        }
        btga_advance_retail_mode_counters(rdram);
        func_80079FF0(rdram, context);
        btga_dedicated_test_force_match_end(rdram, authority.next_tick());
        const auto current_inner_mode = MEM_W(0, static_cast<gpr>(
            static_cast<std::int32_t>(0x802195CCU)));
        if (current_inner_mode != observed_inner_mode) {
            std::fprintf(stderr,
                         "BTGA_AUTHORITY mode transition %d->%d tick=%u\n",
                         observed_inner_mode, current_inner_mode,
                         authority.next_tick());
            std::fflush(stderr);
            observed_inner_mode = current_inner_mode;
        }
        if (!initial_snapshot_sent) {
            const std::lock_guard lock{authority_mutex};
            std::vector<std::uint8_t> state;
            if (!btga::capture_dedicated_checkpoint({rdram, rdram_size}, state)) {
                std::fputs("BTGA_AUTHORITY initial snapshot failed\n", stderr);
                continue;
            }
            initial_snapshot_state = state;
            initial_snapshot_available = true;
            for (std::uint8_t seat = 0; seat < btga::online_player_limit; ++seat) {
                const auto bit = static_cast<std::uint8_t>(1U << seat);
                if ((snapshot_subscribers & bit) != 0U) {
                    snapshot_requests |= bit;
                    bootstrap_requests |= bit;
                }
            }
            initial_snapshot_sent = true;
            std::fputs("BTGA_AUTHORITY initial snapshot queued\n", stderr);
        }
        func_8009C524(rdram, context);
        btga_dedicated_advance_results(rdram, context);
        deadline += tick_period;
        std::this_thread::sleep_until(deadline);
        const auto now = std::chrono::steady_clock::now();
        if (now > deadline) {
            const auto lag = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    now - deadline).count());
            maximum_tick_lag_microseconds =
                (std::max)(maximum_tick_lag_microseconds, lag);
            if (now - deadline > tick_period) ++simulation_stalls;
        }
        if (now > deadline + tick_period * 4) deadline = now;
    }
}

extern "C" void btga_dedicated_tick_end(
    std::uint8_t* rdram, recomp_context*)
{
    if (recovery_replay_running) return;
    const std::lock_guard lock{authority_mutex};
    if (!enabled || !gameplay_tick_active) return;
    std::array<std::uint8_t, btga::gameplay_snapshot_size> canonical_state{};
    if (!btga::capture_gameplay_snapshot(
            {rdram, rdram_size}, canonical_state)) {
        std::fputs("Dedicated authority could not capture gameplay state\n", stderr);
        gameplay_tick_active = false;
        return;
    }
    std::vector<std::uint8_t> checkpoint_state;
    if ((active_tick + 1U) % 30U == 0U || transition_checkpoint_pending) {
        if (!btga::capture_dedicated_checkpoint(
                {rdram, rdram_size}, checkpoint_state)) {
            std::fputs("Dedicated authority could not capture recovery checkpoint\n", stderr);
            gameplay_tick_active = false;
            return;
        }
    }
    const auto frame = authority.finish_tick(
        active_tick, canonical_state, checkpoint_state);
    gameplay_tick_active = false;
    if (!frame) {
        std::fputs("Dedicated authority rejected translated tick completion\n", stderr);
        return;
    }
    if (transition_checkpoint_pending) {
        if (!authority.checkpoint_now(checkpoint_state)) {
            std::fputs("BTGA_AUTHORITY transition checkpoint failed\n", stderr);
            std::abort();
        }
        transition_checkpoint_pending = false;
        std::fprintf(stderr,
                     "BTGA_AUTHORITY transition checkpoint tick=%u\n",
                     authority.next_tick());
        std::fflush(stderr);
    }
    if (!authority.persistent_storage_healthy()) {
        std::fputs("BTGA_AUTHORITY persistent checkpoint storage failed\n", stderr);
        std::abort();
    }
    if (multiplayer && multiplayer->state() == btga::OnlineState::Lobby &&
        !encode_and_send_frame(*frame)) {
        std::fputs("Dedicated authority could not publish canonical frame\n", stderr);
    }
    if (frame->tick % 30U == 0U) {
        std::fprintf(stderr,
                     "BTGA_AUTHORITY tick=%u digest=%016llx connected=%02x "
                     "stalls=%llu max_lag_us=%llu input_horizons=%u,%u,%u,%u\n",
                     frame->tick,
                     static_cast<unsigned long long>(frame->state_digest),
                     authority.connected_mask(),
                     static_cast<unsigned long long>(simulation_stalls),
                     static_cast<unsigned long long>(maximum_tick_lag_microseconds),
                     latest_input_horizons[0], latest_input_horizons[1],
                     latest_input_horizons[2], latest_input_horizons[3]);
        std::fflush(stderr);
    }
}
