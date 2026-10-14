#pragma once

#include <cstdint>

namespace btga {

// Converts elapsed monotonic nanoseconds to a fixed-rate tick index without
// approximating the period as an integer number of milliseconds. Splitting at
// whole seconds avoids overflowing elapsed_nanoseconds * ticks_per_second.
[[nodiscard]] constexpr std::uint64_t fixed_rate_tick(
    std::uint64_t elapsed_nanoseconds,
    std::uint32_t ticks_per_second) noexcept {
    constexpr std::uint64_t nanoseconds_per_second = UINT64_C(1000000000);
    if (ticks_per_second == 0U) return 0U;
    const std::uint64_t seconds = elapsed_nanoseconds / nanoseconds_per_second;
    const std::uint64_t remainder = elapsed_nanoseconds % nanoseconds_per_second;
    return seconds * ticks_per_second +
        (remainder * ticks_per_second) / nanoseconds_per_second;
}

} // namespace btga
