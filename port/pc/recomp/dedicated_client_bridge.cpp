#include "dedicated_client_bridge.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "btga/authoritative_replica.hpp"
#include "btga/client_resync.hpp"
#include "btga/dedicated_resume.hpp"
#include "btga/gameplay_digest.hpp"
#include "btga/gameplay_snapshot.hpp"
#include "btga/input_sync.hpp"
#include "btga/master_client.hpp"
#include "btga/online_multiplayer.hpp"
#include "btga/udp_online_io.hpp"
#include "dedicated_runtime_bridge.hpp"
#include "ultramodern/ultramodern.hpp"

extern "C" void func_8009C524(std::uint8_t* rdram, recomp_context* ctx);
extern "C" void func_80079FF0(std::uint8_t* rdram, recomp_context* ctx);
extern "C" void btga_sample_headless_dedicated_input_for_tick();

extern "C" {
#include <btga/btga_gameplay.h>
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
btga::TickInputHistory local_input_history;
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
std::uint32_t observed_authority_head = 0U;
std::uint32_t installed_snapshot_id = 0U;
std::uint8_t authority_recovery_mask = 0U;
bool recovery_wait_announced = false;
bool fault_diagnostics = false;
btga::ClientResyncPolicy resync_policy;
bool client_quarantined = false;
bool test_desync_enabled = false;
bool test_desync_injected = false;
std::uint32_t test_desync_tick = 0U;
std::uint64_t observed_snapshot_chunks = 0U;
std::chrono::steady_clock::time_point last_authority_contact{};
std::chrono::steady_clock::time_point next_reconnect_attempt{};
btga::RelayEndpoint client_relay;
std::string reconnect_lobby_id;
std::filesystem::path reconnect_resume_path;
bool reconnect_disabled = false;

std::uint64_t state_digest(std::span<const std::uint8_t> state) {
    std::uint64_t digest = UINT64_C(1469598103934665603);
    for (const std::uint8_t byte : state) {
        digest ^= byte;
        digest *= UINT64_C(1099511628211);
    }
    return digest;
}

bool poll_frames_locked() {
    if (!client_online) return false;
    if (client_online->state() == btga::OnlineState::RelayBinding) {
        if (client_online->poll_relay_binding()) {
            last_authority_contact = std::chrono::steady_clock::now();
            next_keepalive = {};
            std::fputs("BTGA_CLIENT relay binding restored\n", stderr);
            std::fflush(stderr);
        }
        return false;
    }
    if (client_online->state() != btga::OnlineState::Lobby) return false;
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
        last_authority_contact = std::chrono::steady_clock::now();
        if (header.message_type == BTGA_NET_MSG_PING && payload.empty()) {
            observed_authority_head = header.acknowledgment;
            authority_recovery_mask = static_cast<std::uint8_t>(
                header.acknowledgment_bits & 0x0fU);
            if (authority_recovery_mask != 0U && !recovery_wait_announced) {
                std::fprintf(stderr,
                             "BTGA_CLIENT synchronized recovery pause mask=%02x head=%u\n",
                             authority_recovery_mask, observed_authority_head);
                std::fflush(stderr);
                recovery_wait_announced = true;
            } else if (authority_recovery_mask == 0U) {
                recovery_wait_announced = false;
            }
            continue;
        }
        if (header.message_type == BTGA_NET_MSG_SNAPSHOT) {
            const bool accepted = replica.accept_snapshot_chunk(payload);
            ++observed_snapshot_chunks;
            if (fault_diagnostics) {
                BtgaNetSnapshotChunk chunk{};
                if (btga_net_decode_snapshot_chunk(
                        &chunk, payload.data(), payload.size()) != 0 &&
                    (observed_snapshot_chunks <= 3U || chunk.offset == 0U)) {
                    std::fprintf(stderr,
                                 "BTGA_CLIENT snapshot chunk count=%llu id=%u "
                                 "offset=%u total=%u accepted=%u\n",
                                 static_cast<unsigned long long>(observed_snapshot_chunks),
                                 chunk.snapshot_id, chunk.offset, chunk.total_size,
                                 accepted ? 1U : 0U);
                }
            }
            if (accepted && replica.snapshot_ready()) {
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
        if (static_cast<std::int32_t>(
                header.acknowledgment - observed_authority_head) > 0) {
            observed_authority_head = header.acknowledgment;
        }
        if (static_cast<std::int32_t>(frame.tick - expected_tick) < 0 ||
            frame.tick - expected_tick >= history_size) continue;
        frames[frame.tick % history_size] = frame;
    }
    return true;
}

bool reconnect_relay_locked() {
    const auto now = std::chrono::steady_clock::now();
    if (reconnect_lobby_id.empty() || reconnect_resume_path.empty() ||
        reconnect_disabled || now < next_reconnect_attempt ||
        !client_online) return false;
    next_reconnect_attempt = now + std::chrono::seconds{5};
    try {
        auto http = btga::make_platform_http_client(btga::master_service_url());
        btga::MasterClient master{*http};
        const std::string resume =
            btga::load_dedicated_resume_token(reconnect_resume_path);
        if (resume.empty()) {
            std::fputs("BTGA_CLIENT reconnect credential is missing\n", stderr);
            return false;
        }
        const btga::JoinRegistration registration =
            master.join(reconnect_lobby_id, resume);
        if (registration.session_id != client_session ||
            registration.seat != local_seat ||
            registration.relay.address != client_relay.address ||
            registration.relay.port != client_relay.port) {
            std::fputs(
                "BTGA_CLIENT reconnect returned a different session, seat, or relay\n",
                stderr);
            reconnect_disabled = true;
            client_quarantined = true;
            client_enabled = false;
            active_inputs.fill({});
            ultramodern::quit();
            return false;
        }
        // Resume credentials rotate on every successful master response. Do
        // not replace the last known-good credential until the response has
        // proven that it belongs to this exact replica. A mismatched or
        // corrupted response must fail closed without destroying the only
        // credential with which the user can retry recovery.
        btga::save_dedicated_resume_token(
            reconnect_resume_path, registration.resume_token);
        const auto token = std::span{
            reinterpret_cast<const std::uint8_t*>(registration.join_token.data()),
            registration.join_token.size()};
        if (!client_online->rebind_relay(false, client_session, token)) {
            std::fputs("BTGA_CLIENT could not restart relay binding\n", stderr);
            return false;
        }
        last_authority_contact = now;
        std::fputs("BTGA_CLIENT reclaiming relay seat after authority timeout\n", stderr);
        std::fflush(stderr);
        return true;
    } catch (const btga::MasterServiceError& exception) {
        if (exception.status() == 401 || exception.status() == 403 ||
            exception.status() == 404 || exception.status() == 409) {
            reconnect_disabled = true;
            client_quarantined = true;
            client_enabled = false;
            active_inputs.fill({});
            ultramodern::quit();
        }
        std::fprintf(stderr, "BTGA_CLIENT reconnect failed: %s\n", exception.what());
        return false;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "BTGA_CLIENT reconnect failed: %s\n", exception.what());
        return false;
    }
}

bool send_sampled_inputs_locked(std::uint32_t tick,
                                btga::OnlineControllerInput input) {
    if (!local_input_history.record(tick, input)) return true;
    const auto batch = local_input_history.batch_ending_at(tick);
    if (batch.count == 0U) return false;
    std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
    const std::size_t size = input_encoder.encode_batch(
        packet, client_session, client_online->relay_slot(), ++input_sequence,
        batch.first_tick, local_seat,
        std::span{batch.inputs}.first(batch.count));
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
    header.acknowledgment_bits = bootstrap_complete ? installed_snapshot_id : 0U;
    header.payload_size = 0U;
    header.peer_id = client_online->relay_slot();
    return btga_net_encode_header(packet.data(), packet.size(), &header) != 0U &&
        client_online->send_gameplay(0U, packet);
}

void send_keepalive_if_due_locked() {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_keepalive) return;
    (void)send_snapshot_subscription_locked();
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
    reconnect_lobby_id.clear();
    reconnect_resume_path.clear();
    reconnect_disabled = false;
    const auto fault_profile = btga::udp_test_fault_profile_from_environment();
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
    client_udp->set_fault_profile(fault_profile);
    fault_diagnostics = fault_profile.drop_outgoing_every != 0U ||
        fault_profile.drop_incoming_every != 0U ||
        fault_profile.delay_milliseconds != 0U;
    test_desync_enabled = false;
    test_desync_injected = false;
    test_desync_tick = 0U;
    if (const char* enabled = std::getenv("BTGA_NET_TEST_FAULTS");
        enabled != nullptr && std::string_view{enabled} == "1") {
        if (const char* configured = std::getenv("BTGA_NET_TEST_DESYNC_TICK");
            configured != nullptr && *configured != '\0') {
            const std::string_view text{configured};
            const auto [end, parse_error] = std::from_chars(
                text.data(), text.data() + text.size(), test_desync_tick);
            test_desync_enabled = parse_error == std::errc{} &&
                end == text.data() + text.size();
            if (!test_desync_enabled) {
                error = "BTGA_NET_TEST_DESYNC_TICK is invalid";
                return false;
            }
        }
    }
    observed_snapshot_chunks = 0U;
    if (fault_profile.drop_outgoing_every != 0U ||
        fault_profile.drop_incoming_every != 0U ||
        fault_profile.delay_milliseconds != 0U) {
        std::fprintf(stderr,
                     "BTGA_CLIENT test UDP loss outgoing=%u incoming=%u delay_ms=%u\n",
                     fault_profile.drop_outgoing_every,
                     fault_profile.drop_incoming_every,
                     fault_profile.delay_milliseconds);
    }
    client_session = session_id;
    client_relay = relay;
    local_seat = static_cast<std::uint8_t>(client_online->relay_slot() - 1U);
    expected_tick = 0U;
    input_sequence = 0U;
    local_input_history.reset();
    frames.fill(std::nullopt);
    pending_snapshot.reset();
    tick_active = false;
    client_enabled = true;
    bootstrap_complete = false;
    divergence_reported = false;
    replay_announced = false;
    current_random_seed = 0U;
    expected_random_seed = 0U;
    observed_authority_head = 0U;
    installed_snapshot_id = 0U;
    authority_recovery_mask = 0U;
    recovery_wait_announced = false;
    resync_policy.reset();
    client_quarantined = false;
    last_authority_contact = std::chrono::steady_clock::now();
    next_keepalive = std::chrono::steady_clock::now();
    next_reconnect_attempt = {};
    if (!send_snapshot_subscription_locked()) {
        error = "could not announce dedicated-client readiness";
        client_enabled = false;
        return false;
    }
    std::fprintf(stderr, "BTGA_CLIENT bound authoritative seat=%u\n",
                 static_cast<unsigned>(local_seat));
    return true;
}

void btga_dedicated_client_configure_reconnect(
    std::string lobby_id, std::filesystem::path resume_path) {
    const std::lock_guard lock{client_mutex};
    reconnect_lobby_id = std::move(lobby_id);
    reconnect_resume_path = std::move(resume_path);
    reconnect_disabled = false;
}

extern "C" int btga_dedicated_client_enabled() {
    const std::lock_guard lock{client_mutex};
    return client_enabled ? 1 : 0;
}

extern "C" int btga_dedicated_client_loop_ready(std::uint8_t* rdram) {
    const std::lock_guard lock{client_mutex};
    return client_enabled &&
        btga::gameplay_frame_input_active({rdram, rdram_size}) ? 1 : 0;
}

extern "C" void btga_dedicated_client_tick_begin(
    std::uint8_t* rdram, recomp_context*) {
    std::unique_lock lock{client_mutex};
    if (!client_enabled || tick_active) return;
    // Returning from this hook would let the translated game execute a local
    // tick with no canonical inputs. A quarantined replica instead stops at
    // the tick boundary and sends no traffic; the authority neutralizes and
    // disconnects its seat on the normal liveness deadlines while the user can
    // safely close/restart this client.
    if (client_quarantined) {
        lock.unlock();
        ultramodern::quit();
        return;
    }
    bool warned = false;
    for (;;) {
        (void)poll_frames_locked();
        send_keepalive_if_due_locked();
        // A checkpoint contains canonical gameplay state, not the receiving
        // process's scheduler, renderer, audio, SI, or other host-local
        // structures. Let the translated game finish its deterministic level
        // bootstrap before installing a cold checkpoint. Restoring into the
        // zero-initialized pre-level process leaves retail intrusive lists and
        // interrupt-owned state uninitialized (for example, a 0 -> 0 render
        // list), so the first gameplay tick can never complete.
        if (!bootstrap_complete &&
            !btga::gameplay_frame_input_active({rdram, rdram_size})) {
            return;
        }
        if (pending_snapshot && bootstrap_complete &&
            pending_snapshot->tick < expected_tick) {
            pending_snapshot.reset();
        }
        if (pending_snapshot &&
            (!bootstrap_complete || pending_snapshot->tick >= expected_tick) &&
            (pending_snapshot->state.size() == btga::gameplay_snapshot_size ||
             btga::is_dedicated_checkpoint(pending_snapshot->state))) {
            const bool extended =
                btga::is_dedicated_checkpoint(pending_snapshot->state);
            std::vector<std::uint8_t> current(
                extended ? 0U : pending_snapshot->state.size());
            const bool captured = extended
                ? btga::capture_dedicated_checkpoint({rdram, rdram_size}, current)
                : btga::capture_gameplay_snapshot({rdram, rdram_size}, current);
            const std::uint64_t current_digest = captured ? state_digest(current) : 0U;
            const bool already_current =
                captured && current_digest == pending_snapshot->state_digest;
            // A cold process may restore a checkpoint from any canonical tick.
            // Host-local audio/timing state is deliberately excluded, and the
            // complete inner/outer retail mode state is included. Applying a
            // checkpoint to an already-running process remains verification
            // only because its host scheduler topology may differ.
            const bool may_restore = !bootstrap_complete;
            bool restored = false;
            if (may_restore && !already_current) {
                restored = extended
                    ? btga::restore_dedicated_checkpoint(
                          {rdram, rdram_size}, pending_snapshot->state)
                    : btga::restore_gameplay_snapshot(
                          {rdram, rdram_size}, pending_snapshot->state);
                if (restored) {
                    std::vector<std::uint8_t> verify(
                        extended ? 0U : pending_snapshot->state.size());
                    restored = (extended
                        ? btga::capture_dedicated_checkpoint(
                              {rdram, rdram_size}, verify)
                        : btga::capture_gameplay_snapshot(
                              {rdram, rdram_size}, verify)) &&
                        state_digest(verify) == pending_snapshot->state_digest;
                }
            }
            if (already_current || restored) {
                if (fault_diagnostics) {
                    std::fprintf(stderr,
                                 "BTGA_CLIENT snapshot install tick=%u current=%d "
                                 "restored=%d current_digest=%016llx snapshot_digest=%016llx\n",
                                 pending_snapshot->tick, already_current ? 1 : 0,
                                 restored ? 1 : 0,
                                 static_cast<unsigned long long>(current_digest),
                                 static_cast<unsigned long long>(
                                     pending_snapshot->state_digest));
                }
                expected_tick = pending_snapshot->tick;
                // Periodic checkpoints are captured at the end of tick N-1
                // and name N as their next tick. The authority advances the
                // two retail outer-loop counters before executing N, so a
                // replacement must do the same exactly once after restore.
                // Tick-zero's initial checkpoint is captured after that
                // advance already happened and must not be advanced again.
                if (restored && expected_tick != 0U) {
                    btga_advance_retail_mode_counters(rdram);
                }
                installed_snapshot_id = pending_snapshot->snapshot_id;
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
        // Canonical input frames are not a substitute for the checkpoint that
        // defines their starting state. Under loss, event frames can arrive
        // before all snapshot chunks; consuming them advanced the cold client
        // from an unrelated local state and made its later acknowledgment stop
        // bootstrap retransmission. Remain paused and keep advertising ack=0
        // until a complete snapshot generation has been installed.
        if (!bootstrap_complete) {
            const bool timed_out =
                std::chrono::steady_clock::now() - last_authority_contact >=
                std::chrono::seconds{10};
            if (!warned && timed_out) {
                std::fprintf(stderr,
                             "BTGA_CLIENT authority timeout awaiting bootstrap\n");
                std::fflush(stderr);
                warned = true;
            }
            if (timed_out) (void)reconnect_relay_locked();
            if (client_quarantined) return;
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            lock.lock();
            continue;
        }
        // After bootstrap, the authority's canonical-frame stream owns each
        // gameplay tick. A replica that is behind must consume its available
        // frame even if its local gameplay predicate drifted false; otherwise
        // it can never repair that state. When it is fully caught up and both
        // sides have stopped producing gameplay frames, however, the retail
        // outer loop must remain free to advance match-transition states.
        // Cold bootstrap remains guarded above because its host-local
        // scheduler state is not present in the checkpoint.
        if (!btga::gameplay_frame_input_active({rdram, rdram_size}) &&
            static_cast<std::int32_t>(
                expected_tick - observed_authority_head) >= 0) {
            return;
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
        if (!warned && std::chrono::steady_clock::now() - last_authority_contact >=
                std::chrono::seconds{10}) {
            std::fprintf(stderr, "BTGA_CLIENT authority timeout at tick=%u\n", expected_tick);
            std::fflush(stderr);
            warned = true; // remain paused; never advance a divergent local tick
        }
        if (std::chrono::steady_clock::now() - last_authority_contact >=
            std::chrono::seconds{10}) {
            (void)reconnect_relay_locked();
        }
        if (client_quarantined) return;
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        lock.lock();
    }
}

extern "C" void btga_dedicated_client_run_loop(
    std::uint8_t* rdram, recomp_context* context) {
    thread_local bool running = false;
    if (running) return;
    running = true;
    constexpr auto tick_period = std::chrono::nanoseconds{1'000'000'000 / 30};
    auto deadline = std::chrono::steady_clock::now();
    while (btga_dedicated_client_enabled() != 0) {
        btga_sample_headless_dedicated_input_for_tick();
        // Mirror the authority's one-call-per-canonical-tick orchestration.
        // Offline play never enters this loop and retains the retail mode loop.
        btga_advance_retail_mode_counters(rdram);
        func_80079FF0(rdram, context);
        btga_dedicated_test_force_match_end(rdram, btga_dedicated_client_next_tick());
        func_8009C524(rdram, context);
        btga_dedicated_advance_results(rdram, context);
        bool catching_up = false;
        {
            const std::lock_guard lock{client_mutex};
            catching_up = client_enabled &&
                static_cast<std::int32_t>(
                    observed_authority_head - expected_tick) > 0;
        }
        if (catching_up) {
            // Replay uses the same complete translated outer-loop boundary and
            // validates every resulting digest. Remove presentation pacing
            // only while canonical debt exists; a fixed 30 Hz consumer can
            // never catch a 30 Hz producer after even one delayed frame.
            deadline = std::chrono::steady_clock::now();
            continue;
        }
        deadline += tick_period;
        std::this_thread::sleep_until(deadline);
        const auto now = std::chrono::steady_clock::now();
        if (now > deadline + tick_period * 4) deadline = now;
    }
    running = false;
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
    if (test_desync_enabled && !test_desync_injected &&
        expected_tick == test_desync_tick) {
        // Test-only, explicitly gated fault: corrupt real canonical state so
        // the acceptance matrix proves that checkpoint installation repairs
        // the replica rather than merely clearing a synthetic error flag.
        (void)btga::restore_gameplay_random_seed(
            {rdram, rdram_size}, expected_random_seed ^ UINT32_C(1));
        test_desync_injected = true;
        std::fprintf(stderr, "BTGA_CLIENT injected test desync tick=%u\n",
                     expected_tick);
    }
    if (btga::capture_gameplay_snapshot({rdram, rdram_size}, canonical_state)) {
        const std::uint64_t digest = state_digest(canonical_state);
        if (digest != expected_digest) {
            const auto parts = btga::gameplay_state_digests(
                {rdram, rdram_size});
            std::fprintf(stderr,
                         "BTGA_CLIENT DESYNC tick=%u authority=%016llx local=%016llx "
                         "parts=%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x\n",
                         expected_tick,
                         static_cast<unsigned long long>(expected_digest),
                         static_cast<unsigned long long>(digest),
                         parts.tasks, parts.objects, parts.player_headers,
                         parts.player_gameplay, parts.team_info,
                         parts.random_seed, parts.frame_counter,
                         parts.game_mode, parts.match_time);
            std::fflush(stderr);
            divergence_reported = true;
            const auto decision = resync_policy.mismatch(
                btga::ClientResyncPolicy::Clock::now());
            if (decision == btga::ClientResyncDecision::quarantine) {
                // Do not repeatedly freeze the shared authority when this
                // executable cannot reproduce canonical state. Keep the
                // process attached for diagnostics but stop simulation and
                // traffic until the user reconnects with a compatible build.
                client_quarantined = true;
                client_enabled = false;
                tick_active = false;
                active_inputs.fill({});
                std::fputs(
                    "BTGA_CLIENT recovery quarantined after three failed "
                    "resynchronizations in five minutes\n", stderr);
                std::fflush(stderr);
                ultramodern::quit();
                return;
            }

            // Re-enter the exact cold-bootstrap protocol. ack=0 asks the
            // authority for its latest persisted checkpoint; the client then
            // installs it at this tick boundary and consumes the canonical
            // journal before simulation resumes. Clearing buffered frames is
            // essential: they belong to the state lineage just rejected.
            bootstrap_complete = false;
            installed_snapshot_id = 0U;
            pending_snapshot.reset();
            replica = {};
            frames.fill(std::nullopt);
            active_inputs.fill({});
            expected_digest = 0U;
            expected_random_seed = 0U;
            authority_recovery_mask = 0U;
            recovery_wait_announced = false;
            tick_active = false;
            next_keepalive = {};
            (void)send_snapshot_subscription_locked();
            std::fputs("BTGA_CLIENT requested canonical resynchronization\n", stderr);
            std::fflush(stderr);
            return;
        }
        if (expected_tick % 30U == 0U) {
            std::fprintf(stderr, "BTGA_CLIENT tick=%u digest=%016llx\n",
                         expected_tick,
                         static_cast<unsigned long long>(digest));
            std::fflush(stderr);
        }
    }
    current_random_seed = expected_random_seed;
    ++expected_tick;
    if (replay_announced && expected_tick >= observed_authority_head) {
        std::fprintf(stderr,
                     "BTGA_CLIENT journal replay complete next=%u authority=%u\n",
                     expected_tick, observed_authority_head);
        std::fflush(stderr);
        replay_announced = false;
    }
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

std::uint32_t btga_dedicated_client_next_tick() {
    const std::lock_guard lock{client_mutex};
    return expected_tick;
}

void btga_dedicated_client_pump() {
    const std::lock_guard lock{client_mutex};
    if (!client_enabled || client_quarantined || !client_online) return;
    (void)poll_frames_locked();
    send_keepalive_if_due_locked();
}

void btga_dedicated_client_submit_local(
    std::uint16_t buttons, float x, float y) {
    const std::lock_guard lock{client_mutex};
    if (!client_enabled || client_quarantined || !client_online) return;
    const auto axis = [](float value) {
        return static_cast<std::int8_t>(std::clamp(std::lround(value * 127.0F),
                                                   -127L, 127L));
    };
    const std::uint32_t target_tick = btga::choose_authority_input_tick(
        expected_tick, observed_authority_head);
    (void)send_sampled_inputs_locked(
        target_tick, {buttons, axis(x), axis(y)});
}
