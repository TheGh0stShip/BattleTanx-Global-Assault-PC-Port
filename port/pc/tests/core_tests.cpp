#include "btga/game.hpp"
#include "btga/sha256.hpp"

#include <cassert>
#include <string_view>

int main() {
    constexpr std::string_view abc = "abc";
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(abc.data());
    assert(btga::hex_digest(btga::sha256({bytes, abc.size()})) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    btga::Game game;
    const float initial_x = game.player().x;
    game.update({.forward = true}, 1.0F / 60.0F);
    assert(game.player().x > initial_x);
    game.update({.fire = true}, 1.0F / 60.0F);
    bool active = false;
    for (const auto& shot : game.projectiles()) active |= shot.active;
    assert(active);
    btga::Framebuffer framebuffer;
    game.render(framebuffer);
    assert(framebuffer.pixels[0] != 0);
}
