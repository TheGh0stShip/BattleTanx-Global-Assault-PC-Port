#include "btga/game.hpp"
#include "btga/platform.hpp"
#include "btga/rom.hpp"
#include "btga/lzari.hpp"
#include "btga/sha256.hpp"
#include "btga/world.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>

int main(int argc, char** argv) {
    const bool headless = argc == 3 && std::string_view(argv[1]) == "--headless";
    if ((!headless && argc != 2) || (headless && argc != 3)) {
        std::cerr << "usage: battletanx_ga_pc [--headless] /path/to/BattleTanxGA.z64\n";
        return 2;
    }
    try {
        const auto rom = btga::Rom::load_verified(argv[headless ? 2 : 1]);
        std::cout << "Verified local BattleTanx: Global Assault ROM (" << rom.bytes().size() << " bytes)\n";
        constexpr std::size_t world_start = 0x3F9B60;
        constexpr std::size_t world_end = 0x3FCCB0;
        const auto world = btga::lzari_decompress(rom.bytes().subspan(world_start, world_end - world_start));
        if (world.size() != 46788) throw std::runtime_error("unexpected decoded world size");
        std::cout << "Decoded retail world stream: " << world.size() << " bytes, SHA-256 "
                  << btga::hex_digest(btga::sha256(world)) << '\n';
        const auto parsed_world = btga::parse_world(world);
        std::cout << "Parsed level_world_000: " << parsed_world.groups.size() << " groups, "
                  << parsed_world.placements.size() << " placements\n";
        auto configure_world = [&parsed_world](btga::Game& game) {
            std::vector<std::array<float, 2>> points;
            points.reserve(parsed_world.placements.size());
            constexpr float min_x=-2610.0F,max_x=6312.0F,min_z=-10703.0F,max_z=6763.0F;
            for(const auto& placement:parsed_world.placements) points.push_back({
                20.0F+(placement.x-min_x)/(max_x-min_x)*(btga::kWidth-40),
                20.0F+(placement.z-min_z)/(max_z-min_z)*(btga::kHeight-40)});
            game.set_map_points(std::move(points));
        };
        if (headless) {
            btga::Game game;
            configure_world(game);
            auto framebuffer = std::make_unique<btga::Framebuffer>();
            for (int frame = 0; frame < 600; ++frame) {
                btga::Input input{};
                input.forward = frame < 180;
                input.right = frame >= 180 && frame < 300;
                input.fire = frame == 360;
                game.update(input, 1.0F / 60.0F);
            }
            game.render(*framebuffer);
            std::uint32_t checksum = 2166136261U;
            for (const auto pixel : framebuffer->pixels) checksum = (checksum ^ pixel) * 16777619U;
            std::cout << "Headless 600-frame runtime checksum: " << std::hex << checksum << '\n';
            return 0;
        }
        std::unique_ptr<btga::Platform> platform(btga::Platform::create("BattleTanx: Global Assault PC bring-up"));
        if (!platform) throw std::runtime_error("could not create a window");
        btga::Game game;
        configure_world(game);
        auto framebuffer = std::make_unique<btga::Framebuffer>();
        auto previous = std::chrono::steady_clock::now();
        bool running = true;
        while (running) {
            btga::Input input;
            running = platform->pump(input);
            const auto now = std::chrono::steady_clock::now();
            const float elapsed = std::chrono::duration<float>(now - previous).count();
            previous = now;
            game.update(input, elapsed);
            game.render(*framebuffer);
            platform->present(*framebuffer);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
