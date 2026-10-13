#include "btga/authoritative_replica.hpp"
#include "btga/authoritative_server.hpp"

extern "C" {
#include <btga/btga_gameplay.h>
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <vector>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "check failed at " << __FILE__ << ':' << __LINE__ \
                  << ": " #condition << '\n'; \
        return 1; \
    } \
} while (false)

namespace {

class SoakSimulation final : public btga::AuthoritativeSimulation {
public:
    bool run_tick(
        std::uint32_t tick,
        std::span<const btga::OnlineControllerInput,
                  btga::online_player_limit> inputs) override {
        if (tick != tick_) return false;
        for (std::size_t seat = 0; seat < inputs.size(); ++seat) {
            const auto index = (tick * 17U + static_cast<std::uint32_t>(seat) * 257U) %
                               world_.size();
            world_[index] ^= static_cast<std::uint8_t>(inputs[seat].buttons);
            world_[(index + 1U) % world_.size()] = static_cast<std::uint8_t>(
                world_[(index + 1U) % world_.size()] + inputs[seat].stick_x);
            world_[(index + 2U) % world_.size()] = static_cast<std::uint8_t>(
                world_[(index + 2U) % world_.size()] + inputs[seat].stick_y);
        }
        ++tick_;
        return true;
    }

    std::uint64_t state_digest() const override {
        std::uint64_t digest = UINT64_C(1469598103934665603);
        for (const auto byte : world_) {
            digest ^= byte;
            digest *= UINT64_C(1099511628211);
        }
        digest ^= tick_;
        return digest;
    }

    bool save_state(std::vector<std::uint8_t>& destination) const override {
        destination.resize(sizeof(tick_) + world_.size());
        std::memcpy(destination.data(), &tick_, sizeof(tick_));
        std::memcpy(destination.data() + sizeof(tick_), world_.data(), world_.size());
        return true;
    }

    std::uint32_t tick_ = 0;
    std::array<std::uint8_t, 4096> world_{};
};

struct PendingInput {
    std::uint32_t delivery_tick = 0;
    std::vector<std::uint8_t> payload;
};

std::uint32_t random_next(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

std::vector<std::uint8_t> make_input(std::uint8_t seat, std::uint32_t tick,
                                     std::uint32_t random) {
    BtgaNetInputBatch batch{};
    batch.first_tick = tick;
    batch.player_id = seat;
    batch.frame_count = 1U;
    batch.frames[0].buttons = static_cast<std::uint16_t>(random);
    batch.frames[0].stick_x = static_cast<std::int8_t>(random >> 16);
    batch.frames[0].stick_y = static_cast<std::int8_t>(random >> 24);
    std::vector<std::uint8_t> payload(BTGA_NET_INPUT_HEADER_SIZE +
                                      BTGA_NET_INPUT_FRAME_SIZE);
    const auto size = btga_net_encode_input_batch(
        payload.data(), payload.size(), &batch);
    payload.resize(size);
    return payload;
}

} // namespace

int main() {
    constexpr std::uint32_t run_ticks = 10000U;
    btga::AuthoritativeServerCore server{0x0fU, 30U, 3U};
    SoakSimulation simulation;
    std::array<btga::AuthoritativeReplica, 4> clients;
    std::vector<PendingInput> pending;
    std::uint32_t random = UINT32_C(0xC0FFEE01);
    std::uint32_t emitted_frames = 0;
    std::uint32_t reconstructed_snapshots = 0;
    bool saw_disconnect = false;
    bool resumed = false;

    for (std::uint32_t clock = 0; clock < run_ticks + 16U; ++clock) {
        if (clock == 2500U) CHECK(server.set_connected(2U, false));
        if (clock == 2800U) {
            CHECK(server.set_connected(2U, true));
            const auto resync = server.make_resync(simulation);
            CHECK(resync && resync->size() > 1U);
            for (auto iterator = resync->rbegin(); iterator != resync->rend(); ++iterator) {
                CHECK(clients[2].accept_snapshot_chunk(*iterator));
            }
            const auto restored = clients[2].take_snapshot();
            CHECK(restored && restored->tick == server.session().next_tick());
            CHECK(restored->state_digest == simulation.state_digest());
            resumed = true;
        }

        if (clock < run_ticks) {
            for (std::uint8_t seat = 0; seat < 4U; ++seat) {
                if (seat == 2U && clock >= 2500U && clock < 2800U) continue;
                const auto value = random_next(random);
                if (value % 20U == 0U) continue; // five percent loss
                pending.push_back({clock + (value % 7U),
                                   make_input(seat, clock, value)});
            }
        }

        for (auto iterator = pending.begin(); iterator != pending.end();) {
            if (iterator->delivery_tick > clock) {
                ++iterator;
                continue;
            }
            (void)server.ingest_input_payload(iterator->payload);
            iterator = pending.erase(iterator);
        }

        const auto emission = server.step(simulation, clock);
        if (!emission) continue;
        ++emitted_frames;
        for (std::size_t client = 0; client < clients.size(); ++client) {
            if (client == 2U && clock >= 2500U && clock < 2800U) continue;
            CHECK(clients[client].accept_frame(emission->frame_payload));
            for (auto iterator = emission->snapshot_payloads.rbegin();
                 iterator != emission->snapshot_payloads.rend(); ++iterator) {
                CHECK(clients[client].accept_snapshot_chunk(*iterator));
            }
            if (clients[client].snapshot_ready()) {
                CHECK(clients[client].take_snapshot());
                ++reconstructed_snapshots;
            }
        }
        if (clock >= 2500U && clock < 2800U) {
            BtgaNetAuthoritativeFrame frame{};
            CHECK(btga_net_decode_authoritative_frame(
                &frame, emission->frame_payload.data(),
                emission->frame_payload.size()));
            CHECK((frame.connected_mask & 0x04U) == 0U);
            CHECK(frame.inputs[2].buttons == 0U);
            saw_disconnect = true;
        }
    }

    CHECK(emitted_frames > 9900U);
    CHECK(simulation.tick_ == server.session().next_tick());
    CHECK(saw_disconnect && resumed);
    CHECK(reconstructed_snapshots > 1000U);
    for (const auto& client : clients) {
        CHECK(client.latest_frame());
        CHECK(client.latest_frame()->tick + 1U <= simulation.tick_);
    }
    return 0;
}
