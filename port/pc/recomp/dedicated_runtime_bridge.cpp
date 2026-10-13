#include "dedicated_runtime_bridge.hpp"
#include "dedicated_client_bridge.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>

#include "btga/authoritative_server.hpp"
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
btga::TranslatedAuthority authority{0x0fU, 30U, 3U};
std::array<btga::OnlineControllerInput, btga::online_player_limit> active_inputs{};
std::array<std::chrono::steady_clock::time_point, btga::online_player_limit>
    client_last_seen{};
std::array<bool, btga::online_player_limit> client_seen{};
std::chrono::steady_clock::time_point authority_started;
bool enabled = false;
bool gameplay_tick_active = false;
std::uint32_t active_tick = 0;
std::uint32_t inactive_tick_probes = 0;
std::uint32_t network_poll_count = 0;
std::uint32_t received_datagrams = 0;
std::uint64_t session_id = 0;
std::uint32_t outgoing_sequence = 0;
std::uint32_t snapshot_id = 0;
unsigned int configured_players = 4;
std::unique_ptr<btga::UdpOnlineIo> udp;
std::unique_ptr<btga::OnlineMultiplayer> multiplayer;
std::jthread network_thread;
std::unique_ptr<btga::HttpClient> master_http;
std::unique_ptr<btga::MasterClient> master;
std::string public_lobby_id;
std::string public_host_secret;
std::chrono::steady_clock::time_point next_heartbeat{};

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
    authority_started = std::chrono::steady_clock::now();
    client_seen.fill(false);
    for (std::uint8_t seat = 0; seat < btga::online_player_limit; ++seat) {
        (void)authority.set_connected(seat, false);
    }
    enabled = true;
    network_thread = std::jthread([](std::stop_token stop) {
        while (!stop.stop_requested()) {
            btga_dedicated_network_poll();
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    });
    return true;
}
}

bool encode_and_send_frame(const btga::AuthoritativeFrame& frame)
{
    BtgaNetAuthoritativeFrame wire{};
    wire.tick = frame.tick;
    wire.state_digest = frame.state_digest;
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
    header.acknowledgment = 0;
    header.acknowledgment_bits = 0;
    header.payload_size = static_cast<std::uint16_t>(payload_size);
    header.peer_id = 0;
    if (btga_net_encode_header(packet.data(), packet.size(), &header) == 0U) return false;
    return multiplayer && multiplayer->send_gameplay(
        BTGA_RELAY_BROADCAST_SLOT,
        {packet.data(), BTGA_NET_HEADER_SIZE + payload_size});
}

bool encode_and_send_snapshot(std::uint32_t tick,
                              std::span<const std::uint8_t> state) {
    std::uint64_t digest = UINT64_C(1469598103934665603);
    for (const std::uint8_t byte : state) {
        digest ^= byte;
        digest *= UINT64_C(1099511628211);
    }
    const auto total = static_cast<std::uint32_t>(state.size());
    const std::uint32_t id = ++snapshot_id;
    for (std::uint32_t offset = 0; offset < total;) {
        const auto data_size = static_cast<std::uint16_t>(std::min<std::uint32_t>(
            BTGA_NET_SNAPSHOT_CHUNK_DATA_LIMIT, total - offset));
        std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
        BtgaNetSnapshotChunk chunk{id, tick, digest, total, offset, data_size,
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
                BTGA_RELAY_BROADCAST_SLOT,
                {packet.data(), BTGA_NET_HEADER_SIZE + payload_size})) return false;
        offset += data_size;
        // Avoid a 33-datagram burst overflowing client UDP receive buffers.
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
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
        auto http = btga::make_platform_http_client(
            "https://btga.thegh0stship.com/api/v1");
        auto client = std::make_unique<btga::MasterClient>(*http);
        const btga::HostRegistration registration = client->host(name, 4);
        {
            const std::lock_guard lock{authority_mutex};
            if (!enable_relay(registration.session_id, registration.relay,
                              registration.relay_token, error)) return false;
            public_lobby_id = registration.lobby.id;
            public_host_secret = registration.host_secret;
            next_heartbeat = std::chrono::steady_clock::now();
            master_http = std::move(http);
            master = std::move(client);
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
        const bool accepted = authority.ingest_client_packet(
                std::span<const std::uint8_t>{packet.data(), size},
                session_id, source_slot);
        if (received_datagrams++ < 12U) {
            std::fprintf(stderr, "BTGA_AUTHORITY input source=%u accepted=%d bytes=%zu\n",
                         static_cast<unsigned>(source_slot), accepted ? 1 : 0, size);
        }
        if (accepted) {
            const auto seat = static_cast<std::size_t>(source_slot - 1U);
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
    constexpr auto client_timeout = std::chrono::seconds{2};
    for (std::uint8_t seat = 0; seat < btga::online_player_limit; ++seat) {
        if (client_seen[seat] && now - client_last_seen[seat] > client_timeout) {
            client_seen[seat] = false;
            (void)authority.set_connected(seat, false);
        }
    }
    if (master && now >= next_heartbeat) {
        unsigned players = 0;
        const auto mask = authority.connected_mask();
        for (unsigned seat = 0; seat < 4; ++seat) players += (mask >> seat) & 1U;
        try {
            master->heartbeat(public_lobby_id, public_host_secret,
                              static_cast<std::uint16_t>(std::max(1U, players)),
                              players == configured_players ? "playing" : "lobby");
        } catch (const std::exception& exception) {
            std::fprintf(stderr, "BTGA_AUTHORITY heartbeat failed: %s\n", exception.what());
        }
        next_heartbeat = now + std::chrono::seconds{15};
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
            if (!btga::capture_gameplay_snapshot({rdram, rdram_size}, state) ||
                !encode_and_send_snapshot(0U, state)) {
                std::fputs("BTGA_AUTHORITY initial snapshot failed\n", stderr);
                continue;
            }
            initial_snapshot_sent = true;
            last_snapshot_tick = 0U;
            std::fputs("BTGA_AUTHORITY initial snapshot published\n", stderr);
        } else {
            const std::lock_guard lock{authority_mutex};
            const std::uint32_t next_tick = authority.next_tick();
            if (next_tick > 1U && next_tick != last_snapshot_tick &&
                next_tick % 30U == 1U) {
                std::array<std::uint8_t, btga::gameplay_snapshot_size> state{};
                if (btga::capture_gameplay_snapshot({rdram, rdram_size}, state) &&
                    encode_and_send_snapshot(next_tick, state)) {
                    last_snapshot_tick = next_tick;
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
