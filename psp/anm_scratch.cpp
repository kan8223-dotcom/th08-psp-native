#include "anm_scratch.hpp"

#include "fileio.hpp"

#include <pspkernel.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace th08::psp
{
namespace
{
// Official 1.00c th08.dat: eff09sk.anm is the largest decompressed ANM at
// 8,520,496 bytes. Reserve one early, fixed-size phase scratch with full-page
// guards on both sides. Resource bytes and texture quality remain unchanged.
constexpr std::size_t kLargestOriginalAnmBytes = 8520496U;
// TH08_PSP_ANM_SCRATCH_COMPACT: only the Last Word effect sets (eff09*.anm,
// 8,520,496 B) need the full reserve.  Every stage 1-6 and Extra resource fits
// in 2,886,088 B (stg6bg.anm), so the compact reserve keeps ~5 MiB of heap
// for the game.  Oversized ANMs fail closed at acquire time (R-056).
#if defined(TH08_PSP_ANM_SCRATCH_COMPACT) && TH08_PSP_ANM_SCRATCH_COMPACT
constexpr std::size_t kLargestSupportedAnmBytes = 2886088U;
constexpr std::size_t kReservedBytes = 0x310000U;
#else
constexpr std::size_t kLargestSupportedAnmBytes = kLargestOriginalAnmBytes;
constexpr std::size_t kReservedBytes = 0x840000U;
#endif
constexpr std::size_t kArenaAlignment = 64U;
constexpr std::size_t kGuardBytes = 0x1000U;
constexpr std::size_t kArenaBytes = kReservedBytes - (2U * kGuardBytes);
constexpr unsigned char kGuardValue = 0xa5U;

static_assert(kArenaBytes >= kLargestSupportedAnmBytes,
              "ANM phase scratch must hold the largest original resource");

unsigned char *gRawAllocation = nullptr;
unsigned char *gAlignedAllocation = nullptr;
unsigned char *gArena = nullptr;
enum ScratchState
{
    ScratchState_Idle = 0,
    ScratchState_Anm = 1,
    ScratchState_Transition = 2,
};
volatile int gBusy = ScratchState_Idle;
volatile int gPoisoned = 0;
volatile std::uint32_t gActiveBytes = 0;
volatile std::uint32_t gGeneration = 0;
volatile int gOwnerIndex = -1;
char gOwner[48]{};
constexpr std::uint64_t kAcquireTimeoutUs = 5000000ULL;

bool EndsWithAnm(const char *owner)
{
    if (owner == nullptr)
        return false;
    const std::size_t length = std::strlen(owner);
    if (length < 4)
        return false;
    const char *suffix = owner + length - 4;
    const char a = static_cast<char>(suffix[1] | 0x20);
    const char n = static_cast<char>(suffix[2] | 0x20);
    const char m = static_cast<char>(suffix[3] | 0x20);
    return suffix[0] == '.' && a == 'a' && n == 'n' && m == 'm';
}

void ResetGuard()
{
    if (gArena != nullptr)
    {
        std::memset(gArena - kGuardBytes, kGuardValue, kGuardBytes);
        std::memset(gArena + kArenaBytes, kGuardValue, kGuardBytes);
    }
}

bool GuardRegionIntact(const unsigned char *guard)
{
    if (guard == nullptr)
        return false;
    for (std::size_t i = 0; i < kGuardBytes; ++i)
    {
        if (guard[i] != kGuardValue)
            return false;
    }
    return true;
}

void CopyOwner(const char *owner)
{
    if (owner == nullptr)
        owner = "unknown";
    std::size_t i = 0;
    while (i + 1 < sizeof(gOwner) && owner[i] != '\0')
    {
        gOwner[i] = owner[i];
        ++i;
    }
    gOwner[i] = '\0';
}
} // namespace

bool AnmScratchInitialize()
{
    if (gArena != nullptr)
        return true;

    const std::size_t allocationBytes = kReservedBytes + kArenaAlignment - 1U;
    gRawAllocation = static_cast<unsigned char *>(std::malloc(allocationBytes));
    if (gRawAllocation == nullptr)
    {
        BootLog("ANM_SCRATCH init=FAILED request=%lu\n",
                static_cast<unsigned long>(allocationBytes));
        return false;
    }

    const std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(gRawAllocation);
    const std::uintptr_t aligned = (raw + kArenaAlignment - 1U) & ~(kArenaAlignment - 1U);
    gAlignedAllocation = reinterpret_cast<unsigned char *>(aligned);
    gArena = gAlignedAllocation + kGuardBytes;
    ResetGuard();
    BootLog("ANM_SCRATCH init=READY base=0x%08lx capacity=%lu reserved=%lu max_original=%lu guards=%lu retained=1\n",
            static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(gArena)),
            static_cast<unsigned long>(kArenaBytes),
            static_cast<unsigned long>(kReservedBytes),
            static_cast<unsigned long>(kLargestSupportedAnmBytes),
            static_cast<unsigned long>(kGuardBytes));
    return true;
}

bool AnmScratchTryAcquire(std::size_t bytes, int anmIndex, const char *owner,
                          AnmScratchLease *outLease)
{
    if (outLease != nullptr)
        *outLease = AnmScratchLease{};
    if (gArena == nullptr || outLease == nullptr || anmIndex < 0 ||
        !EndsWithAnm(owner) || bytes > kArenaBytes || gPoisoned != 0)
    {
        if (owner != nullptr && bytes > kArenaBytes)
        {
            BootLog("ANM_SCRATCH reject owner=%s bytes=%lu capacity=%lu\n", owner,
                    static_cast<unsigned long>(bytes), static_cast<unsigned long>(kArenaBytes));
            FlushBootLog();
        }
        return false;
    }
    const std::uint64_t waitStarted = sceKernelGetSystemTimeWide();
    bool reportedWait = false;
    while (!__sync_bool_compare_and_swap(&gBusy, ScratchState_Idle,
                                         ScratchState_Anm))
    {
        if (gPoisoned != 0 ||
            sceKernelGetSystemTimeWide() - waitStarted >= kAcquireTimeoutUs)
        {
            std::fprintf(stderr,
                         "TH08PSP ANM_SCRATCH acquire_timeout anm=%d owner=%s request=%lu state=%d active=%lu generation=%lu poisoned=%d\n",
                         anmIndex, owner != nullptr ? owner : "unknown",
                         static_cast<unsigned long>(bytes), static_cast<int>(gBusy),
                         static_cast<unsigned long>(gActiveBytes),
                         static_cast<unsigned long>(gGeneration),
                         gPoisoned != 0 ? 1 : 0);
            return false;
        }
        if (!reportedWait)
        {
            std::fprintf(stderr,
                         "TH08PSP ANM_SCRATCH acquire_wait anm=%d owner=%s state=%d generation=%lu\n",
                         anmIndex, owner != nullptr ? owner : "unknown",
                         static_cast<int>(gBusy),
                         static_cast<unsigned long>(gGeneration));
            reportedWait = true;
        }
        sceKernelDelayThread(1000);
    }

    gActiveBytes = static_cast<std::uint32_t>(bytes);
    const std::uint32_t generation = __sync_add_and_fetch(&gGeneration, 1U);
    gOwnerIndex = anmIndex;
    CopyOwner(owner);
    ResetGuard();
    outLease->base = gArena;
    outLease->bytes = bytes;
    outLease->anmIndex = anmIndex;
    outLease->generation = generation;
    std::fprintf(stderr,
                 "TH08PSP ANM_SCRATCH acquire anm=%d generation=%lu owner=%s bytes=%lu capacity=%lu\n",
                 anmIndex, static_cast<unsigned long>(generation),
                 gOwner, static_cast<unsigned long>(bytes),
                 static_cast<unsigned long>(kArenaBytes));
    return true;
}

bool AnmScratchRelease(const AnmScratchLease &lease)
{
    if (lease.base != gArena || lease.anmIndex < 0 || lease.generation == 0)
        return false;
    if (gBusy != ScratchState_Anm || lease.anmIndex != gOwnerIndex ||
        lease.generation != gGeneration || lease.bytes != gActiveBytes)
    {
        gPoisoned = 1;
        std::fprintf(stderr,
                     "TH08PSP ANM_SCRATCH STALE_RELEASE lease_anm=%d active_anm=%d lease_generation=%lu active_generation=%lu lease_bytes=%lu active_bytes=%lu quarantined=1\n",
                     lease.anmIndex, static_cast<int>(gOwnerIndex),
                     static_cast<unsigned long>(lease.generation),
                     static_cast<unsigned long>(gGeneration),
                     static_cast<unsigned long>(lease.bytes),
                     static_cast<unsigned long>(gActiveBytes));
        return false;
    }
    const bool leadGuardIntact = GuardRegionIntact(gArena - kGuardBytes);
    const bool tailGuardIntact = GuardRegionIntact(gArena + kArenaBytes);
    if (!leadGuardIntact || !tailGuardIntact)
    {
        gPoisoned = 1;
        std::fprintf(stderr,
                     "TH08PSP ANM_SCRATCH GUARD_CORRUPT anm=%d generation=%lu owner=%s bytes=%lu lead=%d tail=%d quarantined=1\n",
                     lease.anmIndex, static_cast<unsigned long>(gGeneration), gOwner,
                     static_cast<unsigned long>(gActiveBytes),
                     leadGuardIntact ? 1 : 0, tailGuardIntact ? 1 : 0);
    }
    std::fprintf(stderr,
                 "TH08PSP ANM_SCRATCH release anm=%d generation=%lu owner=%s bytes=%lu retained=1 poisoned=%d\n",
                 lease.anmIndex, static_cast<unsigned long>(gGeneration), gOwner,
                 static_cast<unsigned long>(gActiveBytes), gPoisoned != 0 ? 1 : 0);
    gOwner[0] = '\0';
    gOwnerIndex = -1;
    gActiveBytes = 0;
    __atomic_store_n(&gBusy, ScratchState_Idle, __ATOMIC_RELEASE);
    return leadGuardIntact && tailGuardIntact && gPoisoned == 0;
}

bool AnmScratchReserveTransition(const char *owner)
{
    if (gArena == nullptr || gPoisoned != 0)
        return false;
    if (!__sync_bool_compare_and_swap(&gBusy, ScratchState_Idle,
                                      ScratchState_Transition))
    {
        std::fprintf(stderr,
                     "TH08PSP ANM_SCRATCH transition_reserve_failed owner=%s state=%d active=%lu generation=%lu\n",
                     owner != nullptr ? owner : "unknown", static_cast<int>(gBusy),
                     static_cast<unsigned long>(gActiveBytes),
                     static_cast<unsigned long>(gGeneration));
        return false;
    }

    gActiveBytes = 0;
    const std::uint32_t generation = __sync_add_and_fetch(&gGeneration, 1U);
    gOwnerIndex = -2;
    CopyOwner(owner != nullptr ? owner : "transition");
    ResetGuard();
    std::fprintf(stderr,
                 "TH08PSP ANM_SCRATCH transition_reserved generation=%lu owner=%s capacity=%lu\n",
                 static_cast<unsigned long>(generation), gOwner,
                 static_cast<unsigned long>(kArenaBytes));
    return true;
}

void *AnmScratchTransitionBase()
{
    return gBusy == ScratchState_Transition && gPoisoned == 0 ? gArena : nullptr;
}

bool AnmScratchTransitionSetActiveBytes(std::size_t bytes)
{
    if (gBusy != ScratchState_Transition || gPoisoned != 0 || bytes > kArenaBytes)
        return false;
    gActiveBytes = static_cast<std::uint32_t>(bytes);
    return true;
}

bool AnmScratchReleaseTransition()
{
    if (gBusy != ScratchState_Transition || gOwnerIndex != -2)
        return false;

    const bool leadGuardIntact = GuardRegionIntact(gArena - kGuardBytes);
    const bool tailGuardIntact = GuardRegionIntact(gArena + kArenaBytes);
    if (!leadGuardIntact || !tailGuardIntact)
    {
        gPoisoned = 1;
        std::fprintf(stderr,
                     "TH08PSP ANM_SCRATCH TRANSITION_GUARD_CORRUPT generation=%lu owner=%s bytes=%lu lead=%d tail=%d quarantined=1\n",
                     static_cast<unsigned long>(gGeneration), gOwner,
                     static_cast<unsigned long>(gActiveBytes),
                     leadGuardIntact ? 1 : 0, tailGuardIntact ? 1 : 0);
    }
    std::fprintf(stderr,
                 "TH08PSP ANM_SCRATCH transition_release generation=%lu owner=%s bytes=%lu retained=1 poisoned=%d\n",
                 static_cast<unsigned long>(gGeneration), gOwner,
                 static_cast<unsigned long>(gActiveBytes),
                 gPoisoned != 0 ? 1 : 0);
    gOwner[0] = '\0';
    gOwnerIndex = -1;
    gActiveBytes = 0;
    __atomic_store_n(&gBusy, ScratchState_Idle, __ATOMIC_RELEASE);
    return leadGuardIntact && tailGuardIntact && gPoisoned == 0;
}

bool AnmScratchTransitionActive()
{
    return gBusy == ScratchState_Transition && gPoisoned == 0;
}

bool AnmScratchContains(const void *memory)
{
    if (gAlignedAllocation == nullptr || memory == nullptr)
        return false;
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(memory);
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(gAlignedAllocation);
    return address >= begin && address < begin + kReservedBytes;
}

void AnmScratchRejectGenericFree(void *memory)
{
    if (!AnmScratchContains(memory))
        return;
    gPoisoned = 1;
    std::fprintf(stderr,
                 "TH08PSP ANM_SCRATCH INVALID_GENERIC_FREE ptr=0x%08lx base=0x%08lx anm=%d generation=%lu busy=%d quarantined=1\n",
                 static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(memory)),
                 static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(gArena)),
                 static_cast<int>(gOwnerIndex),
                 static_cast<unsigned long>(gGeneration), gBusy != 0 ? 1 : 0);
}

std::size_t AnmScratchCapacity()
{
    return gArena != nullptr ? kArenaBytes : 0;
}

std::size_t AnmScratchActiveBytes()
{
    return gActiveBytes;
}

bool AnmScratchBusy()
{
    return gBusy != ScratchState_Idle;
}

bool AnmScratchPoisoned()
{
    return gPoisoned != 0;
}

std::uint32_t AnmScratchGeneration()
{
    return gGeneration;
}

int AnmScratchOwnerIndex()
{
    return gOwnerIndex;
}
} // namespace th08::psp
