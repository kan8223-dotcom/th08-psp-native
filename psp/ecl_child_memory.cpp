#include "ecl_child_memory.hpp"

#include "EclManager.hpp"
#include "fileio.hpp"
#include "memory_telemetry.hpp"
#include "render_resource_arena.hpp"

#include <cstddef>
#include <cstdint>

namespace th08::psp
{
namespace
{
constexpr std::uint32_t kBlockBytes =
    static_cast<std::uint32_t>(sizeof(EnemyChildEclBlock));

static_assert(sizeof(EnemyChildEclBlock) == 0x24b0,
              "PSP ECL child allocation ABI changed");
static_assert(alignof(EnemyChildEclBlock) <= 64U,
              "PSP render arena cannot satisfy ECL child alignment");

std::uint32_t gLiveBlocks = 0;
std::uint32_t gPeakBlocks = 0;
std::uint32_t gArenaLiveBlocks = 0;
std::uint32_t gArenaPeakBlocks = 0;
std::uint32_t gHeapLiveBlocks = 0;
std::uint32_t gHeapPeakBlocks = 0;
std::uint32_t gArenaAllocations = 0;
std::uint32_t gArenaMisses = 0;
std::uint32_t gHeapFallbacks = 0;
std::uint32_t gAllocationFailures = 0;
std::uint32_t gFreeFailures = 0;

void UpdatePeak(std::uint32_t *peak, std::uint32_t candidate)
{
    std::uint32_t observed = __atomic_load_n(peak, __ATOMIC_ACQUIRE);
    while (candidate > observed)
    {
        if (__atomic_compare_exchange_n(peak, &observed, candidate, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        {
            return;
        }
    }
}

void RecordAllocation(bool fromArena)
{
    const std::uint32_t live = __atomic_add_fetch(&gLiveBlocks, 1U, __ATOMIC_ACQ_REL);
    UpdatePeak(&gPeakBlocks, live);
    if (fromArena)
    {
        const std::uint32_t arenaLive =
            __atomic_add_fetch(&gArenaLiveBlocks, 1U, __ATOMIC_ACQ_REL);
        UpdatePeak(&gArenaPeakBlocks, arenaLive);
        __atomic_add_fetch(&gArenaAllocations, 1U, __ATOMIC_RELAXED);
    }
    else
    {
        const std::uint32_t heapLive =
            __atomic_add_fetch(&gHeapLiveBlocks, 1U, __ATOMIC_ACQ_REL);
        UpdatePeak(&gHeapPeakBlocks, heapLive);
        __atomic_add_fetch(&gHeapFallbacks, 1U, __ATOMIC_RELAXED);
    }
}

bool DecrementIfNonzero(std::uint32_t *counter)
{
    std::uint32_t observed = __atomic_load_n(counter, __ATOMIC_ACQUIRE);
    while (observed != 0U)
    {
        if (__atomic_compare_exchange_n(counter, &observed, observed - 1U, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        {
            return true;
        }
    }
    return false;
}

void RecordFree(bool fromArena, const void *memory)
{
    const bool classCounterOk = DecrementIfNonzero(
        fromArena ? &gArenaLiveBlocks : &gHeapLiveBlocks);
    const bool totalCounterOk = DecrementIfNonzero(&gLiveBlocks);
    if (classCounterOk && totalCounterOk)
        return;

    __atomic_add_fetch(&gFreeFailures, 1U, __ATOMIC_RELAXED);
    BootLog("ECL_CHILD free=COUNTER_MISMATCH ptr=0x%08lx source=%s "
            "class_counter=%s total_counter=%s\n",
            static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(memory)),
            fromArena ? "render_arena" : "heap",
            classCounterOk ? "OK" : "UNDERFLOW",
            totalCounterOk ? "OK" : "UNDERFLOW");
}
} // namespace

void *EnemyChildEclAllocate()
{
    // Stage texture/surface creation has completed before opcode 135 can run.
    // Use the already-reserved, coalescing Main-RAM arena first so a fragmented
    // newlib heap cannot suppress an ECL child.  This does not evict, resize or
    // otherwise trade away any visual allocation already resident there.
    void *memory = RenderResourceArenaAllocate(
        sizeof(EnemyChildEclBlock), alignof(EnemyChildEclBlock), "ECLInt");
    if (memory != nullptr)
    {
        RecordAllocation(true);
        return memory;
    }

    __atomic_add_fetch(&gArenaMisses, 1U, __ATOMIC_RELAXED);
    memory = th08_psp_tracked_malloc(sizeof(EnemyChildEclBlock), "ECLInt fallback");
    if (memory != nullptr)
    {
        RecordAllocation(false);
        return memory;
    }

    const std::uint32_t failures =
        __atomic_add_fetch(&gAllocationFailures, 1U, __ATOMIC_RELAXED);
    BootLog("ECL_CHILD alloc=FAILED bytes=%lu arena_misses=%lu failures=%lu "
            "fallback=heap sc_only=1\n",
            static_cast<unsigned long>(sizeof(EnemyChildEclBlock)),
            static_cast<unsigned long>(
                __atomic_load_n(&gArenaMisses, __ATOMIC_ACQUIRE)),
            static_cast<unsigned long>(failures));
    return nullptr;
}

void EnemyChildEclFree(void *memory)
{
    if (memory == nullptr)
        return;

    // TryFree accepts only the exact start of a live arena payload.  Interior,
    // stale and double-free pointers are quarantined and must never reach
    // newlib.  NotOwned is therefore the only result allowed down the heap
    // fallback path.
    const RenderResourceArenaFreeResult result =
        RenderResourceArenaTryFree(memory);
    if (result == RenderResourceArenaFreeResult::Freed)
    {
        RecordFree(true, memory);
        return;
    }
    if (result == RenderResourceArenaFreeResult::Quarantined)
    {
        const std::uint32_t failures =
            __atomic_add_fetch(&gFreeFailures, 1U, __ATOMIC_RELAXED);
        BootLog("ECL_CHILD free=QUARANTINED ptr=0x%08lx failures=%lu consumed=1\n",
                static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(memory)),
                static_cast<unsigned long>(failures));
        return;
    }

    th08_psp_tracked_free(memory);
    RecordFree(false, memory);
}

EnemyChildEclMemorySnapshot CaptureEnemyChildEclMemorySnapshot()
{
    EnemyChildEclMemorySnapshot snapshot{};
    snapshot.blockBytes = kBlockBytes;
    snapshot.liveBlocks = __atomic_load_n(&gLiveBlocks, __ATOMIC_ACQUIRE);
    snapshot.peakBlocks = __atomic_load_n(&gPeakBlocks, __ATOMIC_ACQUIRE);
    snapshot.liveBytes = snapshot.liveBlocks * kBlockBytes;
    snapshot.peakBytes = snapshot.peakBlocks * kBlockBytes;
    snapshot.arenaLiveBlocks =
        __atomic_load_n(&gArenaLiveBlocks, __ATOMIC_ACQUIRE);
    snapshot.arenaPeakBlocks =
        __atomic_load_n(&gArenaPeakBlocks, __ATOMIC_ACQUIRE);
    snapshot.heapLiveBlocks =
        __atomic_load_n(&gHeapLiveBlocks, __ATOMIC_ACQUIRE);
    snapshot.heapPeakBlocks =
        __atomic_load_n(&gHeapPeakBlocks, __ATOMIC_ACQUIRE);
    snapshot.arenaAllocations =
        __atomic_load_n(&gArenaAllocations, __ATOMIC_ACQUIRE);
    snapshot.arenaMisses = __atomic_load_n(&gArenaMisses, __ATOMIC_ACQUIRE);
    snapshot.heapFallbacks =
        __atomic_load_n(&gHeapFallbacks, __ATOMIC_ACQUIRE);
    snapshot.allocationFailures =
        __atomic_load_n(&gAllocationFailures, __ATOMIC_ACQUIRE);
    snapshot.freeFailures =
        __atomic_load_n(&gFreeFailures, __ATOMIC_ACQUIRE);
    return snapshot;
}
} // namespace th08::psp
