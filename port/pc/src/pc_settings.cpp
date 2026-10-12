#include "btga/pc_settings.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace btga {
namespace {

std::optional<std::string> environment_value(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return std::nullopt;
    std::string result{value};
    std::free(value);
    if (result.empty()) return std::nullopt;
    return result;
#else
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return std::nullopt;
    return std::string{value};
#endif
}

std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool parse_bool(std::string_view value, bool& output) {
    value = trim(value);
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        output = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
        output = false;
        return true;
    }
    return false;
}

template <typename T>
bool parse_number(std::string_view value, T& output) {
    value = trim(value);
    const auto result = std::from_chars(value.data(), value.data() + value.size(), output);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

template <>
bool parse_number<float>(std::string_view value, float& output) {
    value = trim(value);
    try {
        std::size_t used = 0;
        const std::string text{value};
        const float parsed = std::stof(text, &used);
        if (used != text.size() || !std::isfinite(parsed)) return false;
        output = parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

WindowMode parse_window_mode(std::string_view value, WindowMode fallback) {
    value = trim(value);
    if (value == "windowed") return WindowMode::Windowed;
    if (value == "borderless") return WindowMode::Borderless;
    if (value == "fullscreen") return WindowMode::Fullscreen;
    return fallback;
}

void assign_string(std::string_view value, std::string& output) {
    value = trim(value);
    if (!value.empty()) output.assign(value);
}

void apply_key(PcSettings& settings, std::string_view key, std::string_view value) {
    if (key == "version") parse_number(value, settings.version);
    else if (key == "video.window_mode") settings.window_mode = parse_window_mode(value, settings.window_mode);
    else if (key == "video.window_width") parse_number(value, settings.window_width);
    else if (key == "video.window_height") parse_number(value, settings.window_height);
    else if (key == "video.render_scale") parse_number(value, settings.render_scale);
    else if (key == "video.vsync") parse_bool(value, settings.vsync);
    else if (key == "video.frame_limit") parse_number(value, settings.frame_limit);
    else if (key == "video.antialiasing") parse_number(value, settings.antialiasing);
    else if (key == "audio.master_volume") parse_number(value, settings.master_volume);
    else if (key == "audio.music_volume") parse_number(value, settings.music_volume);
    else if (key == "audio.effects_volume") parse_number(value, settings.effects_volume);
    else if (key == "rom.last_path") assign_string(value, settings.last_rom_path);
    else if (key == "keyboard.stick_up") assign_string(value, settings.keyboard.stick_up);
    else if (key == "keyboard.stick_down") assign_string(value, settings.keyboard.stick_down);
    else if (key == "keyboard.stick_left") assign_string(value, settings.keyboard.stick_left);
    else if (key == "keyboard.stick_right") assign_string(value, settings.keyboard.stick_right);
    else if (key == "keyboard.a") assign_string(value, settings.keyboard.button_a);
    else if (key == "keyboard.b") assign_string(value, settings.keyboard.button_b);
    else if (key == "keyboard.z") assign_string(value, settings.keyboard.button_z);
    else if (key == "keyboard.start") assign_string(value, settings.keyboard.button_start);
    else if (key == "keyboard.l") assign_string(value, settings.keyboard.button_l);
    else if (key == "keyboard.r") assign_string(value, settings.keyboard.button_r);
    else if (key == "keyboard.dpad_up") assign_string(value, settings.keyboard.dpad_up);
    else if (key == "keyboard.dpad_down") assign_string(value, settings.keyboard.dpad_down);
    else if (key == "keyboard.dpad_left") assign_string(value, settings.keyboard.dpad_left);
    else if (key == "keyboard.dpad_right") assign_string(value, settings.keyboard.dpad_right);
    else if (key == "keyboard.c_up") assign_string(value, settings.keyboard.c_up);
    else if (key == "keyboard.c_down") assign_string(value, settings.keyboard.c_down);
    else if (key == "keyboard.c_left") assign_string(value, settings.keyboard.c_left);
    else if (key == "keyboard.c_right") assign_string(value, settings.keyboard.c_right);
    else {
        for (std::size_t i = 0; i < settings.recent_rom_paths.size(); ++i) {
            if (key == "rom.recent" + std::to_string(i + 1)) {
                assign_string(value, settings.recent_rom_paths[i]);
                return;
            }
        }
        for (std::size_t i = 0; i < settings.players.size(); ++i) {
            const std::string prefix = "player" + std::to_string(i + 1) + ".";
            if (!key.starts_with(prefix)) continue;
            const std::string_view field = key.substr(prefix.size());
            PlayerSettings& player = settings.players[i];
            if (field == "deadzone") parse_number(value, player.deadzone);
            else if (field == "sensitivity") parse_number(value, player.sensitivity);
            else if (field == "invert_x") parse_bool(value, player.invert_x);
            else if (field == "invert_y") parse_bool(value, player.invert_y);
            else if (field == "rumble_strength") parse_number(value, player.rumble_strength);
            else if (field == "controller_slot") parse_number(value, player.controller_slot);
            return;
        }
    }
}

void write_binding(std::ostream& stream, const char* name, const std::string& value) {
    stream << "keyboard." << name << '=' << value << '\n';
}

} // namespace

PcSettings PcSettings::defaults() {
    PcSettings settings;
    for (std::size_t i = 0; i < settings.players.size(); ++i) {
        settings.players[i].controller_slot = static_cast<int>(i);
    }
    return settings;
}

PcSettings PcSettings::load(const std::filesystem::path& path) {
    PcSettings settings = defaults();
    std::ifstream stream{path};
    if (!stream) return settings;

    std::string line;
    while (std::getline(stream, line)) {
        const std::string_view clean = trim(line);
        if (clean.empty() || clean.front() == '#' || clean.front() == ';') continue;
        const auto equals = clean.find('=');
        if (equals == std::string_view::npos) continue;
        apply_key(settings, trim(clean.substr(0, equals)), clean.substr(equals + 1));
    }
    settings.validate();
    return settings;
}

void PcSettings::save(const std::filesystem::path& path) const {
    PcSettings clean = *this;
    clean.validate();
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    const std::filesystem::path temporary = path.string() + ".tmp";
    std::ofstream stream{temporary, std::ios::trunc};
    if (!stream) throw std::runtime_error{"unable to create settings file"};

    stream << "# BattleTanx: Global Assault PC settings\n";
    stream << "version=" << current_version << '\n';
    stream << "video.window_mode=" << window_mode_name(clean.window_mode) << '\n';
    stream << "video.window_width=" << clean.window_width << '\n';
    stream << "video.window_height=" << clean.window_height << '\n';
    stream << "video.render_scale=" << clean.render_scale << '\n';
    stream << "video.vsync=" << std::boolalpha << clean.vsync << '\n';
    stream << "video.frame_limit=" << clean.frame_limit << '\n';
    stream << "video.antialiasing=" << clean.antialiasing << '\n';
    stream << "audio.master_volume=" << clean.master_volume << '\n';
    stream << "audio.music_volume=" << clean.music_volume << '\n';
    stream << "audio.effects_volume=" << clean.effects_volume << '\n';
    stream << "rom.last_path=" << clean.last_rom_path << '\n';
    for (std::size_t i = 0; i < clean.recent_rom_paths.size(); ++i) {
        stream << "rom.recent" << i + 1 << '=' << clean.recent_rom_paths[i] << '\n';
    }
    write_binding(stream, "stick_up", clean.keyboard.stick_up);
    write_binding(stream, "stick_down", clean.keyboard.stick_down);
    write_binding(stream, "stick_left", clean.keyboard.stick_left);
    write_binding(stream, "stick_right", clean.keyboard.stick_right);
    write_binding(stream, "a", clean.keyboard.button_a);
    write_binding(stream, "b", clean.keyboard.button_b);
    write_binding(stream, "z", clean.keyboard.button_z);
    write_binding(stream, "start", clean.keyboard.button_start);
    write_binding(stream, "l", clean.keyboard.button_l);
    write_binding(stream, "r", clean.keyboard.button_r);
    write_binding(stream, "dpad_up", clean.keyboard.dpad_up);
    write_binding(stream, "dpad_down", clean.keyboard.dpad_down);
    write_binding(stream, "dpad_left", clean.keyboard.dpad_left);
    write_binding(stream, "dpad_right", clean.keyboard.dpad_right);
    write_binding(stream, "c_up", clean.keyboard.c_up);
    write_binding(stream, "c_down", clean.keyboard.c_down);
    write_binding(stream, "c_left", clean.keyboard.c_left);
    write_binding(stream, "c_right", clean.keyboard.c_right);
    for (std::size_t i = 0; i < clean.players.size(); ++i) {
        const PlayerSettings& player = clean.players[i];
        const std::string prefix = "player" + std::to_string(i + 1) + '.';
        stream << prefix << "deadzone=" << player.deadzone << '\n';
        stream << prefix << "sensitivity=" << player.sensitivity << '\n';
        stream << prefix << "invert_x=" << player.invert_x << '\n';
        stream << prefix << "invert_y=" << player.invert_y << '\n';
        stream << prefix << "rumble_strength=" << player.rumble_strength << '\n';
        stream << prefix << "controller_slot=" << player.controller_slot << '\n';
    }
    stream.close();
    if (!stream) throw std::runtime_error{"unable to write settings file"};

    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error{"unable to replace settings file: " + error.message()};
    }
}

void PcSettings::remember_rom(const std::filesystem::path& path) {
    const std::u8string encoded = path.lexically_normal().generic_u8string();
    const std::string text{reinterpret_cast<const char*>(encoded.data()), encoded.size()};
    if (text.empty()) return;

    std::array<std::string, recent_rom_capacity> updated{};
    updated[0] = text;
    std::size_t output = 1;
    for (const std::string& recent : recent_rom_paths) {
        if (recent.empty() || recent == text || output >= updated.size()) continue;
        updated[output++] = recent;
    }
    last_rom_path = text;
    recent_rom_paths = std::move(updated);
}

void PcSettings::validate() {
    version = current_version;
    window_width = std::clamp(window_width, 640, 7680);
    window_height = std::clamp(window_height, 480, 4320);
    render_scale = std::clamp(render_scale, 0.5F, 8.0F);
    frame_limit = std::clamp(frame_limit, 0, 1000);
    if (frame_limit != 0) frame_limit = std::max(frame_limit, 20);
    if (antialiasing != 0 && antialiasing != 2 && antialiasing != 4 && antialiasing != 8) {
        antialiasing = 0;
    }
    master_volume = std::clamp(master_volume, 0.0F, 1.0F);
    music_volume = std::clamp(music_volume, 0.0F, 1.0F);
    effects_volume = std::clamp(effects_volume, 0.0F, 1.0F);
    std::array<std::string, recent_rom_capacity> deduplicated{};
    std::size_t output = 0;
    for (const std::string& recent : recent_rom_paths) {
        if (recent.empty()) continue;
        const bool duplicate = std::find(deduplicated.begin(), deduplicated.begin() + output,
                                         recent) != deduplicated.begin() + output;
        if (!duplicate && output < deduplicated.size()) deduplicated[output++] = recent;
    }
    recent_rom_paths = std::move(deduplicated);
    for (PlayerSettings& player : players) {
        player.deadzone = std::clamp(player.deadzone, 0.0F, 0.95F);
        player.sensitivity = std::clamp(player.sensitivity, 0.1F, 3.0F);
        player.rumble_strength = std::clamp(player.rumble_strength, 0.0F, 1.0F);
        player.controller_slot = std::clamp(player.controller_slot, -1, 3);
    }
}

std::filesystem::path pc_user_data_directory() {
#if defined(_WIN32)
    if (const auto local_app_data = environment_value("LOCALAPPDATA")) {
        return std::filesystem::path{*local_app_data} / "BattleTanxGA";
    }
    if (const auto app_data = environment_value("APPDATA")) {
        return std::filesystem::path{*app_data} / "BattleTanxGA";
    }
#else
    if (const auto xdg_data_home = environment_value("XDG_DATA_HOME")) {
        return std::filesystem::path{*xdg_data_home} / "BattleTanxGA";
    }
    if (const auto home = environment_value("HOME")) {
        return std::filesystem::path{*home} / ".local" / "share" / "BattleTanxGA";
    }
#endif
    return std::filesystem::temp_directory_path() / "BattleTanxGA";
}

const char* window_mode_name(WindowMode mode) {
    switch (mode) {
    case WindowMode::Windowed: return "windowed";
    case WindowMode::Borderless: return "borderless";
    case WindowMode::Fullscreen: return "fullscreen";
    }
    return "windowed";
}

} // namespace btga
