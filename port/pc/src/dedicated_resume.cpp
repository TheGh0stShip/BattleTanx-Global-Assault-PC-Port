#include "btga/dedicated_resume.hpp"

extern "C" {
#include <btga/btga_handshake.h>
#include <btga/btga_net.h>
}

#include "btga/pc_settings.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace btga {
namespace {
std::string resume_file_override() {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0U;
    if (_dupenv_s(&value, &length, "BTGA_DEDICATED_RESUME_FILE") != 0 ||
        value == nullptr) {
        return {};
    }
    std::string result{value};
    std::free(value);
    return result;
#else
    const char* value = std::getenv("BTGA_DEDICATED_RESUME_FILE");
    return value == nullptr ? std::string{} : std::string{value};
#endif
}
} // namespace

std::filesystem::path dedicated_resume_path(std::string_view lobby_id) {
    const std::string override = resume_file_override();
    if (!override.empty()) return std::filesystem::path{override};
    if (lobby_id.empty()) {
        return pc_user_data_directory() / "dedicated-resume.token";
    }
    for (const unsigned char character : lobby_id) {
        if ((character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_') continue;
        throw std::runtime_error{"Dedicated lobby id is invalid"};
    }
    if (lobby_id.size() > 64U) {
        throw std::runtime_error{"Dedicated lobby id is invalid"};
    }
    return pc_user_data_directory() / "online-seats" /
        (std::string{lobby_id} + ".token");
}

std::string load_dedicated_resume_token(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::ifstream input{path, std::ios::binary};
    if (!input) return {};
    std::string token((std::istreambuf_iterator<char>{input}),
                      std::istreambuf_iterator<char>{});
    while (!token.empty() && (token.back() == '\r' || token.back() == '\n')) {
        token.pop_back();
    }
    if (token.size() > BTGA_NET_MAX_JOIN_TOKEN_SIZE) {
        throw std::runtime_error{"Dedicated resume credential is oversized"};
    }
    return token;
}

void save_dedicated_resume_token(const std::filesystem::path& path,
                                 std::string_view token) {
    if (path.empty() || token.empty() ||
        token.size() > BTGA_NET_MAX_JOIN_TOKEN_SIZE ||
        token.find_first_of("\r\n") != std::string_view::npos) {
        throw std::runtime_error{"Dedicated resume credential is invalid"};
    }
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    if (error) {
        throw std::runtime_error{"Could not create resume credential directory"};
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output ||
            !output.write(token.data(), static_cast<std::streamsize>(token.size())) ||
            !output.put('\n')) {
            throw std::runtime_error{"Could not write resume credential"};
        }
    }
    std::filesystem::permissions(
        temporary, std::filesystem::perms::owner_read |
                   std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        throw std::runtime_error{"Could not protect resume credential"};
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error) {
        std::filesystem::remove(temporary, error);
        throw std::runtime_error{"Could not install resume credential"};
    }
}

} // namespace btga
