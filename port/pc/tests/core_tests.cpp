#include "btga/game.hpp"
#include "btga/controller_pak.hpp"
#include "btga/pc_settings.hpp"
#include "btga/sha256.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
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

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path settings_dir =
        std::filesystem::temp_directory_path() / ("btga-settings-test-" + std::to_string(unique));
    const std::filesystem::path settings_path = settings_dir / "settings.ini";
    btga::PcSettings settings = btga::PcSettings::defaults();
    CHECK(settings.players[0].controller_slot == 0);
    CHECK(settings.players[3].controller_slot == 3);
    CHECK(!settings.mod_extended_visibility);
    CHECK(!settings.mod_independent_turret);
    CHECK(settings.mouse_turret_sensitivity == 1.0F);
    settings.window_mode = btga::WindowMode::Borderless;
    settings.window_width = 2560;
    settings.window_height = 1440;
    settings.aspect_ratio = btga::AspectRatioMode::Ratio21x9;
    settings.render_scale = 2.0F;
    settings.antialiasing = 4;
    settings.master_volume = 0.625F;
    settings.music_volume = 0.5F;
    settings.effects_volume = 0.75F;
    settings.players[1].deadzone = 0.25F;
    settings.players[1].invert_y = true;
    settings.players[1].controller_slot = 3;
    settings.keyboard.button_a = "Space";
    settings.controllers[1].button_a = "rightshoulder";
    settings.controllers[1].c_left = "right_x_negative";
    settings.mod_extended_visibility = true;
    settings.mod_independent_turret = true;
    settings.mouse_turret_sensitivity = 1.75F;
    settings.remember_rom("C:/Games/BattleTanx - Global Assault (USA).z64");
    settings.remember_rom("D:/ROMs/BattleTanxGA.z64");
    settings.save(settings_path);
    const btga::PcSettings loaded = btga::PcSettings::load(settings_path);
    CHECK(loaded.window_mode == btga::WindowMode::Borderless);
    CHECK(loaded.window_width == 2560 && loaded.window_height == 1440);
    CHECK(loaded.aspect_ratio == btga::AspectRatioMode::Ratio21x9);
    CHECK(loaded.render_scale == 2.0F && loaded.antialiasing == 4);
    CHECK(loaded.master_volume == 0.625F);
    CHECK(loaded.music_volume == 0.5F);
    CHECK(loaded.effects_volume == 0.75F);
    CHECK(loaded.players[1].deadzone == 0.25F);
    CHECK(loaded.players[1].invert_y && loaded.players[1].controller_slot == 3);
    CHECK(loaded.keyboard.button_a == "Space");
    CHECK(loaded.controllers[1].button_a == "rightshoulder");
    CHECK(loaded.controllers[1].c_left == "right_x_negative");
    CHECK(loaded.mod_extended_visibility);
    CHECK(loaded.mod_independent_turret);
    CHECK(loaded.mouse_turret_sensitivity == 1.75F);
    CHECK(loaded.last_rom_path == "D:/ROMs/BattleTanxGA.z64");
    CHECK(loaded.recent_rom_paths[0] == "D:/ROMs/BattleTanxGA.z64");
    CHECK(loaded.recent_rom_paths[1] == "C:/Games/BattleTanx - Global Assault (USA).z64");

    {
        std::ofstream malformed{settings_path, std::ios::app};
        malformed << "unknown.future_option=kept-compatible\n"
                  << "video.window_width=20\n"
                  << "video.render_scale=nan\n"
                  << "video.antialiasing=3\n"
                  << "audio.master_volume=9\n"
                  << "audio.music_volume=inf\n"
                  << "mods.mouse_turret_sensitivity=99\n"
                  << "player1.deadzone=-5\n";
    }
    const btga::PcSettings validated = btga::PcSettings::load(settings_path);
    CHECK(validated.window_width == 640);
    CHECK(validated.mouse_turret_sensitivity == 4.0F);
    CHECK(validated.render_scale == 2.0F);
    CHECK(validated.antialiasing == 0);
    CHECK(validated.master_volume == 1.0F);
    CHECK(validated.music_volume == 0.5F);
    CHECK(validated.players[0].deadzone == 0.0F);

    btga::PcSettings invalid_enum = btga::PcSettings::defaults();
    invalid_enum.aspect_ratio = static_cast<btga::AspectRatioMode>(99);
    invalid_enum.validate();
    CHECK(invalid_enum.aspect_ratio == btga::AspectRatioMode::Original);
    std::filesystem::remove_all(settings_dir);

}
