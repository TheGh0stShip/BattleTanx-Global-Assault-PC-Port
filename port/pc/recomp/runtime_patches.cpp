#include <cstdint>

#include "recomp.h"

extern "C" void yield_self_1ms(std::uint8_t* rdram);

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
