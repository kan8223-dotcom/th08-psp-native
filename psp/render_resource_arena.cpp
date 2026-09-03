#include "render_resource_arena.hpp"

#include "fileio.hpp"

#include <pspkernel.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" void *__real_memalign(std::size_t alignment, std::size_t bytes);
extern "C" void *__real_malloc(std::size_t bytes);
extern "C" void *__real_calloc(std::size_t count, std::size_t bytes);
extern "C" void *__real_realloc(void *memory, std::size_t bytes);
extern "C" void __real_free(void *memory);

namespace th08::psp
{
namespace
{
constexpr std::size_t kArenaBytes = 12U * 1024U * 1024U;
constexpr std::size_t kAlignment = 64U;
constexpr std::uint32_t kBlockMagic = 0x52415242U; // RARB
constexpr std::uint32_t kBlockAllocated = 1U;
constexpr int kInitUninitialized = 0;
constexpr int kInitInProgress = 1;
constexpr int kInitReady = 2;
constexpr int kInitFailed = 3;

struct alignas(kAlignment) BlockHeader
{
    std::uint32_t magic;
    std::uint32_t flags;
    std::size_t payloadBytes;
    std::size_t requestedBytes;
    BlockHeader *previous;
    BlockHeader *next;
    std::uint32_t generation;
    char owner[32];
    std::uint32_t tailMagic;
};

static_assert(sizeof(void *) == 4, "PSP renderer arena requires 32-bit pointers");
static_assert(sizeof(std::size_t) == 4, "PSP renderer arena requires 32-bit size_t");
static_assert(sizeof(std::uintptr_t) == 4, "PSP renderer arena requires 32-bit addresses");
static_assert(alignof(BlockHeader) == kAlignment, "renderer block header alignment changed");
static_assert(sizeof(BlockHeader) == kAlignment, "renderer block header must be one cache line");

unsigned char *gArenaBase = nullptr;
BlockHeader *gFirstBlock = nullptr;
volatile int gArenaLock = 0;
volatile int gInitState = kInitUninitialized;
volatile int gArenaPoisoned = 0;
volatile int gScopeThread = -1;
volatile std::uint32_t gScopeDepth = 0;
volatile std::uint32_t gSurfaceDecodeScopeDepth = 0;
const char *gScopeOwner = "unscoped";
std::uint32_t gGeneration = 0;
std::uint32_t gLiveBytes = 0;
std::uint32_t gPeakBytes = 0;
std::uint32_t gAllocationCount = 0;
std::uint32_t gFailureCount = 0;
std::uint32_t gLiveAllocations = 0;
std::uint32_t gQuarantineCount = 0;
std::uint32_t gScopeContentionCount = 0;

struct ArenaWalkSummary
{
    BlockHeader *exactBlock;
    BlockHeader *firstFit;
    std::size_t liveBytes;
    std::size_t freeBytes;
    std::size_t largestFreeBytes;
    std::uint32_t liveAllocations;
};

unsigned char *ArenaBase()
{
    return __atomic_load_n(&gArenaBase, __ATOMIC_ACQUIRE);
}

bool ArenaPoisoned()
{
    return __atomic_load_n(&gArenaPoisoned, __ATOMIC_ACQUIRE) != 0;
}

void CountFailure()
{
    __sync_fetch_and_add(&gFailureCount, 1U);
}

bool ShouldReportCount(std::uint32_t count)
{
    return count <= 4U || (count & (count - 1U)) == 0U;
}

std::uint32_t CountQuarantine(bool poisonArena)
{
    CountFailure();
    const std::uint32_t count = __sync_add_and_fetch(&gQuarantineCount, 1U);
    if (poisonArena)
        __atomic_store_n(&gArenaPoisoned, 1, __ATOMIC_RELEASE);
    return count;
}

void ReportQuarantine(const char *reason, const void *memory, std::uint32_t count,
                      bool poisoned)
{
    if (!ShouldReportCount(count))
        return;
    std::fprintf(stderr,
                 "TH08PSP RENDER_ARENA QUARANTINE reason=%s ptr=0x%08lx "
                 "count=%lu poisoned=%d consumed=1\n",
                 reason != nullptr ? reason : "unknown",
                 static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(memory)),
                 static_cast<unsigned long>(count), poisoned ? 1 : 0);
}

bool TryAlignUp(std::size_t value, std::size_t alignment, std::size_t *result)
{
    if (result == nullptr || alignment == 0U ||
        value > static_cast<std::size_t>(-1) - (alignment - 1U))
    {
        return false;
    }
    *result = (value + alignment - 1U) & ~(alignment - 1U);
    return *result >= value;
}

bool ValidAlignment(std::size_t alignment)
{
    return alignment != 0U && (alignment & (alignment - 1U)) == 0U &&
           alignment <= kAlignment;
}

void LockArena()
{
    while (!__sync_bool_compare_and_swap(&gArenaLock, 0, 1))
        sceKernelDelayThread(100);
}

void UnlockArena()
{
    __sync_lock_release(&gArenaLock);
}

void CopyOwner(char destination[32], const char *owner)
{
    if (owner == nullptr)
        owner = "unknown";
    std::size_t index = 0;
    while (index + 1U < 32U && owner[index] != '\0')
    {
        destination[index] = owner[index];
        ++index;
    }
    destination[index] = '\0';
}

bool OwnerTerminated(const BlockHeader *header)
{
    for (std::size_t index = 0; index < sizeof(header->owner); ++index)
    {
        if (header->owner[index] == '\0')
            return true;
    }
    return false;
}

bool AddressInArenaSpan(const void *memory, bool includeOnePastEnd)
{
    unsigned char *basePointer = ArenaBase();
    if (basePointer == nullptr || memory == nullptr)
        return false;
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(basePointer);
    if (begin > static_cast<std::uintptr_t>(-1) - kArenaBytes)
        return false;
    const std::uintptr_t end = begin + kArenaBytes;
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(memory);
    return address >= begin && (includeOnePastEnd ? address <= end : address < end);
}

// Validate by deriving each physical successor from the current block's size.
// Saved next/previous pointers are compared only; they are never dereferenced
// until their address has independently been proven to be the next block.
bool WalkArenaLocked(const void *exactPayload, std::size_t fitBytes,
                     ArenaWalkSummary *summary)
{
    if (summary == nullptr)
        return false;
    *summary = ArenaWalkSummary{};

    unsigned char *basePointer = ArenaBase();
    if (basePointer == nullptr || gFirstBlock != reinterpret_cast<BlockHeader *>(basePointer))
        return false;
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(basePointer);
    if (begin > static_cast<std::uintptr_t>(-1) - kArenaBytes)
        return false;
    const std::uintptr_t end = begin + kArenaBytes;
    const std::uintptr_t wanted = reinterpret_cast<std::uintptr_t>(exactPayload);
    std::uintptr_t cursor = begin;
    BlockHeader *expectedPrevious = nullptr;
    std::uint32_t blockCount = 0U;
    bool previousWasFree = false;

    while (cursor < end)
    {
        if ((cursor - begin) % kAlignment != 0U ||
            end - cursor < sizeof(BlockHeader) ||
            ++blockCount > kArenaBytes / sizeof(BlockHeader))
        {
            return false;
        }

        BlockHeader *block = reinterpret_cast<BlockHeader *>(cursor);
        if (block->magic != kBlockMagic || block->tailMagic != ~kBlockMagic ||
            (block->flags != 0U && block->flags != kBlockAllocated) ||
            block->previous != expectedPrevious || !OwnerTerminated(block) ||
            block->payloadBytes < kAlignment ||
            block->payloadBytes % kAlignment != 0U)
        {
            return false;
        }

        const std::uintptr_t payload = cursor + sizeof(BlockHeader);
        if (block->payloadBytes > end - payload)
            return false;
        const std::uintptr_t nextAddress = payload + block->payloadBytes;
        BlockHeader *expectedNext = nextAddress == end
                                        ? nullptr
                                        : reinterpret_cast<BlockHeader *>(nextAddress);
        if (nextAddress > end || block->next != expectedNext)
            return false;

        const bool allocated = block->flags == kBlockAllocated;
        if (!allocated && previousWasFree)
            return false;
        if ((allocated &&
             (block->requestedBytes == 0U || block->requestedBytes > block->payloadBytes)) ||
            (!allocated && block->requestedBytes != 0U))
        {
            return false;
        }

        if (wanted == payload)
            summary->exactBlock = block;
        if (!allocated)
        {
            summary->freeBytes += block->payloadBytes;
            if (block->payloadBytes > summary->largestFreeBytes)
                summary->largestFreeBytes = block->payloadBytes;
            if (summary->firstFit == nullptr && fitBytes != 0U &&
                block->payloadBytes >= fitBytes)
            {
                summary->firstFit = block;
            }
        }
        else
        {
            summary->liveBytes += block->payloadBytes;
            ++summary->liveAllocations;
        }

        expectedPrevious = block;
        previousWasFree = !allocated;
        cursor = nextAddress;
    }

    return cursor == end && summary->liveBytes == gLiveBytes &&
           summary->liveAllocations == gLiveAllocations;
}

void MergeWithValidatedNext(BlockHeader *block)
{
    BlockHeader *next = block->next;
    block->payloadBytes += sizeof(BlockHeader) + next->payloadBytes;
    block->next = next->next;
    if (block->next != nullptr)
        block->next->previous = block;
    std::memset(next, 0xdd, sizeof(*next));
}

bool EnterScope(const char *owner)
{
    if (ArenaBase() == nullptr)
        return false;
    const int thread = sceKernelGetThreadId();
    const int current = __atomic_load_n(&gScopeThread, __ATOMIC_ACQUIRE);
    if (current == thread)
    {
        __sync_add_and_fetch(&gScopeDepth, 1U);
        return true;
    }
    if (current != -1 || !__sync_bool_compare_and_swap(&gScopeThread, -1, thread))
    {
        const int ownerThread = __atomic_load_n(&gScopeThread, __ATOMIC_ACQUIRE);
        CountFailure();
        const std::uint32_t count = __sync_add_and_fetch(&gScopeContentionCount, 1U);
        if (ShouldReportCount(count))
        {
            std::fprintf(stderr,
                         "TH08PSP RENDER_ARENA SCOPE_CONTENTION request_thread=%d "
                         "owner_thread=%d owner=%s count=%lu fallback=heap\n",
                         thread, ownerThread, owner != nullptr ? owner : "unknown",
                         static_cast<unsigned long>(count));
        }
        return false;
    }
    gScopeOwner = owner != nullptr ? owner : "unknown";
    __atomic_store_n(&gScopeDepth, 1U, __ATOMIC_RELEASE);
    return true;
}

void LeaveScope()
{
    const int thread = sceKernelGetThreadId();
    if (__atomic_load_n(&gScopeThread, __ATOMIC_ACQUIRE) != thread)
        return;
    const std::uint32_t depth = __sync_sub_and_fetch(&gScopeDepth, 1U);
    if (depth == 0U)
    {
        gScopeOwner = "unscoped";
        __atomic_store_n(&gScopeThread, -1, __ATOMIC_RELEASE);
    }
}
} // namespace

bool RenderResourceArenaInitialize()
{
    for (;;)
    {
        const int state = __atomic_load_n(&gInitState, __ATOMIC_ACQUIRE);
        if (state == kInitReady)
            return true;
        if (state == kInitInProgress)
            return false;
        if (state != kInitUninitialized && state != kInitFailed)
        {
            CountFailure();
            return false;
        }
        if (__sync_bool_compare_and_swap(&gInitState, state, kInitInProgress))
            break;
    }

    unsigned char *arenaBase = static_cast<unsigned char *>(
        __real_memalign(kAlignment, kArenaBytes));
    if (arenaBase == nullptr)
    {
        CountFailure();
        __atomic_store_n(&gInitState, kInitFailed, __ATOMIC_RELEASE);
        BootLog("RENDER_ARENA init=FAILED request=%lu sc_only=1\n",
                static_cast<unsigned long>(kArenaBytes));
        return false;
    }

    const std::uintptr_t baseAddress = reinterpret_cast<std::uintptr_t>(arenaBase);
    if (baseAddress > static_cast<std::uintptr_t>(-1) - kArenaBytes)
    {
        __real_free(arenaBase);
        CountFailure();
        __atomic_store_n(&gInitState, kInitFailed, __ATOMIC_RELEASE);
        BootLog("RENDER_ARENA init=FAILED reason=address_overflow sc_only=1\n");
        return false;
    }

    BlockHeader *firstBlock = reinterpret_cast<BlockHeader *>(arenaBase);
    firstBlock->magic = kBlockMagic;
    firstBlock->flags = 0U;
    firstBlock->payloadBytes = kArenaBytes - sizeof(BlockHeader);
    firstBlock->requestedBytes = 0U;
    firstBlock->previous = nullptr;
    firstBlock->next = nullptr;
    firstBlock->generation = 0U;
    CopyOwner(firstBlock->owner, "free");
    firstBlock->tailMagic = ~kBlockMagic;

    // Publish only after the complete root block is visible. Readers use the
    // acquire load in ArenaBase(), so neither a half-written header nor a null
    // gFirstBlock can be observed as an initialized arena.
    gFirstBlock = firstBlock;
    __atomic_store_n(&gArenaBase, arenaBase, __ATOMIC_RELEASE);
    __atomic_store_n(&gInitState, kInitReady, __ATOMIC_RELEASE);
    BootLog("RENDER_ARENA init=READY base=0x%08lx capacity=%lu alignment=%lu retained=1 "
            "sc_only=1 me=0\n",
            static_cast<unsigned long>(baseAddress),
            static_cast<unsigned long>(kArenaBytes),
            static_cast<unsigned long>(kAlignment));
    return true;
}

RenderResourceAllocationScope::RenderResourceAllocationScope(const char *owner)
    : entered_(EnterScope(owner))
{
}

RenderResourceAllocationScope::~RenderResourceAllocationScope()
{
    if (entered_)
        LeaveScope();
}

SurfaceDecodeAllocationScope::SurfaceDecodeAllocationScope(const char *owner)
    : entered_(EnterScope(owner))
{
    if (entered_)
        __sync_add_and_fetch(&gSurfaceDecodeScopeDepth, 1U);
}

SurfaceDecodeAllocationScope::~SurfaceDecodeAllocationScope()
{
    if (entered_)
    {
        __sync_sub_and_fetch(&gSurfaceDecodeScopeDepth, 1U);
        LeaveScope();
    }
}

void *RenderResourceArenaAllocate(std::size_t bytes, std::size_t alignment,
                                  const char *owner)
{
    std::size_t needed = 0U;
    if (ArenaBase() == nullptr || bytes == 0U || !ValidAlignment(alignment) ||
        !TryAlignUp(bytes, kAlignment, &needed) ||
        needed > kArenaBytes - sizeof(BlockHeader) || ArenaPoisoned())
    {
        CountFailure();
        return nullptr;
    }

    LockArena();
    if (ArenaPoisoned())
    {
        CountFailure();
        UnlockArena();
        return nullptr;
    }

    ArenaWalkSummary walk{};
    if (!WalkArenaLocked(nullptr, needed, &walk))
    {
        const std::uint32_t count = CountQuarantine(true);
        UnlockArena();
        ReportQuarantine("metadata_corrupt_on_allocate", nullptr, count, true);
        return nullptr;
    }

    BlockHeader *block = walk.firstFit;
    if (block == nullptr)
    {
        CountFailure();
        UnlockArena();
        return nullptr;
    }

    const std::size_t remainder = block->payloadBytes - needed;
    if (remainder >= sizeof(BlockHeader) + kAlignment)
    {
        unsigned char *splitAddress = reinterpret_cast<unsigned char *>(block + 1) + needed;
        BlockHeader *split = reinterpret_cast<BlockHeader *>(splitAddress);
        split->magic = kBlockMagic;
        split->flags = 0U;
        split->payloadBytes = remainder - sizeof(BlockHeader);
        split->requestedBytes = 0U;
        split->previous = block;
        split->next = block->next;
        if (split->next != nullptr)
            split->next->previous = split;
        split->generation = 0U;
        CopyOwner(split->owner, "free");
        split->tailMagic = ~kBlockMagic;
        block->next = split;
        block->payloadBytes = needed;
    }

    block->flags = kBlockAllocated;
    block->requestedBytes = bytes;
    block->generation = ++gGeneration;
    CopyOwner(block->owner, owner);
    const std::uint32_t charged = static_cast<std::uint32_t>(block->payloadBytes);
    gLiveBytes += charged;
    if (gLiveBytes > gPeakBytes)
        gPeakBytes = gLiveBytes;
    ++gAllocationCount;
    ++gLiveAllocations;
    void *result = block + 1;
    UnlockArena();
    return result;
}

void *RenderResourceArenaReallocate(void *memory, std::size_t bytes,
                                    std::size_t alignment, const char *owner)
{
    if (memory == nullptr)
        return RenderResourceArenaAllocate(bytes, alignment, owner);
    if (bytes == 0U)
    {
        RenderResourceArenaTryFree(memory);
        return nullptr;
    }

    std::size_t needed = 0U;
    if (!AddressInArenaSpan(memory, false) || !ValidAlignment(alignment) ||
        !TryAlignUp(bytes, kAlignment, &needed) ||
        needed > kArenaBytes - sizeof(BlockHeader) || ArenaPoisoned())
    {
        CountFailure();
        return nullptr;
    }

    LockArena();
    ArenaWalkSummary walk{};
    if (!WalkArenaLocked(memory, 0U, &walk))
    {
        const std::uint32_t count = CountQuarantine(true);
        UnlockArena();
        ReportQuarantine("metadata_corrupt_on_realloc", memory, count, true);
        return nullptr;
    }
    BlockHeader *block = walk.exactBlock;
    if (block == nullptr || block->flags != kBlockAllocated)
    {
        CountFailure();
        UnlockArena();
        return nullptr;
    }

    const std::size_t oldRequestedBytes = block->requestedBytes;
    if (needed <= block->payloadBytes)
    {
        block->requestedBytes = bytes;
        CopyOwner(block->owner, owner);
        UnlockArena();
        return memory;
    }
    UnlockArena();

    // Preserve standard realloc failure semantics: the old allocation remains
    // valid until a complete replacement has been obtained and copied.
    void *replacement = RenderResourceArenaAllocate(bytes, alignment, owner);
    if (replacement == nullptr)
        return nullptr;
    std::memcpy(replacement, memory,
                oldRequestedBytes < bytes ? oldRequestedBytes : bytes);
    if (RenderResourceArenaTryFree(memory) != RenderResourceArenaFreeResult::Freed)
    {
        RenderResourceArenaTryFree(replacement);
        return nullptr;
    }
    return replacement;
}

RenderResourceArenaFreeResult RenderResourceArenaTryFree(void *memory)
{
    if (memory == nullptr)
        return RenderResourceArenaFreeResult::Freed;
    if (!AddressInArenaSpan(memory, true))
        return RenderResourceArenaFreeResult::NotOwned;

    // The one-past-end address is not part of any payload, but sending it to
    // newlib would be just as destructive as sending an interior arena pointer.
    if (!AddressInArenaSpan(memory, false))
    {
        const std::uint32_t count = CountQuarantine(false);
        ReportQuarantine("one_past_end_free", memory, count, false);
        return RenderResourceArenaFreeResult::Quarantined;
    }
    if (ArenaPoisoned())
    {
        const std::uint32_t count = CountQuarantine(false);
        ReportQuarantine("free_after_arena_poison", memory, count, true);
        return RenderResourceArenaFreeResult::Quarantined;
    }

    LockArena();
    if (ArenaPoisoned())
    {
        const std::uint32_t count = CountQuarantine(false);
        UnlockArena();
        ReportQuarantine("free_after_arena_poison", memory, count, true);
        return RenderResourceArenaFreeResult::Quarantined;
    }

    ArenaWalkSummary walk{};
    if (!WalkArenaLocked(memory, 0U, &walk))
    {
        const std::uint32_t count = CountQuarantine(true);
        UnlockArena();
        ReportQuarantine("metadata_corrupt_on_free", memory, count, true);
        return RenderResourceArenaFreeResult::Quarantined;
    }

    BlockHeader *block = walk.exactBlock;
    if (block == nullptr)
    {
        const std::uint32_t count = CountQuarantine(false);
        UnlockArena();
        ReportQuarantine("non_payload_start_free", memory, count, false);
        return RenderResourceArenaFreeResult::Quarantined;
    }
    if (block->flags != kBlockAllocated)
    {
        const std::uint32_t count = CountQuarantine(false);
        UnlockArena();
        ReportQuarantine("double_free", memory, count, false);
        return RenderResourceArenaFreeResult::Quarantined;
    }

    const std::uint32_t charged = static_cast<std::uint32_t>(block->payloadBytes);
    gLiveBytes -= charged;
    --gLiveAllocations;
    block->flags = 0U;
    block->requestedBytes = 0U;
    CopyOwner(block->owner, "free");
    if (block->next != nullptr && block->next->flags == 0U)
        MergeWithValidatedNext(block);
    if (block->previous != nullptr && block->previous->flags == 0U)
    {
        block = block->previous;
        MergeWithValidatedNext(block);
    }
    UnlockArena();
    return RenderResourceArenaFreeResult::Freed;
}

bool RenderResourceArenaFree(void *memory)
{
    return RenderResourceArenaTryFree(memory) != RenderResourceArenaFreeResult::NotOwned;
}

bool RenderResourceArenaContains(const void *memory)
{
    return AddressInArenaSpan(memory, true);
}

bool RenderResourceAllocationScopeActive()
{
    return ArenaBase() != nullptr &&
           __atomic_load_n(&gScopeThread, __ATOMIC_ACQUIRE) == sceKernelGetThreadId() &&
           __atomic_load_n(&gScopeDepth, __ATOMIC_ACQUIRE) != 0U;
}

bool SurfaceDecodeAllocationScopeActive()
{
    // malloc/calloc/realloc are globally wrapped at link time.  Keep their
    // normal, overwhelmingly common path to one atomic load; only the short
    // image-decode window needs the thread-id ownership check.
    if (__atomic_load_n(&gSurfaceDecodeScopeDepth, __ATOMIC_ACQUIRE) == 0U)
        return false;
    return RenderResourceAllocationScopeActive();
}

const char *RenderResourceAllocationOwner()
{
    return RenderResourceAllocationScopeActive() ? gScopeOwner : "unscoped";
}

RenderResourceArenaSnapshot CaptureRenderResourceArenaSnapshot()
{
    RenderResourceArenaSnapshot snapshot{};
    snapshot.failureCount = __atomic_load_n(&gFailureCount, __ATOMIC_ACQUIRE);
    snapshot.quarantineCount = __atomic_load_n(&gQuarantineCount, __ATOMIC_ACQUIRE);
    snapshot.scopeContentionCount =
        __atomic_load_n(&gScopeContentionCount, __ATOMIC_ACQUIRE);
    snapshot.poisoned = ArenaPoisoned() ? 1U : 0U;
    if (ArenaBase() == nullptr)
        return snapshot;

    const char *quarantineReason = nullptr;
    std::uint32_t quarantineReportCount = 0U;
    LockArena();
    snapshot.capacityBytes = static_cast<std::uint32_t>(kArenaBytes);
    snapshot.liveBytes = gLiveBytes;
    snapshot.peakBytes = gPeakBytes;
    snapshot.allocationCount = gAllocationCount;
    snapshot.liveAllocations = gLiveAllocations;
    if (!ArenaPoisoned())
    {
        ArenaWalkSummary walk{};
        if (WalkArenaLocked(nullptr, 0U, &walk))
        {
            snapshot.freeBytes = static_cast<std::uint32_t>(walk.freeBytes);
            snapshot.largestFreeBytes =
                static_cast<std::uint32_t>(walk.largestFreeBytes);
        }
        else
        {
            quarantineReportCount = CountQuarantine(true);
            quarantineReason = "metadata_corrupt_on_snapshot";
        }
    }
    UnlockArena();

    if (quarantineReason != nullptr)
        ReportQuarantine(quarantineReason, nullptr, quarantineReportCount, true);
    snapshot.failureCount = __atomic_load_n(&gFailureCount, __ATOMIC_ACQUIRE);
    snapshot.quarantineCount = __atomic_load_n(&gQuarantineCount, __ATOMIC_ACQUIRE);
    snapshot.scopeContentionCount =
        __atomic_load_n(&gScopeContentionCount, __ATOMIC_ACQUIRE);
    snapshot.poisoned = ArenaPoisoned() ? 1U : 0U;
    return snapshot;
}
} // namespace th08::psp

extern "C" void *__wrap_memalign(std::size_t alignment, std::size_t bytes)
{
    if (th08::psp::RenderResourceAllocationScopeActive())
        return th08::psp::RenderResourceArenaAllocate(
            bytes, alignment, th08::psp::RenderResourceAllocationOwner());
    return __real_memalign(alignment, bytes);
}

extern "C" void *__wrap_malloc(std::size_t bytes)
{
    if (th08::psp::SurfaceDecodeAllocationScopeActive())
    {
        if (bytes == 0U)
            bytes = 1U;
        return th08::psp::RenderResourceArenaAllocate(
            bytes, alignof(std::max_align_t),
            th08::psp::RenderResourceAllocationOwner());
    }
    return __real_malloc(bytes);
}

extern "C" void *__wrap_calloc(std::size_t count, std::size_t bytes)
{
    if (!th08::psp::SurfaceDecodeAllocationScopeActive())
        return __real_calloc(count, bytes);
    if (bytes != 0U && count > static_cast<std::size_t>(-1) / bytes)
        return nullptr;
    std::size_t total = count * bytes;
    if (total == 0U)
        total = 1U;
    void *memory = th08::psp::RenderResourceArenaAllocate(
        total, alignof(std::max_align_t),
        th08::psp::RenderResourceAllocationOwner());
    if (memory != nullptr)
        std::memset(memory, 0, total);
    return memory;
}

extern "C" void *__wrap_realloc(void *memory, std::size_t bytes)
{
    if (memory == nullptr)
        return __wrap_malloc(bytes);
    if (!th08::psp::RenderResourceArenaContains(memory))
        return __real_realloc(memory, bytes);
    if (bytes == 0U)
    {
        th08::psp::RenderResourceArenaTryFree(memory);
        return nullptr;
    }
    // An arena pointer may never be handed to newlib realloc.  Decoded image
    // work remains resizeable while its dedicated scope is active; any escape
    // fails closed and leaves the original allocation intact.
    if (!th08::psp::SurfaceDecodeAllocationScopeActive())
        return nullptr;
    return th08::psp::RenderResourceArenaReallocate(
        memory, bytes, alignof(std::max_align_t),
        th08::psp::RenderResourceAllocationOwner());
}

extern "C" void __wrap_free(void *memory)
{
    const th08::psp::RenderResourceArenaFreeResult result =
        th08::psp::RenderResourceArenaTryFree(memory);
    if (result != th08::psp::RenderResourceArenaFreeResult::NotOwned)
        return;
    __real_free(memory);
}
