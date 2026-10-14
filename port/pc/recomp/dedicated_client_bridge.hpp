#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "btga/master_client.hpp"
#include "recomp.h"

bool btga_dedicated_client_enable(
    std::uint64_t session_id, const btga::RelayEndpoint& relay,
    std::string_view token, std::string& error);
extern "C" int btga_dedicated_client_enabled();
extern "C" void btga_dedicated_client_tick_begin(
    std::uint8_t* rdram, recomp_context* context);
extern "C" void btga_dedicated_client_tick_end(
    std::uint8_t* rdram, recomp_context* context);
extern "C" void btga_dedicated_client_run_loop(
    std::uint8_t* rdram, recomp_context* context);
bool btga_dedicated_client_controller_input(
    int controller, std::uint16_t& buttons, float& x, float& y);
int btga_dedicated_client_local_seat();
void btga_dedicated_client_pump();
void btga_dedicated_client_submit_local(
    std::uint16_t buttons, float x, float y);
