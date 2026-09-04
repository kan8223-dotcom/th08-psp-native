#include "stage_pool_arena.hpp"

#include "BulletManager.hpp"
#include "EnemyManager.hpp"
#include "ItemManager.hpp"
#include "fileio.hpp"
#include "memory_telemetry.hpp"
#include "newlib_heap_geometry.hpp"
#include "swap_triple.hpp"

#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

namespace
{
// R-056: heap headroom at every stage boundary (fragmentation trend).
void LogHeapAtStage(const char *phase, unsigned long generation)
{
    const th08::psp::NewlibHeapGeometrySnapshot heap = th08::psp::CaptureNewlibHeapGeometry();
    th08::psp::BootLog("HEAP_AT_STAGE phase=%s generation=%lu arena=%lu used=%lu free=%lu largest=%lu top=%lu chunks=%lu nogrow=%lu valid=%lu\n",
                       phase, generation, static_cast<unsigned long>(heap.arenaBytes),
                       static_cast<unsigned long>(heap.usedBytes), static_cast<unsigned long>(heap.freeBytes),
                       static_cast<unsigned long>(heap.largestFreeChunkBytes), static_cast<unsigned long>(heap.topChunkBytes),
                       static_cast<unsigned long>(heap.freeChunkCount), static_cast<unsigned long>(heap.largestNoGrowRequestBytes),
                       static_cast<unsigned long>(heap.scanValid));
    th08::psp::FlushBootLog();
}
} // namespace

// Including psptypes.h after the reconstructed engine's original u32 typedef
// creates a duplicate typedef in this translation unit. The SDK ABI uses an
// unsigned microsecond count here, so keep the one required yield declaration
// local instead of importing the full kernel type surface.
extern "C" int sceKernelDelayThread(unsigned int delay);

#if !defined(TH08_PSP_STAGE_POOL_ARENA)
#error stage_pool_arena.cpp requires the PSP stage-pool layout
#endif

namespace th08::psp
{
namespace
{
constexpr std::size_t kAlignment = 64U;
constexpr std::size_t kPageAlignment = 0x1000U;
constexpr std::size_t kSizeMax = static_cast<std::size_t>(-1);
constexpr std::size_t kPoolRedzoneBytes = 0x100U;
constexpr std::size_t kLegacyEnemyStride = 0x53d0U;
constexpr std::size_t kLegacyBulletStride = 0x10b8U;
constexpr std::size_t kLegacyReserveBytes = 0x1200000U;
constexpr std::size_t kLeadGuardOffset = 0x000000U;
constexpr std::size_t kLeadGuardBytes = 0x1000U;
constexpr std::size_t kTailGuardBytes = 0x1000U;
constexpr unsigned char kGuardValue = 0xa7U;
constexpr std::size_t kTransientGuardBytes = 64U;
constexpr unsigned char kTransientGuardValue = 0x5dU;
constexpr std::size_t kMaxTransientLoans = 16U;

constexpr std::size_t AlignUp(std::size_t value, std::size_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static_assert((kAlignment & (kAlignment - 1U)) == 0U, "Arena alignment must be a power of two");
static_assert((kPageAlignment & (kPageAlignment - 1U)) == 0U,
              "Page alignment must be a power of two");
static_assert(kEnemyPoolStorageCount <= kSizeMax / sizeof(Enemy), "Enemy pool byte overflow");
static_assert(kBulletPoolStorageCount <= kSizeMax / sizeof(Bullet), "Bullet pool byte overflow");
static_assert(kLaserPoolStorageCount <= kSizeMax / sizeof(Laser), "Laser pool byte overflow");
static_assert(kItemPoolStorageCount <= kSizeMax / sizeof(Item), "Item pool byte overflow");
constexpr std::size_t kEnemyBytes = sizeof(Enemy) * kEnemyPoolStorageCount;
constexpr std::size_t kBulletBytes = sizeof(Bullet) * kBulletPoolStorageCount;
constexpr std::size_t kLaserBytes = sizeof(Laser) * kLaserPoolStorageCount;
constexpr std::size_t kItemBytes = sizeof(Item) * kItemPoolStorageCount;
static_assert(kEnemyBytes <= kSizeMax - kBulletBytes, "Enemy+bullet byte overflow");
constexpr std::size_t kEnemyBulletBytes = kEnemyBytes + kBulletBytes;
static_assert(kEnemyBulletBytes <= kSizeMax - kLaserBytes, "Enemy+bullet+laser byte overflow");
constexpr std::size_t kEnemyBulletLaserBytes = kEnemyBulletBytes + kLaserBytes;
static_assert(kEnemyBulletLaserBytes <= kSizeMax - kItemBytes, "Pool payload byte overflow");
constexpr std::size_t kGameplayPayloadBytes = kEnemyBulletLaserBytes + kItemBytes;
static_assert(kGameplayPayloadBytes <= kSizeMax - kBulletRuntimeCacheBytes,
              "Pool payload plus Bullet runtime cache overflow");
constexpr std::size_t kPayloadBytes =
    kGameplayPayloadBytes + kBulletRuntimeCacheBytes;

static_assert(kLeadGuardOffset <= kSizeMax - kLeadGuardBytes, "Lead guard overflow");
constexpr std::size_t kLeadGuardEnd = kLeadGuardOffset + kLeadGuardBytes;
static_assert(kLeadGuardEnd <= kSizeMax - (kAlignment - 1U), "Enemy alignment overflow");
constexpr std::size_t kEnemyOffset = AlignUp(kLeadGuardEnd, kAlignment);
static_assert(kEnemyOffset <= kSizeMax - kEnemyBytes, "Enemy end overflow");
constexpr std::size_t kEnemyEnd = kEnemyOffset + kEnemyBytes;
static_assert(kEnemyEnd <= kSizeMax - kPoolRedzoneBytes, "Enemy redzone overflow");
constexpr std::size_t kEnemyRedzoneEnd = kEnemyEnd + kPoolRedzoneBytes;
static_assert(kEnemyRedzoneEnd <= kSizeMax - (kAlignment - 1U), "Bullet alignment overflow");
constexpr std::size_t kBulletOffset = AlignUp(kEnemyRedzoneEnd, kAlignment);
static_assert(kBulletOffset <= kSizeMax - kBulletBytes, "Bullet end overflow");
constexpr std::size_t kBulletEnd = kBulletOffset + kBulletBytes;
static_assert(kBulletEnd <= kSizeMax - kPoolRedzoneBytes, "Bullet redzone overflow");
constexpr std::size_t kBulletRedzoneEnd = kBulletEnd + kPoolRedzoneBytes;
static_assert(kBulletRedzoneEnd <= kSizeMax - (kAlignment - 1U), "Laser alignment overflow");
constexpr std::size_t kLaserOffset = AlignUp(kBulletRedzoneEnd, kAlignment);
static_assert(kLaserOffset <= kSizeMax - kLaserBytes, "Laser end overflow");
constexpr std::size_t kLaserEnd = kLaserOffset + kLaserBytes;
static_assert(kLaserEnd <= kSizeMax - kPoolRedzoneBytes, "Laser redzone overflow");
constexpr std::size_t kLaserRedzoneEnd = kLaserEnd + kPoolRedzoneBytes;
static_assert(kLaserRedzoneEnd <= kSizeMax - (kAlignment - 1U), "Item alignment overflow");
constexpr std::size_t kItemOffset = AlignUp(kLaserRedzoneEnd, kAlignment);
static_assert(kItemOffset <= kSizeMax - kItemBytes, "Item end overflow");
constexpr std::size_t kItemEnd = kItemOffset + kItemBytes;
// The Bullet sidecar can suppress simulation work in production builds, so an
// Item overrun must hit a checked redzone before it can reach the active bits.
// Keep the same explicit 0x100-byte separation used between the gameplay pools.
static_assert(kItemEnd <= kSizeMax - kPoolRedzoneBytes,
              "Item redzone overflow");
constexpr std::size_t kItemRedzoneEnd = kItemEnd + kPoolRedzoneBytes;
static_assert(kItemRedzoneEnd <= kSizeMax - (kAlignment - 1U),
              "Bullet runtime cache alignment overflow");
constexpr std::size_t kBulletRuntimeCacheOffset =
    AlignUp(kItemRedzoneEnd, kAlignment);
static_assert(kBulletRuntimeCacheOffset <= kSizeMax - kBulletRuntimeCacheBytes,
              "Bullet runtime cache end overflow");
constexpr std::size_t kBulletRuntimeCacheEnd =
    kBulletRuntimeCacheOffset + kBulletRuntimeCacheBytes;
static_assert(kBulletRuntimeCacheEnd <= kSizeMax - (kPageAlignment - 1U),
              "Tail alignment overflow");
constexpr std::size_t kTailGuardOffset =
    AlignUp(kBulletRuntimeCacheEnd, kPageAlignment);
static_assert(kTailGuardOffset <= kSizeMax - kTailGuardBytes, "Tail guard overflow");
constexpr std::size_t kTailGuardEnd = kTailGuardOffset + kTailGuardBytes;
static_assert(kTailGuardEnd <= kSizeMax - (kPageAlignment - 1U), "Reserve alignment overflow");
constexpr std::size_t kReserveBytes = AlignUp(kTailGuardEnd, kPageAlignment);
constexpr std::size_t kTransientCapacity = kTailGuardOffset - kEnemyOffset;

static_assert(kLegacyEnemyStride >= sizeof(Enemy), "Enemy scratch sharing grew storage");
constexpr std::size_t kEnemyPoolRecoveredBytes =
    (kLegacyEnemyStride - sizeof(Enemy)) * kEnemyPoolStorageCount;
static_assert(kLegacyBulletStride >= sizeof(Bullet), "Bullet VM compaction grew storage");
constexpr std::size_t kBulletPoolRecoveredBytes =
    (kLegacyBulletStride - sizeof(Bullet)) * kBulletPoolStorageCount;
static_assert(kLegacyReserveBytes >= kReserveBytes, "Stage arena reserve grew unexpectedly");
constexpr std::size_t kReserveRecoveredBytes = kLegacyReserveBytes - kReserveBytes;

static_assert(sizeof(void *) == 4, "PSP pool arena requires 32-bit pointers");
static_assert(sizeof(Enemy) == 0x3e98, "Enemy storage ABI changed");
#if defined(TH08_PSP_COMPACT_BULLET_VM)
static_assert(sizeof(Bullet) == 0xb70, "Compact Bullet storage ABI changed");
#else
static_assert(sizeof(Bullet) == 0x10b8, "Bullet storage ABI changed");
#endif
static_assert(sizeof(Laser) == 0x59c, "Laser storage ABI changed");
static_assert(sizeof(Item) == 0x2e4, "Item storage ABI changed");
static_assert(std::is_trivially_destructible<Enemy>::value, "Enemy arena needs explicit destruction");
static_assert(std::is_trivially_destructible<Bullet>::value, "Bullet arena needs explicit destruction");
static_assert(std::is_trivially_destructible<Laser>::value, "Laser arena needs explicit destruction");
static_assert(std::is_trivially_destructible<Item>::value, "Item arena needs explicit destruction");
static_assert(kAlignment % alignof(Enemy) == 0, "Enemy alignment exceeds arena alignment");
static_assert(kAlignment % alignof(Bullet) == 0, "Bullet alignment exceeds arena alignment");
static_assert(kAlignment % alignof(Laser) == 0, "Laser alignment exceeds arena alignment");
static_assert(kAlignment % alignof(Item) == 0, "Item alignment exceeds arena alignment");
static_assert(kEnemyOffset % kAlignment == 0, "Enemy pool alignment");
static_assert(kBulletOffset % kAlignment == 0, "Bullet pool alignment");
static_assert(kLaserOffset % kAlignment == 0, "Laser pool alignment");
static_assert(kItemOffset % kAlignment == 0, "Item pool alignment");
static_assert(kEnemyEnd + kPoolRedzoneBytes <= kBulletOffset, "Enemy redzone overlap");
static_assert(kBulletEnd + kPoolRedzoneBytes <= kLaserOffset, "Bullet redzone overlap");
static_assert(kLaserEnd + kPoolRedzoneBytes <= kItemOffset, "Laser redzone overlap");
static_assert(kItemEnd + kPoolRedzoneBytes <= kBulletRuntimeCacheOffset,
              "Item redzone/Bullet cache overlap");
static_assert(kBulletRuntimeCacheEnd <= kTailGuardOffset,
              "Bullet cache/tail guard overlap");
static_assert(kTailGuardEnd <= kReserveBytes, "Arena reserve too small");
static_assert(kEnemyOffset == 0x001000U, "Unexpected enemy pool offset");
static_assert(kBulletOffset == 0x75acc0U, "Unexpected bullet pool offset");
#if defined(TH08_PSP_COMPACT_BULLET_VM)
static_assert(kLaserOffset == 0xba5940U, "Unexpected compact laser pool offset");
static_assert(kItemOffset == 0xbff640U, "Unexpected compact item pool offset");
static_assert(kBulletRuntimeCacheOffset == 0xd7a500U,
              "Unexpected compact Bullet cache offset");
static_assert(kBulletRuntimeCacheEnd == 0xd81e80U,
              "Unexpected compact Bullet cache end");
static_assert(kTailGuardOffset == 0xd82000U, "Unexpected compact tail guard offset");
static_assert(kReserveBytes == 0xd83000U, "Unexpected compact arena reserve size");
#else
static_assert(kLaserOffset == 0xda0e80U, "Unexpected laser pool offset");
static_assert(kItemOffset == 0xdfab80U, "Unexpected item pool offset");
static_assert(kBulletRuntimeCacheOffset == 0xf75a40U,
              "Unexpected Bullet cache offset");
static_assert(kBulletRuntimeCacheEnd == 0xf7d3c0U,
              "Unexpected Bullet cache end");
static_assert(kTailGuardOffset == 0xf7e000U, "Unexpected tail guard offset");
static_assert(kReserveBytes == 0xf7f000U, "Unexpected arena reserve size");
#endif
static_assert(kEnemyPoolRecoveredBytes == 2612792U, "Unexpected enemy pool recovery");
#if defined(TH08_PSP_COMPACT_BULLET_VM)
static_assert(kBulletPoolRecoveredBytes == 2078024U, "Unexpected Bullet pool recovery");
static_assert(kReserveRecoveredBytes == 4706304U, "Unexpected compact arena reserve recovery");
#else
    static_assert(kBulletPoolRecoveredBytes == 0U, "Unexpected legacy Bullet pool recovery");
    static_assert(kReserveRecoveredBytes == 2625536U, "Unexpected arena reserve recovery");
#endif

unsigned char *gRawAllocation = nullptr;
unsigned char *gArenaBase = nullptr;
int gBound = 0;
bool gObjectsConstructed = false;
bool gPoisoned = false;
std::uint32_t gGeneration = 0;
int gTransientLock = 0;

struct TransientLoan
{
    unsigned char *base;
    std::size_t requestedBytes;
    std::size_t alignedBytes;
    bool active;
};

TransientLoan gTransientLoans[kMaxTransientLoans] = {};
std::size_t gTransientBumpBytes = 0;
std::size_t gTransientLiveBytes = 0;
std::size_t gTransientPeakBytes = 0;
std::uint32_t gTransientActiveLoans = 0;
std::uint32_t gTransientLoanCount = 0;
std::uint32_t gTransientFailureCount = 0;
std::uint32_t gTransientQuarantineCount = 0;

void LockTransient()
{
    while (!__sync_bool_compare_and_swap(&gTransientLock, 0, 1))
        sceKernelDelayThread(100);
}

void UnlockTransient()
{
    __sync_lock_release(&gTransientLock);
}

Enemy *EnemyPool()
{
    return reinterpret_cast<Enemy *>(gArenaBase + kEnemyOffset);
}

Bullet *BulletPool()
{
    return reinterpret_cast<Bullet *>(gArenaBase + kBulletOffset);
}

Laser *LaserPool()
{
    return reinterpret_cast<Laser *>(gArenaBase + kLaserOffset);
}

Item *ItemPool()
{
    return reinterpret_cast<Item *>(gArenaBase + kItemOffset);
}

void FillGuard(std::size_t begin, std::size_t end)
{
    std::memset(gArenaBase + begin, kGuardValue, end - begin);
}

bool GuardIntact(std::size_t begin, std::size_t end)
{
    for (std::size_t i = begin; i < end; ++i)
    {
        if (gArenaBase[i] != kGuardValue)
            return false;
    }
    return true;
}

void ResetGuards()
{
    FillGuard(kLeadGuardOffset, kLeadGuardOffset + kLeadGuardBytes);
    FillGuard(kEnemyOffset + kEnemyBytes, kBulletOffset);
    FillGuard(kBulletOffset + kBulletBytes, kLaserOffset);
    FillGuard(kLaserOffset + kLaserBytes, kItemOffset);
    FillGuard(kItemOffset + kItemBytes, kBulletRuntimeCacheOffset);
    FillGuard(kBulletRuntimeCacheEnd, kTailGuardOffset);
    FillGuard(kTailGuardOffset, kTailGuardOffset + kTailGuardBytes);
}

bool CheckGuards()
{
    if (gArenaBase == nullptr)
        return true;
    return GuardIntact(kLeadGuardOffset, kLeadGuardOffset + kLeadGuardBytes) &&
           GuardIntact(kEnemyOffset + kEnemyBytes, kBulletOffset) &&
           GuardIntact(kBulletOffset + kBulletBytes, kLaserOffset) &&
           GuardIntact(kLaserOffset + kLaserBytes, kItemOffset) &&
           GuardIntact(kItemOffset + kItemBytes, kBulletRuntimeCacheOffset) &&
           GuardIntact(kBulletRuntimeCacheEnd, kTailGuardOffset) &&
           GuardIntact(kTailGuardOffset, kTailGuardOffset + kTailGuardBytes);
}

bool CheckTransientGuardsUnlocked()
{
    if (gArenaBase == nullptr || gTransientActiveLoans == 0)
        return true;
    if (!GuardIntact(kLeadGuardOffset, kLeadGuardOffset + kLeadGuardBytes) ||
        !GuardIntact(kTailGuardOffset, kTailGuardOffset + kTailGuardBytes))
    {
        return false;
    }

    const unsigned char *const begin = gArenaBase + kEnemyOffset;
    const unsigned char *const end = gArenaBase + kTailGuardOffset;
    std::uint32_t countedLoans = 0;
    std::size_t countedBytes = 0;
    for (std::size_t loanIndex = 0; loanIndex < kMaxTransientLoans; ++loanIndex)
    {
        const TransientLoan &loan = gTransientLoans[loanIndex];
        if (!loan.active)
            continue;
        ++countedLoans;
        countedBytes += loan.requestedBytes;
        if (loan.base < begin || loan.base >= end ||
            loan.alignedBytes < loan.requestedBytes ||
            static_cast<std::size_t>(end - loan.base) <
                loan.alignedBytes + kTransientGuardBytes)
        {
            return false;
        }
        const unsigned char *guard = loan.base + loan.alignedBytes;
        for (std::size_t i = 0; i < kTransientGuardBytes; ++i)
        {
            if (guard[i] != kTransientGuardValue)
                return false;
        }
        for (std::size_t otherIndex = loanIndex + 1U;
             otherIndex < kMaxTransientLoans; ++otherIndex)
        {
            const TransientLoan &other = gTransientLoans[otherIndex];
            if (!other.active)
                continue;
            const unsigned char *loanEnd =
                loan.base + loan.alignedBytes + kTransientGuardBytes;
            const unsigned char *otherEnd =
                other.base + other.alignedBytes + kTransientGuardBytes;
            if (loan.base < otherEnd && other.base < loanEnd)
                return false;
        }
    }
    return countedLoans == gTransientActiveLoans && countedBytes == gTransientLiveBytes;
}

template <typename T>
void ConstructPool(T *pool, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        ::new (static_cast<void *>(&pool[i])) T();
}

void ConstructObjectsOnce()
{
    if (gObjectsConstructed)
        return;
    ConstructPool(EnemyPool(), kEnemyPoolStorageCount);
    ConstructPool(BulletPool(), kBulletPoolStorageCount);
    ConstructPool(LaserPool(), kLaserPoolStorageCount);
    ConstructPool(ItemPool(), kItemPoolStorageCount);
    gObjectsConstructed = true;
}

void BindManagers()
{
    g_EnemyManager.enemies = EnemyPool();
    g_BulletManager.bullets = BulletPool();
    g_BulletManager.lasers = LaserPool();
    g_ItemManager.items = ItemPool();
}

void UnbindManagers()
{
    g_EnemyManager.activeEnemyCount = 0;
    for (std::size_t i = 0; i < 8U; ++i)
        g_EnemyManager.bosses[i] = nullptr;
    for (std::size_t i = 0; i < 4U; ++i)
        g_EnemyManager.drawGroupHeads[i] = nullptr;

    g_BulletManager.activeBulletCount = 0;
    g_BulletManager.bulletCursor = nullptr;
    for (std::size_t i = 0; i < 6U; ++i)
        g_BulletManager.drawBuckets[i] = nullptr;

    g_ItemManager.nextIndex = 0;
    g_ItemManager.itemCount = 0;
    g_ItemManager.itemListHead.next = nullptr;
    g_ItemManager.itemListHead.prev = nullptr;
    g_ItemManager.itemListTail = &g_ItemManager.itemListHead;

    g_EnemyManager.enemies = nullptr;
    g_BulletManager.bullets = nullptr;
    g_BulletManager.lasers = nullptr;
    g_ItemManager.items = nullptr;
}

void FreeBacking()
{
    if (gRawAllocation != nullptr)
        th08_psp_tracked_free(gRawAllocation);
    gRawAllocation = nullptr;
    gArenaBase = nullptr;
    gObjectsConstructed = false;
    std::memset(gTransientLoans, 0, sizeof(gTransientLoans));
    gTransientBumpBytes = 0;
    gTransientLiveBytes = 0;
    gTransientActiveLoans = 0;
}

bool AllocateBackingUnlocked()
{
    if (gArenaBase != nullptr)
        return true;

    constexpr std::size_t kAllocationBytes = kReserveBytes + kAlignment - 1U;
    gRawAllocation = static_cast<unsigned char *>(
        th08_psp_tracked_malloc(kAllocationBytes, "PSP stage pool arena"));
    if (gRawAllocation == nullptr)
        return false;

    const std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(gRawAllocation);
    const std::uintptr_t aligned = (raw + kAlignment - 1U) & ~(kAlignment - 1U);
    gArenaBase = reinterpret_cast<unsigned char *>(aligned);
    ResetGuards();
    return true;
}
} // namespace

bool StagePoolArenaPrepareIdle()
{
#if TH08_PSP_SWAP_TRIPLE_ENABLED
    SwapTripleDrain();
#endif
    LockTransient();
    if (__atomic_load_n(&gBound, __ATOMIC_ACQUIRE) != 0)
    {
        UnlockTransient();
        BootLog("STAGE_POOL prepare=FAILED reason=already_bound generation=%lu\n",
                static_cast<unsigned long>(gGeneration));
        return false;
    }
    if (gTransientActiveLoans != 0)
    {
        const std::size_t activeBytes = gTransientLiveBytes;
        const std::uint32_t activeLoans = gTransientActiveLoans;
        UnlockTransient();
        BootLog("STAGE_POOL prepare=FAILED reason=idle_transient_busy bytes=%lu "
                "loans=%lu generation=%lu\n",
                static_cast<unsigned long>(activeBytes),
                static_cast<unsigned long>(activeLoans),
                static_cast<unsigned long>(gGeneration));
        return false;
    }
    if (gTransientLiveBytes != 0 || gTransientBumpBytes != 0)
    {
        const std::size_t liveBytes = gTransientLiveBytes;
        const std::size_t bumpBytes = gTransientBumpBytes;
        gPoisoned = true;
        UnlockTransient();
        BootLog("STAGE_POOL prepare=FAILED reason=idle_metadata_nonzero live=%lu "
                "bump=%lu generation=%lu\n",
                static_cast<unsigned long>(liveBytes),
                static_cast<unsigned long>(bumpBytes),
                static_cast<unsigned long>(gGeneration));
        return false;
    }
    if (gPoisoned || (gArenaBase != nullptr && !CheckGuards()))
    {
        gPoisoned = true;
        UnlockTransient();
        BootLog("STAGE_POOL prepare=FAILED reason=quarantined_or_guard generation=%lu\n",
                static_cast<unsigned long>(gGeneration));
        return false;
    }

    const bool reused = gArenaBase != nullptr;
    if (!AllocateBackingUnlocked())
    {
        UnlockTransient();
        BootLog("STAGE_POOL prepare=FAILED request=%lu\n",
                static_cast<unsigned long>(kReserveBytes + kAlignment - 1U));
        return false;
    }
    ResetGuards();
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(gArenaBase);
    UnlockTransient();
    BootLog("STAGE_POOL prepare=READY base=0x%08lx reserved=%lu payload=%lu "
            "enemy_pool_saved=%lu bullet_pool_saved=%lu reserve_saved=%lu reused=%d bound=0 "
            "guards=OK sc_only=1\n",
            static_cast<unsigned long>(base),
            static_cast<unsigned long>(kReserveBytes),
            static_cast<unsigned long>(kPayloadBytes),
            static_cast<unsigned long>(kEnemyPoolRecoveredBytes),
            static_cast<unsigned long>(kBulletPoolRecoveredBytes),
            static_cast<unsigned long>(kReserveRecoveredBytes), reused ? 1 : 0);
    return true;
}

bool StagePoolArenaBeginStage()
{
#if TH08_PSP_SWAP_TRIPLE_ENABLED
    SwapTripleDrain();
#endif
    LockTransient();
    if (__atomic_load_n(&gBound, __ATOMIC_ACQUIRE) != 0)
    {
        UnlockTransient();
        BootLog("STAGE_POOL begin=FAILED reason=already_bound generation=%lu\n",
                static_cast<unsigned long>(gGeneration));
        return false;
    }

    if (gTransientActiveLoans != 0)
    {
        const std::size_t activeBytes = gTransientLiveBytes;
        const std::uint32_t activeLoans = gTransientActiveLoans;
        UnlockTransient();
        BootLog("STAGE_POOL begin=FAILED reason=idle_transient_busy bytes=%lu loans=%lu "
                "generation=%lu\n",
                static_cast<unsigned long>(activeBytes),
                static_cast<unsigned long>(activeLoans),
                static_cast<unsigned long>(gGeneration));
        return false;
    }
    if (gTransientLiveBytes != 0 || gTransientBumpBytes != 0)
    {
        const std::size_t liveBytes = gTransientLiveBytes;
        const std::size_t bumpBytes = gTransientBumpBytes;
        gPoisoned = true;
        UnlockTransient();
        BootLog("STAGE_POOL begin=FAILED reason=idle_metadata_nonzero live=%lu "
                "bump=%lu generation=%lu\n",
                static_cast<unsigned long>(liveBytes),
                static_cast<unsigned long>(bumpBytes),
                static_cast<unsigned long>(gGeneration));
        return false;
    }

    if (gPoisoned)
    {
        UnlockTransient();
        BootLog("STAGE_POOL begin=FAILED reason=quarantined generation=%lu\n",
                static_cast<unsigned long>(gGeneration));
        return false;
    }

    if (gArenaBase != nullptr && !CheckGuards())
    {
        BootLog("STAGE_POOL begin=FAILED reason=retained_guard_corrupt generation=%lu\n",
                static_cast<unsigned long>(gGeneration));
        UnbindManagers();
        gPoisoned = true;
        UnlockTransient();
        return false;
    }

    if (!AllocateBackingUnlocked())
    {
        UnlockTransient();
        BootLog("STAGE_POOL begin=FAILED request=%lu\n",
                static_cast<unsigned long>(kReserveBytes + kAlignment - 1U));
        return false;
    }

    ResetGuards();
    // An idle transient ends the lifetime of the trivially-destructible pool
    // objects whose storage it borrows. Reconstruct the full original pools
    // before publishing any manager pointer again.
    ConstructObjectsOnce();
    BindManagers();
    // The process-global manager constructors deliberately do not initialize
    // pointer-backed pools.  Restore the original pre-game invariant only
    // after every base pointer is stable; RegisterChain will initialize again
    // at the same point as the PC implementation.
    g_EnemyManager.Initialize();
    g_BulletManager.Initialize();
    g_ItemManager.Initialize();
    ++gGeneration;
    __atomic_store_n(&gBound, 1, __ATOMIC_RELEASE);
    UnlockTransient();
    BootLog("STAGE_POOL begin=READY generation=%lu base=0x%08lx reserved=%lu payload=%lu "
            "enemy=%lu bullet=%lu laser=%lu item=%lu logical_enemy=%lu "
            "enemy_pool_saved=%lu bullet_pool_saved=%lu reserve_saved=%lu guards=OK sc_only=1\n",
            static_cast<unsigned long>(gGeneration),
            static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(gArenaBase)),
            static_cast<unsigned long>(kReserveBytes),
            static_cast<unsigned long>(kPayloadBytes),
            static_cast<unsigned long>(kEnemyBytes),
            static_cast<unsigned long>(kBulletBytes),
            static_cast<unsigned long>(kLaserBytes),
            static_cast<unsigned long>(kItemBytes),
            static_cast<unsigned long>(kEnemyPoolStorageCount),
            static_cast<unsigned long>(kEnemyPoolRecoveredBytes),
            static_cast<unsigned long>(kBulletPoolRecoveredBytes),
            static_cast<unsigned long>(kReserveRecoveredBytes));
    LogHeapAtStage("begin", static_cast<unsigned long>(gGeneration));
    return true;
}

bool StagePoolArenaEndStage(bool retainBacking)
{
#if TH08_PSP_SWAP_TRIPLE_ENABLED
    // The GE may still read stage textures/vertices in Main RAM.
    SwapTripleDrain();
#endif
    LockTransient();
    if (gArenaBase == nullptr)
    {
        __atomic_store_n(&gBound, 0, __ATOMIC_RELEASE);
        UnbindManagers();
        UnlockTransient();
        return true;
    }

    // No manager may publish an arena pointer after the bound state becomes
    // false. Clear every pointer/list root and validate guards while idle
    // transient allocation is locked out, then publish the phase edge.
    UnbindManagers();

    const bool guardsIntact = CheckGuards();
    if (!guardsIntact)
        gPoisoned = true;

    // Once the exact logical pools have been obtained contiguously, retain
    // that address for the process lifetime. Freeing it at a title or
    // demo boundary only lets unrelated small allocations fragment the run
    // before the next stage. This changes lifetime, not pool capacity or any
    // Enemy/Bullet/Laser/Item indexing semantics.
    retainBacking = true;
    __atomic_store_n(&gBound, 0, __ATOMIC_RELEASE);
    UnlockTransient();

    BootLog("STAGE_POOL end generation=%lu guards=%s retained=%d quarantined=%d\n",
            static_cast<unsigned long>(gGeneration), guardsIntact ? "OK" : "CORRUPT",
            retainBacking ? 1 : 0, gPoisoned ? 1 : 0);
    LogHeapAtStage("end", static_cast<unsigned long>(gGeneration));
    return guardsIntact;
}

void *StagePoolArenaBulletRuntimeCacheStorage()
{
    // BeginStage binds every manager before Initialize() clears its sidecar,
    // but deliberately publishes gBound only after all managers are ready.
    // Validate the owning pool pointer instead of gBound so the accessor is
    // usable during that narrow construction window and impossible to borrow
    // through the idle-transient phase.
    if (gArenaBase == nullptr || g_BulletManager.bullets != BulletPool())
        return nullptr;
    return static_cast<void *>(gArenaBase + kBulletRuntimeCacheOffset);
}

void *StagePoolArenaAcquireIdleTransient(std::size_t bytes, const char *owner)
{
    if (bytes == 0)
        bytes = 1;
    LockTransient();
    const bool allocated = gArenaBase != nullptr;
    const bool bound = __atomic_load_n(&gBound, __ATOMIC_ACQUIRE) != 0;
    if (!allocated || bound)
    {
        UnlockTransient();
        BootLog("STAGE_POOL transient=UNAVAILABLE owner=%s bytes=%lu allocated=%d bound=%d "
                "poisoned=%d capacity=%lu\n",
                owner != nullptr ? owner : "unknown", static_cast<unsigned long>(bytes),
                allocated ? 1 : 0, bound ? 1 : 0,
                gPoisoned ? 1 : 0, static_cast<unsigned long>(kTransientCapacity));
        return nullptr;
    }
    if (gPoisoned || bytes > kTransientCapacity - kTransientGuardBytes)
    {
        ++gTransientFailureCount;
        UnlockTransient();
        BootLog("STAGE_POOL transient=FAILED owner=%s bytes=%lu reason=%s capacity=%lu\n",
                owner != nullptr ? owner : "unknown", static_cast<unsigned long>(bytes),
                gPoisoned ? "poisoned" : "oversize",
                static_cast<unsigned long>(kTransientCapacity));
        return nullptr;
    }

    const bool guardsIntact = gTransientActiveLoans == 0
                                  ? CheckGuards()
                                  : CheckTransientGuardsUnlocked();
    if (!guardsIntact)
    {
        gPoisoned = true;
        ++gTransientQuarantineCount;
        UnlockTransient();
        BootLog("STAGE_POOL transient=FAILED reason=guard_corrupt owner=%s\n",
                owner != nullptr ? owner : "unknown");
        return nullptr;
    }

    std::size_t freeSlot = kMaxTransientLoans;
    for (std::size_t i = 0; i < kMaxTransientLoans; ++i)
    {
        if (!gTransientLoans[i].active)
        {
            freeSlot = i;
            break;
        }
    }
    const std::size_t alignedBytes = (bytes + kAlignment - 1U) & ~(kAlignment - 1U);
    const bool sizeOverflow = alignedBytes < bytes ||
                              alignedBytes > static_cast<std::size_t>(-1) -
                                                 kTransientGuardBytes;
    const std::size_t consumedBytes =
        sizeOverflow ? static_cast<std::size_t>(-1)
                     : alignedBytes + kTransientGuardBytes;
    const bool capacityFailure =
        sizeOverflow || gTransientBumpBytes > kTransientCapacity ||
        consumedBytes > kTransientCapacity - gTransientBumpBytes;
    if (freeSlot == kMaxTransientLoans || capacityFailure)
    {
        ++gTransientFailureCount;
        const std::size_t liveBytes = gTransientLiveBytes;
        const std::size_t bumpBytes = gTransientBumpBytes;
        const std::uint32_t activeLoans = gTransientActiveLoans;
        UnlockTransient();
        BootLog("STAGE_POOL transient=FAILED owner=%s bytes=%lu reason=%s capacity=%lu "
                "live=%lu bump=%lu active=%lu\n",
                owner != nullptr ? owner : "unknown", static_cast<unsigned long>(bytes),
                freeSlot == kMaxTransientLoans ? "loan_table_full" : "capacity",
                static_cast<unsigned long>(kTransientCapacity),
                static_cast<unsigned long>(liveBytes),
                static_cast<unsigned long>(bumpBytes),
                static_cast<unsigned long>(activeLoans));
        return nullptr;
    }

    unsigned char *const transientBase =
        gArenaBase + kEnemyOffset + gTransientBumpBytes;
    TransientLoan &loan = gTransientLoans[freeSlot];
    loan.base = transientBase;
    loan.requestedBytes = bytes;
    loan.alignedBytes = alignedBytes;
    loan.active = true;
    std::memset(transientBase + alignedBytes, kTransientGuardValue,
                kTransientGuardBytes);
    gTransientBumpBytes += consumedBytes;
    gTransientLiveBytes += bytes;
    ++gTransientActiveLoans;
    gObjectsConstructed = false;
    if (gTransientLiveBytes > gTransientPeakBytes)
        gTransientPeakBytes = gTransientLiveBytes;
    ++gTransientLoanCount;
    const std::size_t liveBytes = gTransientLiveBytes;
    const std::size_t bumpBytes = gTransientBumpBytes;
    const std::uint32_t activeLoans = gTransientActiveLoans;
    const std::uint32_t loanNumber = gTransientLoanCount;
    UnlockTransient();
    BootLog("STAGE_POOL transient=ACQUIRED owner=%s bytes=%lu base=0x%08lx "
            "capacity=%lu loan=%lu active=%lu live=%lu bump=%lu\n",
            owner != nullptr ? owner : "unknown", static_cast<unsigned long>(bytes),
            static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(transientBase)),
            static_cast<unsigned long>(kTransientCapacity),
            static_cast<unsigned long>(loanNumber),
            static_cast<unsigned long>(activeLoans),
            static_cast<unsigned long>(liveBytes),
            static_cast<unsigned long>(bumpBytes));
    return transientBase;
}

bool StagePoolArenaContains(const void *memory)
{
    if (gArenaBase == nullptr || memory == nullptr)
        return false;
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(memory);
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(gArenaBase);
    return address >= begin && address < begin + kReserveBytes;
}

bool StagePoolArenaFreeIdleTransient(void *memory)
{
    if (!StagePoolArenaContains(memory))
        return false;

    LockTransient();
    std::size_t loanIndex = kMaxTransientLoans;
    for (std::size_t i = 0; i < kMaxTransientLoans; ++i)
    {
        if (gTransientLoans[i].active && memory == gTransientLoans[i].base)
        {
            loanIndex = i;
            break;
        }
    }
    if (loanIndex == kMaxTransientLoans)
    {
        gPoisoned = true;
        ++gTransientQuarantineCount;
        const std::uint32_t activeLoans = gTransientActiveLoans;
        const std::uint32_t quarantineCount = gTransientQuarantineCount;
        UnlockTransient();
        BootLog("STAGE_POOL transient=QUARANTINED reason=invalid_free ptr=0x%08lx "
                "active_loans=%lu count=%lu\n",
                static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(memory)),
                static_cast<unsigned long>(activeLoans),
                static_cast<unsigned long>(quarantineCount));
        return true;
    }

    const bool guardsIntact = CheckTransientGuardsUnlocked();
    if (!guardsIntact)
    {
        gPoisoned = true;
        ++gTransientQuarantineCount;
    }

    TransientLoan &loan = gTransientLoans[loanIndex];
    const std::size_t releasedBytes = loan.requestedBytes;
    loan = TransientLoan{};
    gTransientLiveBytes -= releasedBytes;
    --gTransientActiveLoans;
    if (gTransientActiveLoans == 0)
    {
        gTransientBumpBytes = 0;
        gTransientLiveBytes = 0;
        std::memset(gTransientLoans, 0, sizeof(gTransientLoans));
        ResetGuards();
    }
    const std::size_t liveBytes = gTransientLiveBytes;
    const std::size_t bumpBytes = gTransientBumpBytes;
    const std::uint32_t activeLoans = gTransientActiveLoans;
    const bool poisoned = gPoisoned;
    UnlockTransient();
    BootLog("STAGE_POOL transient=RELEASED bytes=%lu guards=%s quarantined=%d "
            "active=%lu live=%lu bump=%lu\n",
            static_cast<unsigned long>(releasedBytes), guardsIntact ? "OK" : "CORRUPT",
            poisoned ? 1 : 0, static_cast<unsigned long>(activeLoans),
            static_cast<unsigned long>(liveBytes),
            static_cast<unsigned long>(bumpBytes));
    return true;
}

bool StagePoolArenaIsAllocated()
{
    return gArenaBase != nullptr;
}

bool StagePoolArenaIsBound()
{
    return __atomic_load_n(&gBound, __ATOMIC_ACQUIRE) != 0;
}

bool StagePoolArenaGuardsIntact()
{
    LockTransient();
    const bool intact = gTransientActiveLoans != 0
                            ? CheckTransientGuardsUnlocked()
                            : CheckGuards();
    UnlockTransient();
    return intact;
}

std::uintptr_t StagePoolArenaBase()
{
    return reinterpret_cast<std::uintptr_t>(gArenaBase);
}

std::size_t StagePoolArenaReservedBytes()
{
    return gArenaBase != nullptr ? kReserveBytes : 0U;
}

std::size_t StagePoolArenaPayloadBytes()
{
    return gArenaBase != nullptr ? kPayloadBytes : 0U;
}

std::uint32_t StagePoolArenaGeneration()
{
    return gGeneration;
}

std::size_t StagePoolArenaTransientActiveBytes()
{
    LockTransient();
    const std::size_t bytes = gTransientLiveBytes;
    UnlockTransient();
    return bytes;
}

std::size_t StagePoolArenaTransientPeakBytes()
{
    return gTransientPeakBytes;
}

std::uint32_t StagePoolArenaTransientLoanCount()
{
    return gTransientLoanCount;
}

std::uint32_t StagePoolArenaTransientFailureCount()
{
    return gTransientFailureCount;
}

std::uint32_t StagePoolArenaTransientQuarantineCount()
{
    return gTransientQuarantineCount;
}
} // namespace th08::psp
