#include "btga/controller_pak.hpp"

#include <algorithm>
#include <cstring>

namespace btga {
namespace {
constexpr int inode_page = 1;
constexpr int inode_backup_page = 2;
constexpr int directory_page = 3;
constexpr int first_data_page = 5;
constexpr int page_count = 128;
constexpr std::uint16_t inode_eof = 1;
constexpr std::uint16_t inode_free = 3;
constexpr std::uint8_t occupied = 2;

std::uint8_t inode_checksum(const std::uint8_t* table) {
    std::uint8_t sum = 0;
    for (std::size_t i = first_data_page * 2; i < ControllerPak::page_size; ++i) {
        sum = static_cast<std::uint8_t>(sum + table[i]);
    }
    return sum;
}

bool same_name(const ControllerPak::Name& a, const ControllerPak::Name& b) {
    return a.company_code == b.company_code && a.game_code == b.game_code &&
           a.game_name == b.game_name && a.extension == b.extension;
}
} // namespace

ControllerPak::ControllerPak() { format(); }

std::uint16_t ControllerPak::read16(std::size_t offset) const {
    return static_cast<std::uint16_t>((data_[offset] << 8) | data_[offset + 1]);
}

void ControllerPak::write16(std::size_t offset, std::uint16_t value) {
    data_[offset] = static_cast<std::uint8_t>(value >> 8);
    data_[offset + 1] = static_cast<std::uint8_t>(value);
}

std::uint16_t ControllerPak::inode(int page) const {
    return read16(inode_page * page_size + static_cast<std::size_t>(page) * 2);
}

void ControllerPak::set_inode(int page, std::uint16_t value) {
    write16(inode_page * page_size + static_cast<std::size_t>(page) * 2, value);
}

bool ControllerPak::inode_table_valid(int page) const {
    const auto* table = &data_[static_cast<std::size_t>(page) * page_size];
    return table[1] == inode_checksum(table);
}

void ControllerPak::commit_inodes() {
    auto* table = &data_[inode_page * page_size];
    table[0] = 0;
    table[1] = inode_checksum(table);
    std::copy_n(table, page_size, &data_[inode_backup_page * page_size]);
}

bool ControllerPak::DirectoryEntry::empty() const {
    return name.game_code == 0 || name.company_code == 0;
}

ControllerPak::DirectoryEntry ControllerPak::directory_entry(int file_number) const {
    const std::size_t offset = directory_page * page_size +
                               static_cast<std::size_t>(file_number) * 32;
    DirectoryEntry entry{};
    entry.name.game_code = (static_cast<std::uint32_t>(read16(offset)) << 16) |
                           read16(offset + 2);
    entry.name.company_code = read16(offset + 4);
    entry.start_page = read16(offset + 6);
    entry.status = data_[offset + 8];
    std::copy_n(&data_[offset + 12], 4, entry.name.extension.begin());
    std::copy_n(&data_[offset + 16], 16, entry.name.game_name.begin());
    return entry;
}

void ControllerPak::set_directory_entry(int file_number, const DirectoryEntry& entry) {
    const std::size_t offset = directory_page * page_size +
                               static_cast<std::size_t>(file_number) * 32;
    std::fill_n(&data_[offset], 32, std::uint8_t{0});
    write16(offset, static_cast<std::uint16_t>(entry.name.game_code >> 16));
    write16(offset + 2, static_cast<std::uint16_t>(entry.name.game_code));
    write16(offset + 4, entry.name.company_code);
    write16(offset + 6, entry.start_page);
    data_[offset + 8] = entry.status;
    std::copy(entry.name.extension.begin(), entry.name.extension.end(), &data_[offset + 12]);
    std::copy(entry.name.game_name.begin(), entry.name.game_name.end(), &data_[offset + 16]);
}

std::vector<int> ControllerPak::chain(const DirectoryEntry& entry) const {
    std::vector<int> pages;
    std::uint16_t next = entry.start_page;
    while (next != inode_eof) {
        const int page = next & 0xff;
        if ((next >> 8) != 0 || page < first_data_page || page >= page_count ||
            pages.size() >= page_count - first_data_page ||
            std::find(pages.begin(), pages.end(), page) != pages.end()) {
            return {};
        }
        pages.push_back(page);
        next = inode(page);
    }
    return pages;
}

void ControllerPak::format() {
    data_.fill(0);
    data_[0] = 0x81;
    for (std::size_t i = 1; i < block_size; ++i) data_[i] = static_cast<std::uint8_t>(i);
    std::array<std::uint8_t, block_size> id{};
    id[0] = id[1] = id[2] = id[3] = 0xff;
    id[4] = 'B'; id[5] = 'T'; id[6] = 'G'; id[7] = 'A';
    id[25] = 1; id[26] = 1;
    std::uint16_t sum = 0;
    for (std::size_t i = 0; i < 28; i += 2) {
        sum = static_cast<std::uint16_t>(sum + ((id[i] << 8) | id[i + 1]));
    }
    const std::uint16_t inverted = static_cast<std::uint16_t>(0xfff2 - sum);
    id[28] = static_cast<std::uint8_t>(sum >> 8); id[29] = static_cast<std::uint8_t>(sum);
    id[30] = static_cast<std::uint8_t>(inverted >> 8); id[31] = static_cast<std::uint8_t>(inverted);
    for (int block : {1, 3, 4, 6}) {
        std::copy(id.begin(), id.end(), &data_[static_cast<std::size_t>(block) * block_size]);
    }
    for (int page = 0; page < page_count; ++page) {
        set_inode(page, page < first_data_page ? 0 : inode_free);
    }
    commit_inodes();
}

bool ControllerPak::import_image(std::span<const std::uint8_t> image_data) {
    if (image_data.size() != image_size) return false;
    std::copy(image_data.begin(), image_data.end(), data_.begin());
    if (!inode_table_valid(inode_page)) {
        if (!inode_table_valid(inode_backup_page)) return false;
        std::copy_n(&data_[inode_backup_page * page_size], page_size,
                    &data_[inode_page * page_size]);
    }
    return valid();
}

std::span<const std::uint8_t, ControllerPak::image_size> ControllerPak::image() const {
    return data_;
}

bool ControllerPak::valid() const {
    if (!inode_table_valid(inode_page)) return false;
    for (int i = 0; i < directory_entries; ++i) {
        const auto entry = directory_entry(i);
        if (!entry.empty() && chain(entry).empty()) return false;
    }
    return true;
}

std::size_t ControllerPak::free_bytes() const {
    std::size_t pages = 0;
    for (int page = first_data_page; page < page_count; ++page) pages += inode(page) == inode_free;
    return pages * page_size;
}

int ControllerPak::files_used() const {
    int count = 0;
    for (int i = 0; i < directory_entries; ++i) count += !directory_entry(i).empty();
    return count;
}

ControllerPak::Error ControllerPak::find(const Name& name, int& file_number) const {
    for (int i = 0; i < directory_entries; ++i) {
        const auto entry = directory_entry(i);
        if (!entry.empty() && same_name(entry.name, name)) {
            file_number = i;
            return Error::none;
        }
    }
    file_number = -1;
    return Error::invalid;
}

ControllerPak::Error ControllerPak::allocate(const Name& name, std::size_t bytes, int& file_number) {
    if (name.company_code == 0 || name.game_code == 0 || bytes == 0) return Error::invalid;
    if (find(name, file_number) == Error::none) return Error::exists;
    file_number = -1;
    for (int i = 0; i < directory_entries; ++i) {
        if (directory_entry(i).empty()) { file_number = i; break; }
    }
    if (file_number < 0) return Error::directory_full;
    std::size_t pages_needed = (bytes + page_size - 1) / page_size;
    if (pages_needed * page_size > free_bytes()) return Error::data_full;
    DirectoryEntry entry{};
    entry.name = name;
    entry.status = occupied;
    int previous = -1;
    for (int page = first_data_page; page < page_count && pages_needed != 0; ++page) {
        if (inode(page) != inode_free) continue;
        if (previous < 0) entry.start_page = static_cast<std::uint16_t>(page);
        else set_inode(previous, static_cast<std::uint16_t>(page));
        previous = page;
        --pages_needed;
    }
    set_inode(previous, inode_eof);
    commit_inodes();
    set_directory_entry(file_number, entry);
    return Error::none;
}

ControllerPak::Error ControllerPak::remove(const Name& name) {
    int file_number = -1;
    if (find(name, file_number) != Error::none) return Error::invalid;
    const auto entry = directory_entry(file_number);
    const auto pages = chain(entry);
    if (pages.empty()) return Error::inconsistent;
    for (int page : pages) set_inode(page, inode_free);
    commit_inodes();
    set_directory_entry(file_number, {});
    return Error::none;
}

ControllerPak::Error ControllerPak::state(int file_number, FileState& result) const {
    if (file_number < 0 || file_number >= directory_entries) return Error::invalid;
    const auto entry = directory_entry(file_number);
    if (entry.empty()) return Error::invalid;
    const auto pages = chain(entry);
    if (pages.empty()) return Error::inconsistent;
    result.name = entry.name;
    result.size = pages.size() * page_size;
    return Error::none;
}

ControllerPak::Error ControllerPak::read(int file_number, std::size_t offset,
                                         std::span<std::uint8_t> output) const {
    if (file_number < 0 || file_number >= directory_entries || output.empty() ||
        offset % block_size != 0 || output.size() % block_size != 0) return Error::invalid;
    const auto entry = directory_entry(file_number);
    if (entry.empty()) return Error::invalid;
    const auto pages = chain(entry);
    if (pages.empty()) return Error::inconsistent;
    if (offset + output.size() > pages.size() * page_size) return Error::invalid;
    for (std::size_t i = 0; i < output.size(); ++i) {
        const std::size_t position = offset + i;
        output[i] = data_[static_cast<std::size_t>(pages[position / page_size]) * page_size +
                          position % page_size];
    }
    return Error::none;
}

ControllerPak::Error ControllerPak::write(int file_number, std::size_t offset,
                                          std::span<const std::uint8_t> input) {
    if (file_number < 0 || file_number >= directory_entries || input.empty() ||
        offset % block_size != 0 || input.size() % block_size != 0) return Error::invalid;
    const auto entry = directory_entry(file_number);
    if (entry.empty()) return Error::invalid;
    const auto pages = chain(entry);
    if (pages.empty()) return Error::inconsistent;
    if (offset + input.size() > pages.size() * page_size) return Error::invalid;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const std::size_t position = offset + i;
        data_[static_cast<std::size_t>(pages[position / page_size]) * page_size +
              position % page_size] = input[i];
    }
    return Error::none;
}

} // namespace btga
