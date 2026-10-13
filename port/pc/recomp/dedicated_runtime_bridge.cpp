#include "dedicated_runtime_bridge.hpp"
#include "dedicated_client_bridge.hpp"

#include <algorithm>
#include <array>
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
#include "btga/authority_lease.hpp"
#include "btga/gameplay_digest.hpp"
#include "btga/gameplay_snapshot.hpp"
#include "btga/master_client.hpp"
#include "btga/online_multiplayer.hpp"
#include "btga/translated_authority.hpp"
#include "btga/udp_online_io.hpp"
#include "recomp.h"

extern "C" void func_8009C524(std::uint8_t* rdram, recomp_context* ctx);
extern "C" void func_80079FF0(std::uint8_t* rdram, recomp_context* ctx);

extern "C" {
#include <btga/btga_gameplay.h>
#include <btga/btga_net.h>
#include <btga/btga_relay_client.h>
}

namespace {
constexpr std::size_t rdram_size = 8U * 1024U * 1024U;
std::mutex authority_mutex;
std::mutex master_mutex;
btga::TranslatedAuthority authority{0x0fU, 30U, 3U};
std::array<btga::OnlineControllerInput, btga::online_player_limit> active_inputs{};
std::array<std::chrono::steady_clock::time_point, btga::online_player_limit>
    client_last_seen{};
std::array<bool, btga::online_player_limit> client_seen{};
std::chrono::steady_clock::time_point authority_started;
bool enabled = false;
bool gameplay_tick_active = false;
bool recovery_replay_running = false;
bool recovery_pending = false;
thread_local bool dedicated_loop_running = false;
std::uint32_t active_tick = 0;
std::uint32_t inactive_tick_probes = 0;
std::uint32_t network_poll_count = 0;
std::uint32_t received_datagrams = 0;
std::uint64_t session_id = 0;
std::uint32_t outgoing_sequence = 0;
std::uint32_t snapshot_id = 0;
std::uint8_t snapshot_subscribers = 0;
std::uint8_t snapshot_requests = 0;
std::uint8_t bootstrap_requests = 0;
std::uint8_t bootstrap_pending = 0;
std::uint8_t replay_requests = 0;
std::uint8_t snapshot_cursor = 0;
std::array<std::chrono::steady_clock::time_point, btga::online_player_limit>
    snapshot_last_sent{};
std::array<std::uint32_t, btga::online_player_limit> replay_next_tick{};
std::array<std::uint8_t, btga::gameplay_snapshot_size> initial_snapshot_state{};
bool initial_snapshot_available = false;
unsigned int configured_players = 4;
std::unique_ptr<btga::UdpOnlineIo> udp;
std::unique_ptr<btga::OnlineMultiplayer> multiplayer;
std::jthread network_thread;
std::unique_ptr<btga::HttpClient> master_http;
std::unique_ptr<btga::MasterClient> master;
std::string public_lobby_id;
std::string public_host_secret;
std::chrono::steady_clock::time_point next_heartbeat{};
std::optional<std::filesystem::path> persistent_lease_path;
btga::PersistentAuthorityLease persistent_lease;

std::uint64_t canonical_state_digest(std::span<const std::uint8_t> state) {
    std::uint64_t value = UINT64_C(1469598103934665603);
    for (const auto byte : state) {
        value ^= byte;
        value *= UINT64_C(1099511628211);
    }
    return value;
}

void poll_master_heartbeat() {
    const auto now = std::chrono::steady_clock::now();
    unsigned players = 0;
    std::uint8_t seat_mask = 0U;
    std::uint32_t packet_sequence = 0U;
    {
        const std::lock_guard lock{authority_mutex};
        seat_mask = authority.connected_mask();
        for (unsigned seat = 0; seat < 4; ++seat) {
            players += (seat_mask >> seat) & 1U;
        }
        packet_sequence = outgoing_sequence;
    }
    const std::lock_guard lock{master_mutex};
    if (!master || now < next_heartbeat) return;
    try {
        master->heartbeat(public_lobby_id, public_host_secret,
                          static_cast<std::uint16_t>(std::max(1U, players)),
                          players == configured_players ? "playing" : "lobby",
                          seat_mask);
        if (persistent_lease_path) {
            persistent_lease.packet_sequence = packet_sequence;
            if (!btga::save_authority_lease(
                    *persistent_lease_path, persistent_lease)) {
                std::fputs("BTGA_AUTHORITY could not persist packet sequence\n", stderr);
            }
        }
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "BTGA_AUTHORITY heartbeat failed: %s\n", exception.what());
    }
    next_heartbeat = now + std::chrono::seconds{15};
}

std::uint32_t server_clock_tick() {
    const auto elapsed = std::chrono::steady_clock::now() - authority_started;
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 33);
}

void advance_retail_mode_counters(std::uint8_t* rdram) {
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
    session_id = requested_session;
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
    authority_started = std::chrono::steady_clock::now() -
        std::chrono::milliseconds{static_cast<std::int64_t>(authority.next_tick()) * 33};
    client_seen.fill(false);
    snapshot_subscribers = 0U;
    snapshot_requests = 0U;
    bootstrap_requests = 0U;
    bootstrap_pending = 0U;
    replay_requests = 0U;
    snapshot_cursor = 0U;
    snapshot_last_sent.fill({});
    replay_next_tick.fill(0U);
    initial_snapshot_available = false;
    for (std::uint8_t seat = 0; seat < btga::online_player_limit; ++seat) {
        (void)authority.set_connected(seat, false);
    }
    enabled = true;
    network_thread = std::jthread([](std::stop_token stop) {
        while (!stop.stop_requested()) {
            poll_master_heartbeat();
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    });
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
    wire.connected_mask = authority.connected_mask();
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
    if (payload_size == 0U) return false;
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
    if (btga_net_encode_header(packet.data(), packet.size(), &header) == 0U) return false;
    return multiplayer && multiplayer->send_gameplay(
        destination_slot,
        {packet.data(), BTGA_NET_HEADER_SIZE + payload_size});
}

bool encode_and_send_snapshot(std::uint32_t tick, std::uint16_t destination_slot,
                              std::span<const std::uint8_t> state,
                              std::uint64_t canonical_digest = 0U) {
    if (canonical_digest == 0U) {
        canonical_digest = UINT64_C(1469598103934665603);
        for (const std::uint8_t byte : state) {
            canonical_digest ^= byte;
            canonical_digest *= UINT64_C(1099511628211);
        }
    }
    const auto total = static_cast<std::uint32_t>(state.size());
    const std::uint32_t id = ++snapshot_id;
    for (std::uint32_t offset = 0; offset < total;) {
        const auto data_size = static_cast<std::uint16_t>(std::min<std::uint32_t>(
            BTGA_NET_SNAPSHOT_CHUNK_DATA_LIMIT, total - offset));
        std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
        BtgaNetSnapshotChunk chunk{id, tick, canonical_digest, total, offset, data_size,
                                   state.data() + offset};
        const std::size_t payload_size = btga_net_encode_snapshot_chunk(
            packet.data() + BTGA_NET_HEADER_SIZE,
            packet.size() - BTGA_NET_HEADER_SIZE, &chunk);
        if (payload_size == 0U) return false;
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
            !multiplayer->send_gameplay(
                destination_slot,
                {packet.data(), BTGA_NET_HEADER_SIZE + payload_size})) return false;
        offset += data_size;
        // Avoid a 33-datagram burst overflowing client UDP receive buffers.
        std::this_thread::sleep_for(std::chrono::milliseconds{
            state.size() > btga::gameplay_snapshot_size ? 7 : 2});
    }
    return true;
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
    const std::string players_text = variable("BTGA_DEDICATED_PLAYERS");
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
    if (!players_text.empty()) {
        const auto [players_end, players_error] = std::from_chars(
            players_text.data(), players_text.data() + players_text.size(), configured_players);
        if (players_error != std::errc{} ||
            players_end != players_text.data() + players_text.size() ||
            configured_players < 1U || configured_players > 4U) {
            error = "BTGA_DEDICATED_PLAYERS must be from 1 through 4";
            return false;
        }
    }
    return enable_relay(session_id,
                        {relay_host, static_cast<std::uint16_t>(relay_port)}, token, error);
}

bool btga_dedicated_enable_public(std::string_view name, std::string& error) {
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
            const std::string relay_token = client->heartbeat(
                lease->lobby_id, lease->host_secret, 1U, "playing", 0U);
            // A heartbeat persists the exact sequence at least every 15
            // seconds. Reserve a gap larger than the maximum packets that can
            // be emitted in that interval, and persist it before relay bind.
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
        const btga::HostRegistration registration = client->host(name, 4);
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
    const std::lock_guard lock{authority_mutex};
    if (!enabled || !multiplayer) return;
    if (++network_poll_count == 1U) {
        std::fputs("BTGA_AUTHORITY network pump active\n", stderr);
    }
    if (multiplayer->state() == btga::OnlineState::RelayBinding) {
        (void)multiplayer->poll_relay_binding();
        return;
    }
    if (multiplayer->state() != btga::OnlineState::Lobby) return;
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
        if (received_datagrams++ < 12U) {
            std::fprintf(stderr, "BTGA_AUTHORITY input source=%u accepted=%d bytes=%zu\n",
                         static_cast<unsigned>(source_slot), accepted ? 1 : 0, size);
        }
        if (accepted) {
            const auto seat = static_cast<std::size_t>(source_slot - 1U);
            if (subscription) {
                const auto bit = static_cast<std::uint8_t>(1U << seat);
                constexpr auto retry_delay = std::chrono::seconds{3};
                const bool first_subscription = (snapshot_subscribers & bit) == 0U;
                const bool bootstrap_missing = incoming.acknowledgment == 0U;
                const bool retry_due = snapshot_last_sent[seat] ==
                        std::chrono::steady_clock::time_point{} ||
                    now - snapshot_last_sent[seat] >= retry_delay;
                if (bootstrap_missing) {
                    bootstrap_pending |= bit;
                    bootstrap_requests |= bit;
                    replay_requests &= static_cast<std::uint8_t>(~bit);
                    if (first_subscription || retry_due) snapshot_requests |= bit;
                } else if ((bootstrap_pending & bit) != 0U &&
                           incoming.acknowledgment != 1U) {
                    // A resumed relay seat can still have a delayed PING from
                    // the dead endpoint in flight. Until the replacement
                    // process confirms tick zero, ignore its predecessor's
                    // higher acknowledgment rather than sending a live
                    // snapshot into an unbootstrapped runtime.
                } else {
                    bootstrap_pending &= static_cast<std::uint8_t>(~bit);
                    bootstrap_requests &= static_cast<std::uint8_t>(~bit);
                    snapshot_requests &= static_cast<std::uint8_t>(~bit);
                    // acknowledgment is encoded as next_tick + 1 so zero can
                    // unambiguously mean "no tick-zero bootstrap installed".
                    replay_next_tick[seat] = incoming.acknowledgment - 1U;
                    if (replay_next_tick[seat] < authority.next_tick()) {
                        replay_requests |= bit;
                    } else {
                        replay_requests &= static_cast<std::uint8_t>(~bit);
                    }
                }
                snapshot_subscribers |= bit;
            }
            client_last_seen[seat] = now;
            client_seen[seat] = true;
            (void)authority.set_connected(static_cast<std::uint8_t>(seat), true);
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
    // One bounded journal window is sent per client acknowledgment. A client
    // cannot make the authority burst an unbounded match history, while a
    // recovering client can advance the window by acknowledging consumed
    // ticks. Existing real-time clients normally request zero or one frame.
    constexpr std::size_t replay_window = 32U;
    for (std::uint8_t seat = 0; seat < btga::online_player_limit; ++seat) {
        const auto bit = static_cast<std::uint8_t>(1U << seat);
        if ((replay_requests & bit) == 0U) continue;
        replay_requests &= static_cast<std::uint8_t>(~bit);
        std::array<btga::AuthoritativeFrame, replay_window> frames{};
        if (replay_next_tick[seat] < authority.journal_base_tick()) {
            std::fprintf(stderr,
                         "BTGA_AUTHORITY disk journal replay seat=%u tick=%u base=%u\n",
                         static_cast<unsigned>(seat), replay_next_tick[seat],
                         authority.journal_base_tick());
        }
        const std::size_t count = authority.copy_journal_from(
            replay_next_tick[seat], frames);
        for (std::size_t index = 0; index < count; ++index) {
            if (!encode_and_send_frame(
                    frames[index], static_cast<std::uint16_t>(seat + 1U))) {
                std::fprintf(stderr,
                             "BTGA_AUTHORITY journal replay failed seat=%u tick=%u\n",
                             static_cast<unsigned>(seat), frames[index].tick);
                break;
            }
        }
    }
    constexpr auto client_timeout = std::chrono::seconds{2};
    for (std::uint8_t seat = 0; seat < btga::online_player_limit; ++seat) {
        if (client_seen[seat] && now - client_last_seen[seat] > client_timeout) {
            client_seen[seat] = false;
            const auto bit = static_cast<std::uint8_t>(1U << seat);
            snapshot_subscribers &= static_cast<std::uint8_t>(~bit);
            snapshot_requests &= static_cast<std::uint8_t>(~bit);
            bootstrap_requests &= static_cast<std::uint8_t>(~bit);
            bootstrap_pending &= static_cast<std::uint8_t>(~bit);
            replay_requests &= static_cast<std::uint8_t>(~bit);
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
    // During retail initialization this hook may be the only callback that is
    // running regularly. Keep relay admission and readiness moving here too.
    btga_dedicated_network_poll();
    {
        const std::lock_guard lock{authority_mutex};
        if (!enabled || gameplay_tick_active) return;
    }
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
    std::uint32_t wait_iterations = 0;
    for (;;) {
        btga_dedicated_network_poll();
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
        if (!btga::restore_gameplay_snapshot(
                {rdram, rdram_size}, checkpoint->state)) {
            std::fputs("BTGA_AUTHORITY persistent checkpoint restore failed\n", stderr);
            std::abort();
        }
        recovery_replay_running = true;
        for (const auto& frame : replay) {
            active_inputs = frame.inputs;
            advance_retail_mode_counters(rdram);
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
            const bool captured = btga::capture_gameplay_snapshot({rdram, rdram_size}, state);
            if (!captured || canonical_state_digest(state) != frame.state_digest) {
                std::fprintf(stderr,
                             "BTGA_AUTHORITY persistent replay diverged at tick=%u\n",
                             frame.tick);
                std::abort();
            }
        }
        recovery_replay_running = false;
        recovery_pending = false;
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
    bool initial_snapshot_sent = false;
    std::uint32_t last_snapshot_tick = 0U;
    for (;;) {
        btga_dedicated_network_poll();
        {
            const std::lock_guard lock{authority_mutex};
            const std::uint8_t required = static_cast<std::uint8_t>(
                (1U << configured_players) - 1U);
            if (!announced_ready &&
                (authority.connected_mask() & required) != required) {
                announced_ready = false;
                deadline = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
                continue;
            }
            if (!announced_ready) {
                std::fprintf(stderr, "BTGA_AUTHORITY all %u seats ready\n",
                             configured_players);
                announced_ready = true;
            }
        }
        advance_retail_mode_counters(rdram);
        func_80079FF0(rdram, context);
        if (!initial_snapshot_sent) {
            const std::lock_guard lock{authority_mutex};
            std::array<std::uint8_t, btga::gameplay_snapshot_size> state{};
            if (!btga::capture_gameplay_snapshot({rdram, rdram_size}, state)) {
                std::fputs("BTGA_AUTHORITY initial snapshot failed\n", stderr);
                continue;
            }
            initial_snapshot_state = state;
            initial_snapshot_available = true;
            for (std::uint8_t seat = 0; seat < btga::online_player_limit; ++seat) {
                const auto bit = static_cast<std::uint8_t>(1U << seat);
                if ((snapshot_subscribers & bit) != 0U) {
                    if (!encode_and_send_snapshot(
                            0U, static_cast<std::uint16_t>(seat + 1U), state)) {
                        std::fputs("BTGA_AUTHORITY initial snapshot failed\n", stderr);
                        continue;
                    }
                    snapshot_last_sent[seat] = std::chrono::steady_clock::now();
                    snapshot_requests &= static_cast<std::uint8_t>(~bit);
                    bootstrap_requests &= static_cast<std::uint8_t>(~bit);
                }
            }
            initial_snapshot_sent = true;
            last_snapshot_tick = 0U;
            std::fputs("BTGA_AUTHORITY initial snapshot published\n", stderr);
        } else {
            const std::lock_guard lock{authority_mutex};
            const std::uint32_t next_tick = authority.next_tick();
            std::uint8_t target = UINT8_MAX;
            for (std::uint8_t offset = 0; offset < btga::online_player_limit; ++offset) {
                const auto seat = static_cast<std::uint8_t>(
                    (snapshot_cursor + offset) % btga::online_player_limit);
                if ((snapshot_requests & (1U << seat)) != 0U) {
                    target = seat;
                    break;
                }
            }
            if (target == UINT8_MAX && next_tick > 1U &&
                next_tick != last_snapshot_tick && next_tick % 8U == 1U) {
                for (std::uint8_t offset = 0; offset < btga::online_player_limit; ++offset) {
                    const auto seat = static_cast<std::uint8_t>(
                        (snapshot_cursor + offset) % btga::online_player_limit);
                    if ((snapshot_subscribers & (1U << seat)) != 0U) {
                        target = seat;
                        break;
                    }
                }
                last_snapshot_tick = next_tick;
            }
            if (target != UINT8_MAX) {
                std::array<std::uint8_t, btga::gameplay_snapshot_size> state{};
                const auto bit = static_cast<std::uint8_t>(1U << target);
                const bool bootstrap = initial_snapshot_available &&
                    (bootstrap_pending & bit) != 0U;
                const std::uint32_t snapshot_tick = bootstrap ? 0U : next_tick;
                bool captured = bootstrap;
                if (bootstrap) {
                    state = initial_snapshot_state;
                } else {
                    captured = btga::capture_gameplay_snapshot(
                        {rdram, rdram_size}, state);
                }
                std::uint64_t digest = UINT64_C(1469598103934665603);
                for (const auto byte : state) {
                    digest ^= byte;
                    digest *= UINT64_C(1099511628211);
                }
                if (captured && encode_and_send_snapshot(
                        snapshot_tick, static_cast<std::uint16_t>(target + 1U),
                        state, digest)) {
                    snapshot_last_sent[target] = std::chrono::steady_clock::now();
                    snapshot_requests &= static_cast<std::uint8_t>(~bit);
                    if (bootstrap) {
                        bootstrap_requests &= static_cast<std::uint8_t>(~bit);
                    }
                    snapshot_cursor = static_cast<std::uint8_t>(
                        (target + 1U) % btga::online_player_limit);
                } else {
                    std::fputs("BTGA_AUTHORITY recovery snapshot failed\n", stderr);
                }
            }
        }
        func_8009C524(rdram, context);
        deadline += tick_period;
        std::this_thread::sleep_until(deadline);
        const auto now = std::chrono::steady_clock::now();
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
    const auto frame = authority.finish_tick(active_tick, canonical_state);
    gameplay_tick_active = false;
    if (!frame) {
        std::fputs("Dedicated authority rejected translated tick completion\n", stderr);
        return;
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
                     "BTGA_AUTHORITY tick=%u digest=%016llx connected=%02x\n",
                     frame->tick,
                     static_cast<unsigned long long>(frame->state_digest),
                     authority.connected_mask());
        std::fflush(stderr);
    }
}
