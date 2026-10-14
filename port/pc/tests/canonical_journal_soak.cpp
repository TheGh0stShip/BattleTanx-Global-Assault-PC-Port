#include "btga/translated_authority.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>

#define CHECK(expression) \
    do { \
        if (!(expression)) { \
            std::cerr << "check failed at " << __FILE__ << ':' << __LINE__ \
                      << ": " #expression "\n"; \
            return 1; \
        } \
    } while (false)

int main() {
    constexpr std::uint32_t total_frames = 1'000'128U;
    constexpr std::size_t memory_frames = 64U;
    const auto path = std::filesystem::temp_directory_path() /
        "btga-canonical-journal-soak.journal";
    std::filesystem::remove(path);

    {
        // No occupied seats are required for this storage soak. The authority
        // still executes every normal begin/finish transition and records the
        // exact canonical frame representation used by the live server.
        btga::TranslatedAuthority authority{
            0U, (std::numeric_limits<std::uint32_t>::max)(), 0U};
        CHECK(authority.enable_disk_journal(path, memory_frames));
        std::array<std::uint8_t, 4> state{};
        for (std::uint32_t tick = 0; tick < total_frames; ++tick) {
            state[0] = static_cast<std::uint8_t>(tick);
            CHECK(authority.ready(tick));
            CHECK(authority.begin_tick(tick));
            const auto frame = authority.finish_tick(tick, state);
            CHECK(frame && frame->tick == tick);
        }

        CHECK(authority.next_tick() == total_frames);
        CHECK(authority.journal_size() == total_frames);
        CHECK(authority.journal_base_tick() == total_frames - memory_frames);
        std::array<btga::AuthoritativeFrame, 40> replay{};
        for (const std::uint32_t first : std::array<std::uint32_t, 4>{
                 0U, 499'999U, 999'999U, total_frames - 40U}) {
            CHECK(authority.copy_journal_from(first, replay) == replay.size());
            for (std::size_t index = 0; index < replay.size(); ++index) {
                CHECK(replay[index].tick == first + index);
                CHECK(replay[index].received_mask == 0U);
                CHECK(replay[index].random_seed ==
                    static_cast<std::uint8_t>(first + index));
                for (const auto& input : replay[index].inputs) {
                    CHECK(input.buttons == 0U);
                    CHECK(input.stick_x == 0);
                    CHECK(input.stick_y == 0);
                }
            }
        }
        CHECK(std::filesystem::file_size(path) == 16U +
            static_cast<std::uintmax_t>(total_frames) * 40U);
    }
    CHECK(!std::filesystem::exists(path));
    std::cout << total_frames
              << " canonical frames crossed disk and memory replay boundaries\n";
    return 0;
}
