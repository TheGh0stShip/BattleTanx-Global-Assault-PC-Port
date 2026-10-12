#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace btga {

constexpr int kWidth = 640;
constexpr int kHeight = 480;

struct Input {
    bool forward{};
    bool backward{};
    bool left{};
    bool right{};
    bool fire{};
};

struct Framebuffer {
    std::array<std::uint32_t, kWidth * kHeight> pixels{};
    void clear(std::uint32_t color);
    void rectangle(int x, int y, int width, int height, std::uint32_t color);
    void circle(int cx, int cy, int radius, std::uint32_t color);
    void line(int x0, int y0, int x1, int y1, std::uint32_t color);
};

struct Tank {
    float x{320.0F};
    float y{240.0F};
    float angle{};
    float cooldown{};
};

struct Projectile {
    float x{};
    float y{};
    float vx{};
    float vy{};
    float life{};
    bool active{};
};

class Game {
  public:
    void update(const Input& input, float seconds);
    void render(Framebuffer& target) const;
    [[nodiscard]] const Tank& player() const { return player_; }
    [[nodiscard]] std::span<const Projectile> projectiles() const { return projectiles_; }

  private:
    Tank player_{};
    std::array<Projectile, 32> projectiles_{};
    bool fire_latched_{};
    void spawn_projectile();
};

} // namespace btga
