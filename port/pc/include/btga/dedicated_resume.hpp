#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace btga {

// BTGA_DEDICATED_RESUME_FILE overrides the normal per-user location for
// unattended tests and multi-client development fixtures.
[[nodiscard]] std::filesystem::path dedicated_resume_path(
    std::string_view lobby_id = {});
[[nodiscard]] std::string load_dedicated_resume_token(
    const std::filesystem::path& path);
void save_dedicated_resume_token(const std::filesystem::path& path,
                                 std::string_view token);

} // namespace btga
