#include <algorithm>
#include <bit>
#include <cstdint>
#include <unordered_map>

#include "recomp.h"

extern "C" void yield_self_1ms(std::uint8_t* rdram);
extern "C" bool btga_extended_visibility_enabled();
extern "C" bool btga_independent_turret_enabled();
extern "C" float btga_turret_stick_x(int player);

namespace {
float read_guest_float(std::uint8_t* rdram, std::uint32_t address) {
    const gpr guest_address = static_cast<gpr>(static_cast<std::int32_t>(address));
    return std::bit_cast<float>(static_cast<std::uint32_t>(MEM_W(0, guest_address)));
}
}

extern "C" void btga_shield_flash_step(std::uint8_t* rdram, recomp_context* ctx) {
    // The retail routine truncates the frame step before subtracting it from
    // the shield-hit flash lifetime. On N64 the step is just over 1.0; at an
    // exact host 60 Hz it can be just under 1.0 and truncate to zero, leaving
    // every translucent shield shard alive forever. Preserve the intended
    // elapsed-time behavior by supplying the original fractional step.
    constexpr std::uint32_t frame_step_address = 0x80219488U;
    ctx->f2.fl = read_guest_float(rdram, frame_step_address);
}

extern "C" void btga_turn_step(std::uint8_t* rdram, recomp_context* ctx) {
    constexpr std::uint32_t frame_step_address = 0x80219488U;
    const float step = read_guest_float(rdram, frame_step_address);
    const std::int32_t amount = step > 0.0F ? static_cast<std::int32_t>(step * 1024.0F) : 0;
    ctx->r6 = std::min(amount, 0xFFFF);
}

extern "C" void btga_deadline_step(std::uint8_t* rdram, recomp_context* ctx) {
    constexpr std::uint32_t frame_step_address = 0x80219488U;
    static std::unordered_map<std::uint32_t, float> fractional_steps;
    float& remainder = fractional_steps[static_cast<std::uint32_t>(ctx->r20)];
    const float total = read_guest_float(rdram, frame_step_address) + remainder;
    const std::int32_t whole = total > 0.0F ? static_cast<std::int32_t>(total) : 0;
    remainder = total - static_cast<float>(whole);
    ctx->r3 = whole;
}

extern "C" void btga_score_count_step(std::uint8_t* rdram, recomp_context* ctx) {
    constexpr std::uint32_t hud_step_address = 0x803A5948U;
    const float step = read_guest_float(rdram, hud_step_address);
    if (static_cast<std::int32_t>(ctx->r3) == 0 && step > 0.0F) {
        ctx->r3 = 1;
    }
}

extern "C" void btga_cutscene_far_plane(std::uint8_t*, recomp_context* ctx) {
    // The cutscene camera reads the gameplay draw-distance global before it
    // has necessarily been initialized. Avoid a degenerate near=16/far=0
    // matrix while preserving every valid scripted or gameplay distance.
    constexpr std::int32_t near_plane = 16;
    constexpr std::int32_t fallback_far_plane = 32767;
    if (static_cast<std::int32_t>(ctx->r3) <= near_plane) {
        ctx->r3 = fallback_far_plane;
    }
}

extern "C" void btga_visibility_mod_step(std::uint8_t* rdram, recomp_context*) {
    if (!btga_extended_visibility_enabled()) return;
    constexpr gpr far_plane = static_cast<gpr>(static_cast<std::int32_t>(0x8023A060U));
    // 32767 is already used by the port's safe cutscene fallback and remains
    // representable by all original signed distance paths.
    MEM_W(0, far_plane) = 32767;
}

extern "C" void btga_disable_fog_step(std::uint8_t*, recomp_context* ctx) {
    if (btga_extended_visibility_enabled()) {
        // func_8007A250 branches over the geometry-mode, fog-color and
        // fog-position commands when func_800B0444 returns zero.
        ctx->r2 = 0;
    }
}

extern "C" void btga_projection_far_step(std::uint8_t*, recomp_context* ctx) {
    if (btga_extended_visibility_enabled()) ctx->f4.fl = 32767.0F;
}

extern "C" void btga_independent_turret_step(std::uint8_t* rdram, recomp_context* ctx) {
    if (!btga_independent_turret_enabled()) return;
    const gpr tank = ctx->r18; // func_80091768 keeps its Tank * in s2.
    if (tank == 0) return;
    const std::uint32_t owner = static_cast<std::uint32_t>(MEM_W(0x1D0, tank));
    if (owner == 0) return;
    const int player = MEM_BU(0x0B, static_cast<gpr>(owner));
    const std::int32_t kind = static_cast<std::int32_t>(MEM_W(0x98, tank));
    if (kind < 0) return;
    constexpr gpr definitions = static_cast<gpr>(static_cast<std::int32_t>(0x80122E8CU));
    constexpr std::int32_t definition_stride = 0xD0;
    const gpr definition = ADD32(definitions, kind * definition_stride);
    if ((MEM_W(0x2C, definition) & 1) == 0) return; // No rotating turret.

    const float input = btga_turret_stick_x(player);
    if (input == 0.0F) return;
    constexpr gpr frame_step = static_cast<gpr>(static_cast<std::int32_t>(0x80219488U));
    const float step = read_guest_float(rdram, static_cast<std::uint32_t>(frame_step));
    const std::int32_t delta = static_cast<std::int32_t>(input * step * 1024.0F);
    MEM_H(0x24, tank) = static_cast<std::uint16_t>(MEM_HU(0x24, tank) - delta);
}

extern "C" void btga_turret_camera_step(std::uint8_t* rdram, recomp_context* ctx) {
    if (!btga_independent_turret_enabled()) return;
    const gpr tank = ctx->r4;
    const gpr query_mode = ctx->r6;
    if (tank == 0 || query_mode == 0 || (MEM_W(0x1E0, tank) & 2) == 0) return;
    const std::int32_t kind = static_cast<std::int32_t>(MEM_W(0x98, tank));
    if (kind < 0) return;
    constexpr gpr definitions = static_cast<gpr>(static_cast<std::int32_t>(0x80122E8CU));
    const gpr definition = ADD32(definitions, kind * 0xD0);
    if ((MEM_W(0x2C, definition) & 1) == 0) return;
    // Camera transform queries normally request the hull frame (mode 0).
    // Mode 1 is the game's existing turret transform, also used by the intro
    // presentation, so this reuses retail camera math and smoothing.
    MEM_W(0, query_mode) = 1;
}

extern "C" void func_80098B40(std::uint8_t* rdram, recomp_context*) {
    constexpr gpr controller_read_pending = static_cast<gpr>(static_cast<std::int32_t>(0x80219230U));
    while (MEM_W(0, controller_read_pending) != 0) {
        // The R4300 is preempted by VI interrupts in this loop. Cooperatively
        // yield on the host so the scheduler and controller worker can run.
        yield_self_1ms(rdram);
    }
}

extern "C" void func_800A1384(std::uint8_t* rdram, recomp_context*) {
    constexpr gpr scheduler_state = static_cast<gpr>(static_cast<std::int32_t>(0x80222930U));
    for (;;) {
        const std::uint16_t kind = MEM_HU(0x1F0, scheduler_state);
        const std::uint16_t valid = MEM_HU(0x1F4, scheduler_state);
        const std::int32_t pointer = MEM_W(0x1FC, scheduler_state);
        if (valid == 0 && pointer == 0 && kind >= 5) {
            return;
        }
        // The original repeatedly snapshots this record until an interrupt
        // updates it. Let the cooperative scheduler deliver that interrupt.
        yield_self_1ms(rdram);
    }
}

extern "C" void func_80079FF0(std::uint8_t* rdram, recomp_context*) {
    constexpr gpr state_global = static_cast<gpr>(static_cast<std::int32_t>(0x801144F8U));
    const gpr state = MEM_W(0, state_global);
    const gpr source = ADD32(state, 0xC8);

    while (MEM_W(0xC4, MEM_W(0, state_global)) != 0) {
        // The original loop is interrupted by the N64 scheduler. Yield to the
        // cooperative host scheduler so the task-completion interrupt can run.
        yield_self_1ms(rdram);
    }

    const gpr destination = ADD32(state, 0x90 + (MEM_HU(0xC0, state) << 4));
    MEM_W(0, source) = MEM_W(4, destination);
    MEM_W(4, source) = MEM_W(8, destination);
    MEM_W(8, source) = ADD32(MEM_W(12, destination), 0x40);
}

namespace {
std::int16_t find_free_render_slot(std::uint8_t* rdram, gpr context) {
    for (std::int16_t index = 0; index < 3; ++index) {
        if (index != MEM_H(0xB6, context) &&
            index != MEM_H(0xB4, context) &&
            index != MEM_H(0xB2, context)) {
            return index;
        }
    }
    return -1;
}
}

extern "C" void func_8007A818(std::uint8_t* rdram, recomp_context*) {
    constexpr gpr context_global = static_cast<gpr>(static_cast<std::int32_t>(0x80114500U));
    const gpr context = MEM_W(0, context_global);

    while (MEM_H(0xB0, context) != -1) {
        yield_self_1ms(rdram);
    }

    while (find_free_render_slot(rdram, context) == -1) {
        yield_self_1ms(rdram);
    }

    const gpr current_context = MEM_W(0, context_global);
    MEM_H(0xB0, current_context) = find_free_render_slot(rdram, current_context);
}
