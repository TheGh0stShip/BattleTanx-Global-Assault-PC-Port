#include "btga/game.hpp"
#include "btga/controller_pak.hpp"
#include "btga/sha256.hpp"

#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

#define CHECK(expression) \
    do { \
        if (!(expression)) { \
            std::cerr << "check failed at " << __FILE__ << ':' << __LINE__ << ": " #expression "\n"; \
            return 1; \
        } \
    } while (false)

int main() {
    constexpr std::string_view abc = "abc";
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(abc.data());
    CHECK(btga::hex_digest(btga::sha256({bytes, abc.size()})) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    btga::Game game;
    const float initial_x = game.player().x;
    game.update({.forward = true}, 1.0F / 60.0F);
    CHECK(game.player().x > initial_x);
    game.update({.fire = true}, 1.0F / 60.0F);
    bool active = false;
    for (const auto& shot : game.projectiles()) active |= shot.active;
    CHECK(active);
    auto framebuffer = std::make_unique<btga::Framebuffer>();
    game.render(*framebuffer);
    CHECK(framebuffer->pixels[0] != 0);

    btga::ControllerPak pak;
    CHECK(pak.valid());
    CHECK(pak.free_bytes() == 123 * btga::ControllerPak::page_size);
    btga::ControllerPak::Name name{};
    name.company_code = 0x3459;
    name.game_code = 0x42544741;
    name.game_name[0] = 'B';
    name.extension[0] = 'A';
    int file_number = -1;
    CHECK(pak.allocate(name, 256, file_number) == btga::ControllerPak::Error::none);
    CHECK(file_number == 0);
    CHECK(pak.files_used() == 1);
    int found = -1;
    CHECK(pak.find(name, found) == btga::ControllerPak::Error::none && found == file_number);
    std::array<std::uint8_t, 256> written{};
    for (std::size_t i = 0; i < written.size(); ++i) written[i] = static_cast<std::uint8_t>(i);
    CHECK(pak.write(file_number, 0, written) == btga::ControllerPak::Error::none);
    std::array<std::uint8_t, 256> read{};
    CHECK(pak.read(file_number, 0, read) == btga::ControllerPak::Error::none);
    CHECK(read == written);
    CHECK(pak.read(file_number, 1, read) == btga::ControllerPak::Error::invalid);
    btga::ControllerPak::FileState state{};
    CHECK(pak.state(file_number, state) == btga::ControllerPak::Error::none);
    CHECK(state.size == 256 && state.name.game_code == name.game_code);
    const std::vector<std::uint8_t> image(pak.image().begin(), pak.image().end());
    btga::ControllerPak restored;
    CHECK(restored.import_image(image));
    read.fill(0);
    CHECK(restored.read(file_number, 0, read) == btga::ControllerPak::Error::none);
    CHECK(read == written);
    auto damaged_primary = image;
    damaged_primary[btga::ControllerPak::page_size + 1] ^= 0xff;
    btga::ControllerPak recovered;
    CHECK(recovered.import_image(damaged_primary));
    read.fill(0);
    CHECK(recovered.read(file_number, 0, read) == btga::ControllerPak::Error::none);
    CHECK(read == written);
    auto damaged_both = damaged_primary;
    damaged_both[2 * btga::ControllerPak::page_size + 1] ^= 0xff;
    CHECK(!recovered.import_image(damaged_both));
    CHECK(restored.remove(name) == btga::ControllerPak::Error::none);
    CHECK(restored.files_used() == 0);
    CHECK(restored.free_bytes() == 123 * btga::ControllerPak::page_size);
}
