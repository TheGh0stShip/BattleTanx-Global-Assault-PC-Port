#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>

#include "recomp.h"
#include "btga/gameplay_digest.hpp"
#include "btga/online_bootstrap.hpp"
#include "dedicated_runtime_bridge.hpp"
#include "dedicated_client_bridge.hpp"

namespace btga::renderer {
bool online_simulation_frame_barrier();
}

extern "C" void yield_self_1ms(std::uint8_t* rdram);
extern "C" bool btga_extended_visibility_enabled();
extern "C" bool btga_independent_turret_enabled();
extern "C" float btga_turret_stick_x(int player);
extern "C" bool btga_turret_aim_active(int player);
extern "C" float btga_screen_shake_intensity();
extern "C" float btga_music_volume_setting();
extern "C" float btga_effects_volume_setting();
extern "C" void func_8009C524(std::uint8_t* rdram, recomp_context* ctx);

namespace {
std::int32_t effects_master_requested = 0x7FFF;
std::int32_t music_master_requested = 0x7FFF;

struct OnlineClockSample {
    std::uint8_t* rdram = nullptr;
    std::uint32_t frame = 0;
};

thread_local OnlineClockSample online_clock_sample;

constexpr std::size_t n64_rdram_size = 8U * 1024U * 1024U;

struct ReplayEquivalenceProbe {
    std::unique_ptr<std::uint8_t[]> rdram;
    recomp_context entry_context{};
    recomp_context first_exit_context{};
    recomp_context replay_exit_context{};
    bool captured = false;
    bool replaying = false;
    bool replay_end_seen = false;
    bool finished = false;
};

thread_local ReplayEquivalenceProbe replay_probe;

bool replay_probe_requested() {
    static const bool requested = [] {
        const char* value = std::getenv("BTGA_REPLAY_EQUIVALENCE_PROBE");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return requested;
}

bool deterministic_network_clock_enabled() {
    return btga::preboot_online_seed().has_value() ||
           btga_dedicated_enabled() != 0 ||
           btga_dedicated_client_enabled() != 0;
}

constexpr std::uint64_t synthetic_n64_clock(std::uint32_t frame) {
    constexpr std::uint64_t n64_ticks_per_network_frame = UINT64_C(1'562'500);
    return static_cast<std::uint64_t>(frame) * n64_ticks_per_network_frame;
}

std::uint64_t fnv1a64(const std::uint8_t* bytes, std::size_t size) {
    std::uint64_t hash = UINT64_C(1469598103934665603);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

void report_replay_difference(const std::uint8_t* expected,
                              const std::uint8_t* actual,
                              bool context_equal) {
    std::size_t first = n64_rdram_size;
    std::size_t count = 0;
    for (std::size_t index = 0; index < n64_rdram_size; ++index) {
        if (expected[index] == actual[index]) continue;
        if (first == n64_rdram_size) first = index;
        ++count;
    }
    std::fprintf(stderr,
                 "BTGA_REPLAY_PROBE FAIL rdram_differences=%zu first=0x%06zx "
                 "expected_hash=%016llx actual_hash=%016llx context_equal=%d\n",
                 count, first,
                 static_cast<unsigned long long>(fnv1a64(expected, n64_rdram_size)),
                 static_cast<unsigned long long>(fnv1a64(actual, n64_rdram_size)),
                 context_equal ? 1 : 0);
    std::fflush(stderr);
}

float read_guest_float(std::uint8_t* rdram, std::uint32_t address) {
    const gpr guest_address = static_cast<gpr>(static_cast<std::int32_t>(address));
    return std::bit_cast<float>(static_cast<std::uint32_t>(MEM_W(0, guest_address)));
}

extern "C" void btga_online_seed_step(std::uint8_t*, recomp_context* ctx) {
    if (const auto seed = btga::preboot_online_seed()) ctx->r4 = *seed;
}

extern "C" void btga_replay_probe_begin(std::uint8_t* rdram, recomp_context* ctx) {
    btga_dedicated_tick_begin(rdram, ctx);
    btga_dedicated_client_tick_begin(rdram, ctx);
    if (replay_probe.replaying || replay_probe.finished || !replay_probe_requested()) return;
    if (!btga::preboot_online_seed()) return;
    if (!btga::gameplay_frame_input_active({rdram, n64_rdram_size})) return;
    if (!replay_probe.rdram) replay_probe.rdram = std::make_unique<std::uint8_t[]>(n64_rdram_size);
    std::memcpy(replay_probe.rdram.get(), rdram, n64_rdram_size);
    replay_probe.entry_context = *ctx;
    replay_probe.captured = true;
}

extern "C" void btga_replay_probe_end(std::uint8_t* rdram, recomp_context* ctx) {
    btga_dedicated_tick_end(rdram, ctx);
    btga_dedicated_client_tick_end(rdram, ctx);
    if (replay_probe.replaying) {
        replay_probe.replay_exit_context = *ctx;
        replay_probe.replay_end_seen = true;
        return;
    }
    if (replay_probe.finished || !replay_probe.captured) return;

    std::unique_ptr<std::uint8_t[]> first_result =
        std::make_unique<std::uint8_t[]>(n64_rdram_size);
    std::memcpy(first_result.get(), rdram, n64_rdram_size);
    replay_probe.first_exit_context = *ctx;

    std::memcpy(rdram, replay_probe.rdram.get(), n64_rdram_size);
    *ctx = replay_probe.entry_context;
    replay_probe.replaying = true;
    replay_probe.replay_end_seen = false;
    func_8009C524(rdram, ctx);
    replay_probe.replaying = false;

    const bool rdram_equal =
        std::memcmp(first_result.get(), rdram, n64_rdram_size) == 0;
    const bool context_equal = replay_probe.replay_end_seen &&
        std::memcmp(&replay_probe.first_exit_context,
                    &replay_probe.replay_exit_context,
                    sizeof(recomp_context)) == 0;
    if (rdram_equal && context_equal) {
        std::fprintf(stderr,
                     "BTGA_REPLAY_PROBE PASS hash=%016llx bytes=%zu\n",
                     static_cast<unsigned long long>(fnv1a64(rdram, n64_rdram_size)),
                     n64_rdram_size);
        std::fflush(stderr);
    } else {
        report_replay_difference(first_result.get(), rdram, context_equal);
        if (!context_equal) {
            std::fprintf(stderr, "BTGA_REPLAY_PROBE FAIL cpu_context_differs=1\n");
            std::fflush(stderr);
        }
    }

    // The outer invocation still has its epilogue to execute.  Preserve its
    // pre-epilogue CPU context while retaining the replayed RDRAM result.
    *ctx = replay_probe.first_exit_context;
    replay_probe.finished = true;
    replay_probe.captured = false;
}

extern "C" void btga_online_clock_begin(std::uint8_t* rdram, recomp_context*) {
    if (replay_probe.replaying) return;
    if (!deterministic_network_clock_enabled()) return;
    // The legacy peer mode needs a frame rendezvous. Dedicated sessions are
    // already paced by canonical authority frames and must not enter the
    // peer-to-peer renderer barrier.
    if (btga::preboot_online_seed()) {
        (void)btga::renderer::online_simulation_frame_barrier();
    }
    constexpr gpr frame_counter = static_cast<gpr>(static_cast<std::int32_t>(0x8021945CU));
    online_clock_sample.rdram = rdram;
    online_clock_sample.frame = static_cast<std::uint32_t>(MEM_W(0, frame_counter));
}

extern "C" void btga_online_clock_source(std::uint8_t* rdram, recomp_context* ctx) {
    if (!deterministic_network_clock_enabled()) return;
    constexpr gpr frame_counter = static_cast<gpr>(static_cast<std::int32_t>(0x8021945CU));
    const auto frame = static_cast<std::uint32_t>(MEM_W(0, frame_counter));
    const std::uint64_t clock = synthetic_n64_clock(frame);
    ctx->r2 = static_cast<std::uint32_t>(clock >> 32U);
    ctx->r3 = static_cast<std::uint32_t>(clock);
}

extern "C" void btga_online_clock_end(std::uint8_t* rdram, recomp_context*) {
    if (replay_probe.replaying) {
        constexpr gpr frame_counter = static_cast<gpr>(static_cast<std::int32_t>(0x8021945CU));
        constexpr gpr frame_step = static_cast<gpr>(static_cast<std::int32_t>(0x80219488U));
        constexpr gpr time_mode = static_cast<gpr>(static_cast<std::int32_t>(0x802195CCU));
        const std::uint32_t mode = static_cast<std::uint32_t>(MEM_W(0, time_mode));
        const bool paused = mode == 5U || mode == 6U || mode == 7U;
        MEM_W(0, frame_counter) = static_cast<std::int32_t>(
            online_clock_sample.frame + (paused ? 0U : 1U));
        MEM_W(0, frame_step) = static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(1.0F));
        return;
    }
    if (!deterministic_network_clock_enabled() ||
        online_clock_sample.rdram != rdram) return;
    constexpr gpr frame_counter = static_cast<gpr>(static_cast<std::int32_t>(0x8021945CU));
    constexpr gpr frame_step = static_cast<gpr>(static_cast<std::int32_t>(0x80219488U));
    constexpr gpr time_mode = static_cast<gpr>(static_cast<std::int32_t>(0x802195CCU));
    const std::uint32_t mode = static_cast<std::uint32_t>(MEM_W(0, time_mode));
    const bool paused = mode == 5U || mode == 6U || mode == 7U;
    MEM_W(0, frame_counter) = static_cast<std::int32_t>(
        online_clock_sample.frame + (paused ? 0U : 1U));
    MEM_W(0, frame_step) = static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(1.0F));
    const std::uint64_t clock = synthetic_n64_clock(online_clock_sample.frame);
    const auto high = static_cast<std::int32_t>(clock >> 32U);
    const auto low = static_cast<std::int32_t>(clock);
    for (const std::uint32_t address : {UINT32_C(0x80219250),
                                        UINT32_C(0x80219478),
                                        UINT32_C(0x80219480)}) {
        const gpr target = static_cast<gpr>(static_cast<std::int32_t>(address));
        MEM_W(0, target) = high;
        MEM_W(4, target) = low;
    }
}

void apply_audio_bus_volumes(std::uint8_t* rdram) {
    constexpr gpr effects_master = static_cast<gpr>(static_cast<std::int32_t>(0x803AD98CU));
    constexpr gpr music_master = static_cast<gpr>(static_cast<std::int32_t>(0x803AD98EU));
    const auto effects = static_cast<std::int32_t>(effects_master_requested * btga_effects_volume_setting());
    const auto music = static_cast<std::int32_t>(music_master_requested * btga_music_volume_setting());
    MEM_H(0, effects_master) = std::clamp(effects, 0, 0x7FFF);
    MEM_H(0, music_master) = std::clamp(music, 0, 0x7FFF);
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

extern "C" void btga_turret_right_test(std::uint8_t* rdram, recomp_context* ctx) {
    if (!btga_independent_turret_enabled()) return;
    // This runs after retail has evaluated btnTurretR and before its branch.
    // Override only the boolean result; no guest object or pad is modified.
    const int controller = MEM_BU(0x95, ctx->r18);
    if (controller < 4 && btga_turret_stick_x(controller) < 0.0F) ctx->r2 = 1;
}

extern "C" void btga_turret_left_test(std::uint8_t* rdram, recomp_context* ctx) {
    if (!btga_independent_turret_enabled()) return;
    // Positive mouse/right-stick motion matches the former direct hook's
    // decreasing-angle direction, which is retail's btnTurretL path.
    const int controller = MEM_BU(0x95, ctx->r18);
    if (controller < 4 && btga_turret_stick_x(controller) > 0.0F) ctx->r2 = 1;
}

extern "C" void btga_turret_control_scheme_step(std::uint8_t* rdram, recomp_context* ctx) {
    if (!btga_independent_turret_enabled()) return;
    const int controller = MEM_BU(0x95, ctx->r18);
    if (controller >= 4 || !btga_turret_aim_active(controller)) return;
    // Scheme 2 deliberately turns the hull toward the turret while moving.
    // While aim mode is held, use retail's normal hull-steering path instead:
    // W/S move on the hull heading and A/D turn it. Once aim mode is released,
    // leave this register untouched and the selected retail scheme resumes.
    ctx->r3 = 0;
}

extern "C" void btga_turret_camera_step(std::uint8_t* rdram, recomp_context* ctx) {
    // Forcing transform mode 1 globally also affects vehicle previews during
    // campaign selection and can stall the guest state before gameplay begins.
    // Keep this hook inert until it can be gated by the active gameplay camera;
    // independent turret steering itself is handled by the input-update hook.
    (void)rdram;
    (void)ctx;
}

extern "C" void btga_screen_shake_step(std::uint8_t*, recomp_context* ctx) {
    // func_800A6588 passes the current shake amplitude in f12 to each of its
    // three random camera-offset calls. Scale that transient argument while
    // leaving the retail shake state and decay untouched.
    ctx->f12.fl *= btga_screen_shake_intensity();
}

extern "C" void MusSetMasterVolume(std::uint8_t* rdram, recomp_context* ctx) {
    const std::int32_t type = static_cast<std::int32_t>(ctx->r4);
    const std::int32_t requested = std::clamp(static_cast<std::int32_t>(ctx->r5), 0, 0x7FFF);
    if (type & 1) effects_master_requested = requested;
    if (type & 2) music_master_requested = requested;
    apply_audio_bus_volumes(rdram);
}

extern "C" void btga_audio_volume_step(std::uint8_t* rdram, recomp_context*) {
    apply_audio_bus_volumes(rdram);
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
