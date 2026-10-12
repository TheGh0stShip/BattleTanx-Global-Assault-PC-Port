#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace btga {

class ControllerPak {
public:
    static constexpr std::size_t image_size = 32 * 1024;
    static constexpr std::size_t page_size = 256;
    static constexpr std::size_t block_size = 32;
    static constexpr int directory_entries = 16;

    enum class Error {
        none,
        invalid,
        inconsistent,
        data_full,
        directory_full,
        exists,
    };

    struct Name {
        std::uint16_t company_code{};
        std::uint32_t game_code{};
        std::array<std::uint8_t, 16> game_name{};
        std::array<std::uint8_t, 4> extension{};
    };

    struct FileState {
        Name name{};
        std::size_t size{};
    };

    ControllerPak();

    void format();
    bool import_image(std::span<const std::uint8_t> image);
    [[nodiscard]] std::span<const std::uint8_t, image_size> image() const;
    [[nodiscard]] bool valid() const;
    [[nodiscard]] std::size_t free_bytes() const;
    [[nodiscard]] int files_used() const;

    Error allocate(const Name& name, std::size_t bytes, int& file_number);
    Error remove(const Name& name);
    Error find(const Name& name, int& file_number) const;
    Error state(int file_number, FileState& state) const;
    Error read(int file_number, std::size_t offset, std::span<std::uint8_t> output) const;
    Error write(int file_number, std::size_t offset, std::span<const std::uint8_t> input);

private:
    struct DirectoryEntry {
        Name name{};
        std::uint16_t start_page{};
        std::uint8_t status{};
        [[nodiscard]] bool empty() const;
    };

    std::array<std::uint8_t, image_size> data_{};

    [[nodiscard]] std::uint16_t read16(std::size_t offset) const;
    void write16(std::size_t offset, std::uint16_t value);
    [[nodiscard]] std::uint16_t inode(int page) const;
    void set_inode(int page, std::uint16_t value);
    [[nodiscard]] bool inode_table_valid(int page) const;
    void commit_inodes();
    [[nodiscard]] DirectoryEntry directory_entry(int file_number) const;
    void set_directory_entry(int file_number, const DirectoryEntry& entry);
    [[nodiscard]] std::vector<int> chain(const DirectoryEntry& entry) const;
};

} // namespace btga
