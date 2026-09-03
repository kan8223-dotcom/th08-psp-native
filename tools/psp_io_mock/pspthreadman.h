#pragma once

#include <cstdint>

using SceUID = int;
using SceUInt32 = std::uint32_t;
using u32 = std::uint32_t;

enum PspLwMutexAttributes
{
    PSP_LW_MUTEX_ATTR_THFIFO = 0x0000U,
};

struct SceLwMutexWorkarea
{
    std::uint32_t opaque[8];
};

extern "C" {
int sceKernelCreateLwMutex(SceLwMutexWorkarea *workarea,
                           const char *name,
                           SceUInt32 attributes,
                           int initialCount,
                           u32 *options);
int sceKernelLockLwMutex(SceLwMutexWorkarea *workarea,
                         int lockCount,
                         unsigned int *timeout);
int sceKernelUnlockLwMutex(SceLwMutexWorkarea *workarea, int lockCount);
}
