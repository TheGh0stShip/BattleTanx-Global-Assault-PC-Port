#include "btga/authoritative_replica.hpp"
#include "btga/input_sync.hpp"
#include "btga/master_client.hpp"
#include "btga/online_multiplayer.hpp"
#include "btga/udp_online_io.hpp"

extern "C" {
#include <btga/btga_net.h>
}

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
}

template <typename T>
T number(std::string_view text, const char* name, T minimum, T maximum) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() ||
        value < minimum || value > maximum) {
        throw std::runtime_error(std::string{name} + " is invalid");
    }
    return value;
}

bool bind_relay(btga::OnlineMultiplayer& online, std::uint64_t session,
                std::string_view token) {
    const auto bytes = std::span{
        reinterpret_cast<const std::uint8_t*>(token.data()), token.size()};
    if (!online.enter_online() || !online.begin_relay(false, session, bytes)) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        if (online.poll_relay_binding()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const bool public_join = argc == 5 && std::string_view{argv[1]} == "--join";
        const bool recovery_join = argc == 4 &&
            std::string_view{argv[3]} == "--resume";
        if ((argc != 3 && !recovery_join) && !public_join) {
            std::cerr << "usage: btga_dedicated_client_bot SEAT SECONDS [--resume]\n"
                         "   or: btga_dedicated_client_bot --join LOBBY SEAT SECONDS\n";
            return 2;
        }
        auto seat = number<unsigned int>(public_join ? argv[3] : argv[1],
                                         "seat", 0U, 3U);
        const auto seconds = number<unsigned int>(public_join ? argv[4] : argv[2],
                                                  "seconds", 1U, 300U);
        std::uint64_t session = 0;
        unsigned int port = 0;
        std::string host;
        std::string token;
        std::unique_ptr<btga::HttpClient> http;
        if (public_join) {
            http = btga::make_platform_http_client(btga::master_service_url());
            btga::MasterClient master{*http};
            const auto registration = master.join(argv[2]);
            session = registration.session_id;
            host = registration.relay.address;
            port = registration.relay.port;
            token = registration.join_token;
        } else {
            session = number<std::uint64_t>(
                environment("BTGA_DEDICATED_SESSION"), "BTGA_DEDICATED_SESSION", 1U,
                UINT64_MAX);
            port = number<unsigned int>(
                environment("BTGA_DEDICATED_RELAY_PORT"), "BTGA_DEDICATED_RELAY_PORT",
                1U, 65535U);
            host = environment("BTGA_DEDICATED_RELAY_HOST");
            token = environment("BTGA_DEDICATED_TOKEN");
        }
        if (host.empty() || token.empty()) throw std::runtime_error("relay settings missing");

        const auto fault_profile = btga::udp_test_fault_profile_from_environment();
        btga::UdpOnlineIo io{host, static_cast<std::uint16_t>(port)};
        btga::OnlineMultiplayer online{io};
        if (!bind_relay(online, session, token)) {
            throw std::runtime_error("relay bind timed out");
        }
        io.set_fault_profile(fault_profile);
        if (public_join) {
            seat = online.relay_slot() - 1U;
        } else if (online.relay_slot() != seat + 1U) {
            throw std::runtime_error("master-assigned relay seat does not match requested seat");
        }
        std::cout << "BOUND_SEAT=" << seat << '\n' << std::flush;

        btga::InputSynchronizer encoder;
        btga::TickInputHistory sampled_inputs;
        btga::AuthoritativeReplica replica;
        std::optional<btga::CanonicalFrame> latest_frame;
        std::map<std::uint32_t, btga::CanonicalFrame> pending_frames;
        std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
        const btga::OnlineControllerInput distinctive{
            static_cast<std::uint16_t>(UINT16_C(0x0100) << seat),
            static_cast<std::int8_t>(20 + static_cast<int>(seat) * 17),
            static_cast<std::int8_t>(-20 - static_cast<int>(seat) * 13)};
        std::uint32_t sequence = 0;
        std::uint32_t next_input_tick = 0;
        std::uint32_t accepted_frames = 0;
        std::uint32_t echoed_frames = 0;
        std::uint32_t installed_snapshot_id = 0;
        bool bootstrapped = !recovery_join;
        bool recovery_ready_announced = false;
        const auto send_inputs = [&](std::uint32_t sampled_tick) {
            // This bot has a stable synthetic controller, but it still makes
            // exactly one decision for the tick being sampled. Older actual
            // decisions are carried only as loss redundancy; no future tick
            // is pre-committed on behalf of a controller that has not sampled
            // it yet.
            if (!sampled_inputs.record(sampled_tick, distinctive)) {
                throw std::runtime_error("conflicting local input sample");
            }
            const auto batch = sampled_inputs.batch_ending_at(sampled_tick);
            if (batch.count == 0U) {
                throw std::runtime_error("local input history disappeared");
            }
            const std::size_t encoded = encoder.encode_batch(
                packet, session, online.relay_slot(), ++sequence,
                batch.first_tick, static_cast<std::uint8_t>(seat),
                std::span{batch.inputs}.first(batch.count));
            if (encoded == 0U || !online.send_gameplay(0U, {packet.data(), encoded})) {
                throw std::runtime_error("input transmission failed");
            }
        };
        const auto send_subscription = [&] {
            std::array<std::uint8_t, BTGA_NET_HEADER_SIZE> message{};
            BtgaNetHeader header{};
            header.protocol_version = BTGA_NET_PROTOCOL_VERSION;
            header.message_type = BTGA_NET_MSG_PING;
            header.flags = BTGA_NET_FLAG_RELAYED;
            header.session_id = session;
            header.sequence = ++sequence;
            header.acknowledgment = bootstrapped ? next_input_tick + 1U : 0U;
            header.acknowledgment_bits = bootstrapped ? installed_snapshot_id : 0U;
            header.payload_size = 0U;
            header.peer_id = online.relay_slot();
            if (!btga_net_encode_header(message.data(), message.size(), &header) ||
                !online.send_gameplay(0U, message)) {
                throw std::runtime_error("snapshot subscription failed");
            }
        };
        if (recovery_join) send_subscription();
        send_inputs(0U);
        auto next_readiness = std::chrono::steady_clock::now() +
            std::chrono::milliseconds{250};
        auto next_keepalive = next_readiness;
        // A full checkpoint is about 124 KiB and is deliberately transferred
        // through the same lossy datagram path as gameplay. Leave enough time
        // for chunk retries before declaring a resumed process dead.
        const auto startup_deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds{45};
        auto run_deadline = startup_deadline;
        bool session_started = false;

        while (std::chrono::steady_clock::now() < run_deadline) {
            const auto now = std::chrono::steady_clock::now();
            if (!session_started && now >= next_readiness) {
                if (recovery_join) send_subscription();
                send_inputs(0U);
                next_readiness = now + std::chrono::milliseconds{250};
            } else if (session_started && now >= next_keepalive) {
                // Keep the relay binding and authority-side seat alive while
                // a replacement authority restores and replays its journal.
                // Re-sending the next uncommitted tick is idempotent.
                if (recovery_join) send_subscription();
                send_inputs(next_input_tick);
                next_keepalive = now + std::chrono::milliseconds{250};
            }
            bool received_new_frame = false;
            std::uint16_t source = 0;
            const std::size_t size = online.receive_gameplay(packet, source);
            if (size != 0U) {
                BtgaNetHeader header{};
                if (source != 0U ||
                    !btga_net_decode_header(&header, packet.data(), size) ||
                    header.session_id != session || header.peer_id != 0U ||
                    header.payload_size != size - BTGA_NET_HEADER_SIZE) {
                    throw std::runtime_error("invalid authoritative packet");
                }
                if (header.message_type == BTGA_NET_MSG_SNAPSHOT) {
                    const bool accepted = replica.accept_snapshot_chunk(
                        {packet.data() + BTGA_NET_HEADER_SIZE,
                         header.payload_size});
                    // Completed generations are deliberately transmitted more
                    // than once under loss. Once take_snapshot() retires that
                    // generation, its remaining duplicate chunks are stale
                    // rather than fatal.
                    if (accepted && replica.snapshot_ready()) {
                        auto snapshot = replica.take_snapshot();
                        if (!snapshot) {
                            throw std::runtime_error("completed snapshot disappeared");
                        }
                        installed_snapshot_id = snapshot->snapshot_id;
                        next_input_tick = snapshot->tick;
                        bootstrapped = true;
                        std::cout << "BOOTSTRAP_TICK=" << snapshot->tick << '\n'
                                  << std::flush;
                        send_subscription();
                    }
                    continue;
                }
                if (header.message_type == BTGA_NET_MSG_PING &&
                    header.payload_size == 0U) {
                    continue;
                }
                btga::CanonicalFrame frame{};
                if (header.message_type != BTGA_NET_MSG_EVENT) {
                    throw std::runtime_error(
                        "unexpected authoritative message type " +
                        std::to_string(header.message_type));
                }
                if (!bootstrapped) continue;
                if (!btga::decode_authoritative_frame(
                        {packet.data() + BTGA_NET_HEADER_SIZE,
                         header.payload_size}, frame)) {
                    throw std::runtime_error(
                        "invalid authoritative frame payload size " +
                        std::to_string(header.payload_size));
                }
                if (static_cast<std::int32_t>(frame.tick - next_input_tick) < 0) {
                    if (latest_frame && frame.tick == latest_frame->tick &&
                        frame.state_digest != latest_frame->state_digest) {
                        throw std::runtime_error("conflicting authoritative frame");
                    }
                    continue;
                }
                const auto [pending, inserted] = pending_frames.emplace(
                    frame.tick, frame);
                if (!inserted &&
                    pending->second.state_digest != frame.state_digest) {
                    throw std::runtime_error("conflicting pending frame");
                }
                while (true) {
                    const auto contiguous = pending_frames.find(next_input_tick);
                    if (contiguous == pending_frames.end()) break;
                    latest_frame = contiguous->second;
                    pending_frames.erase(contiguous);
                    ++accepted_frames;
                    if (!session_started) {
                        session_started = true;
                        next_keepalive = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds{250};
                        run_deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds{seconds};
                    }
                    received_new_frame = true;
                    if ((latest_frame->received_mask & (1U << seat)) != 0U &&
                        latest_frame->inputs[seat] == distinctive) {
                        ++echoed_frames;
                    }
                    ++next_input_tick;
                    if (recovery_join && !recovery_ready_announced &&
                        accepted_frames >= 30U && echoed_frames >= 1U) {
                        std::cout << "RECOVERY_READY tick=" << next_input_tick
                                  << '\n' << std::flush;
                        recovery_ready_announced = true;
                    }
                }
            }

            if (received_new_frame && next_input_tick != 0U) {
                send_inputs(next_input_tick);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }

        if (accepted_frames < 30U || echoed_frames < 10U) {
            throw std::runtime_error("insufficient canonical frames or echoed inputs");
        }
        std::cout << "seat=" << seat << " canonical_frames=" << accepted_frames
                  << " echoed_inputs=" << echoed_frames << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dedicated client bot: " << error.what() << '\n';
        return 1;
    }
}
