#include "backbuffer_shadow.hpp"

#include "anm_scratch.hpp"
#include "fileio.hpp"

#include <cstdint>

namespace th08::psp
{
namespace
{
constexpr std::size_t kNativeReadbackBytes = 480U * 272U * 2U;
constexpr std::size_t kAlignment = 64U;
unsigned char *gBase = nullptr;
std::size_t gBytes = 0U;
unsigned char *gNative = nullptr;
unsigned long gAcquires = 0UL;
unsigned long gFailures = 0UL;

std::size_t AlignUp(std::size_t value)
{
    return (value + kAlignment - 1U) & ~(kAlignment - 1U);
}
} // namespace

bool BackbufferShadowAvailable()
{
    return gBase == nullptr && !AnmScratchBusy() && !AnmScratchPoisoned();
}

void *BackbufferShadowAcquire(std::size_t bytes)
{
    if (gBase != nullptr || bytes == 0U)
        return nullptr;
    const std::size_t shadowBytes = AlignUp(bytes);
    const std::size_t total = shadowBytes + kNativeReadbackBytes;
    if (total > AnmScratchCapacity())
        return nullptr;
    if (!AnmScratchReserveTransition("backbuffer shadow"))
    {
        ++gFailures;
        return nullptr;
    }
    void *base = AnmScratchTransitionBase();
    if (base == nullptr || !AnmScratchTransitionSetActiveBytes(total))
    {
        AnmScratchReleaseTransition();
        ++gFailures;
        return nullptr;
    }
    gBase = static_cast<unsigned char *>(base);
    gBytes = bytes;
    gNative = gBase + shadowBytes;
    ++gAcquires;
    if (gAcquires <= 4UL || (gAcquires % 64UL) == 0UL)
        BootLog("BACKBUFFER_SHADOW source=scratch bytes=%lu native=%lu acquires=%lu failures=%lu\n",
                static_cast<unsigned long>(bytes), static_cast<unsigned long>(kNativeReadbackBytes),
                gAcquires, gFailures);
    return gBase;
}

bool BackbufferShadowRelease(void *memory)
{
    if (gBase == nullptr || memory != gBase)
        return false;
    gBase = nullptr;
    gBytes = 0U;
    gNative = nullptr;
    if (!AnmScratchReleaseTransition())
        BootLog("BACKBUFFER_SHADOW release_failed\n");
    return true;
}

void *BackbufferShadowNativeBuffer()
{
    return gNative;
}

void *BackbufferShadowBase()
{
    return gBase;
}

bool BackbufferShadowHeld()
{
    return gBase != nullptr;
}
} // namespace th08::psp
