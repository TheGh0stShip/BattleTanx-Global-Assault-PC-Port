#pragma once

#include <array>
#include <filesystem>
#include <string>

namespace btga {

enum class WindowMode {
    Windowed,
    Borderless,
    Fullscreen,
};

enum class AspectRatioMode {
    Original,
    MatchWindow,
    Ratio16x9,
    Ratio16x10,
    Ratio21x9,
    Ratio32x9,
};

struct PlayerSettings {
    float deadzone = 0.15F;
    float sensitivity = 1.0F;
    bool invert_x = false;
    bool invert_y = false;
    float rumble_strength = 1.0F;
    int controller_slot = -1;
};

struct KeyboardBindings {
    std::string stick_up = "W";
    std::string stick_down = "S";
    std::string stick_left = "A";
    std::string stick_right = "D";
    std::string button_a = "X";
    std::string button_b = "Z";
    std::string button_z = "Left Shift";
    std::string button_start = "Return";
    std::string button_l = "Q";
    std::string button_r = "E";
    std::string dpad_up = "Up";
    std::string dpad_down = "Down";
    std::string dpad_left = "Left";
    std::string dpad_right = "Right";
    std::string c_up = "I";
    std::string c_down = "K";
    std::string c_left = "J";
    std::string c_right = "L";
};

struct ControllerBindings {
    std::string button_a = "a";
    std::string button_b = "x";
    std::string button_z = "left_trigger";
    std::string button_start = "start";
    std::string button_l = "leftshoulder";
    std::string button_r = "rightshoulder";
    std::string dpad_up = "dpup";
    std::string dpad_down = "dpdown";
    std::string dpad_left = "dpleft";
    std::string dpad_right = "dpright";
    std::string c_up = "right_y_negative";
    std::string c_down = "right_y_positive";
    std::string c_left = "right_x_negative";
    std::string c_right = "right_x_positive";
};

struct PcSettings {
    static constexpr int current_version = 9;
    static constexpr std::size_t recent_rom_capacity = 5;

    int version = current_version;
    WindowMode window_mode = WindowMode::Windowed;
    int window_width = 960;
    int window_height = 720;
    AspectRatioMode aspect_ratio = AspectRatioMode::Original;
    float render_scale = 1.0F;
    bool vsync = true;
    int frame_limit = 0;
    int antialiasing = 0;
    float master_volume = 1.0F;
    float music_volume = 1.0F;
    float effects_volume = 1.0F;
    bool mono_audio = false;
    float screen_shake_intensity = 1.0F;
    bool texture_packs_enabled = true;
    bool mod_extended_visibility = false;
    bool mod_independent_turret = false;
    float mouse_turret_sensitivity = 1.0F;
    std::string last_rom_path;
    std::array<std::string, recent_rom_capacity> recent_rom_paths{};
    KeyboardBindings keyboard{};
    std::array<ControllerBindings, 4> controllers{};
    std::array<PlayerSettings, 4> players{};

    static PcSettings defaults();
    static PcSettings load(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;
    void remember_rom(const std::filesystem::path& path);
    void validate();
};

std::filesystem::path pc_user_data_directory();
void configure_pc_user_data_directory(const std::filesystem::path& executable_path);
const char* window_mode_name(WindowMode mode);
const char* aspect_ratio_mode_name(AspectRatioMode mode);

} // namespace btga
