#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "runtime_compat.h"

namespace {
[[noreturn]] void unsupported(const char* name) {
    std::cerr << "Unsupported translated libultra internal reached: " << name << '\n';
    std::abort();
}
}

extern "C" void __osGetSR_recomp(std::uint8_t*, recomp_context* ctx) {
    ctx->r2 = cop0_status_read(ctx);
}

#define BTGA_UNSUPPORTED_RECOMP(name) \
    extern "C" void name(std::uint8_t*, recomp_context*) { unsupported(#name); }

BTGA_UNSUPPORTED_RECOMP(__osCheckPackId_recomp)
BTGA_UNSUPPORTED_RECOMP(__osContAddressCrc_recomp)
BTGA_UNSUPPORTED_RECOMP(__osContRamRead_recomp)
BTGA_UNSUPPORTED_RECOMP(__osContRamWrite_recomp)
BTGA_UNSUPPORTED_RECOMP(__osDequeueThread_recomp)
BTGA_UNSUPPORTED_RECOMP(__osDispatchThread_recomp)
BTGA_UNSUPPORTED_RECOMP(__osPfsRWInode_recomp)
BTGA_UNSUPPORTED_RECOMP(__osPfsSelectBank_recomp)
BTGA_UNSUPPORTED_RECOMP(__osRepairPackId_recomp)
BTGA_UNSUPPORTED_RECOMP(__osSiCreateAccessQueue_recomp)

#undef BTGA_UNSUPPORTED_RECOMP
