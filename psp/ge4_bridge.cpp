#include "ge4_bridge.hpp"

#include "fileio.hpp"

#include <kubridge.h>
#include <pspge.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspmodulemgr.h>
#include <psppower.h>
#include <pspsdk.h>
#include <systemctrl.h>

#include <climits>
#include <cstddef>
#include <cstdint>

namespace
{
// This is the frozen TH07 wrapper binary.  Its internal module name is part of
// the verified binary ABI and intentionally remains unchanged in the TH08
// consumer.
constexpr char kWrapperPath[] = "./ge4wrap_texv1.prx";
constexpr char kWrapperModule[] = "th07_ge4_texbw_v1_wrap";
constexpr char kWrapperLibrary[] = "ge4wrap_texv1";
constexpr unsigned int kGetHwSizeNid = 0x2ddac688u;
constexpr unsigned int kGetModelNid = 0xbb75238fu;
constexpr unsigned int kSetSizeNid = 0x703b997bu;
constexpr unsigned int kTwoMiB = 0x00200000u;
constexpr unsigned int kFourMiB = 0x00400000u;
constexpr unsigned int kExpectedEdramBase = 0x04000000u;
constexpr unsigned int kUpperEdramBase =
    kExpectedEdramBase + kTwoMiB;
constexpr unsigned int kUpperEdramEnd =
    kExpectedEdramBase + kFourMiB;
#if defined(TH08_PSP_SLIMPLUS_GE4) && TH08_PSP_SLIMPLUS_GE4
constexpr int kMinimumModel = 1;
#else
constexpr int kRequiredModel = 3;
#endif
constexpr unsigned int kMaxGePolls = 50000000u;
constexpr unsigned long long kGeTimeoutUs = 5000000ull;
constexpr unsigned int kGePollDelayUs = 50u;

static_assert(sizeof(KernelCallArg) == 56u,
              "KUBridge KernelCallArg ABI size changed");
static_assert(offsetof(KernelCallArg, ret1) == 48u,
              "KUBridge ret1 ABI offset changed");

struct ExportCall
{
    int outer;
    unsigned int value;
};

SceUID gWrapper = -1;
bool gPrepareAttempted = false;
bool gEnableAttempted = false;
volatile int gPrepared = 0;
volatile int gPowerLocked = 0;
volatile int gActive = 0;
int gHardwareModel = -1;
unsigned int gGetHwSizeAddress = 0;
unsigned int gGetModelAddress = 0;
unsigned int gSetSizeAddress = 0;

volatile unsigned int gUpperAllocatedBytes = 0;
volatile unsigned int gUpperFreedBytes = 0;
volatile unsigned int gUpperLiveBytes = 0;
volatile unsigned int gUpperPeakLiveBytes = 0;
volatile unsigned int gUpperAllocationCount = 0;
volatile unsigned int gUpperFreeCount = 0;
volatile unsigned int gUpperLiveAllocationCount = 0;
volatile unsigned int gUpperPeakLiveAllocationCount = 0;
volatile unsigned int gPromotionAttemptCount = 0;
volatile unsigned int gPromotionSuccessCount = 0;
volatile unsigned int gPromotionFallbackCount = 0;
volatile unsigned int gPromotionRequestedBytes = 0;
volatile unsigned int gLowerEvictionBlockCount = 0;
volatile unsigned int gCompactionBlockCount = 0;
volatile unsigned int gUpperAllocNoApertureCount = 0;
volatile unsigned int gUpperAllocCapacityCount = 0;
volatile unsigned int gUpperAllocFragmentationCount = 0;
volatile unsigned int gUpperAllocMetadataCount = 0;
volatile unsigned int gUpperLargestGapBytes = kTwoMiB;
volatile unsigned int gUpperSmallestLargestGapBytes = kTwoMiB;
volatile unsigned int gUpperMapDeniedCount = 0;
volatile unsigned int gStaticUploadDepth = 0;
volatile unsigned int gTelemetryViolations = 0;

[[noreturn]] void ColdOffLoop(const char *reason, int result,
                              unsigned int size);

unsigned int SaturatingAtomicAdd(volatile unsigned int *value,
                                 unsigned int increment)
{
    unsigned int observed = __atomic_load_n(value, __ATOMIC_ACQUIRE);
    for (;;)
    {
        const unsigned int desired =
            increment > UINT_MAX - observed ? UINT_MAX : observed + increment;
        if (__atomic_compare_exchange_n(value, &observed, desired, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return desired;
    }
}

bool AtomicSubtractChecked(volatile unsigned int *value,
                           unsigned int decrement,
                           unsigned int *resultOut)
{
    unsigned int observed = __atomic_load_n(value, __ATOMIC_ACQUIRE);
    for (;;)
    {
        if (observed < decrement)
            return false;
        const unsigned int desired = observed - decrement;
        if (__atomic_compare_exchange_n(value, &observed, desired, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        {
            if (resultOut)
                *resultOut = desired;
            return true;
        }
    }
}

void AtomicRaisePeak(volatile unsigned int *peak, unsigned int candidate)
{
    unsigned int observed = __atomic_load_n(peak, __ATOMIC_ACQUIRE);
    while (observed < candidate &&
           !__atomic_compare_exchange_n(peak, &observed, candidate, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
    }
}

void NoteTelemetryViolation(const char *operation, const void *address,
                            unsigned int bytes)
{
    SaturatingAtomicAdd(&gTelemetryViolations, 1u);
    th08::psp::BootLog(
        "GE4 telemetry violation op=%s address=0x%08lx bytes=%u active=%d\n",
        operation ? operation : "unknown",
        static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(address)),
        bytes, __atomic_load_n(&gActive, __ATOMIC_ACQUIRE));
}

void ResetUpperTelemetry()
{
    __atomic_store_n(&gUpperAllocatedBytes, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gUpperFreedBytes, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gUpperLiveBytes, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gUpperPeakLiveBytes, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gUpperAllocationCount, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gUpperFreeCount, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gUpperLiveAllocationCount, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gUpperPeakLiveAllocationCount, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gPromotionAttemptCount, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gPromotionSuccessCount, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gPromotionFallbackCount, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gPromotionRequestedBytes, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gLowerEvictionBlockCount, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gCompactionBlockCount, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gUpperAllocNoApertureCount, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gUpperAllocCapacityCount, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gUpperAllocFragmentationCount, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gUpperAllocMetadataCount, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gUpperLargestGapBytes, kTwoMiB, __ATOMIC_RELEASE);
    __atomic_store_n(&gUpperSmallestLargestGapBytes, kTwoMiB,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&gUpperMapDeniedCount, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gStaticUploadDepth, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gTelemetryViolations, 0u, __ATOMIC_RELEASE);
}

bool IsCanonicalUpperRange(const void *address, unsigned int bytes)
{
    if (!address || bytes == 0u || bytes > kTwoMiB)
        return false;

    // Accept the normal and uncached/kernel aliases of the GE address while
    // comparing the underlying 29-bit PSP physical address.
    const unsigned int canonical = static_cast<unsigned int>(
        reinterpret_cast<std::uintptr_t>(address) & 0x1fffffffu);
    return canonical >= kUpperEdramBase && canonical < kUpperEdramEnd &&
           bytes <= kUpperEdramEnd - canonical;
}

ExportCall CallKernelExport(unsigned int address, unsigned int arg1 = 0)
{
    KernelCallArg args{};
    args.arg1 = arg1;
    // The resolved kernel address is never invoked as a user-mode function
    // pointer. KUBridge is the sole call boundary for every wrapper export.
    const int outer = kuKernelCall(
        reinterpret_cast<void *>(static_cast<std::uintptr_t>(address)), &args);
    return ExportCall{outer, args.ret1};
}

void ClearExportAddresses()
{
    gGetHwSizeAddress = 0;
    gGetModelAddress = 0;
    gSetSizeAddress = 0;
}

bool LoadWrapper()
{
    gWrapper = pspSdkLoadStartModule(kWrapperPath, PSP_MEMORY_PARTITION_KERNEL);
    if (gWrapper < 0)
    {
        th08::psp::BootLog("GE4 inactive: proven wrapper load rc=%08x\n",
                           static_cast<unsigned int>(gWrapper));
        return false;
    }
    return true;
}

bool ResolveWrapperExports()
{
    gGetHwSizeAddress =
        sctrlHENFindFunction(kWrapperModule, kWrapperLibrary, kGetHwSizeNid);
    gGetModelAddress =
        sctrlHENFindFunction(kWrapperModule, kWrapperLibrary, kGetModelNid);
    gSetSizeAddress =
        sctrlHENFindFunction(kWrapperModule, kWrapperLibrary, kSetSizeNid);
    if (gGetHwSizeAddress == 0 || gGetModelAddress == 0 ||
        gSetSizeAddress == 0)
    {
        th08::psp::BootLog(
            "GE4 inactive: export resolve HW=%08x M=%08x S=%08x\n",
            gGetHwSizeAddress, gGetModelAddress, gSetSizeAddress);
        return false;
    }
    return true;
}

bool StopUnloadWrapper(int *stopResultOut = nullptr,
                       int *unloadResultOut = nullptr)
{
    if (gWrapper < 0)
    {
        if (stopResultOut)
            *stopResultOut = 0;
        if (unloadResultOut)
            *unloadResultOut = 0;
        ClearExportAddresses();
        return true;
    }
    int status = 0;
    const int stopResult =
        sceKernelStopModule(gWrapper, 0, nullptr, &status, nullptr);
    const int unloadResult =
        stopResult < 0 ? stopResult : sceKernelUnloadModule(gWrapper);
    if (stopResultOut)
        *stopResultOut = stopResult;
    if (unloadResultOut)
        *unloadResultOut = unloadResult;
    if (stopResult < 0 || unloadResult < 0)
    {
        th08::psp::BootLog(
            "GE4 wrapper unload failed stop=%08x unload=%08x\n",
            static_cast<unsigned int>(stopResult),
            static_cast<unsigned int>(unloadResult));
        return false;
    }
    gWrapper = -1;
    ClearExportAddresses();
    return true;
}

void UnlockPower()
{
    if (!__atomic_load_n(&gPowerLocked, __ATOMIC_ACQUIRE))
        return;
    const int result = scePowerUnlock(0);
    if (result != 0)
        ColdOffLoop("cleanup-power-unlock", result, sceGeEdramGetSize());
    __atomic_store_n(&gPowerLocked, 0, __ATOMIC_RELEASE);
}

[[noreturn]] void ColdOffLoop(const char *reason, int result,
                              unsigned int size)
{
    th08::psp::BootLog("GE4 COLD OFF REQUIRED %s rc=%08x size=%08x\n",
                       reason, static_cast<unsigned int>(result), size);
    th08::psp::FlushBootLog();
    sceIoSync("ms0:", 0);
    sceIoSync("ef0:", 0);
    // An uncertain aperture or power-lock state is not recoverable in-process.
    for (;;)
        sceKernelDelayThread(1000 * 1000);
}

void FailClosedCleanup()
{
    int stopResult = 0;
    int unloadResult = 0;
    if (!StopUnloadWrapper(&stopResult, &unloadResult))
        ColdOffLoop("cleanup-wrapper-unload",
                    stopResult < 0 ? stopResult : unloadResult,
                    sceGeEdramGetSize());
    UnlockPower();
}

int WaitForGeIdle()
{
    const unsigned long long start =
        static_cast<unsigned long long>(sceKernelGetSystemTimeWide());
    for (unsigned int polls = 0; polls < kMaxGePolls; ++polls)
    {
        const int state = sceGeDrawSync(1);
        if (state == PSP_GE_LIST_DONE || state < 0)
            return state;
        const int delay = sceKernelDelayThread(kGePollDelayUs);
        if (delay < 0)
            return delay;
        const unsigned long long now =
            static_cast<unsigned long long>(sceKernelGetSystemTimeWide());
        if (now - start >= kGeTimeoutUs)
            return -0x3e30;
    }
    return -0x3e31;
}

void RestoreTwoMiBOrCold(const char *reason)
{
    const int syncResult = WaitForGeIdle();
    if (syncResult != PSP_GE_LIST_DONE)
        ColdOffLoop("restore2-sync", syncResult, sceGeEdramGetSize());

    sceKernelDcacheWritebackInvalidateAll();
    const ExportCall restore = CallKernelExport(gSetSizeAddress, kTwoMiB);
    const unsigned int sizeFinal = sceGeEdramGetSize();
    if (restore.outer < 0 || static_cast<int>(restore.value) < 0 ||
        sizeFinal != kTwoMiB)
    {
        th08::psp::BootLog(
            "GE4 restore2 failed outer=%08x inner=%08x size=%08x\n",
            static_cast<unsigned int>(restore.outer), restore.value, sizeFinal);
        ColdOffLoop(reason,
                    restore.outer < 0 ? restore.outer
                                      : static_cast<int>(restore.value),
                    sizeFinal);
    }
}
} // namespace

extern "C" int th08_psp_ge4_prepare()
{
    if (gPrepareAttempted)
        return __atomic_load_n(&gPrepared, __ATOMIC_ACQUIRE);
    gPrepareAttempted = true;
    ResetUpperTelemetry();

    if (!LoadWrapper())
        return 0;
    if (!ResolveWrapperExports())
    {
        FailClosedCleanup();
        return 0;
    }

    const ExportCall modelCall = CallKernelExport(gGetModelAddress);
    const ExportCall hwSizeCall = CallKernelExport(gGetHwSizeAddress);
    const unsigned int base = static_cast<unsigned int>(
        reinterpret_cast<std::uintptr_t>(sceGeEdramGetAddr()));
    const unsigned int sizeBefore = sceGeEdramGetSize();
    if (modelCall.outer < 0 || hwSizeCall.outer < 0)
    {
        th08::psp::BootLog("GE4 inactive: export call M=%08x H=%08x\n",
                           static_cast<unsigned int>(modelCall.outer),
                           static_cast<unsigned int>(hwSizeCall.outer));
        FailClosedCleanup();
        return 0;
    }

    const int model = static_cast<int>(modelCall.value);
    const unsigned int hwSize = hwSizeCall.value;
#if defined(TH08_PSP_SLIMPLUS_GE4) && TH08_PSP_SLIMPLUS_GE4
    if (model < kMinimumModel || base != kExpectedEdramBase ||
        hwSize != kFourMiB || sizeBefore != kTwoMiB)
    {
        th08::psp::BootLog(
            "GE4 inactive: Slim+ gate M%d/MIN1 B%08x H%08x S%08x\n",
            model, base, hwSize, sizeBefore);
        FailClosedCleanup();
        return 0;
    }
#else
    if (model != kRequiredModel || base != kExpectedEdramBase ||
        hwSize != kFourMiB || sizeBefore != kTwoMiB)
    {
        th08::psp::BootLog(
            "GE4 inactive: runtime gate M%d B%08x H%08x S%08x\n", model,
            base, hwSize, sizeBefore);
        FailClosedCleanup();
        return 0;
    }
#endif

    const int lockResult = scePowerLock(0);
    if (lockResult < 0)
    {
        th08::psp::BootLog("GE4 inactive: power lock rc=%08x\n",
                           static_cast<unsigned int>(lockResult));
        FailClosedCleanup();
        return 0;
    }
    if (lockResult > 0)
    {
        // Only exact zero proves that this process owns the sole shared lock.
        // A positive result leaves ownership uncertain, so retain all state.
        ColdOffLoop("power-lock-uncertain", lockResult, sceGeEdramGetSize());
    }
    __atomic_store_n(&gPowerLocked, 1, __ATOMIC_RELEASE);
    gHardwareModel = model;
    __atomic_store_n(&gPrepared, 1, __ATOMIC_RELEASE);
    th08::psp::BootLog("GE4 prepared: wrapper resolved at 2MiB model=%d "
                       "gate=%s\n",
                       model,
#if defined(TH08_PSP_SLIMPLUS_GE4) && TH08_PSP_SLIMPLUS_GE4
                       "Slim+"
#else
                       "model3"
#endif
    );
    return 1;
}

extern "C" int th08_psp_ge4_enable_after_gu_idle()
{
    if (!__atomic_load_n(&gPrepared, __ATOMIC_ACQUIRE))
    {
        // The renderer may initialize on a fallback platform where prepare
        // safely rejected the wrapper.  Keep this path visible in the log.
        th08::psp::BootLog("GE4 enable skipped: bridge not prepared\n");
        return th08_psp_ge4_active();
    }
    if (gEnableAttempted)
        return th08_psp_ge4_active();
    gEnableAttempted = true;

    if (!__atomic_load_n(&gPowerLocked, __ATOMIC_ACQUIRE) || gWrapper < 0 ||
        gSetSizeAddress == 0)
        ColdOffLoop("enable-state-invalid", -1, sceGeEdramGetSize());

    const unsigned int sizeBefore = sceGeEdramGetSize();
    if (sizeBefore != kTwoMiB)
        ColdOffLoop("enable-baseline-size", -1, sizeBefore);

    // The renderer calls here only after its initial finish/sync. Keep an
    // independent GE-idle gate before changing the hardware aperture.
    const int syncResult = WaitForGeIdle();
    if (syncResult != PSP_GE_LIST_DONE)
        ColdOffLoop("pre-enable-sync", syncResult, sizeBefore);

    sceKernelDcacheWritebackInvalidateAll();
    const ExportCall set4 = CallKernelExport(gSetSizeAddress, kFourMiB);
    const unsigned int sizeAfter = sceGeEdramGetSize();
    if (set4.outer < 0 || static_cast<int>(set4.value) < 0 ||
        sizeAfter != kFourMiB)
    {
        if (sizeAfter == kFourMiB)
            RestoreTwoMiBOrCold("enable-fail-restore2");
        else if (sizeAfter != kTwoMiB)
            ColdOffLoop("enable-fail-unknown-size",
                        set4.outer < 0 ? set4.outer
                                     : static_cast<int>(set4.value),
                        sizeAfter);

        th08::psp::BootLog(
            "GE4 inactive: Set4 outer=%08x inner=%08x size=%08x\n",
            static_cast<unsigned int>(set4.outer), set4.value, sizeAfter);
        __atomic_store_n(&gPrepared, 0, __ATOMIC_RELEASE);
        FailClosedCleanup();
        return 0;
    }

    const int postSet4SyncResult = WaitForGeIdle();
    if (postSet4SyncResult != PSP_GE_LIST_DONE)
    {
        // Set4/readback succeeded, but the proven acceptance gate also
        // requires GE idle after the transition.
        RestoreTwoMiBOrCold("post-enable-restore2");
        th08::psp::BootLog("GE4 inactive: post-Set4 sync rc=%08x\n",
                           static_cast<unsigned int>(postSet4SyncResult));
        __atomic_store_n(&gPrepared, 0, __ATOMIC_RELEASE);
        FailClosedCleanup();
        return 0;
    }

    __atomic_store_n(&gActive, 1, __ATOMIC_RELEASE);
    th08::psp::BootLog(
        "GE4 ACTIVE model=%d upper=2097152 owner=renderer lock=shared\n",
        gHardwareModel);
    return 1;
}

extern "C" int th08_psp_ge4_active()
{
    return __atomic_load_n(&gActive, __ATOMIC_ACQUIRE);
}

extern "C" int th08_psp_ge4_power_lock_held()
{
    return __atomic_load_n(&gActive, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&gPowerLocked, __ATOMIC_ACQUIRE);
}

extern "C" void th08_psp_ge4_fail_closed(const char *reason)
{
    ColdOffLoop(reason ? reason : "unspecified", -1, sceGeEdramGetSize());
}

extern "C" int th08_psp_ge4_is_upper_range(const void *address,
                                            unsigned int bytes)
{
    return __atomic_load_n(&gActive, __ATOMIC_ACQUIRE) &&
           sceGeEdramGetSize() == kFourMiB &&
           IsCanonicalUpperRange(address, bytes);
}

extern "C" void th08_psp_ge4_note_upper_alloc(const void *address,
                                               unsigned int bytes)
{
    if (!th08_psp_ge4_is_upper_range(address, bytes))
    {
        NoteTelemetryViolation("alloc-range", address, bytes);
        return;
    }

    const unsigned int liveBytes =
        SaturatingAtomicAdd(&gUpperLiveBytes, bytes);
    const unsigned int liveCount =
        SaturatingAtomicAdd(&gUpperLiveAllocationCount, 1u);
    SaturatingAtomicAdd(&gUpperAllocatedBytes, bytes);
    SaturatingAtomicAdd(&gUpperAllocationCount, 1u);
    AtomicRaisePeak(&gUpperPeakLiveBytes, liveBytes);
    AtomicRaisePeak(&gUpperPeakLiveAllocationCount, liveCount);

    if (liveBytes > kTwoMiB)
        NoteTelemetryViolation("alloc-overcommit", address, bytes);
    th08::psp::BootLog(
        "GE4_UPPER_ALLOC address=0x%08lx bytes=%u live=%u count=%u\n",
        static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(address)),
        bytes, liveBytes, liveCount);
}

extern "C" void th08_psp_ge4_note_upper_free(const void *address,
                                              unsigned int bytes)
{
    if (!th08_psp_ge4_is_upper_range(address, bytes))
    {
        NoteTelemetryViolation("free-range", address, bytes);
        return;
    }

    // PSPGL owns this telemetry from its single render thread.  Check both
    // counters before mutating either so a malformed free cannot leave a
    // half-applied accounting state.
    const unsigned int liveBytes =
        __atomic_load_n(&gUpperLiveBytes, __ATOMIC_ACQUIRE);
    const unsigned int liveCount =
        __atomic_load_n(&gUpperLiveAllocationCount, __ATOMIC_ACQUIRE);
    if (liveBytes < bytes || liveCount == 0u)
    {
        NoteTelemetryViolation("free-underflow", address, bytes);
        return;
    }
    unsigned int ignored = 0;
    if (!AtomicSubtractChecked(&gUpperLiveBytes, bytes, &ignored) ||
        !AtomicSubtractChecked(&gUpperLiveAllocationCount, 1u, &ignored))
        ColdOffLoop("telemetry-free-race", -1, sceGeEdramGetSize());
    SaturatingAtomicAdd(&gUpperFreedBytes, bytes);
    SaturatingAtomicAdd(&gUpperFreeCount, 1u);
    th08::psp::BootLog(
        "GE4_UPPER_FREE address=0x%08lx bytes=%u live=%u count=%u\n",
        static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(address)),
        bytes, __atomic_load_n(&gUpperLiveBytes, __ATOMIC_ACQUIRE),
        __atomic_load_n(&gUpperLiveAllocationCount, __ATOMIC_ACQUIRE));
}

extern "C" void th08_psp_ge4_note_promotion(
    unsigned int bytes, int promoted, unsigned int textureId,
    unsigned int width, unsigned int height, unsigned int hardwareFormat,
    unsigned int oldDomain, const void *newAddress)
{
    SaturatingAtomicAdd(&gPromotionAttemptCount, 1u);
    SaturatingAtomicAdd(&gPromotionRequestedBytes, bytes);
    SaturatingAtomicAdd(promoted ? &gPromotionSuccessCount
                                 : &gPromotionFallbackCount,
                        1u);
    th08::psp::BootLog(
        "GE4_PROMOTION result=%s id=%u size=%ux%u hwfmt=%u bytes=%u "
        "old_domain=%u new_address=0x%08lx\n",
        promoted ? "upper" : "fallback", textureId, width, height,
        hardwareFormat, bytes, oldDomain,
        static_cast<unsigned long>(
            reinterpret_cast<std::uintptr_t>(newAddress)));
}

extern "C" void th08_psp_ge4_note_allocator_block(int compaction)
{
    SaturatingAtomicAdd(compaction ? &gCompactionBlockCount
                                   : &gLowerEvictionBlockCount,
                        1u);
}

extern "C" void th08_psp_ge4_note_upper_alloc_failure(
    int reason, unsigned int requestedBytes, unsigned int totalFreeBytes,
    unsigned int largestGapBytes)
{
    volatile unsigned int *counter = &gUpperAllocMetadataCount;
    if (reason == 0)
        counter = &gUpperAllocNoApertureCount;
    else if (reason == 1)
        counter = &gUpperAllocCapacityCount;
    else if (reason == 2)
        counter = &gUpperAllocFragmentationCount;
    SaturatingAtomicAdd(counter, 1u);
    __atomic_store_n(&gUpperLargestGapBytes, largestGapBytes,
                     __ATOMIC_RELEASE);
    unsigned int observed = __atomic_load_n(&gUpperSmallestLargestGapBytes,
                                             __ATOMIC_ACQUIRE);
    while (largestGapBytes < observed &&
           !__atomic_compare_exchange_n(&gUpperSmallestLargestGapBytes,
                                        &observed, largestGapBytes, false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
    {
    }
    th08::psp::BootLog(
        "GE4_UPPER_ALLOC_FAIL reason=%d requested=%u total_free=%u "
        "largest_gap=%u\n",
        reason, requestedBytes, totalFreeBytes, largestGapBytes);
}

extern "C" void th08_psp_ge4_note_upper_map_denied(const void *address,
                                                     unsigned int bytes)
{
    SaturatingAtomicAdd(&gUpperMapDeniedCount, 1u);
    th08::psp::BootLog(
        "GE4_UPPER_MAP_DENIED address=0x%08lx bytes=%u\n",
        static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(address)),
        bytes);
}

extern "C" void
th08_psp_ge4_get_upper_telemetry(Th08PspGe4UpperTelemetry *out)
{
    if (!out)
        return;
    out->allocated_bytes =
        __atomic_load_n(&gUpperAllocatedBytes, __ATOMIC_ACQUIRE);
    out->freed_bytes =
        __atomic_load_n(&gUpperFreedBytes, __ATOMIC_ACQUIRE);
    out->live_bytes = __atomic_load_n(&gUpperLiveBytes, __ATOMIC_ACQUIRE);
    out->peak_live_bytes =
        __atomic_load_n(&gUpperPeakLiveBytes, __ATOMIC_ACQUIRE);
    out->allocation_count =
        __atomic_load_n(&gUpperAllocationCount, __ATOMIC_ACQUIRE);
    out->free_count = __atomic_load_n(&gUpperFreeCount, __ATOMIC_ACQUIRE);
    out->live_allocation_count =
        __atomic_load_n(&gUpperLiveAllocationCount, __ATOMIC_ACQUIRE);
    out->peak_live_allocation_count =
        __atomic_load_n(&gUpperPeakLiveAllocationCount, __ATOMIC_ACQUIRE);
    out->promotion_attempt_count =
        __atomic_load_n(&gPromotionAttemptCount, __ATOMIC_ACQUIRE);
    out->promotion_success_count =
        __atomic_load_n(&gPromotionSuccessCount, __ATOMIC_ACQUIRE);
    out->promotion_fallback_count =
        __atomic_load_n(&gPromotionFallbackCount, __ATOMIC_ACQUIRE);
    out->promotion_requested_bytes =
        __atomic_load_n(&gPromotionRequestedBytes, __ATOMIC_ACQUIRE);
    out->lower_eviction_block_count =
        __atomic_load_n(&gLowerEvictionBlockCount, __ATOMIC_ACQUIRE);
    out->compaction_block_count =
        __atomic_load_n(&gCompactionBlockCount, __ATOMIC_ACQUIRE);
    out->upper_alloc_no_aperture_count =
        __atomic_load_n(&gUpperAllocNoApertureCount, __ATOMIC_ACQUIRE);
    out->upper_alloc_capacity_count =
        __atomic_load_n(&gUpperAllocCapacityCount, __ATOMIC_ACQUIRE);
    out->upper_alloc_fragmentation_count =
        __atomic_load_n(&gUpperAllocFragmentationCount, __ATOMIC_ACQUIRE);
    out->upper_alloc_metadata_count =
        __atomic_load_n(&gUpperAllocMetadataCount, __ATOMIC_ACQUIRE);
    out->upper_largest_gap_bytes =
        __atomic_load_n(&gUpperLargestGapBytes, __ATOMIC_ACQUIRE);
    out->upper_smallest_largest_gap_bytes =
        __atomic_load_n(&gUpperSmallestLargestGapBytes, __ATOMIC_ACQUIRE);
    out->upper_map_denied_count =
        __atomic_load_n(&gUpperMapDeniedCount, __ATOMIC_ACQUIRE);
    out->static_upload_depth =
        __atomic_load_n(&gStaticUploadDepth, __ATOMIC_ACQUIRE);
    out->violations =
        __atomic_load_n(&gTelemetryViolations, __ATOMIC_ACQUIRE);
}

extern "C" void th08_psp_ge4_static_upload_hint_begin()
{
    const unsigned int depth =
        SaturatingAtomicAdd(&gStaticUploadDepth, 1u);
    if (depth == UINT_MAX)
        NoteTelemetryViolation("static-hint-overflow", nullptr, 0u);
}

extern "C" void th08_psp_ge4_static_upload_hint_end()
{
    unsigned int ignored = 0;
    if (!AtomicSubtractChecked(&gStaticUploadDepth, 1u, &ignored))
        NoteTelemetryViolation("static-hint-underflow", nullptr, 0u);
}

extern "C" int th08_psp_ge4_static_upload_hint_active()
{
    return __atomic_load_n(&gActive, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&gStaticUploadDepth, __ATOMIC_ACQUIRE) != 0u;
}

extern "C" void th08_psp_ge4_shutdown()
{
    if (!gPrepareAttempted)
        return;
    if (gWrapper < 0)
    {
        if (__atomic_load_n(&gPrepared, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&gActive, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&gPowerLocked, __ATOMIC_ACQUIRE))
            ColdOffLoop("shutdown-state-invalid", -1, sceGeEdramGetSize());
        return;
    }

    if (!__atomic_load_n(&gPrepared, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&gPowerLocked, __ATOMIC_ACQUIRE) ||
        gSetSizeAddress == 0)
        ColdOffLoop("shutdown-state-invalid", -1, sceGeEdramGetSize());

    Th08PspGe4UpperTelemetry telemetry{};
    th08_psp_ge4_get_upper_telemetry(&telemetry);
    if (telemetry.live_bytes != 0u ||
        telemetry.live_allocation_count != 0u ||
        telemetry.static_upload_depth != 0u || telemetry.violations != 0u)
    {
        th08::psp::BootLog(
            "GE4 shutdown refused live_bytes=%u live_count=%u hint_depth=%u "
            "violations=%u\n",
            telemetry.live_bytes, telemetry.live_allocation_count,
            telemetry.static_upload_depth, telemetry.violations);
        ColdOffLoop("upper-owner-state-invalid", -1, sceGeEdramGetSize());
    }

    // The caller has released all upper allocations, drained GE, and
    // terminated the renderer. Restore/read back 2 MiB while the frozen
    // wrapper and the sole shared lock are still alive.
    RestoreTwoMiBOrCold("restore2");
    __atomic_store_n(&gActive, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&gPrepared, 0, __ATOMIC_RELEASE);
    th08::psp::BootLog(
        "GE4 restored 2MiB alloc_bytes=%u free_bytes=%u peak_bytes=%u "
        "alloc_count=%u free_count=%u peak_count=%u promotions=%u/%u "
        "fallbacks=%u requested_bytes=%u evict_blocked=%u "
        "compact_blocked=%u alloc_fail_no_aperture=%u "
        "alloc_fail_capacity=%u alloc_fail_fragmentation=%u "
        "alloc_fail_metadata=%u largest_gap=%u min_largest_gap=%u "
        "map_denied=%u\n",
        telemetry.allocated_bytes, telemetry.freed_bytes,
        telemetry.peak_live_bytes, telemetry.allocation_count,
        telemetry.free_count, telemetry.peak_live_allocation_count,
        telemetry.promotion_success_count, telemetry.promotion_attempt_count,
        telemetry.promotion_fallback_count,
        telemetry.promotion_requested_bytes,
        telemetry.lower_eviction_block_count,
        telemetry.compaction_block_count,
        telemetry.upper_alloc_no_aperture_count,
        telemetry.upper_alloc_capacity_count,
        telemetry.upper_alloc_fragmentation_count,
        telemetry.upper_alloc_metadata_count,
        telemetry.upper_largest_gap_bytes,
        telemetry.upper_smallest_largest_gap_bytes,
        telemetry.upper_map_denied_count);

    int stopResult = 0;
    int unloadResult = 0;
    if (!StopUnloadWrapper(&stopResult, &unloadResult))
        ColdOffLoop("final-wrapper-unload",
                    stopResult < 0 ? stopResult : unloadResult,
                    sceGeEdramGetSize());
    UnlockPower();
    // Deliberately one-shot. Never reload or widen after terminal restore in
    // the same process.
}
