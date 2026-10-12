#include "btga/game.hpp"
#include "btga/input_sync.hpp"
#include "btga/controller_pak.hpp"
#include "btga/pc_settings.hpp"
#include "btga/online_multiplayer.hpp"
#include "btga/master_client.hpp"
#include "btga/sha256.hpp"
#include "btga/state_sync.hpp"
#include "btga/texture_packs.hpp"
#include "btga/udp_online_io.hpp"

extern "C" {
#include <btga/btga_gameplay.h>
#include <btga/btga_net.h>
}

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
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
    {
        std::vector<std::uint8_t> state(196733);
        for (std::size_t i = 0; i < state.size(); ++i) {
            state[i] = (i / 4096U) % 3U == 0U ? 0U :
                static_cast<std::uint8_t>((i * 37U + i / 11U) & 0xFFU);
        }
        const auto compressed = btga::StateSynchronizer::compress(state);
        CHECK(!compressed.empty());
        CHECK(btga::StateSynchronizer::decompress(compressed, state.size()) == state);
        CHECK(!btga::StateSynchronizer::decompress(compressed, state.size() + 1).has_value());

        const std::uint16_t count = static_cast<std::uint16_t>(
            (compressed.size() + btga::StateSynchronizer::max_fragment_bytes - 1U) /
            btga::StateSynchronizer::max_fragment_bytes);
        btga::StateSynchronizer receiver;
        std::vector<std::vector<std::uint8_t>> packets;
        for (std::uint16_t index = 0; index < count; ++index) {
            const std::size_t offset = static_cast<std::size_t>(index) *
                                       btga::StateSynchronizer::max_fragment_bytes;
            const std::size_t bytes = std::min(
                btga::StateSynchronizer::max_fragment_bytes, compressed.size() - offset);
            std::vector<std::uint8_t> packet(
                btga::StateSynchronizer::fragment_header_size + bytes);
            const btga::StateFragment fragment{
                0x12345678U, static_cast<std::uint32_t>(state.size()),
                static_cast<std::uint32_t>(compressed.size()),
                btga::StateSynchronizer::checksum(state), index, count,
                std::span<const std::uint8_t>{compressed}.subspan(offset, bytes),
            };
            CHECK(btga::StateSynchronizer::encode_fragment(packet, fragment) == packet.size());
            packets.push_back(std::move(packet));
        }
        for (auto it = packets.rbegin(); it != packets.rend(); ++it) CHECK(receiver.accept(*it));
        CHECK(receiver.accept(packets.front())); // Duplicate fragments are idempotent.
        CHECK(receiver.complete());
        CHECK(receiver.transfer_id() == 0x12345678U);
        CHECK(receiver.restore() == state);

        auto corrupt = packets.front();
        corrupt.back() ^= 0x80U;
        btga::StateSynchronizer corrupt_receiver;
        CHECK(corrupt_receiver.accept(corrupt));
        for (std::size_t i = 1; i < packets.size(); ++i) CHECK(corrupt_receiver.accept(packets[i]));
        CHECK(corrupt_receiver.complete());
        CHECK(!corrupt_receiver.restore().has_value());
    }
    {
        btga::InputSynchronizer sender;
        btga::InputSynchronizer receiver;
        std::array<std::uint8_t, BTGA_NET_MAX_DATAGRAM_SIZE> packet{};
        const btga::OnlineControllerInput input{0xA510, -73, 91};
        const std::size_t size = sender.encode(
            packet, UINT64_C(0x1020304050607080), 1, 42, 900, 1, input);
        CHECK(size == BTGA_NET_HEADER_SIZE + BTGA_NET_INPUT_HEADER_SIZE +
                      BTGA_NET_INPUT_FRAME_SIZE);
        CHECK(receiver.accept({packet.data(), size},
                              UINT64_C(0x1020304050607080), 1));
        CHECK(receiver.input_for(1, 900) == input);
        CHECK(!receiver.input_for(1, 899).has_value());
        CHECK(!receiver.accept({packet.data(), size},
                               UINT64_C(0x1020304050607081), 1));

        // Replays and older ticks must not replace the newest accepted input.
        CHECK(receiver.accept({packet.data(), size},
                              UINT64_C(0x1020304050607080), 1));
        const btga::OnlineControllerInput newer{0x2000, 10, -10};
        const std::size_t newer_size = sender.encode(
            packet, UINT64_C(0x1020304050607080), 1, 43, 901, 1, newer);
        CHECK(receiver.accept({packet.data(), newer_size},
                              UINT64_C(0x1020304050607080), 1));
        CHECK(receiver.input_for(1, 901) == newer);
        CHECK(receiver.input_for(1, 900) == input);
        CHECK(receiver.latest_tick_for(1) == 901);

        const std::array redundant{
            input, newer, btga::OnlineControllerInput{0x1000, 1, 2},
        };
        const std::size_t batch_size = sender.encode_batch(
            packet, UINT64_C(0x1020304050607080), 1, 44, 900, 1, redundant,
            btga::OnlineSyncOrigin{900, 17});
        CHECK(batch_size == BTGA_NET_HEADER_SIZE + BTGA_NET_INPUT_HEADER_SIZE +
                            redundant.size() * BTGA_NET_INPUT_FRAME_SIZE);
        CHECK(receiver.accept({packet.data(), batch_size},
                              UINT64_C(0x1020304050607080), 1));
        CHECK(receiver.input_for(1, 900) == input);
        CHECK(receiver.input_for(1, 901) == newer);
        CHECK(receiver.input_for(1, 902) == redundant[2]);
        const btga::OnlineSyncOrigin expected_origin{900, 17};
        CHECK(receiver.sync_origin_for(1) == expected_origin);
    }
    class TestHttpClient final : public btga::HttpClient {
    public:
        btga::HttpResponse request(std::string_view method, std::string_view path,
                                   std::string_view body,
                                   std::string_view bearer) override {
            last_method = method;
            last_path = path;
            last_body = body;
            last_bearer = bearer;
            CHECK_INDEX();
            return responses[index++];
        }
        void CHECK_INDEX() {
            if (index >= responses.size()) throw std::runtime_error("unexpected HTTP request");
        }
        std::vector<btga::HttpResponse> responses;
        std::size_t index = 0;
        std::string last_method;
        std::string last_path;
        std::string last_body;
        std::string last_bearer;
    } http;
    http.responses = {
        {200, R"({"lobbies":[{"id":"lobby-1","name":"Desert Duel","build":"pc-port-v1","platform":"pc","region":"eastus","state":"lobby","capabilities":2,"max_players":4,"current_players":2}]})"},
        {201, R"({"lobby":{"id":"hosted-1","name":"My Lobby","build":"pc-port-v1","platform":"pc","region":"eastus","state":"lobby","capabilities":2,"max_players":4,"current_players":1},"host_secret":"host-secret","session_id":"1020304050607080","relay":{"address":"relay.example","udp_port":30420},"relay_token":"host.token"})"},
        {200, R"({"session_id":"1020304050607080","relay":{"address":"relay.example","udp_port":30420},"join_token":"join.token"})"},
        {200, R"({"id":"hosted-1","name":"My Lobby","build":"pc-port-v1","platform":"pc","region":"eastus","state":"playing","capabilities":2,"max_players":4,"current_players":2,"expires_at":1800000090,"relay_token":"refreshed.token"})"},
        {204, ""}
    };
    btga::MasterClient master{http};
    const auto lobbies = master.list_lobbies();
    CHECK(lobbies.size() == 1 && lobbies[0].current_players == 2);
    CHECK(http.last_method == "GET" && http.last_path == "/lobbies?protocol=1&build=pc-port-v1");
    const auto hosted = master.host("My Lobby", 4);
    CHECK(hosted.session_id == UINT64_C(0x1020304050607080));
    CHECK(hosted.relay.address == "relay.example" && hosted.relay.port == 30420);
    CHECK(http.last_method == "POST" && http.last_path == "/lobbies");
    const auto joined = master.join("hosted-1");
    CHECK(joined.session_id == hosted.session_id && joined.join_token == "join.token");
    master.heartbeat("hosted-1", "host-secret", 2, "playing");
    CHECK(http.last_method == "PUT" && http.last_bearer == "host-secret");
    master.remove("hosted-1", "host-secret");
    CHECK(http.last_method == "DELETE" && http.last_bearer == "host-secret");

    class TestOnlineIo final : public btga::OnlineIo {
    public:
        bool open() override { ++open_calls; return open_result; }
        void close() override { ++close_calls; }
        bool send_relay(std::span<const std::uint8_t> packet) override {
            ++send_calls;
            last_packet.assign(packet.begin(), packet.end());
            return send_result;
        }
        std::size_t receive_relay(std::span<std::uint8_t>) override { return 0; }
        bool open_result = true;
        bool send_result = true;
        int open_calls = 0;
        int close_calls = 0;
        int send_calls = 0;
        std::vector<std::uint8_t> last_packet;
    } online_io;
    {
        btga::OnlineMultiplayer online{online_io};
        CHECK(online.state() == btga::OnlineState::Offline);
        CHECK(online_io.open_calls == 0 && online_io.send_calls == 0);
        CHECK(online.enter_online());
        CHECK(online_io.open_calls == 1);
        static constexpr std::string_view token = "signed.token";
        CHECK(online.begin_relay(false, UINT64_C(0x1020304050607080), {
            reinterpret_cast<const std::uint8_t*>(token.data()), token.size()
        }));
        CHECK(online.state() == btga::OnlineState::RelayBinding);
        CHECK(online_io.send_calls == 1 && !online_io.last_packet.empty());

        BtgaRelayEnvelope bound{};
        bound.message_type = BTGA_RELAY_MSG_BOUND;
        bound.session_id = UINT64_C(0x1020304050607080);
        bound.source_slot = 1;
        bound.destination_slot = 1;
        std::array<std::uint8_t, BTGA_RELAY_HEADER_SIZE> response{};
        CHECK(btga_relay_encode(response.data(), response.size(), &bound) ==
              response.size());
        CHECK(online.accept_relay_datagram(response));
        CHECK(online.state() == btga::OnlineState::Lobby);
        CHECK(online.relay_slot() == 1);
        online.leave_online();
        CHECK(online.state() == btga::OnlineState::Offline);
        CHECK(online_io.close_calls == 1);
    }
    CHECK(online_io.close_calls == 1);

    btga::UdpOnlineIo udp_io{"127.0.0.1", 9};
    CHECK(!udp_io.is_open());
    CHECK(udp_io.open());
    CHECK(udp_io.is_open());
    udp_io.close();
    CHECK(!udp_io.is_open());

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

    const std::filesystem::path portable_dir = settings_dir / "portable-install";
    std::filesystem::create_directories(portable_dir);
    {
        std::ofstream marker{portable_dir / "portable.txt"};
        marker << "BattleTanxGA portable mode\n";
    }
    btga::configure_pc_user_data_directory(portable_dir / "BattleTanxGA.exe");
    CHECK(btga::pc_user_data_directory() == portable_dir / "userdata");
    btga::configure_pc_user_data_directory({});

    btga::PcSettings settings = btga::PcSettings::defaults();
    CHECK(settings.players[0].controller_slot == 0);
    CHECK(settings.players[3].controller_slot == 3);
    CHECK(!settings.mod_extended_visibility);
    CHECK(!settings.mod_independent_turret);
    CHECK(settings.mouse_turret_sensitivity == 1.0F);
    CHECK(settings.texture_packs_enabled);
    settings.window_mode = btga::WindowMode::Borderless;
    settings.window_width = 2560;
    settings.window_height = 1440;
    settings.aspect_ratio = btga::AspectRatioMode::Ratio21x9;
    settings.render_scale = 2.0F;
    settings.antialiasing = 4;
    settings.master_volume = 0.625F;
    settings.music_volume = 0.5F;
    settings.effects_volume = 0.75F;
    settings.mono_audio = true;
    settings.screen_shake_intensity = 0.25F;
    settings.texture_packs_enabled = false;
    settings.players[1].deadzone = 0.25F;
    settings.players[1].invert_y = true;
    settings.players[1].turret_sensitivity = 1.75F;
    settings.players[1].invert_turret_x = true;
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
    CHECK(loaded.mono_audio);
    CHECK(loaded.screen_shake_intensity == 0.25F);
    CHECK(!loaded.texture_packs_enabled);
    CHECK(loaded.players[1].deadzone == 0.25F);
    CHECK(loaded.players[1].invert_y && loaded.players[1].controller_slot == 3);
    CHECK(loaded.players[1].turret_sensitivity == 1.75F);
    CHECK(loaded.players[1].invert_turret_x);
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
                  << "accessibility.screen_shake=-3\n"
                  << "mods.mouse_turret_sensitivity=99\n"
                  << "player1.deadzone=-5\n"
                  << "player1.turret_sensitivity=99\n";
    }
    const btga::PcSettings validated = btga::PcSettings::load(settings_path);
    CHECK(validated.window_width == 640);
    CHECK(validated.mouse_turret_sensitivity == 4.0F);
    CHECK(validated.render_scale == 2.0F);
    CHECK(validated.antialiasing == 0);
    CHECK(validated.master_volume == 1.0F);
    CHECK(validated.music_volume == 0.5F);
    CHECK(validated.screen_shake_intensity == 0.0F);
    CHECK(validated.players[0].deadzone == 0.0F);
    CHECK(validated.players[0].turret_sensitivity == 4.0F);

    btga::PcSettings invalid_enum = btga::PcSettings::defaults();
    invalid_enum.aspect_ratio = static_cast<btga::AspectRatioMode>(99);
    invalid_enum.validate();
    CHECK(invalid_enum.aspect_ratio == btga::AspectRatioMode::Original);

    const std::filesystem::path texture_dir = settings_dir / "texture-packs";
    std::filesystem::create_directories(texture_dir / "City Remaster");
    std::filesystem::create_directories(texture_dir / "Not A Pack");
    {
        std::ofstream manifest{texture_dir / "City Remaster" / "rt64.json"};
        manifest << "{}";
        std::ofstream archive{texture_dir / "Armor.RTZ", std::ios::binary};
        archive << "test";
        std::ofstream ignored{texture_dir / "notes.txt"};
        ignored << "ignored";
    }
    const std::vector<btga::TexturePackInfo> packs = btga::discover_texture_packs(texture_dir);
    CHECK(packs.size() == 2);
    CHECK(packs[0].display_name == "Armor" && packs[0].packaged);
    CHECK(packs[1].display_name == "City Remaster" && !packs[1].packaged);

    const std::filesystem::path incoming = settings_dir / "Incoming.RTZ";
    {
        std::ofstream stream{incoming, std::ios::binary};
        stream << "replacement-pack";
    }
    const std::filesystem::path installed = btga::install_texture_pack(incoming, texture_dir);
    CHECK(installed == texture_dir / "Incoming.RTZ");
    {
        std::ifstream stream{installed, std::ios::binary};
        std::string contents{std::istreambuf_iterator<char>{stream}, {}};
        CHECK(contents == "replacement-pack");
    }
    {
        std::ofstream stream{incoming, std::ios::binary};
        stream << "replacement-pack-v2";
    }
    CHECK(btga::install_texture_pack(incoming, texture_dir) == installed);
    {
        std::ifstream stream{installed, std::ios::binary};
        std::string contents{std::istreambuf_iterator<char>{stream}, {}};
        CHECK(contents == "replacement-pack-v2");
    }

    const std::filesystem::path invalid_pack = settings_dir / "not-a-pack.txt";
    {
        std::ofstream stream{invalid_pack};
        stream << "not a texture pack";
    }
    bool rejected_non_pack = false;
    try {
        (void)btga::install_texture_pack(invalid_pack, texture_dir);
    }
    catch (const std::runtime_error&) {
        rejected_non_pack = true;
    }
    CHECK(rejected_non_pack);
    std::filesystem::remove_all(settings_dir);

}
