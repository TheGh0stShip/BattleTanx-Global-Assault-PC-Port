#include "btga/gameplay_snapshot.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace btga {
namespace {
constexpr std::size_t rdram_size = 8U * 1024U * 1024U;
constexpr std::size_t random_seed_offset = 0x00114C80U;
constexpr std::size_t task_sequence_offset = 0x00114CB0U;
constexpr std::size_t task_sequence_size = sizeof(std::uint32_t);
constexpr std::size_t task_state_offset = 0x00224B50U;
// Free-list head, immutable class dispatch table, 65 active-list heads,
// 1,024 0x44-byte gameplay task/entity records, and the pending-list head.
// Despite its historical "scheduler" name this state drives AI, pickups,
// projectiles, mines, spawners, destroyed objects, and object callbacks.
// Include the two bytes of alignment padding after the pending head so the
// following serialized N64 structures retain their original word alignment.
constexpr std::size_t task_state_size = 0x113A4U;
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
constexpr std::size_t player_projection_offset = 0x120U;
constexpr std::size_t player_projection_size = 0x44U;
constexpr std::size_t collision_state_offset = 0x00397650U;
constexpr std::size_t collision_state_size = 0xDD50U;
constexpr std::size_t gameplay_bss_offset = 0x003A53A0U;
constexpr std::size_t gameplay_bss_size = 0x85B5U;
constexpr std::size_t grid_descriptors_offset = 0x003977F0U;
constexpr std::size_t grid_descriptor_size = 0x18U;
constexpr std::size_t grid_descriptor_limit = 6U;
constexpr std::array<std::uint8_t, 8> grid_state_magic{
    'B', 'T', 'G', 'A', 'G', 'R', 'D', '1'};
constexpr std::size_t grid_state_header_size = 12U;
constexpr std::size_t grid_entry_header_size = 8U;
constexpr std::size_t maximum_grid_state_size = 1024U * 1024U;

std::uint32_t load_word(std::span<const std::uint8_t> rdram, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, rdram.data() + offset, sizeof(value));
    return value;
}

std::uint16_t load_half(std::span<const std::uint8_t> rdram, std::size_t offset) {
    std::uint16_t value = 0;
    std::memcpy(&value, rdram.data() + (offset ^ 2U), sizeof(value));
    return value;
}

void put_u32_be(std::uint8_t* output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 24U);
    output[1] = static_cast<std::uint8_t>(value >> 16U);
    output[2] = static_cast<std::uint8_t>(value >> 8U);
    output[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t get_u32_be(const std::uint8_t* input) {
    return (static_cast<std::uint32_t>(input[0]) << 24U) |
           (static_cast<std::uint32_t>(input[1]) << 16U) |
           (static_cast<std::uint32_t>(input[2]) << 8U) |
           static_cast<std::uint32_t>(input[3]);
}

struct GridCellRange {
    std::uint32_t address = 0;
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct GridCheckpointEntry {
    std::uint32_t address = 0;
    std::size_t size = 0;
    std::size_t payload_offset = 0;
};

bool guest_offset(std::uint32_t address, std::size_t size, std::size_t& offset);

bool grid_cell_ranges_from_descriptors(
                      std::span<const std::uint8_t> bytes,
                      std::size_t descriptors_offset,
                      std::array<GridCellRange, grid_descriptor_limit>& ranges,
                      std::size_t& count, std::size_t& total_bytes) {
    count = 0;
    total_bytes = 0;
    if (descriptors_offset > bytes.size() ||
        grid_descriptor_limit * grid_descriptor_size >
            bytes.size() - descriptors_offset) return false;
    for (std::size_t grid = 0; grid < grid_descriptor_limit; ++grid) {
        const std::size_t descriptor =
            descriptors_offset + grid * grid_descriptor_size;
        const std::uint32_t address = load_word(bytes, descriptor + 0x0CU);
        if (address == 0U) continue;
        const std::size_t columns = load_half(bytes, descriptor + 0x08U);
        const std::size_t rows = load_half(bytes, descriptor + 0x0AU);
        if (columns == 0U || rows == 0U) return false;
        const std::size_t cells = (columns * (rows << 2U)) | 1U;
        if (cells > maximum_grid_state_size / sizeof(std::uint16_t)) return false;
        const std::size_t size = cells * sizeof(std::uint16_t);
        std::size_t offset = 0;
        if (!guest_offset(address, size, offset) ||
            total_bytes > maximum_grid_state_size - size) return false;
        for (std::size_t prior = 0; prior < count; ++prior) {
            if (ranges[prior].address == address) return false;
        }
        ranges[count++] = GridCellRange{address, offset, size};
        total_bytes += size;
    }
    return true;
}

bool grid_cell_ranges(std::span<const std::uint8_t> rdram,
                      std::array<GridCellRange, grid_descriptor_limit>& ranges,
                      std::size_t& count, std::size_t& bytes) {
    return grid_cell_ranges_from_descriptors(
        rdram, grid_descriptors_offset, ranges, count, bytes);
}

bool parse_grid_checkpoint(
        std::span<const std::uint8_t> checkpoint,
        std::array<GridCheckpointEntry, grid_descriptor_limit>& entries,
        std::size_t& count) {
    if (checkpoint.size() < dedicated_checkpoint_base_size +
            grid_state_header_size) return false;
    std::size_t cursor = dedicated_checkpoint_base_size;
    if (!std::equal(grid_state_magic.begin(), grid_state_magic.end(),
                    checkpoint.begin() + static_cast<std::ptrdiff_t>(cursor))) return false;
    count = get_u32_be(checkpoint.data() + cursor + 8U);
    if (count > grid_descriptor_limit) return false;
    cursor += grid_state_header_size;
    std::size_t total_bytes = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (cursor > checkpoint.size() ||
            checkpoint.size() - cursor < grid_entry_header_size) return false;
        const std::uint32_t address = get_u32_be(checkpoint.data() + cursor);
        const std::size_t size = get_u32_be(checkpoint.data() + cursor + 4U);
        cursor += grid_entry_header_size;
        std::size_t ignored_offset = 0;
        if (size == 0U || size > maximum_grid_state_size ||
            total_bytes > maximum_grid_state_size - size ||
            size > checkpoint.size() - cursor ||
            !guest_offset(address, size, ignored_offset)) return false;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (entries[prior].address == address) return false;
        }
        entries[index] = GridCheckpointEntry{address, size, cursor};
        total_bytes += size;
        cursor += size;
    }
    return cursor == checkpoint.size();
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
    if (snapshot.size() != gameplay_snapshot_size &&
        !is_dedicated_checkpoint(snapshot)) return false;
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
    const std::size_t players_cursor = cursor;
    append(player_pool_offset, player_pool_size);
    // Each player record embeds guPerspectiveF's normalization value and 4x4
    // projection matrix. A rendered client legitimately rebuilds these with a
    // different viewport from a headless authority. They never drive world
    // simulation, so canonical state must not compare or transplant them.
    for (std::size_t player = 0; player < 4U; ++player) {
        std::memset(snapshot.data() + players_cursor + player * player_size +
                        player_projection_offset,
                    0, player_projection_size);
    }
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
    for (std::size_t player = 0; player < 4U; ++player) {
        const std::size_t base = player_pool_offset + player * player_size;
        restore(base, player_projection_offset);
        cursor += player_projection_size;
        restore(base + player_projection_offset + player_projection_size,
                player_size - player_projection_offset - player_projection_size);
    }
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

bool capture_dedicated_checkpoint(std::span<const std::uint8_t> rdram,
                                  std::vector<std::uint8_t>& checkpoint) {
    if (rdram.size() < rdram_size) return false;
    std::array<GridCellRange, grid_descriptor_limit> ranges{};
    std::size_t range_count = 0;
    std::size_t grid_bytes = 0;
    if (!grid_cell_ranges(rdram, ranges, range_count, grid_bytes)) return false;
    const std::size_t total = dedicated_checkpoint_base_size +
        grid_state_header_size + range_count * grid_entry_header_size + grid_bytes;
    checkpoint.assign(total, 0U);
    std::span<std::uint8_t> output{checkpoint};
    const auto compact = output.first(gameplay_snapshot_size);
    if (!capture_gameplay_snapshot(rdram, compact)) return false;
    std::size_t cursor = gameplay_snapshot_size;
    std::memcpy(checkpoint.data() + cursor,
                rdram.data() + task_sequence_offset, task_sequence_size);
    cursor += task_sequence_size;
    std::memcpy(checkpoint.data() + cursor,
                rdram.data() + task_state_offset, task_state_size);
    cursor += task_state_size;
    std::memcpy(checkpoint.data() + cursor,
                rdram.data() + collision_state_offset, collision_state_size);
    cursor += collision_state_size;
    std::memcpy(checkpoint.data() + cursor,
                rdram.data() + gameplay_bss_offset, gameplay_bss_size);
    cursor += gameplay_bss_size;
    if (cursor != dedicated_checkpoint_base_size) return false;
    std::copy(grid_state_magic.begin(), grid_state_magic.end(),
              checkpoint.begin() + static_cast<std::ptrdiff_t>(cursor));
    put_u32_be(checkpoint.data() + cursor + 8U,
               static_cast<std::uint32_t>(range_count));
    cursor += grid_state_header_size;
    for (std::size_t index = 0; index < range_count; ++index) {
        const auto& range = ranges[index];
        put_u32_be(checkpoint.data() + cursor, range.address);
        put_u32_be(checkpoint.data() + cursor + 4U,
                   static_cast<std::uint32_t>(range.size));
        cursor += grid_entry_header_size;
        std::memcpy(checkpoint.data() + cursor,
                    rdram.data() + range.offset, range.size);
        cursor += range.size;
    }
    return cursor == checkpoint.size();
}

bool restore_dedicated_checkpoint(std::span<std::uint8_t> rdram,
                                  std::span<const std::uint8_t> checkpoint) {
    if (rdram.size() < rdram_size) return false;
    std::array<GridCheckpointEntry, grid_descriptor_limit> entries{};
    std::size_t entry_count = 0;
    if (!parse_grid_checkpoint(checkpoint, entries, entry_count)) return false;

    // Validate the trailer against the descriptors contained in the checkpoint
    // before mutating RDRAM. This makes malformed persistent state fail closed
    // instead of leaving a half-restored authority process behind.
    constexpr std::size_t checkpoint_grid_descriptors =
        gameplay_snapshot_size +
        task_sequence_size + task_state_size +
        (grid_descriptors_offset - collision_state_offset);
    std::array<GridCellRange, grid_descriptor_limit> expected{};
    std::size_t expected_count = 0;
    std::size_t expected_bytes = 0;
    if (!grid_cell_ranges_from_descriptors(
            checkpoint, checkpoint_grid_descriptors, expected,
            expected_count, expected_bytes) ||
        entry_count != expected_count) return false;
    for (std::size_t index = 0; index < entry_count; ++index) {
        if (entries[index].address != expected[index].address ||
            entries[index].size != expected[index].size) return false;
    }

    const auto compact = checkpoint.first(gameplay_snapshot_size);
    if (!restore_gameplay_snapshot(rdram, compact)) return false;
    std::size_t cursor = gameplay_snapshot_size;
    std::memcpy(rdram.data() + task_sequence_offset,
                checkpoint.data() + cursor, task_sequence_size);
    cursor += task_sequence_size;
    std::memcpy(rdram.data() + task_state_offset,
                checkpoint.data() + cursor, task_state_size);
    cursor += task_state_size;
    std::memcpy(rdram.data() + collision_state_offset,
                checkpoint.data() + cursor, collision_state_size);
    cursor += collision_state_size;
    std::memcpy(rdram.data() + gameplay_bss_offset,
                checkpoint.data() + cursor, gameplay_bss_size);
    cursor += gameplay_bss_size;
    if (cursor != dedicated_checkpoint_base_size) return false;
    for (std::size_t index = 0; index < entry_count; ++index) {
        const auto& range = expected[index];
        std::memcpy(rdram.data() + range.offset,
                    checkpoint.data() + entries[index].payload_offset,
                    entries[index].size);
    }
    return true;
}

bool is_dedicated_checkpoint(std::span<const std::uint8_t> checkpoint) {
    std::array<GridCheckpointEntry, grid_descriptor_limit> entries{};
    std::size_t count = 0;
    return parse_grid_checkpoint(checkpoint, entries, count);
}

} // namespace btga
