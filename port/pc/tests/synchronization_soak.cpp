#include "btga/input_sync.hpp"
#include "btga/snapshot_transfer.hpp"

extern "C" {
#include <btga/btga_net.h>
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

int main() {
    constexpr std::size_t state_size = 32'316U;
    constexpr std::uint32_t frame_tag = UINT32_C(0x80000000);
    std::mt19937 random{UINT32_C(0x42544741)};
    btga::SnapshotAssembler receiver(state_size);
    for (std::uint32_t epoch = 1; epoch <= 500U; ++epoch) {
        std::vector<std::uint8_t> authoritative(state_size);
        for (auto& byte : authoritative) byte = static_cast<std::uint8_t>(random());
        const std::uint32_t checksum = btga::snapshot_fnv1a(authoritative);
        const std::uint16_t count = static_cast<std::uint16_t>(
            (state_size + btga::snapshot_chunk_payload_size - 1U) /
            btga::snapshot_chunk_payload_size);
        std::vector<std::vector<std::uint8_t>> packets;
        for (std::uint16_t index = 0; index < count; ++index) {
            const std::size_t offset = index * btga::snapshot_chunk_payload_size;
            const std::size_t size = std::min(
                btga::snapshot_chunk_payload_size, state_size - offset);
            packets.emplace_back(btga::snapshot_chunk_header_size + size);
            const btga::SnapshotChunk chunk{
                epoch, index, count, static_cast<std::uint16_t>(size), checksum,
                {authoritative.data() + offset, size}};
            if (!btga::encode_snapshot_chunk(packets.back(), chunk)) return 1;
        }

        for (unsigned round = 0;
             round < 20U && !(receiver.verified() && receiver.generation() == epoch);
             ++round) {
            std::vector<std::size_t> order(packets.size());
            std::iota(order.begin(), order.end(), 0U);
            std::shuffle(order.begin(), order.end(), random);
            for (const std::size_t index : order) {
                if ((random() % 100U) < 23U) continue; // loss
                std::vector<std::uint8_t> wire = packets[index];
                const bool corrupt = (random() % 1000U) < 7U;
                if (corrupt) wire.back() ^= 0x80U;
                btga::SnapshotChunk decoded{};
                if (!btga::decode_snapshot_chunk(decoded, wire)) return 2;
                (void)receiver.accept(decoded);
                if ((random() % 100U) < 19U) (void)receiver.accept(decoded); // duplicate
                if (corrupt) receiver.reset(); // checksum failure forces a clean retransmit epoch
            }
        }
        if (!receiver.verified() || receiver.generation() != epoch ||
            !std::equal(authoritative.begin(), authoritative.end(), receiver.bytes().begin())) {
            std::cerr << "snapshot recovery failed at epoch " << epoch << '\n';
            return 3;
        }

        // ACK loss must not strand the sender: it retransmits the completed
        // epoch, the receiver accepts duplicate chunks idempotently, and a
        // later ACK ends the barrier.
        bool acknowledged = false;
        for (unsigned attempt = 0; attempt < 20U && !acknowledged; ++attempt) {
            acknowledged = (random() % 100U) >= 35U;
            if (!acknowledged) {
                for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
                    btga::SnapshotChunk duplicate{};
                    if (!btga::decode_snapshot_chunk(duplicate, *it) ||
                        !receiver.accept(duplicate)) return 5;
                }
            }
        }
        if (!acknowledged) return 6;

        btga::InputSynchronizer input_history;
        btga::prime_frame_input_epoch(input_history, epoch * 100U);
        const std::uint32_t next = epoch * 100U + 1U;
        for (std::uint32_t frame = next - 3U; frame < next + 4U; ++frame) {
            if (!input_history.input_for(0, frame_tag | frame) ||
                !input_history.input_for(1, frame_tag | frame)) {
                std::cerr << "input rebase hole at epoch " << epoch << '\n';
                return 4;
            }
        }
    }

    // Exercise the actual delayed-input protocol across repeated authoritative
    // rebases.  Every published packet carries an eight-frame window, so four
    // independent publication opportunities can repair a lost input before it
    // is consumed.  This is the headless regression for the post-snapshot
    // freeze: clearing history without priming the new epoch leaves an
    // unfillable hole in this loop.
    constexpr std::uint64_t session_id = UINT64_C(0x425447414f4e4c49);
    constexpr std::uint32_t input_delay = 4U;
    constexpr std::uint32_t redundancy = 8U;
    std::array<std::uint32_t, 2> sequence{1U, 1U};
    for (std::uint32_t epoch = 1; epoch <= 50U; ++epoch) {
        const std::uint32_t generation = epoch * 10'000U;
        std::array<btga::InputSynchronizer, 2> peers;
        for (auto& peer : peers) btga::prime_frame_input_epoch(peer, generation);

        for (std::uint32_t frame = generation + 1U;
             frame < generation + 241U; ++frame) {
            std::array<std::vector<std::uint8_t>, 2> wire;
            for (std::uint8_t sender = 0; sender < 2U; ++sender) {
                const std::uint32_t publish = frame + input_delay;
                const btga::OnlineControllerInput current{
                    static_cast<std::uint16_t>((frame * 17U + sender) & 0xffffU),
                    static_cast<std::int8_t>((frame + sender * 31U) & 0x7fU),
                    static_cast<std::int8_t>(-static_cast<int>((frame * 3U) & 0x7fU)),
                };
                peers[sender].record_local(sender, frame_tag | publish,
                                           sequence[sender], current);
                const std::uint32_t first = publish - (redundancy - 1U);
                std::array<btga::OnlineControllerInput, redundancy> window{};
                for (std::uint32_t offset = 0; offset < redundancy; ++offset) {
                    if (const auto value = peers[sender].input_for(
                            sender, frame_tag | (first + offset))) {
                        window[offset] = *value;
                    }
                }
                wire[sender].resize(BTGA_NET_HEADER_SIZE + BTGA_NET_MAX_PAYLOAD_SIZE);
                const std::size_t size = peers[sender].encode_batch(
                    wire[sender], session_id, sender, sequence[sender]++,
                    frame_tag | first, sender, window);
                if (size == 0U) return 7;
                wire[sender].resize(size);
            }

            // Loss, duplication, and reordering are applied independently in
            // each direction.  If every opportunistic delivery was lost, a
            // bounded final retransmission models the runtime's reliable wait.
            const bool reverse = (random() & 1U) != 0U;
            for (unsigned order = 0; order < 2U; ++order) {
                const std::uint8_t sender = static_cast<std::uint8_t>(reverse ? 1U - order : order);
                const std::uint8_t receiver_index = static_cast<std::uint8_t>(1U - sender);
                if ((random() % 100U) >= 23U) {
                    if (!peers[receiver_index].accept(wire[sender], session_id, sender)) return 8;
                    if ((random() % 100U) < 19U &&
                        !peers[receiver_index].accept(wire[sender], session_id, sender)) return 9;
                }
            }

            for (std::uint8_t player = 0; player < 2U; ++player) {
                const std::uint8_t remote = static_cast<std::uint8_t>(1U - player);
                if (!peers[player].input_for(remote, frame_tag | frame)) {
                    if (!peers[player].accept(wire[remote], session_id, remote) ||
                        !peers[player].input_for(remote, frame_tag | frame)) {
                        std::cerr << "delayed input recovery failed at epoch "
                                  << epoch << " frame " << frame << '\n';
                        return 10;
                    }
                }
            }
        }
    }
    std::cout << "500 randomized snapshot/rebase epochs and 12,000 delayed-input frames passed\n";
    return 0;
}
