#pragma once

#include "AnmManager.hpp"
#include "Supervisor.hpp"

namespace th08
{
enum ItemType
{
    ITEM_POWER_SMALL,
    ITEM_POINT,
    ITEM_POWER_BIG,
    ITEM_BOMB,
    ITEM_POWER_FULL,
    ITEM_EXTEND,
    ITEM_POINT_STAR,
    ITEM_TIME,
    ITEM_POINT_SMALL,
    ITEM_RESERVED_9,
    // Spawn request converted immediately to ITEM_TIME with apex-gated collection.
    ITEM_TIME_APEX_AUTOCOLLECT_REQUEST,
};

struct ItemTimeOrbTimerStorage
{
    i32 current;
    f32 subFrame;
    i32 previous;
};

enum ItemState
{
    ITEM_STATE_DEFAULT,
    ITEM_STATE_AUTOCOLLECT,
    ITEM_STATE_DEATH_DROP_SPREAD,
    ITEM_STATE_TIME_RISING,
    ITEM_STATE_RESERVED_4,
    ITEM_STATE_TIME_RISING_TO_APEX,
};

struct Item
{
    Item();

    AnmVm sprite;

    Float3 currentPosition;
    Float3 startPositionOrVelocity;
    Float3 targetPosition;

    ZunTimer timer;

    i8 itemType;
    i8 isInUse;
    i8 isOnscreen;
    i8 state;
    i8 isMaxValue;

    Item *next;
    Item *prev;

    void CollectPowerSmall();
    void CollectPoint();
    void CollectPointSmall();
    void CollectPowerBig();
    void CollectTimeOrb();

    void Delete();
};
C_ASSERT(sizeof(Item) == 0x2e4);

#define MAX_ITEMS 2096

#if defined(PSP) && defined(TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT) && \
    TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT
// Audit-only evidence for a possible bit-exact autocollect velocity cache.
// The audited build still executes the canonical atan2/cos/sin path on every
// item.  A hit only means that the angle and speed input bits equal the most
// recent inputs for this logical slot; matching output bits prove how many of
// those calls a later, separately gated optimization could safely reuse.
struct ItemAutocollectCacheAuditStats
{
    u32 canonicalCalls;
    u32 exactInputRepeats;
    u32 exactOutputMatches;
    u32 exactOutputMismatches;
    u32 invalidSlotObservations;
};

const ItemAutocollectCacheAuditStats &GetItemAutocollectCacheAuditStats();
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_AUTOCOLLECT_CACHE) && \
    TH08_PSP_ITEM_AUTOCOLLECT_CACHE
// Replay-audit counters for the separately gated production cache.  The cache
// key and outputs are raw IEEE-754 bits; a miss always executes the canonical
// FromAngleMagnitude path.
struct ItemAutocollectCacheStats
{
    u32 lookups;
    u32 hits;
    u32 misses;
    u32 conflicts;
    u32 invalidSlotLookups;
};

const ItemAutocollectCacheStats &GetItemAutocollectCacheStats();
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT) && \
        TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT && \
    defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH) && \
        TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH
#error "ITEM_TIME spawn-init audit and fast path are mutually exclusive"
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH) && \
    TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH
#define TH08_PSP_ITEM_TIME_SPAWN_INIT_PRODUCT_ENABLED 1
#else
#define TH08_PSP_ITEM_TIME_SPAWN_INIT_PRODUCT_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT) && \
    TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT
// Audit-only proof for a possible stock ITEM_TIME script-68 spawn
// initializer.  SpawnItem always commits the canonical SetAndExecuteScriptIdx
// result.  The candidate runs only on a full copy of the pre-call VM, and the
// complete 0x2a4-byte result plus every externally visible counter/identity is
// compared after the canonical call.
struct ItemTimeSpawnInitAuditStats
{
    u32 candidates;
    u32 eligible;
    u32 canonicalFallbacks;
    u32 fullVmMatches;
    u32 fullVmMismatches;
    u32 fieldMatches;
    u32 fieldMismatches;
    // SetAndExecuteScript discards ExecuteScript's return.  These fields only
    // observe that the shadow keeps the proven FALSE continuation contract;
    // they are not a comparison with an observable canonical return value.
    u32 candidateReturnContractMatches;
    u32 candidateReturnContractMismatches;
    u32 pointerMatches;
    u32 pointerMismatches;
    u32 counterMatches;
    u32 counterMismatches;
    u32 spriteMatches;
    u32 spriteMismatches;
    u32 loadGenerationMatches;
    u32 loadGenerationMismatches;
    u32 scriptFingerprintFallbacks;
    u32 compactRangeFallbacks;
    u32 loadStateFallbacks;
    u32 canonicalStateFallbacks;
    u32 observedStageFrame;
    u32 currentFrameCandidates;
    u32 currentFrameEligible;
    u32 peakCandidatesPerFrame;
    u32 peakCandidatesStageFrame;
    u32 peakEligiblePerFrame;
    u32 peakEligibleStageFrame;
};

const ItemTimeSpawnInitAuditStats &GetItemTimeSpawnInitAuditStats();
#endif

#if defined(PSP)
// This exact counter shape is reserved in every PSP build so the isolated
// product OFF/ON comparison cannot move later BSS objects.  Only the gated
// product mutates or exposes it.  Every rejected owner/load/table/range/
// fingerprint state then executes the canonical SetAndExecuteScriptIdx path.
struct ItemTimeSpawnInitFastpathStats
{
    u32 calls;
    u32 hits;
    u32 canonicalFallbacks;
    u32 ownerFallbacks;
    u32 generationFallbacks;
    u32 readinessFallbacks;
    u32 scriptFallbacks;
    u32 rangeFallbacks;
    u32 fingerprintFallbacks;
    u32 cacheHits;
    u32 cacheRevalidations;
    u32 cacheGenerationChanges;
    u32 cacheValidationFailures;
    u32 cacheResets;
};

#if TH08_PSP_ITEM_TIME_SPAWN_INIT_PRODUCT_ENABLED
const ItemTimeSpawnInitFastpathStats &GetItemTimeSpawnInitFastpathStats();
#endif
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_ANM_IDLE_AUDIT) && \
        TH08_PSP_ITEM_TIME_ANM_IDLE_AUDIT && \
    defined(TH08_PSP_ITEM_TIME_ANM_IDLE_FASTPATH) && \
        TH08_PSP_ITEM_TIME_ANM_IDLE_FASTPATH
#error "ITEM_TIME ANM idle audit and fast path are mutually exclusive"
#endif

// Keep the product selector out of the audit implementation translation-unit
// contracts while preserving TH08_PSP_ITEM_TIME_ANM_IDLE_FASTPATH as the sole
// external build flag.
#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_ANM_IDLE_FASTPATH) && \
    TH08_PSP_ITEM_TIME_ANM_IDLE_FASTPATH
#define TH08_PSP_ITEM_TIME_ANM_IDLE_PRODUCT_ENABLED 1
#else
#define TH08_PSP_ITEM_TIME_ANM_IDLE_PRODUCT_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_ANM_IDLE_AUDIT) && \
    TH08_PSP_ITEM_TIME_ANM_IDLE_AUDIT
// Audit-only proof for the stock ITEM_TIME script-68 idle interval.  The
// authoritative Item VM is always advanced by AnmManager::ExecuteScript; a
// private shadow executes the proposed idle tail and is then compared byte
// for byte.  No field in this structure enables a production fast path.
struct ItemTimeAnmIdleAuditStats
{
    u32 canonicalCalls;
    u32 script68Candidates;
    u32 eligibleIdleCalls;
    u32 rejectedWindowCalls;
    u32 rejectedDynamicStateCalls;
    u32 fullVmMatches;
    u32 fullVmMismatches;
    u32 returnMatches;
    u32 returnMismatches;
    u32 counterMatches;
    u32 counterMismatches;
};

const ItemTimeAnmIdleAuditStats &GetItemTimeAnmIdleAuditStats();
#endif

#if TH08_PSP_ITEM_TIME_ANM_IDLE_PRODUCT_ENABLED
// Production counters are incremented only in replay-audit builds.  They are
// exposed unconditionally with the feature so logging code does not need a
// second ABI shape; non-audit production builds keep them at zero.
struct ItemTimeAnmIdleFastpathStats
{
    u32 calls;
    u32 hits;
    u32 canonicalFallbacks;
    u32 identityFallbacks;
    u32 windowFallbacks;
    u32 dynamicFallbacks;
    u32 cachePointerChanges;
    u32 cacheRevalidations;
    u32 cacheValidationFailures;
};

const ItemTimeAnmIdleFastpathStats &GetItemTimeAnmIdleFastpathStats();
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT) && \
        TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT && \
    defined(TH08_PSP_ITEM_TIME_INLINE_DRAW_FASTPATH) && \
        TH08_PSP_ITEM_TIME_INLINE_DRAW_FASTPATH
#error "ITEM_TIME inline-draw audit and fast path are mutually exclusive"
#endif

// This experiment replaces only the Draw2D -> DrawNoRotation -> DrawInner
// frontend for the stock axis-aligned ITEM_TIME VM.  It deliberately feeds
// the existing AddSpriteToDrawBuffer path, so linked-list order, state/texture
// flush boundaries, and whichever canonical 6V or already-gated PSP 4V
// backend owns the pass remain unchanged.
#if defined(PSP) && \
    (((defined(TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT) && \
       TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT)) || \
     ((defined(TH08_PSP_ITEM_TIME_INLINE_DRAW_FASTPATH) && \
       TH08_PSP_ITEM_TIME_INLINE_DRAW_FASTPATH)))
#define TH08_PSP_ITEM_TIME_INLINE_DRAW_ENABLED 1
#else
#define TH08_PSP_ITEM_TIME_INLINE_DRAW_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_INLINE_DRAW_FASTPATH) && \
    TH08_PSP_ITEM_TIME_INLINE_DRAW_FASTPATH
#define TH08_PSP_ITEM_TIME_INLINE_DRAW_PRODUCT_ENABLED 1
#else
#define TH08_PSP_ITEM_TIME_INLINE_DRAW_PRODUCT_ENABLED 0
#endif

#if TH08_PSP_ITEM_TIME_INLINE_DRAW_ENABLED && \
    (TH08_PSP_ITEM_TIME_DRAW_PAIR_ENABLED || \
     TH08_PSP_ITEM_NATURAL_QUADS_ENABLED)
#error "ITEM_TIME inline draw cannot overlap pair-run or natural-quad owners"
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT) && \
        TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT && \
    (((defined(TH08_PSP_ITEM_DIRECT_GE) && TH08_PSP_ITEM_DIRECT_GE)) || \
     TH08_PSP_ITEM_MIXED_QUADS_ENABLED)
#error "ITEM_TIME inline-draw M0 requires the canonical 6V Item backend"
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT) && \
    TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT
// M0 always renders canonically.  The candidate is built in private storage,
// then compared against the canonical four-corner scratch and final 6V
// append.  These counters are audit-only and never enter a product build.
struct ItemTimeInlineDrawAuditStats
{
    u32 candidates;
    u32 eligible;
    u32 canonicalFallbacks;
    u32 visible;
    u32 culled;
    u32 quadMatches;
    u32 quadMismatches;
    u32 streamMatches;
    u32 streamMismatches;
};

const ItemTimeInlineDrawAuditStats &GetItemTimeInlineDrawAuditStats();
#endif

struct ItemManager
{
    ItemManager();
    void Initialize();

#if defined(TH08_PSP_STAGE_POOL_ARENA)
    Item *items;
#else
    Item items[MAX_ITEMS + 1];
#endif

    i32 nextIndex;
    u32 itemCount;

    Item itemListHead;
    Item *itemListTail;

    Item *SpawnItem(Float3 *position, ItemType itemType, int state);
    static void UpdatePointItemExtendThreshold();
    void OnUpdate();
    void AutoCollectAllItems();
    void ConvertAllPowerItemsToTimeOrbs(Item *item);
    void CancelAutoCollect();
    void OnDraw();
    i32 GetTimeOrbCount();
};
#if defined(TH08_PSP_STAGE_POOL_ARENA)
C_ASSERT(sizeof(ItemManager) == 0x2f4);
C_ASSERT(offsetof(ItemManager, items) == 0x0);
C_ASSERT(offsetof(ItemManager, itemListHead) == 0xc);
C_ASSERT(offsetof(ItemManager, itemListTail) == 0x2f0);
#else
C_ASSERT(sizeof(ItemManager) == 0x17b094);
#endif

DIFFABLE_EXTERN(ItemManager, g_ItemManager);

}; // namespace th08
