#pragma once

#include <cstdint>

namespace btga {

// Coordinates cold/restarted clients with a live authoritative simulation.
// A recovering process must replay the canonical journal from tick zero, but
// it cannot converge while the authority keeps extending that journal. The
// authority therefore pauses simulation (not networking) until every marked
// seat acknowledges the current journal head.
class RecoveryBarrier {
public:
    static constexpr std::uint8_t player_limit = 4U;

    void reset() noexcept;
    void begin(std::uint8_t seat, std::uint32_t authority_head) noexcept;
    void acknowledge(std::uint8_t seat, std::uint32_t client_next_tick,
                     std::uint32_t authority_head) noexcept;
    void disconnect(std::uint8_t seat) noexcept;

    [[nodiscard]] bool paused() const noexcept;
    [[nodiscard]] bool recovering(std::uint8_t seat) const noexcept;
    [[nodiscard]] std::uint8_t mask() const noexcept;

private:
    std::uint8_t recovering_mask_ = 0U;
};

} // namespace btga
