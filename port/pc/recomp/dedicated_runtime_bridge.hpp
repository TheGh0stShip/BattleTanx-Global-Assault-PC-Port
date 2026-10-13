#pragma once

#include <cstdint>
#include <string>

#include "recomp.h"

bool btga_dedicated_enable_from_environment(std::string& error);
bool btga_dedicated_enable_public(std::string_view name, std::string& error);
extern "C" int btga_dedicated_enabled();
void btga_dedicated_network_poll();
bool btga_dedicated_controller_input(
    int controller, std::uint16_t& buttons, float& x, float& y);

extern "C" void btga_dedicated_tick_begin(
    std::uint8_t* rdram, recomp_context* context);
extern "C" void btga_dedicated_tick_end(
    std::uint8_t* rdram, recomp_context* context);
extern "C" void btga_dedicated_match_bootstrap(
    std::uint8_t* rdram, recomp_context* context);
extern "C" void btga_dedicated_run_loop(
    std::uint8_t* rdram, recomp_context* context);
