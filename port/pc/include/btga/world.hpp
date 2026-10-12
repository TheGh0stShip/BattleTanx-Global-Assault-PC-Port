#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace btga {
struct WorldGroup {
    std::uint16_t placement_count{}, first_placement{};
    std::int16_t min_x{}, min_y{}, min_z{}, max_x{}, max_y{}, max_z{};
};
struct WorldPlacement {
    std::int16_t x{}, y{}, z{};
    std::uint16_t yaw{};
    std::uint32_t definition_offset{};
};
struct World {
    std::vector<WorldGroup> groups;
    std::vector<WorldPlacement> placements;
};
World parse_world(std::span<const std::uint8_t> bytes);
} // namespace btga
