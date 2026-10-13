#pragma once

#include <cstdint>
#include <span>

namespace btga {

enum class OnlineLocalView : std::uint8_t {
    HostPlayerOne,
    ClientPlayerTwo,
};

// Rewrites only the verified ROM's two-player viewport/scissor templates.
// The caller supplies the runtime-private ROM copy; the user's ROM is never
// modified. Returns false without changing anything if any expected byte does
// not match the USA Rev 0 layout.
[[nodiscard]] bool patch_online_local_view(
    std::span<std::uint8_t> rom, OnlineLocalView view);

} // namespace btga
