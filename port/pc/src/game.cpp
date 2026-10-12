#include "btga/game.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace btga {
namespace {
constexpr float kPi = 3.14159265358979323846F;
constexpr std::uint32_t kGround = 0xFF182418U;
}

void Framebuffer::clear(std::uint32_t color) { pixels.fill(color); }

void Framebuffer::rectangle(int x, int y, int width, int height, std::uint32_t color) {
    const int left = std::clamp(x, 0, kWidth);
    const int top = std::clamp(y, 0, kHeight);
    const int right = std::clamp(x + width, 0, kWidth);
    const int bottom = std::clamp(y + height, 0, kHeight);
    for (int py = top; py < bottom; ++py) {
        std::fill_n(pixels.begin() + py * kWidth + left, right - left, color);
    }
}

void Framebuffer::circle(int cx, int cy, int radius, std::uint32_t color) {
    for (int y = -radius; y <= radius; ++y) {
        const int extent = static_cast<int>(std::sqrt(static_cast<float>(radius * radius - y * y)));
        rectangle(cx - extent, cy + y, extent * 2 + 1, 1, color);
    }
}

void Framebuffer::line(int x0, int y0, int x1, int y1, std::uint32_t color) {
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        rectangle(x0, y0, 2, 2, color);
        if (x0 == x1 && y0 == y1) break;
        const int twice = 2 * error;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

void Game::spawn_projectile() {
    auto entry = std::find_if(projectiles_.begin(), projectiles_.end(),
                              [](const Projectile& value) { return !value.active; });
    if (entry == projectiles_.end()) return;
    const float dx = std::cos(player_.angle);
    const float dy = std::sin(player_.angle);
    *entry = {player_.x + dx * 22.0F, player_.y + dy * 22.0F,
              dx * 280.0F, dy * 280.0F, 1.8F, true};
}

void Game::set_map_points(std::vector<std::array<float, 2>> points) {
    map_points_ = std::move(points);
}

void Game::update(const Input& input, float seconds) {
    seconds = std::clamp(seconds, 0.0F, 0.05F);
    const float turn = (input.right ? 1.0F : 0.0F) - (input.left ? 1.0F : 0.0F);
    const float drive = (input.forward ? 1.0F : 0.0F) - (input.backward ? 1.0F : 0.0F);
    player_.angle += turn * 2.2F * seconds;
    if (player_.angle > kPi) player_.angle -= 2.0F * kPi;
    if (player_.angle < -kPi) player_.angle += 2.0F * kPi;
    player_.x = std::clamp(player_.x + std::cos(player_.angle) * drive * 120.0F * seconds,
                           20.0F, static_cast<float>(kWidth - 20));
    player_.y = std::clamp(player_.y + std::sin(player_.angle) * drive * 120.0F * seconds,
                           20.0F, static_cast<float>(kHeight - 20));
    player_.cooldown = std::max(0.0F, player_.cooldown - seconds);
    if (input.fire && !fire_latched_ && player_.cooldown == 0.0F) {
        spawn_projectile();
        player_.cooldown = 0.25F;
    }
    fire_latched_ = input.fire;
    for (Projectile& projectile : projectiles_) {
        if (!projectile.active) continue;
        projectile.x += projectile.vx * seconds;
        projectile.y += projectile.vy * seconds;
        projectile.life -= seconds;
        projectile.active = projectile.life > 0.0F && projectile.x >= 0.0F &&
                            projectile.x < kWidth && projectile.y >= 0.0F && projectile.y < kHeight;
    }
}

void Game::render(Framebuffer& target) const {
    target.clear(kGround);
    for (int x = 0; x < kWidth; x += 40) target.line(x, 0, x, kHeight, 0xFF203020U);
    for (int y = 0; y < kHeight; y += 40) target.line(0, y, kWidth, y, 0xFF203020U);
    target.rectangle(12, 12, kWidth - 24, 4, 0xFF647048U);
    target.rectangle(12, kHeight - 16, kWidth - 24, 4, 0xFF647048U);
    target.rectangle(12, 12, 4, kHeight - 24, 0xFF647048U);
    target.rectangle(kWidth - 16, 12, 4, kHeight - 24, 0xFF647048U);
    for (const auto& point : map_points_) {
        target.rectangle(static_cast<int>(point[0]), static_cast<int>(point[1]), 2, 2, 0xFF708050U);
    }
    const int x = static_cast<int>(player_.x), y = static_cast<int>(player_.y);
    target.circle(x, y, 15, 0xFF4068B0U);
    target.line(x, y, x + static_cast<int>(std::cos(player_.angle) * 25.0F),
                y + static_cast<int>(std::sin(player_.angle) * 25.0F), 0xFFE0D080U);
    for (const Projectile& projectile : projectiles_) {
        if (projectile.active) target.circle(static_cast<int>(projectile.x),
                                             static_cast<int>(projectile.y), 3, 0xFFFFA020U);
    }
}
} // namespace btga
