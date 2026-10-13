#include "btga/gameplay_snapshot.hpp"

#include <cstring>

namespace btga {
namespace {
constexpr std::size_t rdram_size = 8U * 1024U * 1024U;
constexpr std::size_t random_seed_offset = 0x00114C80U;
constexpr std::size_t object_allocator_offset = 0x001AD120U;
constexpr std::size_t object_allocator_size = 8U;
constexpr std::size_t object_pool_offset = 0x001AD128U;
constexpr std::size_t object_size = 0x4CCU;
constexpr std::size_t object_sound_handles_offset = 0x280U;
constexpr std::size_t object_sound_handles_size = 3U * sizeof(std::uint32_t);
constexpr std::size_t gameplay_globals_offset = 0x00219458U;
constexpr std::size_t gameplay_globals_size = 0x178U;
constexpr std::size_t gameplay_clock_fields_offset = 0x20U;   // D_80219478
constexpr std::size_t gameplay_clock_fields_size = 0x18U;     // through D_8021948C
constexpr std::size_t gameplay_mode_state_offset = 0x002195D0U;
constexpr std::size_t gameplay_mode_state_size = 0x10U;
constexpr std::size_t player_pool_offset = 0x00235F00U;
constexpr std::size_t player_pool_size = 4U * 0x250U;
constexpr std::size_t player_size = 0x250U;
constexpr std::size_t team_pointer_offset = 0x10U;
constexpr std::size_t team_info_size = 0x18U;

std::uint32_t load_word(std::span<const std::uint8_t> rdram, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, rdram.data() + offset, sizeof(value));
    return value;
}

bool guest_offset(std::uint32_t address, std::size_t size, std::size_t& offset) {
    constexpr std::uint32_t kseg0 = UINT32_C(0x80000000);
    if (address < kseg0) return false;
    offset = static_cast<std::size_t>(address - kseg0);
    return offset <= rdram_size && size <= rdram_size - offset;
}
} // namespace

bool gameplay_snapshot_random_seed(std::span<const std::uint8_t> snapshot,
                                   std::uint32_t& random_seed) {
    if (snapshot.size() != gameplay_snapshot_size) return false;
    std::memcpy(&random_seed, snapshot.data(), sizeof(random_seed));
    return true;
}

bool restore_gameplay_random_seed(std::span<std::uint8_t> rdram,
                                  std::uint32_t random_seed) {
    if (rdram.size() < rdram_size) return false;
    std::memcpy(rdram.data() + random_seed_offset, &random_seed, sizeof(random_seed));
    return true;
}

bool capture_gameplay_snapshot(std::span<const std::uint8_t> rdram,
                               std::span<std::uint8_t> snapshot) {
    if (rdram.size() < rdram_size || snapshot.size() != gameplay_snapshot_size) return false;
    std::size_t cursor = 0;
    auto append = [&](std::size_t offset, std::size_t size) {
        std::memcpy(snapshot.data() + cursor, rdram.data() + offset, size);
        cursor += size;
    };
    append(random_seed_offset, sizeof(std::uint32_t));
    append(object_allocator_offset, object_allocator_size);
    for (std::size_t object = 0; object < 24U; ++object) {
        const std::size_t base = object_pool_offset + object * object_size;
        append(base, object_sound_handles_offset);
        std::memset(snapshot.data() + cursor, 0, object_sound_handles_size);
        cursor += object_sound_handles_size;
        append(base + object_sound_handles_offset + object_sound_handles_size,
               object_size - object_sound_handles_offset - object_sound_handles_size);
    }
    const std::size_t globals_cursor = cursor;
    append(gameplay_globals_offset, gameplay_globals_size);
    // Wall-clock samples, render delta, measured FPS and elapsed host frames
    // vary after process replacement. They do not drive authoritative world
    // state and must never participate in a canonical digest.
    std::memset(snapshot.data() + globals_cursor + gameplay_clock_fields_offset,
                0, gameplay_clock_fields_size);
    append(gameplay_mode_state_offset, gameplay_mode_state_size);
    append(player_pool_offset, player_pool_size);
    for (std::size_t player = 0; player < 4U; ++player) {
        const std::uint32_t address = load_word(
            rdram, player_pool_offset + player * player_size + team_pointer_offset);
        std::size_t offset = 0;
        if (guest_offset(address, team_info_size, offset)) {
            append(offset, team_info_size);
        } else {
            std::memset(snapshot.data() + cursor, 0, team_info_size);
            cursor += team_info_size;
        }
    }
    return cursor == snapshot.size();
}

bool restore_gameplay_snapshot(std::span<std::uint8_t> rdram,
                               std::span<const std::uint8_t> snapshot) {
    if (rdram.size() < rdram_size || snapshot.size() != gameplay_snapshot_size) return false;
    std::size_t cursor = 0;
    auto restore = [&](std::size_t offset, std::size_t size) {
        std::memcpy(rdram.data() + offset, snapshot.data() + cursor, size);
        cursor += size;
    };
    restore(random_seed_offset, sizeof(std::uint32_t));
    restore(object_allocator_offset, object_allocator_size);
    for (std::size_t object = 0; object < 24U; ++object) {
        const std::size_t base = object_pool_offset + object * object_size;
        restore(base, object_sound_handles_offset);
        cursor += object_sound_handles_size; // host-local audio handles
        restore(base + object_sound_handles_offset + object_sound_handles_size,
                object_size - object_sound_handles_offset - object_sound_handles_size);
    }
    restore(gameplay_globals_offset, gameplay_clock_fields_offset);
    cursor += gameplay_clock_fields_size;
    restore(gameplay_globals_offset + gameplay_clock_fields_offset +
                gameplay_clock_fields_size,
            gameplay_globals_size - gameplay_clock_fields_offset -
                gameplay_clock_fields_size);
    restore(gameplay_mode_state_offset, gameplay_mode_state_size);
    restore(player_pool_offset, player_pool_size);
    for (std::size_t player = 0; player < 4U; ++player) {
        const std::uint32_t address = load_word(
            rdram, player_pool_offset + player * player_size + team_pointer_offset);
        std::size_t offset = 0;
        if (guest_offset(address, team_info_size, offset)) {
            restore(offset, team_info_size);
        } else {
            cursor += team_info_size;
        }
    }
    return cursor == snapshot.size();
}

} // namespace btga
