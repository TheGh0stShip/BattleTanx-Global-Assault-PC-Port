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
