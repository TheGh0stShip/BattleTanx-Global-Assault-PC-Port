#ifndef BTGA_RUNTIME_COMPAT_H
#define BTGA_RUNTIME_COMPAT_H

#include <stdint.h>
#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

void __osCheckPackId_recomp(uint8_t*, recomp_context*);
void __osContAddressCrc_recomp(uint8_t*, recomp_context*);
void __osContRamRead_recomp(uint8_t*, recomp_context*);
void __osContRamWrite_recomp(uint8_t*, recomp_context*);
void __osDequeueThread_recomp(uint8_t*, recomp_context*);
void __osDispatchThread_recomp(uint8_t*, recomp_context*);
void __osGetSR_recomp(uint8_t*, recomp_context*);
void __osPfsRWInode_recomp(uint8_t*, recomp_context*);
void __osPfsSelectBank_recomp(uint8_t*, recomp_context*);
void __osRepairPackId_recomp(uint8_t*, recomp_context*);
void __osSiCreateAccessQueue_recomp(uint8_t*, recomp_context*);
void __udivdi3_recomp(uint8_t*, recomp_context*);
void __umoddi3_recomp(uint8_t*, recomp_context*);
void func_80098B40(uint8_t*, recomp_context*);
void func_800A1384(uint8_t*, recomp_context*);

#ifdef __cplusplus
}
#endif

#endif
