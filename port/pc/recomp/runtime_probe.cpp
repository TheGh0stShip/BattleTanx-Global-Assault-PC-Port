#include <cstdint>
#include <iostream>

#include "funcs.h"
#include "librecomp/game.hpp"

gpr get_entrypoint_address();

int main() {
    const recomp::GameEntry game{
        .rom_hash = 0,
        .internal_name = "BATTLETANX2",
        .display_name = "BattleTanx: Global Assault",
        .game_id = u8"battletanx-ga.n64.us.1.0",
        .mod_game_id = "battletanx-ga",
        .save_type = recomp::SaveType::Eep16k,
        .thumbnail_bytes = {},
        .is_enabled = true,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
    };

    if (!recomp::register_game(game)) {
        std::cerr << "Failed to register BattleTanx: Global Assault with the runtime\n";
        return 1;
    }

    std::cout << "Registered all translated game code at entrypoint 0x"
              << std::hex << static_cast<std::uint32_t>(game.entrypoint_address) << '\n';
    return 0;
}
