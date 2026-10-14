#include "btga/gameplay_digest.hpp"

#include <array>
#include <cstddef>
#include <cstring>

namespace btga {
namespace {

constexpr std::size_t rdram_size = 8U * 1024U * 1024U;
constexpr std::size_t random_seed_offset = 0x00114C80U;
constexpr std::size_t task_sequence_offset = 0x00114CB0U;
constexpr std::size_t task_sequence_size = sizeof(std::uint32_t);
constexpr std::size_t task_state_offset = 0x00224B50U;
constexpr std::size_t task_state_size = 0x113A4U;
constexpr std::size_t object_pool_offset = 0x001AD128U;
constexpr std::size_t object_pool_size = 24U * 0x4CCU;
constexpr std::size_t object_size = 0x4CCU;
constexpr std::size_t object_core_size = 0x290U;
constexpr std::size_t object_sound_handles_offset = 0x280U;
constexpr std::size_t object_sound_handles_size = 3U * sizeof(std::uint32_t);
constexpr std::size_t frame_counter_offset = 0x0021945CU;
constexpr std::size_t game_mode_offset = 0x002194A0U;
constexpr std::size_t match_time_offset = 0x002194B0U;
constexpr std::size_t world_offset = 0x00219498U;
constexpr std::size_t human_player_count_offset = 0x002194A4U;
constexpr std::size_t game_update_state_offset = 0x002195CCU;
constexpr std::size_t player_pool_offset = 0x00235F00U;
constexpr std::size_t player_size = 0x250U;
constexpr std::size_t player_count = 4U;
constexpr std::size_t player_header_size = 0x78U;
constexpr std::size_t player_gameplay_offset = 0x1B0U;
constexpr std::size_t player_gameplay_size = player_size - player_gameplay_offset;
constexpr std::size_t player_team_info_pointer_offset = 0x10U;
constexpr std::size_t team_info_size = 0x18U;

std::uint32_t append_fnv1a(std::uint32_t hash,
                           std::span<const std::uint8_t> bytes) {
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= UINT32_C(16777619);
    }
    return hash;
}

std::uint32_t append_u32(std::uint32_t hash, std::uint32_t value) {
    const std::array bytes{
        static_cast<std::uint8_t>(value >> 24),
        static_cast<std::uint8_t>(value >> 16),
        static_cast<std::uint8_t>(value >> 8),
        static_cast<std::uint8_t>(value),
    };
    return append_fnv1a(hash, bytes);
}

std::span<const std::uint8_t> region(std::span<const std::uint8_t> rdram,
                                     std::size_t offset, std::size_t size) {
    return rdram.subspan(offset, size);
}

std::uint32_t load_guest_word(std::span<const std::uint8_t> rdram,
                              std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, rdram.data() + offset, sizeof(value));
    return value;
}

bool guest_region(std::span<const std::uint8_t> rdram, std::uint32_t address,
                  std::size_t size, std::span<const std::uint8_t>& output) {
    constexpr std::uint32_t kseg0 = UINT32_C(0x80000000);
    if (address < kseg0) return false;
    const std::size_t offset = static_cast<std::size_t>(address - kseg0);
    if (offset > rdram.size() || size > rdram.size() - offset) return false;
    output = region(rdram, offset, size);
    return true;
}

} // namespace

std::uint32_t gameplay_state_digest(std::span<const std::uint8_t> rdram) {
    return gameplay_state_digests(rdram).combined;
}

GameplayStateDigests gameplay_state_digests(
    std::span<const std::uint8_t> rdram) {
    if (rdram.size() < rdram_size) return {};
    constexpr std::uint32_t initial = UINT32_C(2166136261);
    std::array<std::uint8_t, object_pool_size> canonical_objects{};
    std::memcpy(canonical_objects.data(), rdram.data() + object_pool_offset,
                canonical_objects.size());
    for (std::size_t index = 0; index < 24U; ++index) {
        std::memset(canonical_objects.data() + index * object_size +
                        object_sound_handles_offset,
                    0, object_sound_handles_size);
    }
    const auto objects = std::span<const std::uint8_t>{canonical_objects};
    std::uint32_t tasks = append_fnv1a(
        initial, region(rdram, task_sequence_offset, task_sequence_size));
    tasks = append_fnv1a(
        tasks, region(rdram, task_state_offset, task_state_size));
    const auto random_seed = region(rdram, random_seed_offset, sizeof(std::uint32_t));
    const auto frame_counter = region(rdram, frame_counter_offset, sizeof(std::uint32_t));
    const auto game_mode = region(rdram, game_mode_offset, sizeof(std::uint32_t));
    const auto match_time = region(rdram, match_time_offset, sizeof(std::uint32_t));

    std::uint32_t object_core = initial;
    std::uint32_t object_tail = initial;
    std::array<std::uint32_t, 24> object_slots{};
    for (std::size_t index = 0; index < 24U; ++index) {
        const std::size_t base = index * object_size;
        object_slots[index] = append_fnv1a(
            initial, std::span<const std::uint8_t>{canonical_objects}.subspan(
                         base, object_size));
        object_core = append_fnv1a(
            object_core, std::span<const std::uint8_t>{canonical_objects}.subspan(
                             base, object_core_size));
        object_tail = append_fnv1a(
            object_tail,
            std::span<const std::uint8_t>{canonical_objects}.subspan(
                base + object_core_size, object_size - object_core_size));
    }

    std::uint32_t player_headers = initial;
    std::uint32_t player_gameplay = initial;
    std::uint32_t team_info = initial;
    for (std::size_t index = 0; index < player_count; ++index) {
        const std::size_t base = player_pool_offset + index * player_size;
        player_headers = append_fnv1a(
            player_headers, region(rdram, base, player_header_size));
        player_gameplay = append_fnv1a(
            player_gameplay,
            region(rdram, base + player_gameplay_offset, player_gameplay_size));

        const std::uint32_t info_address = load_guest_word(
            rdram, base + player_team_info_pointer_offset);
        std::span<const std::uint8_t> info;
        if (guest_region(rdram, info_address, team_info_size, info)) {
            team_info = append_fnv1a(team_info, info);
        } else {
            // Invalid/null pointers are still part of deterministic state.
            team_info = append_fnv1a(
                team_info,
                region(rdram, base + player_team_info_pointer_offset,
                       sizeof(std::uint32_t)));
        }
    }

    GameplayStateDigests digests{
        .tasks = tasks,
        .objects = append_fnv1a(initial, objects),
        .object_core = object_core,
        .object_tail = object_tail,
        .object_slots = object_slots,
        .player_headers = player_headers,
        .player_gameplay = player_gameplay,
        .team_info = team_info,
        .random_seed = append_fnv1a(initial, random_seed),
        .frame_counter = append_fnv1a(initial, frame_counter),
        .game_mode = append_fnv1a(initial, game_mode),
        .match_time = append_fnv1a(initial, match_time),
    };
    std::uint32_t hash = UINT32_C(2166136261);
    hash = append_u32(hash, tasks);
    hash = append_fnv1a(hash, objects);
    for (const std::uint32_t component : {
             player_headers, player_gameplay, team_info}) {
        hash = append_u32(hash, component);
    }
    hash = append_fnv1a(hash, random_seed);
    hash = append_fnv1a(hash, frame_counter);
    hash = append_fnv1a(hash, game_mode);
    digests.combined = append_fnv1a(hash, match_time);
    return digests;
}

bool gameplay_frame_input_active(std::span<const std::uint8_t> rdram) {
    if (rdram.size() < rdram_size) return false;
    const std::uint32_t world = load_guest_word(rdram, world_offset);
    // Match N64Recomp's MEM_BU byte-lane mapping inside a native-order word.
    const std::uint8_t human_players = rdram[human_player_count_offset ^ 3U];
    const std::uint32_t update_state =
        load_guest_word(rdram, game_update_state_offset);
    return world != 0U && human_players >= 2U && update_state == 3U;
}

} // namespace btga
