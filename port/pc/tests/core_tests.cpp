#include "btga/game.hpp"
#include "btga/authority_lease.hpp"
#include "btga/gameplay_digest.hpp"
#include "btga/authoritative_session.hpp"
#include "btga/authoritative_server.hpp"
#include "btga/authoritative_replica.hpp"
#include "btga/translated_authority.hpp"
#include "btga/gameplay_snapshot.hpp"
#include "btga/input_sync.hpp"
#include "btga/controller_pak.hpp"
#include "btga/pc_settings.hpp"
#include "btga/recovery_barrier.hpp"
#include "btga/online_multiplayer.hpp"
#include "btga/online_local_view.hpp"
#include "btga/master_client.hpp"
#include "btga/sha256.hpp"
#include "btga/snapshot_transfer.hpp"
#include "btga/texture_packs.hpp"
#include "btga/udp_online_io.hpp"

extern "C" {
#include <btga/btga_gameplay.h>
#include <btga/btga_net.h>
}

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <algorithm>

#define CHECK(expression) \
    do { \
        if (!(expression)) { \
            std::cerr << "check failed at " << __FILE__ << ':' << __LINE__ << ": " #expression "\n"; \
            return 1; \
        } \
    } while (false)

int main() {
    {
        btga::RecoveryBarrier barrier;
        CHECK(!barrier.paused());
        barrier.begin(0U, 0U);
        CHECK(!barrier.paused());

        barrier.begin(0U, 120U);
        barrier.begin(2U, 120U);
        CHECK(barrier.paused());
        CHECK(barrier.mask() == 0x05U);
        barrier.acknowledge(0U, 119U, 120U);
        CHECK(barrier.recovering(0U));
        barrier.acknowledge(0U, 120U, 120U);
        CHECK(!barrier.recovering(0U));
        CHECK(barrier.recovering(2U));

        // The moving head cannot be cleared by an acknowledgment that only
        // reached an earlier observation of it.
        barrier.acknowledge(2U, 120U, 121U);
        CHECK(barrier.paused());
        barrier.acknowledge(2U, 121U, 121U);
        CHECK(!barrier.paused());

        barrier.begin(3U, 500U);
        barrier.disconnect(3U);
        CHECK(!barrier.paused());
        barrier.begin(1U, 500U);
        barrier.reset();
        CHECK(barrier.mask() == 0U);

        // Invalid seats are ignored rather than shifting outside the mask.
        barrier.begin(4U, 500U);
        barrier.acknowledge(4U, 500U, 500U);
        barrier.disconnect(4U);
        CHECK(!barrier.paused());
    }
    {
        const auto path = std::filesystem::temp_directory_path() /
            "btga-authority-lease-test.json";
        std::filesystem::remove(path);
        const btga::PersistentAuthorityLease expected{
            "lobby-id", "host-secret", UINT64_C(0x1020304050607080),
            {"relay.example", 30420U}, UINT32_C(0x12345678)};
        CHECK(btga::save_authority_lease(path, expected));
        const auto loaded = btga::load_authority_lease(path);
        CHECK(loaded && loaded->lobby_id == expected.lobby_id);
        CHECK(loaded->host_secret == expected.host_secret);
        CHECK(loaded->session_id == expected.session_id);
        CHECK(loaded->relay.address == expected.relay.address);
        CHECK(loaded->relay.port == expected.relay.port);
        CHECK(loaded->packet_sequence == expected.packet_sequence);
        {
            std::ofstream corrupt{path, std::ios::binary | std::ios::trunc};
            corrupt << "{not-json";
        }
        CHECK(!btga::load_authority_lease(path));
        std::filesystem::remove(path);
    }
    const auto translated_journal_path =
        std::filesystem::temp_directory_path() / "btga-translated-authority-test.journal";
    std::filesystem::remove(translated_journal_path);
    {
        // This is the execution model used by the translated dedicated
        // runtime: the guest tick owns the call stack, while the authority
        // freezes inputs at entry and captures canonical RDRAM at exit.
        btga::TranslatedAuthority authority{0x0fU, 2U, 3U};
        CHECK(authority.enable_disk_journal(translated_journal_path, 2U));
        CHECK(!authority.enable_disk_journal(translated_journal_path, 2U));
        std::vector<std::uint8_t> rdram(8U * 1024U * 1024U, 0U);
        for (std::uint8_t seat = 0; seat < 4; ++seat) {
            BtgaNetInputBatch batch{};
            batch.first_tick = 0;
            batch.player_id = seat;
            batch.frame_count = 1;
            batch.frames[0] = {static_cast<std::uint16_t>(seat + 1U),
                               static_cast<std::int8_t>(seat), 0};
            std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
            const std::size_t payload_size = btga_net_encode_input_batch(
                packet.data() + BTGA_NET_HEADER_SIZE,
                packet.size() - BTGA_NET_HEADER_SIZE, &batch);
            BtgaNetHeader header{};
            header.protocol_version = BTGA_NET_PROTOCOL_VERSION;
            header.message_type = BTGA_NET_MSG_INPUT;
            header.flags = BTGA_NET_FLAG_RELAYED;
            header.session_id = UINT64_C(0x123456789abcdef0);
            header.sequence = seat + 1U;
            header.payload_size = static_cast<std::uint16_t>(payload_size);
            header.peer_id = static_cast<std::uint16_t>(seat + 1U);
            CHECK(btga_net_encode_header(packet.data(), packet.size(), &header));
            CHECK(authority.ingest_client_packet(
                {packet.data(), BTGA_NET_HEADER_SIZE + payload_size},
                header.session_id, header.peer_id));
        }
        CHECK(authority.ready(0U));
        CHECK(authority.begin_tick(0U));
        CHECK(!authority.begin_tick(0U));
        CHECK(authority.inputs()[3].buttons == 4U);
        rdram[0x1234] = 0x5aU;
        const auto frame0 = authority.finish_tick(0U, rdram);
        CHECK(frame0 && frame0->tick == 0U && frame0->received_mask == 0x0fU);
        CHECK(authority.journal_size() == 1U);
        CHECK(authority.next_tick() == 1U);
        CHECK(!authority.latest_snapshot());

        CHECK(authority.submit(0, 1U, {9U, 0, 0}));
        CHECK(!authority.ready(2U));
        CHECK(authority.ready(4U));
        CHECK(authority.begin_tick(4U));
        // Connected missing seats repeat their last accepted input only after
        // the bounded jitter deadline.
        CHECK(authority.inputs()[1].buttons == 2U);
        CHECK(authority.set_connected(2U, false));
        // Disconnects become neutral on the next tick and never repeat a
        // stuck drive or fire control.
        rdram[0x1235] = 0xa5U;
        auto portable_checkpoint = rdram;
        portable_checkpoint[0x1235] = 0x6cU;
        const auto frame1 = authority.finish_tick(
            1U, rdram, portable_checkpoint);
        CHECK(frame1 && authority.latest_snapshot());
        std::array<btga::AuthoritativeFrame, 4> replay{};
        CHECK(authority.copy_journal_from(0U, replay) == 2U);
        CHECK(replay[0].tick == 0U && replay[0].inputs[3].buttons == 4U);
        CHECK(replay[1].tick == 1U && replay[1].inputs[0].buttons == 9U);
        CHECK(authority.copy_journal_from(1U, replay) == 1U);
        CHECK(replay[0].tick == 1U);
        CHECK(authority.copy_journal_from(2U, replay) == 0U);
        CHECK(authority.latest_snapshot()->next_tick == 2U);
        CHECK(authority.latest_snapshot()->state.size() == rdram.size());
        CHECK(authority.latest_snapshot()->state[0x1235] == 0x6cU);
        CHECK(frame1->state_digest != authority.latest_snapshot()->state_digest);

        CHECK(authority.ready(5U));
        CHECK(authority.begin_tick(5U));
        CHECK(authority.inputs()[2].buttons == 0U);
        CHECK(authority.finish_tick(2U, rdram));

        // A newly authenticated endpoint resumes with a fresh packet sequence
        // epoch after timeout. The old endpoint's sequence was 3; sequence 1
        // must be accepted only after set_connected(false) reset that epoch.
        CHECK(authority.set_connected(2U, true));
        btga::InputSynchronizer resume_encoder;
        std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> resume_packet{};
        const auto resume_size = resume_encoder.encode(
            resume_packet, UINT64_C(0x123456789abcdef0), 3U, 1U, 3U, 2U,
            {0x55U, 1, -1});
        CHECK(resume_size != 0U);
        CHECK(authority.ingest_client_packet(
            {resume_packet.data(), resume_size}, UINT64_C(0x123456789abcdef0), 3U));
        CHECK(authority.ready(6U));
        CHECK(authority.begin_tick(6U));
        CHECK(authority.inputs()[2].buttons == 0x55U);
        CHECK(authority.finish_tick(3U, rdram));
        CHECK(authority.journal_size() == 4U);
        CHECK(authority.journal_base_tick() == 2U);
        CHECK(authority.copy_journal_from(0U, replay) == 4U);
        CHECK(std::filesystem::file_size(translated_journal_path) == 16U + 4U * 36U);
        CHECK(replay[0].tick == 0U && replay[0].inputs[3].buttons == 4U);
        CHECK(replay[1].tick == 1U && replay[1].inputs[0].buttons == 9U);
        CHECK(replay[2].tick == 2U && replay[2].inputs[2].buttons == 0U);
        CHECK(replay[3].tick == 3U && replay[3].inputs[2].buttons == 0x55U);
    }
    CHECK(!std::filesystem::exists(translated_journal_path));
    {
        const auto persistent_path = std::filesystem::temp_directory_path() /
            "btga-translated-authority-persistent-test.journal";
        std::filesystem::remove(persistent_path);
        std::filesystem::remove(persistent_path.string() + ".snapshot.0");
        std::filesystem::remove(persistent_path.string() + ".snapshot.1");
        {
            btga::TranslatedAuthority authority{0U, 2U, 0U};
            CHECK(authority.enable_persistent_journal(persistent_path, 2U));
            std::array<std::uint8_t, 4> state{};
            for (std::uint32_t tick = 0; tick < 4U; ++tick) {
                state[0] = static_cast<std::uint8_t>(tick + 1U);
                CHECK(authority.begin_tick(tick));
                CHECK(authority.finish_tick(tick, state));
            }
            CHECK(authority.latest_snapshot());
            CHECK(authority.latest_snapshot()->next_tick == 4U);
            CHECK(authority.persistent_storage_healthy());
        }
        CHECK(std::filesystem::exists(persistent_path));
        CHECK(std::filesystem::exists(persistent_path.string() + ".snapshot.0"));
        CHECK(std::filesystem::exists(persistent_path.string() + ".snapshot.1"));

        // A process can die after writing only part of the next record. The
        // replacement authority discards that tail, validates every complete
        // canonical record and selects the newest intact checkpoint.
        {
            std::ofstream damaged{persistent_path, std::ios::binary | std::ios::app};
            const std::array<char, 7> partial{'p', 'a', 'r', 't', 'i', 'a', 'l'};
            damaged.write(partial.data(), partial.size());
        }
        {
            btga::TranslatedAuthority recovered{0U, 2U, 0U};
            CHECK(recovered.open_persistent_journal(persistent_path, 2U));
            CHECK(recovered.next_tick() == 4U);
            CHECK(recovered.journal_base_tick() == 2U);
            CHECK(recovered.latest_snapshot());
            CHECK(recovered.latest_snapshot()->next_tick == 4U);
            CHECK(recovered.latest_snapshot()->state[0] == 4U);
            std::array<btga::AuthoritativeFrame, 4> replay{};
            CHECK(recovered.copy_journal_from(0U, replay) == replay.size());
            for (std::size_t index = 0; index < replay.size(); ++index) {
                CHECK(replay[index].tick == index);
                CHECK(replay[index].random_seed == index + 1U);
            }
            CHECK(std::filesystem::file_size(persistent_path) == 16U + 4U * 36U);

            // After the caller restores the portable checkpoint and replays
            // any newer journal frames, canonical publication can continue at
            // the recovered tick without changing authority ownership.
            std::array<std::uint8_t, 4> state{5U, 0U, 0U, 0U};
            CHECK(recovered.begin_tick(4U));
            CHECK(recovered.finish_tick(4U, state));
            CHECK(recovered.next_tick() == 5U);
        }
        CHECK(std::filesystem::exists(persistent_path));
        std::filesystem::remove(persistent_path);
        std::filesystem::remove(persistent_path.string() + ".snapshot.0");
        std::filesystem::remove(persistent_path.string() + ".snapshot.1");
    }
    {
        class FourSeatSimulation final : public btga::AuthoritativeSimulation {
        public:
            bool run_tick(
                std::uint32_t tick,
                std::span<const btga::OnlineControllerInput,
                          btga::online_player_limit> inputs) override {
                if (tick != ticks) return false;
                for (std::size_t seat = 0; seat < inputs.size(); ++seat) {
                    state[seat] += inputs[seat].buttons;
                    state[seat] += static_cast<std::int32_t>(inputs[seat].stick_x);
                    state[seat] += static_cast<std::int32_t>(inputs[seat].stick_y);
                }
                ++ticks;
                return true;
            }
            std::uint64_t state_digest() const override {
                std::uint64_t digest = ticks;
                for (const auto value : state) {
                    digest = digest * 1099511628211ULL ^
                             static_cast<std::uint32_t>(value);
                }
                return digest;
            }
            bool save_state(std::vector<std::uint8_t>& destination) const override {
                destination.resize(sizeof(ticks) + sizeof(state));
                std::memcpy(destination.data(), &ticks, sizeof(ticks));
                std::memcpy(destination.data() + sizeof(ticks), state.data(), sizeof(state));
                return true;
            }
            std::uint32_t ticks = 0;
            std::array<std::int32_t, btga::online_player_limit> state{};
        } simulation;

        btga::AuthoritativeSession session{0x0fU, 2U};
        CHECK(!session.ready());
        CHECK(session.submit(0, 0, {1, 1, 1}));
        CHECK(session.submit(1, 0, {2, 2, 2}));
        CHECK(session.submit(2, 0, {3, 3, 3}));
        CHECK(!session.ready());
        CHECK(session.submit(3, 0, {4, 4, 4}));
        CHECK(session.ready());
        const auto first = session.advance(simulation);
        CHECK(first && first->tick == 0 && first->inputs[3].buttons == 4);
        CHECK(first->received_mask == 0x0fU);
        CHECK(session.next_tick() == 1 && !session.latest_snapshot());
        CHECK(!session.submit(3, 0, {}));
        for (std::uint8_t seat = 0; seat < btga::online_player_limit; ++seat) {
            CHECK(session.submit(seat, 1, {static_cast<std::uint16_t>(seat + 5U), 0, 0}));
        }
        const auto second = session.advance(simulation);
        CHECK(second && second->tick == 1);
        CHECK(session.latest_snapshot());
        CHECK(session.latest_snapshot()->next_tick == 2);
        CHECK(session.latest_snapshot()->state_digest == second->state_digest);
        CHECK(!session.latest_snapshot()->state.empty());

        CHECK(session.submit(0, 2, {9, 1, 0}));
        CHECK(session.submit(1, 2, {10, 0, 0}));
        CHECK(session.submit(2, 2, {11, 0, 0}));
        CHECK(!session.advance_due(simulation, 4U));
        const auto timed_out = session.advance_due(simulation, 5U);
        CHECK(timed_out && timed_out->tick == 2);
        CHECK(timed_out->received_mask == 0x07U);
        CHECK(timed_out->inputs[3].buttons == 8U);
        CHECK(!session.submit(3, 2, {12, 0, 0}));
    }
    {
        class ServerSimulation final : public btga::AuthoritativeSimulation {
        public:
            bool run_tick(
                std::uint32_t tick,
                std::span<const btga::OnlineControllerInput,
                          btga::online_player_limit> inputs) override {
                if (tick != ticks) return false;
                for (std::size_t seat = 0; seat < inputs.size(); ++seat) {
                    state[seat] += inputs[seat].buttons;
                }
                ++ticks;
                return true;
            }
            std::uint64_t state_digest() const override {
                std::uint64_t result = ticks;
                for (const auto value : state) result = result * 131U + value;
                return result;
            }
            bool save_state(std::vector<std::uint8_t>& destination) const override {
                destination.resize(sizeof(ticks) + sizeof(state));
                std::memcpy(destination.data(), &ticks, sizeof(ticks));
                std::memcpy(destination.data() + sizeof(ticks), state.data(),
                            sizeof(state));
                return true;
            }
            std::uint32_t ticks = 0;
            std::array<std::uint32_t, btga::online_player_limit> state{};
        } simulation;

        btga::AuthoritativeServerCore server{0x0fU, 2U, 3U};
        btga::AuthoritativeReplica replica;
        auto submit = [&](std::uint8_t seat, std::uint32_t tick,
                          std::uint16_t buttons) {
            BtgaNetInputBatch batch{};
            std::array<std::uint8_t, BTGA_NET_MAX_PAYLOAD_SIZE> payload{};
            batch.first_tick = tick;
            batch.player_id = seat;
            batch.frame_count = 1U;
            batch.frames[0] = {buttons, 0, 0};
            const auto size = btga_net_encode_input_batch(
                payload.data(), payload.size(), &batch);
            return size != 0U && server.ingest_input_payload(
                std::span<const std::uint8_t>{payload.data(), size});
        };
        auto submit_packet = [&](std::uint8_t seat, std::uint16_t source_slot,
                                 std::uint32_t tick, std::uint32_t sequence) {
            btga::InputSynchronizer encoder;
            std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
            const auto size = encoder.encode(
                packet, UINT64_C(0x1020304050607080), source_slot, sequence, tick,
                seat, {static_cast<std::uint16_t>(seat + 20U), 0, 0});
            return size != 0U && server.ingest_client_packet(
                std::span<const std::uint8_t>{packet.data(), size},
                UINT64_C(0x1020304050607080), source_slot);
        };
        CHECK(!submit_packet(0U, 2U, 10U, 1U));
        CHECK(submit_packet(0U, 1U, 10U, 1U));
        CHECK(submit_packet(0U, 1U, 10U, 1U));
        for (std::uint8_t seat = 0; seat < btga::online_player_limit; ++seat) {
            CHECK(submit(seat, 0U, static_cast<std::uint16_t>(seat + 1U)));
        }
        const auto first = server.step(simulation, 0U);
        CHECK(first && first->snapshot_payloads.empty());
        CHECK(replica.accept_frame(first->frame_payload));
        BtgaNetAuthoritativeFrame frame{};
        CHECK(btga_net_decode_authoritative_frame(
            &frame, first->frame_payload.data(), first->frame_payload.size()));
        CHECK(frame.tick == 0U && frame.received_mask == 0x0fU);
        CHECK(frame.connected_mask == 0x0fU);

        CHECK(submit(0U, 1U, 9U));
        CHECK(submit(1U, 1U, 10U));
        CHECK(submit(2U, 1U, 11U));
        CHECK(!server.step(simulation, 3U));
        const auto second = server.step(simulation, 4U);
        CHECK(second && second->snapshot_payloads.size() == 1U);
        CHECK(replica.accept_frame(second->frame_payload));
        btga::CanonicalFrame historical{};
        CHECK(btga::decode_authoritative_frame(first->frame_payload, historical));
        CHECK(historical.tick == 0U && historical.received_mask == 0x0fU);
        // The latest-frame tracker remains monotonic, while journal consumers
        // can decode and place authenticated historical frames by tick.
        CHECK(!replica.accept_frame(first->frame_payload));
        CHECK(replica.accept_snapshot_chunk(second->snapshot_payloads[0]));
        CHECK(replica.accept_snapshot_chunk(second->snapshot_payloads[0]));
        CHECK(replica.snapshot_ready());
        CHECK(btga_net_decode_authoritative_frame(
            &frame, second->frame_payload.data(), second->frame_payload.size()));
        CHECK(frame.tick == 1U && frame.received_mask == 0x07U);
        CHECK(frame.inputs[3].buttons == 4U);
        BtgaNetSnapshotChunk chunk{};
        CHECK(btga_net_decode_snapshot_chunk(
            &chunk, second->snapshot_payloads[0].data(),
            second->snapshot_payloads[0].size()));
        CHECK(chunk.tick == 2U && chunk.offset == 0U);
        CHECK(chunk.total_size == sizeof(simulation.ticks) + sizeof(simulation.state));
        CHECK(chunk.data_size == chunk.total_size);
        const auto canonical = replica.take_snapshot();
        CHECK(canonical && canonical->tick == 2U);
        CHECK(canonical->state.size() == chunk.total_size);

        CHECK(server.set_connected(3U, false));
        CHECK(!submit(3U, 2U, 0xffffU));
        CHECK(submit(0U, 2U, 1U));
        CHECK(submit(1U, 2U, 1U));
        CHECK(submit(2U, 2U, 1U));
        const auto disconnected = server.step(simulation, 5U);
        CHECK(disconnected);
        CHECK(replica.accept_frame(disconnected->frame_payload));
        CHECK(btga_net_decode_authoritative_frame(
            &frame, disconnected->frame_payload.data(),
            disconnected->frame_payload.size()));
        CHECK(frame.inputs[3].buttons == 0U);
        CHECK(frame.connected_mask == 0x07U);
        CHECK(replica.latest_frame()->connected_mask == 0x07U);
        CHECK((frame.received_mask & 0x08U) == 0U);
        CHECK(server.set_connected(3U, true));
        CHECK(submit(3U, 3U, 12U));
        const auto resync = server.make_resync(simulation);
        CHECK(resync && !resync->empty());
        btga::AuthoritativeReplica returning_client;
        for (auto iterator = resync->rbegin(); iterator != resync->rend(); ++iterator) {
            CHECK(returning_client.accept_snapshot_chunk(*iterator));
        }
        const auto resumed = returning_client.take_snapshot();
        CHECK(resumed && resumed->tick == server.session().next_tick());
        CHECK(resumed->state_digest == simulation.state_digest());
    }
    {
        // A full bootstrap is larger than one UDP datagram. Retransmitting the
        // same snapshot generation in a different chunk order must fill holes
        // left by loss without discarding chunks already received.
        constexpr std::uint32_t snapshot_id = 77U;
        constexpr std::uint32_t snapshot_tick = 123U;
        std::vector<std::uint8_t> state(
            BTGA_NET_SNAPSHOT_CHUNK_DATA_LIMIT * 2U + 17U);
        for (std::size_t index = 0; index < state.size(); ++index) {
            state[index] = static_cast<std::uint8_t>((index * 37U + 11U) & 0xffU);
        }
        std::uint64_t digest = UINT64_C(1469598103934665603);
        for (const auto byte : state) {
            digest ^= byte;
            digest *= UINT64_C(1099511628211);
        }
        const auto encode_chunk = [&](std::uint32_t offset) {
            const auto remaining = static_cast<std::uint32_t>(state.size()) - offset;
            const auto size = static_cast<std::uint16_t>((std::min)(
                remaining,
                static_cast<std::uint32_t>(BTGA_NET_SNAPSHOT_CHUNK_DATA_LIMIT)));
            BtgaNetSnapshotChunk chunk{
                snapshot_id, snapshot_tick, digest,
                static_cast<std::uint32_t>(state.size()), offset, size,
                state.data() + offset};
            std::vector<std::uint8_t> payload(BTGA_NET_MAX_DATAGRAM_SIZE);
            const auto encoded = btga_net_encode_snapshot_chunk(
                payload.data(), payload.size(), &chunk);
            if (encoded == 0U) {
                throw std::runtime_error("snapshot test chunk encoding failed");
            }
            payload.resize(encoded);
            return payload;
        };
        const auto chunk0 = encode_chunk(0U);
        const auto chunk1 = encode_chunk(BTGA_NET_SNAPSHOT_CHUNK_DATA_LIMIT);
        const auto chunk2 = encode_chunk(BTGA_NET_SNAPSHOT_CHUNK_DATA_LIMIT * 2U);
        btga::AuthoritativeReplica replica;
        CHECK(replica.accept_snapshot_chunk(chunk0));
        CHECK(replica.accept_snapshot_chunk(chunk2));
        CHECK(!replica.snapshot_ready());
        // A retry can repeat known chunks and deliver the missing one last.
        CHECK(replica.accept_snapshot_chunk(chunk2));
        CHECK(replica.accept_snapshot_chunk(chunk1));
        CHECK(replica.snapshot_ready());
        const auto completed = replica.take_snapshot();
        CHECK(completed && completed->snapshot_id == snapshot_id);
        CHECK(completed->tick == snapshot_tick);
        CHECK(completed->state_digest == digest);
        CHECK(completed->state == state);
    }
    {
        constexpr std::size_t full_vp = 0xB7E30U;
        constexpr std::size_t p1_vp = 0xB7E40U;
        constexpr std::size_t p2_vp = 0xB7E50U;
        constexpr std::size_t p1_scissor = 0xB7F88U;
        constexpr std::size_t p2_scissor = 0xB7FA0U;
        const std::array<std::uint8_t, 16> full{
            0x02,0x80,0x01,0xE0,0x01,0xFF,0,0,
            0x02,0x80,0x01,0xE0,0x01,0xFF,0,0};
        const std::array<std::uint8_t, 16> top{
            0x02,0x80,0x00,0xF0,0x01,0xFF,0,0,
            0x02,0x80,0x00,0xF0,0x01,0xFF,0,0};
        const std::array<std::uint8_t, 16> bottom{
            0x02,0x80,0x00,0xF0,0x01,0xFF,0,0,
            0x02,0x80,0x02,0xD0,0x01,0xFF,0,0};
        const std::array<std::uint8_t, 8> top_scissor{
            0xED,0,0,0,0,0x50,0x01,0xDC};
        const std::array<std::uint8_t, 8> bottom_scissor{
            0xED,0,0x01,0xE0,0,0x50,0x03,0xC0};
        const std::array<std::uint8_t, 8> full_scissor{
            0xED,0,0,0,0,0x50,0x03,0xC0};
        const std::array<std::uint8_t, 8> hidden_scissor{
            0xED,0,0,0,0,0,0x40,0x04};
        auto make_rom = [&]() {
            std::vector<std::uint8_t> rom(0xB8000U);
            std::copy(full.begin(), full.end(), rom.begin() + full_vp);
            std::copy(top.begin(), top.end(), rom.begin() + p1_vp);
            std::copy(bottom.begin(), bottom.end(), rom.begin() + p2_vp);
            std::copy(top_scissor.begin(), top_scissor.end(), rom.begin() + p1_scissor);
            std::copy(bottom_scissor.begin(), bottom_scissor.end(), rom.begin() + p2_scissor);
            return rom;
        };
        auto host = make_rom();
        CHECK(btga::patch_online_local_view(
            host, btga::OnlineLocalView::HostPlayerOne));
        CHECK(std::equal(full.begin(), full.end(), host.begin() + p1_vp));
        CHECK(std::equal(full_scissor.begin(), full_scissor.end(),
                         host.begin() + p1_scissor));
        CHECK(std::equal(hidden_scissor.begin(), hidden_scissor.end(),
                         host.begin() + p2_scissor));

        auto client = make_rom();
        CHECK(btga::patch_online_local_view(
            client, btga::OnlineLocalView::ClientPlayerTwo));
        CHECK(std::equal(full.begin(), full.end(), client.begin() + p2_vp));
        CHECK(std::equal(hidden_scissor.begin(), hidden_scissor.end(),
                         client.begin() + p1_scissor));
        CHECK(std::equal(full_scissor.begin(), full_scissor.end(),
                         client.begin() + p2_scissor));

        auto invalid = make_rom();
        invalid[p1_vp] ^= 1U;
        const auto before = invalid;
        CHECK(!btga::patch_online_local_view(
            invalid, btga::OnlineLocalView::HostPlayerOne));
        CHECK(invalid == before);
    }
    {
        constexpr std::size_t rdram_size = 8U * 1024U * 1024U;
        constexpr std::size_t players = 0x00235F00U;
        auto host = std::make_unique<std::vector<std::uint8_t>>(rdram_size);
        auto client = std::make_unique<std::vector<std::uint8_t>>(rdram_size);
        for (std::size_t i = 0; i < host->size(); ++i) {
            (*host)[i] = static_cast<std::uint8_t>(i * 29U + 7U);
        }
        for (std::size_t player = 0; player < 4U; ++player) {
            const std::uint32_t pointer = player == 3U
                ? 0U : UINT32_C(0x80050000) + static_cast<std::uint32_t>(player * 0x20U);
            std::memcpy(host->data() + players + player * 0x250U + 0x10U,
                        &pointer, sizeof(pointer));
        }
        std::vector<std::uint8_t> snapshot(btga::gameplay_snapshot_size);
        CHECK(btga::capture_gameplay_snapshot(*host, snapshot));
        std::uint32_t captured_seed = 0U;
        CHECK(btga::gameplay_snapshot_random_seed(snapshot, captured_seed));
        *client = *host;
        std::fill(client->begin() + 0x00114C80U,
                  client->begin() + 0x00114C84U, std::uint8_t{0xEE});
        std::fill(client->begin() + 0x001AD120U,
                  client->begin() + 0x001AD128U, std::uint8_t{0xEF});
        std::fill(client->begin() + 0x001AD128U,
                  client->begin() + 0x001AD128U + 24U * 0x4CCU, std::uint8_t{0xAA});
        constexpr std::size_t sound_handles = 0x001AD128U + 0x280U;
        const std::array<std::uint8_t, 12> client_audio_handles{
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        std::copy(client_audio_handles.begin(), client_audio_handles.end(),
                  client->begin() + sound_handles);
        std::fill(client->begin() + 0x00219458U,
                  client->begin() + 0x00219458U + 0x178U, std::uint8_t{0xBB});
        std::fill(client->begin() + 0x002195D0U,
                  client->begin() + 0x002195E0U, std::uint8_t{0xBC});
        std::fill(client->begin() + players,
                  client->begin() + players + 4U * 0x250U, std::uint8_t{0xCC});
        for (std::size_t player = 0; player < 3U; ++player) {
            std::fill(client->begin() + 0x50000U + player * 0x20U,
                      client->begin() + 0x50000U + player * 0x20U + 0x18U,
                      std::uint8_t{0xDD});
        }
        CHECK(btga::restore_gameplay_snapshot(*client, snapshot));
        CHECK(btga::restore_gameplay_random_seed(*client, captured_seed));
        CHECK(std::all_of(client->begin() + 0x00219478U,
                          client->begin() + 0x00219490U,
                          [](std::uint8_t value) { return value == 0xBBU; }));
        CHECK(std::equal(host->begin() + 0x001AD120U,
                         host->begin() + 0x001AD128U,
                         client->begin() + 0x001AD120U));
        CHECK(std::equal(client_audio_handles.begin(), client_audio_handles.end(),
                         client->begin() + sound_handles));
        CHECK(btga::gameplay_state_digests(*host).combined ==
              btga::gameplay_state_digests(*client).combined);
        CHECK(std::equal(host->begin() + 0x002195D0U,
                         host->begin() + 0x002195E0U,
                         client->begin() + 0x002195D0U));
        for (std::size_t player = 0; player < 4U; ++player) {
            CHECK(std::all_of(
                client->begin() + players + player * 0x250U + 0x120U,
                client->begin() + players + player * 0x250U + 0x164U,
                [](std::uint8_t value) { return value == 0xCCU; }));
        }
        CHECK(std::equal(host->begin() + 0x50000U,
                         host->begin() + 0x50000U + 0x58U,
                         client->begin() + 0x50000U));
        CHECK(!btga::capture_gameplay_snapshot(
            std::span<const std::uint8_t>{host->data(), 1024U}, snapshot));

        constexpr std::size_t grid_descriptors = 0x003977F0U;
        constexpr std::size_t grid_cells = 0x00320000U;
        constexpr std::size_t second_grid_cells = 0x00321000U;
        std::fill(host->begin() + grid_descriptors,
                  host->begin() + grid_descriptors + 6U * 0x18U,
                  std::uint8_t{0});
        auto store_half = [&](std::size_t offset, std::uint16_t value) {
            std::memcpy(host->data() + (offset ^ 2U), &value, sizeof(value));
        };
        store_half(grid_descriptors + 0x08U, 3U);
        store_half(grid_descriptors + 0x0AU, 2U);
        const std::uint32_t grid_cells_address = UINT32_C(0x80320000);
        std::memcpy(host->data() + grid_descriptors + 0x0CU,
                    &grid_cells_address, sizeof(grid_cells_address));
        constexpr std::size_t grid_cell_bytes = ((3U * (2U << 2U)) | 1U) * 2U;
        for (std::size_t index = 0; index < grid_cell_bytes; ++index) {
            (*host)[grid_cells + index] = static_cast<std::uint8_t>(index ^ 0xA5U);
        }
        constexpr std::size_t second_descriptor = grid_descriptors + 0x18U;
        store_half(second_descriptor + 0x08U, 2U);
        store_half(second_descriptor + 0x0AU, 1U);
        const std::uint32_t second_grid_address = UINT32_C(0x80321000);
        std::memcpy(host->data() + second_descriptor + 0x0CU,
                    &second_grid_address, sizeof(second_grid_address));
        constexpr std::size_t second_grid_bytes = ((2U * (1U << 2U)) | 1U) * 2U;
        for (std::size_t index = 0; index < second_grid_bytes; ++index) {
            (*host)[second_grid_cells + index] =
                static_cast<std::uint8_t>(index ^ 0x5AU);
        }
        *client = *host;
        std::vector<std::uint8_t> checkpoint;
        CHECK(btga::capture_dedicated_checkpoint(*host, checkpoint));
        CHECK(btga::is_dedicated_checkpoint(checkpoint));
        CHECK(btga::gameplay_snapshot_random_seed(checkpoint, captured_seed));
        constexpr std::size_t trailer = btga::dedicated_checkpoint_base_size;
        auto check_rejected_without_mutation = [&](std::vector<std::uint8_t> malformed) {
            auto target = *client;
            const auto original = target;
            return !btga::restore_dedicated_checkpoint(target, malformed) &&
                   target == original;
        };
        {
            auto malformed = checkpoint;
            malformed[trailer] ^= 0x01U;
            CHECK(!btga::is_dedicated_checkpoint(malformed));
            CHECK(check_rejected_without_mutation(std::move(malformed)));
        }
        {
            auto malformed = checkpoint;
            malformed.pop_back();
            CHECK(!btga::is_dedicated_checkpoint(malformed));
            CHECK(check_rejected_without_mutation(std::move(malformed)));
        }
        {
            auto malformed = checkpoint;
            malformed[trailer + 15U] ^= 0x02U; // in-range guest address
            CHECK(btga::is_dedicated_checkpoint(malformed));
            CHECK(check_rejected_without_mutation(std::move(malformed)));
        }
        {
            auto malformed = checkpoint;
            malformed[trailer + 19U] ^= 0x02U; // encoded byte count
            CHECK(!btga::is_dedicated_checkpoint(malformed));
            CHECK(check_rejected_without_mutation(std::move(malformed)));
        }
        {
            std::vector<std::uint8_t> arbitrary(
                btga::dedicated_checkpoint_base_size, std::uint8_t{0});
            CHECK(!btga::gameplay_snapshot_random_seed(arbitrary, captured_seed));
        }
        constexpr std::size_t collision_begin = 0x00397650U;
        constexpr std::size_t collision_end = 0x003A53A0U;
        constexpr std::size_t gameplay_bss_begin = 0x003A53A0U;
        constexpr std::size_t gameplay_bss_end = 0x003AD955U;
        std::fill(client->begin() + collision_begin,
                  client->begin() + collision_end, std::uint8_t{0x31});
        std::fill(client->begin() + gameplay_bss_begin,
                  client->begin() + gameplay_bss_end, std::uint8_t{0x42});
        std::fill(client->begin() + grid_cells,
                  client->begin() + grid_cells + grid_cell_bytes,
                  std::uint8_t{0x43});
        std::fill(client->begin() + second_grid_cells,
                  client->begin() + second_grid_cells + second_grid_bytes,
                  std::uint8_t{0x44});
        constexpr std::size_t excluded_after_bss = 0x003AD960U;
        const std::array<std::uint8_t, 8> host_local_sentinel{
            0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7};
        std::copy(host_local_sentinel.begin(), host_local_sentinel.end(),
                  client->begin() + excluded_after_bss);
        CHECK(btga::restore_dedicated_checkpoint(*client, checkpoint));
        CHECK(std::equal(host->begin() + collision_begin,
                         host->begin() + collision_end,
                         client->begin() + collision_begin));
        CHECK(std::equal(host->begin() + gameplay_bss_begin,
                         host->begin() + gameplay_bss_end,
                         client->begin() + gameplay_bss_begin));
        CHECK(std::equal(host->begin() + grid_cells,
                         host->begin() + grid_cells + grid_cell_bytes,
                         client->begin() + grid_cells));
        CHECK(std::equal(host->begin() + second_grid_cells,
                         host->begin() + second_grid_cells + second_grid_bytes,
                         client->begin() + second_grid_cells));
        CHECK(std::equal(host_local_sentinel.begin(), host_local_sentinel.end(),
                         client->begin() + excluded_after_bss));
        CHECK(!btga::capture_dedicated_checkpoint(
            std::span<const std::uint8_t>{host->data(), 1024U}, checkpoint));

        // A world with no allocated collision grids has a valid empty trailer.
        std::fill(host->begin() + grid_descriptors,
                  host->begin() + grid_descriptors + 6U * 0x18U,
                  std::uint8_t{0});
        CHECK(btga::capture_dedicated_checkpoint(*host, checkpoint));
        CHECK(checkpoint.size() ==
              btga::dedicated_checkpoint_base_size + 12U);
        CHECK(btga::is_dedicated_checkpoint(checkpoint));
        CHECK(btga::restore_dedicated_checkpoint(*client, checkpoint));
    }
    {
        btga::InputSynchronizer sync;
        constexpr std::uint32_t tag = UINT32_C(0x80000000);
        btga::prime_frame_input_epoch(sync, 109U);
        // The next publication at frame 110 advertises an eight-frame
        // redundancy window beginning at 107; all preceding entries must be
        // contiguous or batch encoding shifts the input timeline.
        for (std::uint32_t frame = 107U; frame <= 113U; ++frame) {
            CHECK(sync.input_for(0, tag | frame).has_value());
            CHECK(sync.input_for(1, tag | frame).has_value());
        }
        CHECK(!sync.input_for(0, tag | 106U).has_value());
        CHECK(!sync.input_for(0, tag | 114U).has_value());
    }
    {
        CHECK(!btga::snapshot_generation_is_newer(77U, 77U));
        CHECK(btga::snapshot_generation_is_newer(78U, 77U));
        CHECK(!btga::snapshot_generation_is_newer(76U, 77U));
        CHECK(btga::snapshot_generation_is_newer(1U, UINT32_MAX));
        CHECK(!btga::snapshot_generation_is_newer(UINT32_MAX, 1U));

        std::vector<std::uint8_t> state(32'316U);
        for (std::size_t i = 0; i < state.size(); ++i) state[i] = static_cast<std::uint8_t>(i * 37U);
        const std::uint32_t checksum = btga::snapshot_fnv1a(state);
        const std::uint16_t count = static_cast<std::uint16_t>(
            (state.size() + btga::snapshot_chunk_payload_size - 1U) /
            btga::snapshot_chunk_payload_size);
        std::vector<std::vector<std::uint8_t>> packets;
        for (std::uint16_t index = 0; index < count; ++index) {
            const std::size_t offset = index * btga::snapshot_chunk_payload_size;
            const std::size_t size = std::min(
                btga::snapshot_chunk_payload_size, state.size() - offset);
            packets.emplace_back(btga::snapshot_chunk_header_size + size);
            CHECK(btga::encode_snapshot_chunk(packets.back(), {
                77U, index, count, static_cast<std::uint16_t>(size), checksum,
                {state.data() + offset, size}}));
        }
        btga::SnapshotAssembler assembler(state.size());
        for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
            btga::SnapshotChunk chunk{};
            CHECK(btga::decode_snapshot_chunk(chunk, *it));
            CHECK(assembler.accept(chunk));
            CHECK(assembler.accept(chunk)); // duplicate datagrams are idempotent
        }
        CHECK(assembler.complete() && assembler.verified());
        CHECK(std::equal(state.begin(), state.end(), assembler.bytes().begin()));

        assembler.reset();
        for (std::size_t i = 1; i < packets.size(); ++i) {
            btga::SnapshotChunk chunk{};
            CHECK(btga::decode_snapshot_chunk(chunk, packets[i]));
            CHECK(assembler.accept(chunk));
        }
        CHECK(!assembler.complete() && !assembler.verified());
        btga::SnapshotChunk stale{};
        CHECK(btga::decode_snapshot_chunk(stale, packets[0]));
        stale.generation = 76U;
        CHECK(!assembler.accept(stale));
        assembler.reset();
        CHECK(btga::decode_snapshot_chunk(stale, packets[0]));
        stale.checksum ^= 1U;
        CHECK(assembler.accept(stale));
        for (std::size_t i = 1; i < packets.size(); ++i) {
            btga::SnapshotChunk chunk{};
            CHECK(btga::decode_snapshot_chunk(chunk, packets[i]));
            chunk.checksum = stale.checksum;
            CHECK(assembler.accept(chunk));
        }
        CHECK(assembler.complete() && !assembler.verified());
    }
    {
        constexpr std::size_t rdram_size = 8U * 1024U * 1024U;
        constexpr std::array<std::size_t, 8> gameplay_offsets{
            0x00114C80U, // RNG seed
            0x001AD128U, // object pool
            0x0021945CU, // game frame counter
            0x002194A0U, // game mode
            0x002194B0U, // match time
            0x00235F00U, // player header
            0x00235F00U + 0x1B0U, // player gameplay fields
            0x00235F00U + 0x250U + 0x1B0U, // second player gameplay fields
        };
        auto baseline = std::make_unique<std::vector<std::uint8_t>>(rdram_size, 0U);
        auto changed = std::make_unique<std::vector<std::uint8_t>>(*baseline);
        CHECK(btga::gameplay_state_digest(
                  std::span<const std::uint8_t>{baseline->data(), rdram_size - 1U}) == 0U);
        const std::uint32_t baseline_digest = btga::gameplay_state_digest(*baseline);
        CHECK(baseline_digest != 0U);
        const auto baseline_parts = btga::gameplay_state_digests(*baseline);
        CHECK(baseline_parts.combined == baseline_digest);
        CHECK(baseline_parts.objects != 0U && baseline_parts.player_headers != 0U &&
              baseline_parts.player_gameplay != 0U && baseline_parts.team_info != 0U);
        CHECK(btga::gameplay_state_digest(*changed) == baseline_digest);
        for (const std::size_t offset : gameplay_offsets) {
            (*changed)[offset] ^= 0x5AU;
            CHECK(btga::gameplay_state_digest(*changed) != baseline_digest);
            (*changed)[offset] ^= 0x5AU;
        }
        // Presentation/runtime-only memory is intentionally outside the digest.
        (*changed)[0x00001000U] = 0xA5U;
        CHECK(btga::gameplay_state_digest(*changed) == baseline_digest);
        // Per-window camera state is deliberately excluded.
        (*changed)[0x00235F00U + 0x78U] = 0xA5U;
        CHECK(btga::gameplay_state_digest(*changed) == baseline_digest);

        // Team scores live behind a 32-bit guest pointer in each player.
        constexpr std::size_t team_info_offset = 0x00300000U;
        std::uint32_t team_info_pointer = 0x80000000U + team_info_offset;
        std::memcpy(changed->data() + 0x00235F00U + 0x10U,
                    &team_info_pointer, sizeof(team_info_pointer));
        const std::uint32_t pointer_digest = btga::gameplay_state_digest(*changed);
        (*changed)[team_info_offset + 0x0CU] ^= 0x5AU;
        CHECK(btga::gameplay_state_digest(*changed) != pointer_digest);

        auto gate = std::make_unique<std::vector<std::uint8_t>>(rdram_size, 0U);
        CHECK(!btga::gameplay_frame_input_active(*gate));
        const auto store_word = [&](std::size_t offset, std::uint32_t value) {
            std::memcpy(gate->data() + offset, &value, sizeof(value));
        };
        store_word(0x00219498U, 0x80200000U);
        store_word(0x002195CCU, 3U);
        (*gate)[0x002194A4U ^ 3U] = 1U;
        CHECK(!btga::gameplay_frame_input_active(*gate));
        (*gate)[0x002194A4U ^ 3U] = 2U;
        CHECK(btga::gameplay_frame_input_active(*gate));
        store_word(0x002195CCU, 7U);
        CHECK(!btga::gameplay_frame_input_active(*gate));
        store_word(0x002195CCU, 3U);
        store_word(0x00219498U, 0U);
        CHECK(!btga::gameplay_frame_input_active(*gate));
        CHECK(!btga::gameplay_frame_input_active(
            std::span<const std::uint8_t>{gate->data(), rdram_size - 1U}));
    }
    {
        btga::InputSynchronizer sender;
        btga::InputSynchronizer receiver;
        std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
        const btga::OnlineControllerInput input{0xA510, -73, 91};
        const std::size_t size = sender.encode(
            packet, UINT64_C(0x1020304050607080), 1, 42, 900, 1, input);
        CHECK(size == BTGA_NET_HEADER_SIZE + BTGA_NET_INPUT_HEADER_SIZE +
                      BTGA_NET_INPUT_FRAME_SIZE);
        CHECK(receiver.accept({packet.data(), size},
                              UINT64_C(0x1020304050607080), 1));
        CHECK(receiver.input_for(1, 900) == input);
        CHECK(!receiver.input_for(1, 899).has_value());
        CHECK(!receiver.accept({packet.data(), size},
                               UINT64_C(0x1020304050607081), 1));

        // Replays and older ticks must not replace the newest accepted input.
        CHECK(receiver.accept({packet.data(), size},
                              UINT64_C(0x1020304050607080), 1));
        const btga::OnlineControllerInput newer{0x2000, 10, -10};
        const std::size_t newer_size = sender.encode(
            packet, UINT64_C(0x1020304050607080), 1, 43, 901, 1, newer);
        CHECK(receiver.accept({packet.data(), newer_size},
                              UINT64_C(0x1020304050607080), 1));
        CHECK(receiver.input_for(1, 901) == newer);
        CHECK(receiver.input_for(1, 900) == input);
        CHECK(receiver.latest_tick_for(1) == 901);

        const std::array redundant{
            input, newer, btga::OnlineControllerInput{0x1000, 1, 2},
        };
        const std::size_t batch_size = sender.encode_batch(
            packet, UINT64_C(0x1020304050607080), 1, 44, 900, 1, redundant,
            btga::OnlineSyncOrigin{900, 17});
        CHECK(batch_size == BTGA_NET_HEADER_SIZE + BTGA_NET_INPUT_HEADER_SIZE +
                            redundant.size() * BTGA_NET_INPUT_FRAME_SIZE);
        CHECK(receiver.accept({packet.data(), batch_size},
                              UINT64_C(0x1020304050607080), 1));
        CHECK(receiver.input_for(1, 900) == input);
        CHECK(receiver.input_for(1, 901) == newer);
        CHECK(receiver.input_for(1, 902) == redundant[2]);
        const btga::OnlineSyncOrigin expected_origin{900, 17};
        CHECK(receiver.sync_origin_for(1) == expected_origin);
    }
    class TestHttpClient final : public btga::HttpClient {
    public:
        btga::HttpResponse request(std::string_view method, std::string_view path,
                                   std::string_view body,
                                   std::string_view bearer) override {
            last_method = method;
            last_path = path;
            last_body = body;
            last_bearer = bearer;
            methods.emplace_back(method);
            paths.emplace_back(path);
            bodies.emplace_back(body);
            bearers.emplace_back(bearer);
            CHECK_INDEX();
            return responses[index++];
        }
        void CHECK_INDEX() {
            if (index >= responses.size()) throw std::runtime_error("unexpected HTTP request");
        }
        std::vector<btga::HttpResponse> responses;
        std::size_t index = 0;
        std::string last_method;
        std::string last_path;
        std::string last_body;
        std::string last_bearer;
        std::vector<std::string> methods;
        std::vector<std::string> paths;
        std::vector<std::string> bodies;
        std::vector<std::string> bearers;
    } http;
    http.responses = {
        {200, R"({"lobbies":[{"id":"lobby-1","name":"Desert Duel","build":"pc-port-v2","platform":"pc","region":"eastus","state":"lobby","capabilities":2,"max_players":4,"current_players":2}]})"},
        {201, R"({"lobby":{"id":"hosted-1","name":"My Lobby","build":"pc-port-v2","platform":"pc","region":"eastus","state":"lobby","capabilities":2,"max_players":4,"current_players":1},"host_secret":"host-secret","session_id":"1020304050607080","relay":{"address":"relay.example","udp_port":30420},"relay_token":"host.token"})"},
        {200, R"({"session_id":"1020304050607080","seat":2,"relay":{"address":"relay.example","udp_port":30420},"join_token":"join.token","resume_token":"resume.token"})"},
        {200, R"({"id":"hosted-1","name":"My Lobby","build":"pc-port-v2","platform":"pc","region":"eastus","state":"playing","capabilities":2,"max_players":4,"current_players":2,"expires_at":1800000090,"relay_token":"refreshed.token"})"},
        {204, ""}
    };
    btga::MasterClient master{http};
    const auto lobbies = master.list_lobbies();
    CHECK(lobbies.size() == 1 && lobbies[0].current_players == 2);
    CHECK(http.last_method == "GET" && http.last_path == "/lobbies?protocol=1&build=pc-port-v2");
    const auto hosted = master.host("My Lobby", 4);
    CHECK(hosted.session_id == UINT64_C(0x1020304050607080));
    CHECK(hosted.relay.address == "relay.example" && hosted.relay.port == 30420);
    CHECK(http.last_method == "POST" && http.last_path == "/lobbies");
    const auto joined = master.join("hosted-1");
    CHECK(joined.session_id == hosted.session_id && joined.join_token == "join.token");
    CHECK(joined.seat == 2 && joined.resume_token == "resume.token");
    CHECK(http.last_body.find("resume_token") == std::string::npos);
    CHECK(master.heartbeat("hosted-1", "host-secret", 2, "playing", 0x05U) ==
          "refreshed.token");
    CHECK(http.last_method == "PUT" && http.last_bearer == "host-secret");
    CHECK(http.last_body.find("\"seat_mask\":5") != std::string::npos);
    try {
        master.heartbeat("hosted-1", "host-secret", 2, "playing", 0x10U);
        CHECK(false);
    } catch (const std::invalid_argument&) {}
    master.remove("hosted-1", "host-secret");
    CHECK(http.last_method == "DELETE" && http.last_bearer == "host-secret");

    TestHttpClient missing_lobby_http;
    missing_lobby_http.responses = {{404, R"({"message":"lobby not found"})"}};
    btga::MasterClient missing_lobby_master{missing_lobby_http};
    try {
        (void)missing_lobby_master.heartbeat(
            "expired-lobby", "expired-secret", 1, "playing", 0U);
        CHECK(false);
    } catch (const btga::MasterServiceError& service_error) {
        CHECK(service_error.status() == 404);
        CHECK(std::string_view{service_error.what()}.find("lobby not found") !=
              std::string_view::npos);
    }

    constexpr std::string_view joined_body =
        R"({"session_id":"1020304050607080","seat":2,"relay":{"address":"relay.example","udp_port":30420},"join_token":"join.token","resume_token":"rotated.token"})";
    TestHttpClient throttled_join_http;
    throttled_join_http.responses = {
        {429, R"({"message":"too many requests; retry later"})",
         std::chrono::seconds(7)},
        {200, std::string(joined_body)}
    };
    std::vector<std::chrono::milliseconds> retry_delays;
    btga::MasterClient throttled_join_master{
        throttled_join_http, "pc-port-v2",
        [&](std::chrono::milliseconds delay) { retry_delays.push_back(delay); }};
    const auto resumed = throttled_join_master.join("hosted-1", "resume.token");
    CHECK(resumed.seat == 2 && resumed.resume_token == "rotated.token");
    CHECK(throttled_join_http.index == 2 && retry_delays.size() == 1);
    CHECK(retry_delays[0] == std::chrono::seconds(7));
    CHECK(throttled_join_http.paths[0] == throttled_join_http.paths[1]);
    CHECK(throttled_join_http.bodies[0] == throttled_join_http.bodies[1]);
    CHECK(throttled_join_http.bodies[0].find("resume.token") != std::string::npos);

    TestHttpClient fallback_retry_http;
    fallback_retry_http.responses = {
        {429, R"({"message":"slow down"})"},
        {429, R"({"message":"slow down"})"},
        {200, R"({"lobbies":[]})"}
    };
    retry_delays.clear();
    btga::MasterClient fallback_retry_master{
        fallback_retry_http, "pc-port-v2",
        [&](std::chrono::milliseconds delay) { retry_delays.push_back(delay); }};
    CHECK(fallback_retry_master.list_lobbies().empty());
    CHECK((retry_delays == std::vector<std::chrono::milliseconds>{
        std::chrono::seconds(1), std::chrono::seconds(2)}));

    TestHttpClient capped_retry_http;
    capped_retry_http.responses = {
        {429, R"({"message":"slow down"})", std::chrono::seconds(900)},
        {429, R"({"message":"slow down"})", std::chrono::seconds(900)},
        {429, R"({"message":"slow down"})", std::chrono::seconds(900)},
        {429, R"({"message":"still limited"})", std::chrono::seconds(900)}
    };
    retry_delays.clear();
    btga::MasterClient capped_retry_master{
        capped_retry_http, "pc-port-v2",
        [&](std::chrono::milliseconds delay) { retry_delays.push_back(delay); }};
    try {
        (void)capped_retry_master.list_lobbies();
        CHECK(false);
    } catch (const btga::MasterServiceError& service_error) {
        CHECK(service_error.status() == 429);
    }
    CHECK(capped_retry_http.index == 4 && retry_delays.size() == 3);
    CHECK(std::ranges::all_of(retry_delays, [](std::chrono::milliseconds delay) {
        return delay == std::chrono::seconds(30);
    }));

    TestHttpClient no_retry_http;
    no_retry_http.responses = {{503, R"({"message":"unavailable"})"}};
    retry_delays.clear();
    btga::MasterClient no_retry_master{
        no_retry_http, "pc-port-v2",
        [&](std::chrono::milliseconds delay) { retry_delays.push_back(delay); }};
    try {
        (void)no_retry_master.list_lobbies();
        CHECK(false);
    } catch (const btga::MasterServiceError& service_error) {
        CHECK(service_error.status() == 503);
    }
    CHECK(no_retry_http.index == 1 && retry_delays.empty());

    class TestOnlineIo final : public btga::OnlineIo {
    public:
        bool open() override { ++open_calls; return open_result; }
        void close() override { ++close_calls; }
        bool send_relay(std::span<const std::uint8_t> packet) override {
            ++send_calls;
            last_packet.assign(packet.begin(), packet.end());
            return send_result;
        }
        std::size_t receive_relay(std::span<std::uint8_t>) override { return 0; }
        bool open_result = true;
        bool send_result = true;
        int open_calls = 0;
        int close_calls = 0;
        int send_calls = 0;
        std::vector<std::uint8_t> last_packet;
    } online_io;
    {
        btga::OnlineMultiplayer online{online_io};
        CHECK(online.state() == btga::OnlineState::Offline);
        CHECK(online_io.open_calls == 0 && online_io.send_calls == 0);
        CHECK(online.enter_online());
        CHECK(online_io.open_calls == 1);
        static constexpr std::string_view token = "signed.token";
        CHECK(online.begin_relay(false, UINT64_C(0x1020304050607080), {
            reinterpret_cast<const std::uint8_t*>(token.data()), token.size()
        }));
        CHECK(online.state() == btga::OnlineState::RelayBinding);
        CHECK(online_io.send_calls == 1 && !online_io.last_packet.empty());

        BtgaRelayEnvelope bound{};
        bound.message_type = BTGA_RELAY_MSG_BOUND;
        bound.session_id = UINT64_C(0x1020304050607080);
        bound.source_slot = 1;
        bound.destination_slot = 1;
        std::array<std::uint8_t, BTGA_RELAY_HEADER_SIZE> response{};
        CHECK(btga_relay_encode(response.data(), response.size(), &bound) ==
              response.size());
        CHECK(online.accept_relay_datagram(response));
        CHECK(online.state() == btga::OnlineState::Lobby);
        CHECK(online.relay_slot() == 1);
        online.leave_online();
        CHECK(online.state() == btga::OnlineState::Offline);
        CHECK(online_io.close_calls == 1);
    }
    CHECK(online_io.close_calls == 1);

    btga::UdpOnlineIo udp_io{"127.0.0.1", 9};
    CHECK(!udp_io.is_open());
    CHECK(udp_io.open());
    CHECK(udp_io.is_open());
    udp_io.close();
    CHECK(!udp_io.is_open());

    constexpr std::string_view abc = "abc";
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(abc.data());
    CHECK(btga::hex_digest(btga::sha256({bytes, abc.size()})) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    btga::Game game;
    const float initial_x = game.player().x;
    game.update({.forward = true}, 1.0F / 60.0F);
    CHECK(game.player().x > initial_x);
    game.update({.fire = true}, 1.0F / 60.0F);
    bool active = false;
    for (const auto& shot : game.projectiles()) active |= shot.active;
    CHECK(active);
    auto framebuffer = std::make_unique<btga::Framebuffer>();
    game.render(*framebuffer);
    CHECK(framebuffer->pixels[0] != 0);

    btga::ControllerPak pak;
    CHECK(pak.valid());
    CHECK(pak.free_bytes() == 123 * btga::ControllerPak::page_size);
    btga::ControllerPak::Name name{};
    name.company_code = 0x3459;
    name.game_code = 0x42544741;
    name.game_name[0] = 'B';
    name.extension[0] = 'A';
    int file_number = -1;
    CHECK(pak.allocate(name, 256, file_number) == btga::ControllerPak::Error::none);
    CHECK(file_number == 0);
    CHECK(pak.files_used() == 1);
    int found = -1;
    CHECK(pak.find(name, found) == btga::ControllerPak::Error::none && found == file_number);
    std::array<std::uint8_t, 256> written{};
    for (std::size_t i = 0; i < written.size(); ++i) written[i] = static_cast<std::uint8_t>(i);
    CHECK(pak.write(file_number, 0, written) == btga::ControllerPak::Error::none);
    std::array<std::uint8_t, 256> read{};
    CHECK(pak.read(file_number, 0, read) == btga::ControllerPak::Error::none);
    CHECK(read == written);
    CHECK(pak.read(file_number, 1, read) == btga::ControllerPak::Error::invalid);
    btga::ControllerPak::FileState state{};
    CHECK(pak.state(file_number, state) == btga::ControllerPak::Error::none);
    CHECK(state.size == 256 && state.name.game_code == name.game_code);
    const std::vector<std::uint8_t> image(pak.image().begin(), pak.image().end());
    btga::ControllerPak restored;
    CHECK(restored.import_image(image));
    read.fill(0);
    CHECK(restored.read(file_number, 0, read) == btga::ControllerPak::Error::none);
    CHECK(read == written);
    auto damaged_primary = image;
    damaged_primary[btga::ControllerPak::page_size + 1] ^= 0xff;
    btga::ControllerPak recovered;
    CHECK(recovered.import_image(damaged_primary));
    read.fill(0);
    CHECK(recovered.read(file_number, 0, read) == btga::ControllerPak::Error::none);
    CHECK(read == written);
    auto damaged_both = damaged_primary;
    damaged_both[2 * btga::ControllerPak::page_size + 1] ^= 0xff;
    CHECK(!recovered.import_image(damaged_both));
    CHECK(restored.remove(name) == btga::ControllerPak::Error::none);
    CHECK(restored.files_used() == 0);
    CHECK(restored.free_bytes() == 123 * btga::ControllerPak::page_size);

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path settings_dir =
        std::filesystem::temp_directory_path() / ("btga-settings-test-" + std::to_string(unique));
    const std::filesystem::path settings_path = settings_dir / "settings.ini";

    const std::filesystem::path portable_dir = settings_dir / "portable-install";
    std::filesystem::create_directories(portable_dir);
    {
        std::ofstream marker{portable_dir / "portable.txt"};
        marker << "BattleTanxGA portable mode\n";
    }
    btga::configure_pc_user_data_directory(portable_dir / "BattleTanxGA.exe");
    CHECK(btga::pc_user_data_directory() == portable_dir / "userdata");
    btga::configure_pc_user_data_directory({});

    btga::PcSettings settings = btga::PcSettings::defaults();
    CHECK(settings.players[0].controller_slot == 0);
    CHECK(settings.players[3].controller_slot == 3);
    CHECK(!settings.mod_extended_visibility);
    CHECK(!settings.mod_independent_turret);
    CHECK(settings.mouse_turret_sensitivity == 1.0F);
    CHECK(settings.texture_packs_enabled);
    settings.window_mode = btga::WindowMode::Borderless;
    settings.window_width = 2560;
    settings.window_height = 1440;
    settings.aspect_ratio = btga::AspectRatioMode::Ratio21x9;
    settings.render_scale = 2.0F;
    settings.antialiasing = 4;
    settings.master_volume = 0.625F;
    settings.music_volume = 0.5F;
    settings.effects_volume = 0.75F;
    settings.mono_audio = true;
    settings.screen_shake_intensity = 0.25F;
    settings.texture_packs_enabled = false;
    settings.players[1].deadzone = 0.25F;
    settings.players[1].invert_y = true;
    settings.players[1].turret_sensitivity = 1.75F;
    settings.players[1].invert_turret_x = true;
    settings.players[1].controller_slot = 3;
    settings.keyboard.button_a = "Space";
    settings.controllers[1].button_a = "rightshoulder";
    settings.controllers[1].c_left = "right_x_negative";
    settings.mod_extended_visibility = true;
    settings.mod_independent_turret = true;
    settings.mouse_turret_sensitivity = 1.75F;
    settings.remember_rom("C:/Games/BattleTanx - Global Assault (USA).z64");
    settings.remember_rom("D:/ROMs/BattleTanxGA.z64");
    settings.save(settings_path);
    const btga::PcSettings loaded = btga::PcSettings::load(settings_path);
    CHECK(loaded.window_mode == btga::WindowMode::Borderless);
    CHECK(loaded.window_width == 2560 && loaded.window_height == 1440);
    CHECK(loaded.aspect_ratio == btga::AspectRatioMode::Ratio21x9);
    CHECK(loaded.render_scale == 2.0F && loaded.antialiasing == 4);
    CHECK(loaded.master_volume == 0.625F);
    CHECK(loaded.music_volume == 0.5F);
    CHECK(loaded.effects_volume == 0.75F);
    CHECK(loaded.mono_audio);
    CHECK(loaded.screen_shake_intensity == 0.25F);
    CHECK(!loaded.texture_packs_enabled);
    CHECK(loaded.players[1].deadzone == 0.25F);
    CHECK(loaded.players[1].invert_y && loaded.players[1].controller_slot == 3);
    CHECK(loaded.players[1].turret_sensitivity == 1.75F);
    CHECK(loaded.players[1].invert_turret_x);
    CHECK(loaded.keyboard.button_a == "Space");
    CHECK(loaded.controllers[1].button_a == "rightshoulder");
    CHECK(loaded.controllers[1].c_left == "right_x_negative");
    CHECK(loaded.mod_extended_visibility);
    CHECK(loaded.mod_independent_turret);
    CHECK(loaded.mouse_turret_sensitivity == 1.75F);
    CHECK(loaded.last_rom_path == "D:/ROMs/BattleTanxGA.z64");
    CHECK(loaded.recent_rom_paths[0] == "D:/ROMs/BattleTanxGA.z64");
    CHECK(loaded.recent_rom_paths[1] == "C:/Games/BattleTanx - Global Assault (USA).z64");

    {
        std::ofstream malformed{settings_path, std::ios::app};
        malformed << "unknown.future_option=kept-compatible\n"
                  << "video.window_width=20\n"
                  << "video.render_scale=nan\n"
                  << "video.antialiasing=3\n"
                  << "audio.master_volume=9\n"
                  << "audio.music_volume=inf\n"
                  << "accessibility.screen_shake=-3\n"
                  << "mods.mouse_turret_sensitivity=99\n"
                  << "player1.deadzone=-5\n"
                  << "player1.turret_sensitivity=99\n";
    }
    const btga::PcSettings validated = btga::PcSettings::load(settings_path);
    CHECK(validated.window_width == 640);
    CHECK(validated.mouse_turret_sensitivity == 4.0F);
    CHECK(validated.render_scale == 2.0F);
    CHECK(validated.antialiasing == 0);
    CHECK(validated.master_volume == 1.0F);
    CHECK(validated.music_volume == 0.5F);
    CHECK(validated.screen_shake_intensity == 0.0F);
    CHECK(validated.players[0].deadzone == 0.0F);
    CHECK(validated.players[0].turret_sensitivity == 4.0F);

    btga::PcSettings invalid_enum = btga::PcSettings::defaults();
    invalid_enum.aspect_ratio = static_cast<btga::AspectRatioMode>(99);
    invalid_enum.validate();
    CHECK(invalid_enum.aspect_ratio == btga::AspectRatioMode::Original);

    const std::filesystem::path texture_dir = settings_dir / "texture-packs";
    std::filesystem::create_directories(texture_dir / "City Remaster");
    std::filesystem::create_directories(texture_dir / "Not A Pack");
    {
        std::ofstream manifest{texture_dir / "City Remaster" / "rt64.json"};
        manifest << "{}";
        std::ofstream archive{texture_dir / "Armor.RTZ", std::ios::binary};
        archive << "test";
        std::ofstream ignored{texture_dir / "notes.txt"};
        ignored << "ignored";
    }
    const std::vector<btga::TexturePackInfo> packs = btga::discover_texture_packs(texture_dir);
    CHECK(packs.size() == 2);
    CHECK(packs[0].display_name == "Armor" && packs[0].packaged);
    CHECK(packs[1].display_name == "City Remaster" && !packs[1].packaged);

    const std::filesystem::path incoming = settings_dir / "Incoming.RTZ";
    {
        std::ofstream stream{incoming, std::ios::binary};
        stream << "replacement-pack";
    }
    const std::filesystem::path installed = btga::install_texture_pack(incoming, texture_dir);
    CHECK(installed == texture_dir / "Incoming.RTZ");
    {
        std::ifstream stream{installed, std::ios::binary};
        std::string contents{std::istreambuf_iterator<char>{stream}, {}};
        CHECK(contents == "replacement-pack");
    }
    {
        std::ofstream stream{incoming, std::ios::binary};
        stream << "replacement-pack-v2";
    }
    CHECK(btga::install_texture_pack(incoming, texture_dir) == installed);
    {
        std::ifstream stream{installed, std::ios::binary};
        std::string contents{std::istreambuf_iterator<char>{stream}, {}};
        CHECK(contents == "replacement-pack-v2");
    }

    const std::filesystem::path invalid_pack = settings_dir / "not-a-pack.txt";
    {
        std::ofstream stream{invalid_pack};
        stream << "not a texture pack";
    }
    bool rejected_non_pack = false;
    try {
        (void)btga::install_texture_pack(invalid_pack, texture_dir);
    }
    catch (const std::runtime_error&) {
        rejected_non_pack = true;
    }
    CHECK(rejected_non_pack);
    std::filesystem::remove_all(settings_dir);

}
