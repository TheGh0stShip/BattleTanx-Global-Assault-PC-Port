#include "dedicated_client_bridge.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <thread>

#include "btga/authoritative_replica.hpp"
#include "btga/gameplay_digest.hpp"
#include "btga/gameplay_snapshot.hpp"
#include "btga/input_sync.hpp"
#include "btga/online_multiplayer.hpp"
#include "btga/udp_online_io.hpp"

extern "C" {
#include <btga/btga_net.h>
}

namespace {
constexpr std::size_t history_size = 256;
constexpr std::size_t rdram_size = 8U * 1024U * 1024U;
std::mutex client_mutex;
std::unique_ptr<btga::UdpOnlineIo> client_udp;
std::unique_ptr<btga::OnlineMultiplayer> client_online;
std::array<std::optional<btga::CanonicalFrame>, history_size> frames{};
std::array<btga::OnlineControllerInput, 4> active_inputs{};
btga::InputSynchronizer input_encoder;
btga::AuthoritativeReplica replica;
std::optional<btga::CanonicalSnapshot> pending_snapshot;
std::uint64_t client_session = 0;
std::uint32_t expected_tick = 0;
std::uint32_t input_sequence = 0;
std::uint8_t local_seat = 0;
bool client_enabled = false;
bool tick_active = false;
bool bootstrap_complete = false;
bool divergence_reported = false;
bool replay_announced = false;
std::uint64_t expected_digest = 0;
std::chrono::steady_clock::time_point next_keepalive{};
std::uint32_t current_random_seed = 0U;
std::uint32_t expected_random_seed = 0U;

std::uint64_t state_digest(std::span<const std::uint8_t> state) {
    std::uint64_t digest = UINT64_C(1469598103934665603);
    for (const std::uint8_t byte : state) {
        digest ^= byte;
        digest *= UINT64_C(1099511628211);
    }
    return digest;
}

bool poll_frames_locked() {
    if (!client_online || client_online->state() != btga::OnlineState::Lobby) return false;
    std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
    for (;;) {
        std::uint16_t source = 0;
        const std::size_t size = client_online->receive_gameplay(packet, source);
        if (size == 0U) break;
        BtgaNetHeader header{};
        if (source != 0U || !btga_net_decode_header(&header, packet.data(), size) ||
            header.protocol_version != BTGA_NET_PROTOCOL_VERSION ||
            header.session_id != client_session || header.peer_id != 0U ||
            header.payload_size != size - BTGA_NET_HEADER_SIZE) continue;
        const auto payload = std::span<const std::uint8_t>{
            packet.data() + BTGA_NET_HEADER_SIZE, header.payload_size};
        if (header.message_type == BTGA_NET_MSG_SNAPSHOT) {
            if (replica.accept_snapshot_chunk(payload) && replica.snapshot_ready()) {
                pending_snapshot = replica.take_snapshot();
            }
            continue;
        }
        if (header.message_type != BTGA_NET_MSG_EVENT) continue;
        btga::CanonicalFrame frame{};
        if (!btga::decode_authoritative_frame(payload, frame)) continue;
        if (!replay_announced && header.acknowledgment > expected_tick + 8U) {
            std::fprintf(stderr,
                         "BTGA_CLIENT journal replay active next=%u authority=%u\n",
                         expected_tick, header.acknowledgment);
            replay_announced = true;
        }
        if (static_cast<std::int32_t>(frame.tick - expected_tick) < 0 ||
            frame.tick - expected_tick >= history_size) continue;
        frames[frame.tick % history_size] = frame;
    }
    return true;
}

bool send_inputs_locked(std::uint32_t first_tick,
                        btga::OnlineControllerInput input) {
    constexpr std::size_t count = 8;
    std::array<btga::OnlineControllerInput, count> inputs{};
    inputs.fill(input);
    std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
    const std::size_t size = input_encoder.encode_batch(
        packet, client_session, client_online->relay_slot(), ++input_sequence,
        first_tick, local_seat, inputs);
    return size != 0U && client_online->send_gameplay(0U, {packet.data(), size});
}

bool send_snapshot_subscription_locked() {
    std::array<std::uint8_t, BTGA_NET_HEADER_SIZE> packet{};
    BtgaNetHeader header{};
    header.protocol_version = BTGA_NET_PROTOCOL_VERSION;
    header.message_type = BTGA_NET_MSG_PING;
    header.flags = BTGA_NET_FLAG_RELAYED;
    header.session_id = client_session;
    header.sequence = ++input_sequence;
    // Zero means this process has not yet installed an authority checkpoint.
    // Once bootstrapped, tick+1 keeps tick zero distinguishable from that state.
    header.acknowledgment = bootstrap_complete ? expected_tick + 1U : 0U;
    header.payload_size = 0U;
    header.peer_id = client_online->relay_slot();
    return btga_net_encode_header(packet.data(), packet.size(), &header) != 0U &&
        client_online->send_gameplay(0U, packet);
}

void send_keepalive_if_due_locked() {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_keepalive) return;
    (void)send_snapshot_subscription_locked();
    (void)send_inputs_locked(expected_tick, {});
    next_keepalive = now + std::chrono::milliseconds{250};
}
} // namespace

bool btga_dedicated_client_enable(
    std::uint64_t session_id, const btga::RelayEndpoint& relay,
    std::string_view token, std::string& error) {
    const std::lock_guard lock{client_mutex};
    if (client_enabled || session_id == 0U || relay.address.empty() ||
        relay.port == 0U || token.empty()) {
        error = "invalid dedicated-client registration";
        return false;
    }
    client_udp = std::make_unique<btga::UdpOnlineIo>(relay.address, relay.port);
    client_online = std::make_unique<btga::OnlineMultiplayer>(*client_udp);
    const auto bytes = std::span{
        reinterpret_cast<const std::uint8_t*>(token.data()), token.size()};
    if (!client_online->enter_online() ||
        !client_online->begin_relay(false, session_id, bytes)) {
        error = "could not begin dedicated relay binding";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < deadline &&
           !client_online->poll_relay_binding()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    if (client_online->state() != btga::OnlineState::Lobby ||
        client_online->relay_slot() < 1U || client_online->relay_slot() > 4U) {
        error = "dedicated relay binding timed out or assigned an invalid seat";
        return false;
    }
    client_session = session_id;
    local_seat = static_cast<std::uint8_t>(client_online->relay_slot() - 1U);
    expected_tick = 0U;
    input_sequence = 0U;
    frames.fill(std::nullopt);
    pending_snapshot.reset();
    tick_active = false;
    client_enabled = true;
    bootstrap_complete = false;
    divergence_reported = false;
    replay_announced = false;
    current_random_seed = 0U;
    expected_random_seed = 0U;
    next_keepalive = std::chrono::steady_clock::now();
    if (!send_snapshot_subscription_locked() || !send_inputs_locked(0U, {})) {
        error = "could not announce dedicated-client readiness";
        client_enabled = false;
        return false;
    }
    std::fprintf(stderr, "BTGA_CLIENT bound authoritative seat=%u\n",
                 static_cast<unsigned>(local_seat));
    return true;
}

extern "C" int btga_dedicated_client_enabled() {
    const std::lock_guard lock{client_mutex};
    return client_enabled ? 1 : 0;
}

extern "C" void btga_dedicated_client_tick_begin(
    std::uint8_t* rdram, recomp_context*) {
    std::unique_lock lock{client_mutex};
    if (!client_enabled || tick_active) return;
    if (!btga::gameplay_frame_input_active({rdram, rdram_size})) return;
    const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    bool warned = false;
    for (;;) {
        (void)poll_frames_locked();
        send_keepalive_if_due_locked();
        if (pending_snapshot && bootstrap_complete &&
            pending_snapshot->tick < expected_tick) {
            pending_snapshot.reset();
        }
        if (pending_snapshot && !bootstrap_complete && pending_snapshot->tick != 0U) {
            std::fprintf(stderr,
                         "BTGA_CLIENT rejected pre-bootstrap live snapshot tick=%u\n",
                         pending_snapshot->tick);
            pending_snapshot.reset();
        }
        if (pending_snapshot &&
            (!bootstrap_complete || pending_snapshot->tick >= expected_tick) &&
            pending_snapshot->state.size() == btga::gameplay_snapshot_size) {
            std::array<std::uint8_t, btga::gameplay_snapshot_size> current{};
            const bool captured =
                btga::capture_gameplay_snapshot({rdram, rdram_size}, current);
            const std::uint64_t current_digest = captured ? state_digest(current) : 0U;
            const bool already_current =
                captured && current_digest == pending_snapshot->state_digest;
            // This compact snapshot deliberately excludes host-local and
            // pointer-owned runtime state. It is safe for the cold-bootstrap
            // rendezvous, where both processes still share the same clean
            // runtime topology, but it is not a portable live checkpoint.
            // Applying it to a running client can splice authority gameplay
            // fields into unrelated local scheduler/audio state and make the
            // very next tick diverge. After bootstrap it is verification only;
            // recovery will use the canonical input journal plus an explicit
            // portable checkpoint once that schema is complete.
            const bool may_restore = !bootstrap_complete;
            bool restored = false;
            if (may_restore && !already_current) {
                restored = btga::restore_gameplay_snapshot(
                    {rdram, rdram_size}, pending_snapshot->state);
                if (restored) {
                    std::array<std::uint8_t, btga::gameplay_snapshot_size> verify{};
                    restored = btga::capture_gameplay_snapshot(
                        {rdram, rdram_size}, verify) &&
                        state_digest(verify) == pending_snapshot->state_digest;
                }
            }
            if (already_current || restored) {
                expected_tick = pending_snapshot->tick;
                (void)btga::gameplay_snapshot_random_seed(
                    pending_snapshot->state, current_random_seed);
                bootstrap_complete = true;
                if (!already_current) divergence_reported = false;
                if (may_restore) frames.fill(std::nullopt);
                std::fprintf(stderr, "BTGA_CLIENT %s snapshot tick=%u\n",
                             already_current ? "confirmed" : "applied",
                             expected_tick);
            } else if (bootstrap_complete) {
                std::fprintf(stderr,
                             "BTGA_CLIENT ignored nonportable live snapshot tick=%u "
                             "snapshot=%016llx current=%016llx previous=%016llx\n",
                             pending_snapshot->tick,
                             static_cast<unsigned long long>(pending_snapshot->state_digest),
                             static_cast<unsigned long long>(current_digest),
                             static_cast<unsigned long long>(expected_digest));
            }
            pending_snapshot.reset();
        }
        auto& slot = frames[expected_tick % history_size];
        if (slot && slot->tick == expected_tick) {
            if (!btga::restore_gameplay_random_seed(
                    {rdram, rdram_size}, current_random_seed)) {
                return;
            }
            std::ranges::copy(slot->inputs, active_inputs.begin());
            expected_digest = slot->state_digest;
            expected_random_seed = slot->random_seed;
            slot.reset();
            tick_active = true;
            return;
        }
        if (!warned && std::chrono::steady_clock::now() >= timeout) {
            std::fprintf(stderr, "BTGA_CLIENT authority timeout at tick=%u\n", expected_tick);
            std::fflush(stderr);
            warned = true; // remain paused; never advance a divergent local tick
        }
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        lock.lock();
    }
}

extern "C" void btga_dedicated_client_tick_end(
    std::uint8_t* rdram, recomp_context*) {
    const std::lock_guard lock{client_mutex};
    if (!client_enabled || !tick_active) return;
    std::array<std::uint8_t, btga::gameplay_snapshot_size> canonical_state{};
    // Presentation and host pacing can consume the retail RNG between or
    // during authoritative ticks. The server-provided end seed is canonical.
    (void)btga::restore_gameplay_random_seed(
        {rdram, rdram_size}, expected_random_seed);
    if (btga::capture_gameplay_snapshot({rdram, rdram_size}, canonical_state)) {
        const std::uint64_t digest = state_digest(canonical_state);
        if (digest != expected_digest && !divergence_reported) {
            std::fprintf(stderr,
                         "BTGA_CLIENT DESYNC tick=%u authority=%016llx local=%016llx\n",
                         expected_tick,
                         static_cast<unsigned long long>(expected_digest),
                         static_cast<unsigned long long>(digest));
            std::fflush(stderr);
            divergence_reported = true;
        }
    }
    current_random_seed = expected_random_seed;
    ++expected_tick;
    tick_active = false;
}

bool btga_dedicated_client_controller_input(
    int controller, std::uint16_t& buttons, float& x, float& y) {
    const std::lock_guard lock{client_mutex};
    if (!client_enabled || !tick_active || controller < 0 || controller >= 4) return false;
    const auto& input = active_inputs[static_cast<std::size_t>(controller)];
    buttons = input.buttons;
    x = static_cast<float>(input.stick_x) / 127.0F;
    y = static_cast<float>(input.stick_y) / 127.0F;
    return true;
}

int btga_dedicated_client_local_seat() {
    const std::lock_guard lock{client_mutex};
    return client_enabled ? local_seat : -1;
}

void btga_dedicated_client_pump() {
    const std::lock_guard lock{client_mutex};
    if (!client_enabled || !client_online) return;
    (void)poll_frames_locked();
    send_keepalive_if_due_locked();
}

void btga_dedicated_client_submit_local(
    std::uint16_t buttons, float x, float y) {
    const std::lock_guard lock{client_mutex};
    if (!client_enabled || !client_online) return;
    const auto axis = [](float value) {
        return static_cast<std::int8_t>(std::clamp(std::lround(value * 127.0F),
                                                   -127L, 127L));
    };
    (void)send_inputs_locked(expected_tick + 2U, {buttons, axis(x), axis(y)});
}
