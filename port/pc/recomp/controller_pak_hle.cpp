// Controller Pak HLE for BattleTanx: Global Assault.
//
// The original cartridge has no save chip. Its profiles are libultra PFS
// files on a 32 KiB Controller Pak. The filesystem core is independently
// testable in btga/controller_pak.*; this file only translates the MIPS o32
// ABI and persists a standard emulator-compatible .mpk image.
#include "btga/controller_pak.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "librecomp/files.hpp"
#include "recomp.h"
#include "ultramodern/ultramodern.hpp"

namespace {
constexpr int max_ports = 4;
constexpr std::int32_t pfs_initialized = 1;
constexpr int pfs_status = 0x00;
constexpr int pfs_queue = 0x04;
constexpr int pfs_channel = 0x08;
constexpr int pfs_id = 0x0c;
constexpr int pfs_label = 0x2c;
constexpr int pfs_version = 0x4c;
constexpr int pfs_dir_size = 0x50;
constexpr int pfs_inode_table = 0x54;
constexpr int pfs_minode_table = 0x58;
constexpr int pfs_dir_table = 0x5c;
constexpr int pfs_inode_start = 0x60;
constexpr int pfs_banks = 0x64;
constexpr int pfs_active_bank = 0x65;

enum PfsError : std::int32_t {
    err_no_pak = 1,
    err_inconsistent = 3,
    err_controller = 4,
    err_invalid = 5,
    err_data_full = 7,
    err_directory_full = 8,
    err_exists = 9,
    err_id_fatal = 10,
};

std::mutex pak_mutex;
std::array<btga::ControllerPak, max_ports> paks;

gpr argument(recomp_context* ctx, int index) { return (&ctx->r4)[index]; }
gpr stack_argument(std::uint8_t* rdram, recomp_context* ctx, int index) {
    return static_cast<gpr>(static_cast<std::int32_t>(MEM_W(index * 4, ctx->r29)));
}
void result(recomp_context* ctx, std::int32_t value) { ctx->r2 = static_cast<gpr>(value); }

std::filesystem::path pak_path(int port) {
    const auto save = ultramodern::get_save_file_path();
    return save.parent_path() /
           (save.stem().string() + "_pak" + std::to_string(port + 1) + ".mpk");
}

void load_pak(int port) {
    std::ifstream input = recomp::open_input_file_with_backup(pak_path(port), std::ios::binary);
    if (!input.good()) {
        paks[port].format();
        return;
    }
    std::array<std::uint8_t, btga::ControllerPak::image_size> image{};
    input.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(image.size()));
    if (input.gcount() != static_cast<std::streamsize>(image.size()) ||
        !paks[port].import_image(image)) {
        paks[port].format();
    }
}

bool save_pak(int port) {
    const auto path = pak_path(port);
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream output = recomp::open_output_file_with_backup(path, std::ios::binary);
        if (!output.good()) return false;
        const auto image = paks[port].image();
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        if (!output.good()) return false;
    }
    return recomp::finalize_output_file_with_backup(path);
}

int port_from_pfs(std::uint8_t* rdram, gpr pfs) {
    if ((MEM_W(pfs_status, pfs) & pfs_initialized) == 0) return -err_invalid;
    const int port = MEM_W(pfs_channel, pfs);
    return port >= 0 && port < max_ports ? port : -err_invalid;
}

btga::ControllerPak::Name read_name(std::uint8_t* rdram, recomp_context* ctx) {
    btga::ControllerPak::Name name{};
    name.company_code = static_cast<std::uint16_t>(argument(ctx, 1));
    name.game_code = static_cast<std::uint32_t>(argument(ctx, 2));
    const gpr game_name = argument(ctx, 3);
    const gpr extension = stack_argument(rdram, ctx, 4);
    for (int i = 0; i < 16; ++i) name.game_name[i] = static_cast<std::uint8_t>(MEM_B(i, game_name));
    for (int i = 0; i < 4; ++i) name.extension[i] = static_cast<std::uint8_t>(MEM_B(i, extension));
    return name;
}

std::int32_t map_error(btga::ControllerPak::Error error) {
    using Error = btga::ControllerPak::Error;
    switch (error) {
    case Error::none: return 0;
    case Error::inconsistent: return err_inconsistent;
    case Error::data_full: return err_data_full;
    case Error::directory_full: return err_directory_full;
    case Error::exists: return err_exists;
    case Error::invalid: return err_invalid;
    }
    return err_invalid;
}
} // namespace

extern "C" void osPfsInitPak_recomp(std::uint8_t* rdram, recomp_context* ctx) {
    const gpr queue = argument(ctx, 0);
    const gpr pfs = argument(ctx, 1);
    const int port = static_cast<int>(argument(ctx, 2));
    if (port < 0 || port >= max_ports) { result(ctx, err_controller); return; }
    // Port 1 is the playable controller. Additional ports remain absent until
    // multiplayer input is configured rather than mirroring player one.
    // Player one defaults to a memory pak for saves. Additional connected
    // controllers are exposed as Rumble Paks; the game recognizes them when
    // PFS initialization returns the real ID-area failure code.
    if (port != 0) { result(ctx, err_id_fatal); return; }
    std::lock_guard lock{pak_mutex};
    load_pak(port);
    const auto image = paks[port].image();
    MEM_W(pfs_status, pfs) = pfs_initialized;
    MEM_W(pfs_queue, pfs) = static_cast<std::int32_t>(queue);
    MEM_W(pfs_channel, pfs) = port;
    for (int i = 0; i < 32; ++i) {
        MEM_B(pfs_id + i, pfs) = static_cast<std::int8_t>(image[32 + i]);
        MEM_B(pfs_label + i, pfs) = static_cast<std::int8_t>(image[i]);
    }
    MEM_W(pfs_version, pfs) = image[59];
    MEM_W(pfs_dir_size, pfs) = btga::ControllerPak::directory_entries;
    MEM_W(pfs_inode_table, pfs) = 8;
    MEM_W(pfs_minode_table, pfs) = 16;
    MEM_W(pfs_dir_table, pfs) = 24;
    MEM_W(pfs_inode_start, pfs) = 5;
    MEM_B(pfs_banks, pfs) = 1;
    MEM_B(pfs_active_bank, pfs) = 0;
    result(ctx, 0);
}

extern "C" void osPfsFreeBlocks_recomp(std::uint8_t* rdram, recomp_context* ctx) {
    const int port = port_from_pfs(rdram, argument(ctx, 0));
    if (port < 0) { result(ctx, -port); return; }
    std::lock_guard lock{pak_mutex};
    MEM_W(0, argument(ctx, 1)) = static_cast<std::int32_t>(paks[port].free_bytes());
    result(ctx, 0);
}

extern "C" void osPfsAllocateFile_recomp(std::uint8_t* rdram, recomp_context* ctx) {
    const int port = port_from_pfs(rdram, argument(ctx, 0));
    if (port < 0) { result(ctx, -port); return; }
    const auto name = read_name(rdram, ctx);
    const auto bytes = static_cast<std::int32_t>(stack_argument(rdram, ctx, 5));
    if (bytes <= 0) { result(ctx, err_invalid); return; }
    std::lock_guard lock{pak_mutex};
    int file_number = -1;
    const auto error = paks[port].allocate(name, static_cast<std::size_t>(bytes), file_number);
    if (error == btga::ControllerPak::Error::none) {
        MEM_W(0, stack_argument(rdram, ctx, 6)) = file_number;
        if (!save_pak(port)) { result(ctx, err_controller); return; }
    }
    result(ctx, map_error(error));
}

extern "C" void osPfsDeleteFile_recomp(std::uint8_t* rdram, recomp_context* ctx) {
    const int port = port_from_pfs(rdram, argument(ctx, 0));
    if (port < 0) { result(ctx, -port); return; }
    std::lock_guard lock{pak_mutex};
    const auto error = paks[port].remove(read_name(rdram, ctx));
    if (error == btga::ControllerPak::Error::none && !save_pak(port)) {
        result(ctx, err_controller); return;
    }
    result(ctx, map_error(error));
}

extern "C" void osPfsFileState_recomp(std::uint8_t* rdram, recomp_context* ctx) {
    const int port = port_from_pfs(rdram, argument(ctx, 0));
    if (port < 0) { result(ctx, -port); return; }
    const int file_number = static_cast<std::int32_t>(argument(ctx, 1));
    const gpr output = argument(ctx, 2);
    std::lock_guard lock{pak_mutex};
    btga::ControllerPak::FileState state{};
    const auto error = paks[port].state(file_number, state);
    if (error == btga::ControllerPak::Error::none) {
        MEM_W(0x00, output) = static_cast<std::int32_t>(state.size);
        MEM_W(0x04, output) = static_cast<std::int32_t>(state.name.game_code);
        MEM_H(0x08, output) = static_cast<std::int16_t>(state.name.company_code);
        for (int i = 0; i < 4; ++i) MEM_B(0x0a + i, output) = static_cast<std::int8_t>(state.name.extension[i]);
        for (int i = 0; i < 16; ++i) MEM_B(0x0e + i, output) = static_cast<std::int8_t>(state.name.game_name[i]);
    }
    result(ctx, map_error(error));
}

extern "C" void osPfsFindFile_recomp(std::uint8_t* rdram, recomp_context* ctx) {
    const int port = port_from_pfs(rdram, argument(ctx, 0));
    if (port < 0) { result(ctx, -port); return; }
    std::lock_guard lock{pak_mutex};
    int file_number = -1;
    const auto error = paks[port].find(read_name(rdram, ctx), file_number);
    MEM_W(0, stack_argument(rdram, ctx, 5)) = file_number;
    result(ctx, map_error(error));
}

extern "C" void osPfsReadWriteFile_recomp(std::uint8_t* rdram, recomp_context* ctx) {
    const int port = port_from_pfs(rdram, argument(ctx, 0));
    if (port < 0) { result(ctx, -port); return; }
    const int file_number = static_cast<std::int32_t>(argument(ctx, 1));
    const bool write = static_cast<std::uint8_t>(argument(ctx, 2)) != 0;
    const auto offset = static_cast<std::int32_t>(argument(ctx, 3));
    const auto size = static_cast<std::int32_t>(stack_argument(rdram, ctx, 4));
    const gpr buffer = stack_argument(rdram, ctx, 5);
    if (offset < 0 || size <= 0) { result(ctx, err_invalid); return; }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (write) for (int i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>(MEM_B(i, buffer));
    std::lock_guard lock{pak_mutex};
    const auto error = write
        ? paks[port].write(file_number, static_cast<std::size_t>(offset), bytes)
        : paks[port].read(file_number, static_cast<std::size_t>(offset), bytes);
    if (error == btga::ControllerPak::Error::none) {
        if (write) {
            if (!save_pak(port)) { result(ctx, err_controller); return; }
        } else {
            for (int i = 0; i < size; ++i) MEM_B(i, buffer) = static_cast<std::int8_t>(bytes[i]);
        }
    }
    result(ctx, map_error(error));
}

extern "C" void osPfsChecker_recomp(std::uint8_t* rdram, recomp_context* ctx) {
    const int port = port_from_pfs(rdram, argument(ctx, 0));
    result(ctx, port < 0 ? -port : 0);
}

extern "C" void osPfsNumFiles_recomp(std::uint8_t* rdram, recomp_context* ctx) {
    const int port = port_from_pfs(rdram, argument(ctx, 0));
    if (port < 0) { result(ctx, -port); return; }
    std::lock_guard lock{pak_mutex};
    MEM_W(0, argument(ctx, 1)) = btga::ControllerPak::directory_entries;
    MEM_W(0, argument(ctx, 2)) = paks[port].files_used();
    result(ctx, 0);
}

extern "C" void osPfsRepairId_recomp(std::uint8_t* rdram, recomp_context* ctx) {
    const int port = port_from_pfs(rdram, argument(ctx, 0));
    result(ctx, port < 0 ? -port : 0);
}
