#include "btga/online_local_view.hpp"

#include <array>
#include <cstddef>
#include <cstring>

namespace btga {
namespace {

constexpr std::size_t full_viewport_offset = 0xB7E30U;
constexpr std::size_t player_one_viewport_offset = 0xB7E40U;
constexpr std::size_t player_two_viewport_offset = 0xB7E50U;
constexpr std::size_t player_one_scissor_offset = 0xB7F88U;
constexpr std::size_t player_two_scissor_offset = 0xB7FA0U;
constexpr std::size_t viewport_size = 16U;

constexpr std::array<std::uint8_t, viewport_size> full_viewport{
    0x02, 0x80, 0x01, 0xE0, 0x01, 0xFF, 0x00, 0x00,
    0x02, 0x80, 0x01, 0xE0, 0x01, 0xFF, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, viewport_size> player_one_viewport{
    0x02, 0x80, 0x00, 0xF0, 0x01, 0xFF, 0x00, 0x00,
    0x02, 0x80, 0x00, 0xF0, 0x01, 0xFF, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, viewport_size> player_two_viewport{
    0x02, 0x80, 0x00, 0xF0, 0x01, 0xFF, 0x00, 0x00,
    0x02, 0x80, 0x02, 0xD0, 0x01, 0xFF, 0x00, 0x00,
};
// These include the existing outer-edge correction from 319x239 to 320x240.
constexpr std::array<std::uint8_t, 8> player_one_scissor{
    0xED, 0x00, 0x00, 0x00, 0x00, 0x50, 0x01, 0xDC,
};
constexpr std::array<std::uint8_t, 8> player_two_scissor{
    0xED, 0x00, 0x01, 0xE0, 0x00, 0x50, 0x03, 0xC0,
};
constexpr std::array<std::uint8_t, 8> full_scissor{
    0xED, 0x00, 0x00, 0x00, 0x00, 0x50, 0x03, 0xC0,
};
// RDP scissor coordinates use 10.2 fixed point. A valid 1x1 rectangle avoids
// relying on undefined equal-edge behavior while effectively suppressing the
// non-local camera without changing simulation or display-list structure.
constexpr std::array<std::uint8_t, 8> hidden_scissor{
    0xED, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x04,
};

template <std::size_t Size>
bool matches(std::span<const std::uint8_t> bytes, std::size_t offset,
             const std::array<std::uint8_t, Size>& expected) {
    return offset <= bytes.size() && Size <= bytes.size() - offset &&
           std::memcmp(bytes.data() + offset, expected.data(), Size) == 0;
}

template <std::size_t Size>
void replace(std::span<std::uint8_t> bytes, std::size_t offset,
             const std::array<std::uint8_t, Size>& value) {
    std::memcpy(bytes.data() + offset, value.data(), Size);
}

} // namespace

bool patch_online_local_view(std::span<std::uint8_t> rom,
                             OnlineLocalView view) {
    if (!matches(rom, full_viewport_offset, full_viewport) ||
        !matches(rom, player_one_viewport_offset, player_one_viewport) ||
        !matches(rom, player_two_viewport_offset, player_two_viewport) ||
        !matches(rom, player_one_scissor_offset, player_one_scissor) ||
        !matches(rom, player_two_scissor_offset, player_two_scissor)) {
        return false;
    }

    if (view == OnlineLocalView::HostPlayerOne) {
        replace(rom, player_one_viewport_offset, full_viewport);
        replace(rom, player_one_scissor_offset, full_scissor);
        replace(rom, player_two_scissor_offset, hidden_scissor);
    } else {
        replace(rom, player_two_viewport_offset, full_viewport);
        replace(rom, player_one_scissor_offset, hidden_scissor);
        replace(rom, player_two_scissor_offset, full_scissor);
    }
    return true;
}

} // namespace btga
