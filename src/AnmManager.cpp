#include "th_pch.h"

#if defined(PSP)
#include "anm_scratch.hpp"
#include "fileio.hpp"
#include "modern/linux/d3d8_internal.hpp"
#include "render_math.hpp"
#include "render_perf_telemetry.hpp"
#include "gui_border_replay.hpp"
#if defined(TH08_PSP_STAGE_POOL_ARENA)
#include "stage_pool_arena.hpp"
#endif

extern "C" unsigned int sceKernelTotalFreeMemSize(void);
#endif

#include "AnmManager.hpp"
#if TH08_PSP_BULLET_ONEPASS_4V_ENABLED
#include "bullet_onepass_4v.hpp"
#endif
#if TH08_PSP_ITEM_TIME_DRAW_PAIR_ENABLED
#include "GameManager.hpp"
#endif
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_BATCH) && \
    TH08_PSP_ASCII_POPUP_BATCH
#include "AsciiManager.hpp"
#endif
#include "Background.hpp"
#include "TextHelper.hpp"
#include "ZunMath.hpp"
#include "i18n.hpp"
#include "utils.hpp"
#include <stdarg.h>
#include <stdio.h>
#if defined(PSP) && \
    ((defined(TH08_PSP_ASCII_POPUP_BATCH) && \
      TH08_PSP_ASCII_POPUP_BATCH) || \
     (defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) && \
      TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) || \
     (defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH) && \
      TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH) || \
     (defined(TH08_PSP_BULLET_MIXED_QUADS_AUDIT) && \
      TH08_PSP_BULLET_MIXED_QUADS_AUDIT) || \
     (defined(TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) && \
      TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) || \
     (defined(TH08_PSP_ITEM_MIXED_QUADS_AUDIT) && \
      TH08_PSP_ITEM_MIXED_QUADS_AUDIT) || \
     (defined(TH08_PSP_ITEM_MIXED_QUADS_FASTPATH) && \
      TH08_PSP_ITEM_MIXED_QUADS_FASTPATH) || \
     (defined(TH08_PSP_BULLET_ONEPASS_4V_AUDIT) && \
      TH08_PSP_BULLET_ONEPASS_4V_AUDIT) || \
     (defined(TH08_PSP_BULLET_ONEPASS_4V_FASTPATH) && \
      TH08_PSP_BULLET_ONEPASS_4V_FASTPATH))
#include <cmath>
#endif

namespace th08
{

namespace
{
inline i32 LoadPendingAnmEntries(const AnmLoaded *anmLoaded)
{
#if defined(PSP)
    return __atomic_load_n(&anmLoaded->numberEntriesToBeLoaded, __ATOMIC_ACQUIRE);
#else
    return anmLoaded->numberEntriesToBeLoaded;
#endif
}

inline void StorePendingAnmEntries(AnmLoaded *anmLoaded, i32 value)
{
#if defined(PSP)
    __atomic_store_n(&anmLoaded->numberEntriesToBeLoaded, value, __ATOMIC_RELEASE);
#else
    anmLoaded->numberEntriesToBeLoaded = value;
#endif
}
} // namespace

#if defined(PSP)
namespace
{
enum class PspAnmPhase : u32
{
    Empty,
    Loading,
    Finalizing,
    Ready,
    Failed,
};

struct PspAnmCompactState
{
    u8 *data;
    u32 sourceSize;
    u32 compactSize;
    u32 entryCount;
    u32 spriteCount;
    u32 scriptCount;
    u32 loadGeneration;
    volatile u32 phase;
    bool sourceIsScratch;
    th08::psp::AnmScratchLease sourceLease;
};

PspAnmCompactState g_PspAnmCompact[25];
u32 g_PspAnmLoadGeneration[25];

PspAnmPhase GetPspAnmPhase(i32 anmIdx)
{
    if (anmIdx < 0 || anmIdx >= static_cast<i32>(ARRAY_SIZE(g_PspAnmCompact)))
        return PspAnmPhase::Failed;
    return static_cast<PspAnmPhase>(
        __atomic_load_n(&g_PspAnmCompact[anmIdx].phase, __ATOMIC_ACQUIRE));
}

void SetPspAnmPhase(i32 anmIdx, PspAnmPhase phase)
{
    if (anmIdx < 0 || anmIdx >= static_cast<i32>(ARRAY_SIZE(g_PspAnmCompact)))
        return;
    __atomic_store_n(&g_PspAnmCompact[anmIdx].phase,
                     static_cast<u32>(phase), __ATOMIC_RELEASE);
}

bool BeginPspAnmLoad(i32 anmIdx)
{
    if (anmIdx < 0 || anmIdx >= static_cast<i32>(ARRAY_SIZE(g_PspAnmCompact)))
        return false;
    const PspAnmPhase oldPhase = GetPspAnmPhase(anmIdx);
    if (oldPhase == PspAnmPhase::Loading || oldPhase == PspAnmPhase::Finalizing)
        return false;
    PspAnmCompactState &state = g_PspAnmCompact[anmIdx];
    memset(&state, 0, sizeof(state));
    state.loadGeneration = ++g_PspAnmLoadGeneration[anmIdx];
    SetPspAnmPhase(anmIdx, PspAnmPhase::Loading);
    return true;
}

u32 PspAnmLoadGeneration(i32 anmIdx)
{
    if (anmIdx < 0 || anmIdx >= static_cast<i32>(ARRAY_SIZE(g_PspAnmCompact)))
        return 0;
    return __atomic_load_n(&g_PspAnmCompact[anmIdx].loadGeneration,
                           __ATOMIC_ACQUIRE);
}

bool PspAnmLoadReady(i32 anmIdx, u32 expectedGeneration)
{
    return GetPspAnmPhase(anmIdx) == PspAnmPhase::Ready &&
           PspAnmLoadGeneration(anmIdx) == expectedGeneration;
}

void LogPspAnmMemory(const char *phase, const AnmLoaded *anmLoaded,
                     i32 entryNumber, const AnmRawEntry *entry)
{
    fprintf(stderr,
            "TH08PSP ANM phase=%s anm=%d entry=%d size=%ldx%ld fmt=%lu "
            "external_free=%lu\n",
            phase,
            anmLoaded != NULL ? anmLoaded->anmIdx : -1,
            entryNumber,
            entry != NULL ? static_cast<long>(entry->width) : 0L,
            entry != NULL ? static_cast<long>(entry->height) : 0L,
            entry != NULL ? static_cast<unsigned long>(entry->format) : 0UL,
            static_cast<unsigned long>(sceKernelTotalFreeMemSize()));
}

void ReleasePspAnmCandidate(i32 anmIdx)
{
    if (anmIdx < 0 || anmIdx >= static_cast<i32>(ARRAY_SIZE(g_PspAnmCompact)))
        return;
    PspAnmCompactState &state = g_PspAnmCompact[anmIdx];
    if (state.data != NULL)
        g_ZunMemory.Free(state.data);
    state.data = NULL;
    state.compactSize = 0;
}

bool ReleasePspAnmScratchSource(i32 anmIdx, void *sourceData)
{
    if (anmIdx < 0 || anmIdx >= static_cast<i32>(ARRAY_SIZE(g_PspAnmCompact)))
        return false;
    PspAnmCompactState &state = g_PspAnmCompact[anmIdx];
    if (!state.sourceIsScratch)
        return false;
    if (sourceData != state.sourceLease.base)
    {
        fprintf(stderr,
                "TH08PSP ANM_SCRATCH owner_mismatch anm=%ld load_generation=%lu source=0x%08lx lease=0x%08lx\n",
                static_cast<long>(anmIdx),
                static_cast<unsigned long>(state.loadGeneration),
                static_cast<unsigned long>(reinterpret_cast<uintptr_t>(sourceData)),
                static_cast<unsigned long>(reinterpret_cast<uintptr_t>(state.sourceLease.base)));
        th08::psp::AnmScratchRejectGenericFree(state.sourceLease.base);
    }
    else
    {
        th08::psp::AnmScratchRelease(state.sourceLease);
    }
    state.sourceIsScratch = false;
    state.sourceLease = th08::psp::AnmScratchLease{};
    return true;
}

void ClearPspAnmState(i32 anmIdx)
{
    if (anmIdx < 0 || anmIdx >= static_cast<i32>(ARRAY_SIZE(g_PspAnmCompact)))
        return;
    memset(&g_PspAnmCompact[anmIdx], 0, sizeof(g_PspAnmCompact[anmIdx]));
    SetPspAnmPhase(anmIdx, PspAnmPhase::Empty);
}

bool PreparePspAnmCompact(i32 anmIdx, AnmRawEntry *sourceBase, u32 sourceSize,
                          u32 *outEntryCount, u32 *outSpriteCount,
                          u32 *outScriptCount)
{
    if (anmIdx < 0 || anmIdx >= static_cast<i32>(ARRAY_SIZE(g_PspAnmCompact)) ||
        sourceBase == NULL || sourceSize < sizeof(AnmRawEntry) ||
        outEntryCount == NULL || outSpriteCount == NULL || outScriptCount == NULL)
        return false;

    ReleasePspAnmCandidate(anmIdx);
    u32 sourceOffset = 0;
    u32 compactSize = 0;
    u32 entryCount = 0;
    u32 spriteCount = 0;
    u32 scriptCount = 0;
    while (true)
    {
        if (sourceOffset > sourceSize || sourceSize - sourceOffset < sizeof(AnmRawEntry))
            return false;
        AnmRawEntry *entry = reinterpret_cast<AnmRawEntry *>(
            reinterpret_cast<u8 *>(sourceBase) + sourceOffset);
        const u32 entrySpan = entry->nextOffset != 0 ? entry->nextOffset
                                                      : sourceSize - sourceOffset;
        if (entrySpan < sizeof(AnmRawEntry) || entrySpan > sourceSize - sourceOffset ||
            entry->version != 3 || entry->numSprites < 0 || entry->numScripts < 0)
            return false;
        const u32 metadataSize = entry->hasData ? entry->textureOffset : entrySpan;
        if (metadataSize < sizeof(AnmRawEntry) || metadataSize > entrySpan ||
            metadataSize > 0xfffffffcu)
            return false;
        const u32 sprites = static_cast<u32>(entry->numSprites);
        const u32 scripts = static_cast<u32>(entry->numScripts);
        if (scripts > (0xffffffffu - sprites) / 2u)
            return false;
        const u32 tableWords = sprites + scripts * 2u;
        if (tableWords > (metadataSize - sizeof(AnmRawEntry)) / sizeof(u32))
            return false;
        if (spriteCount > 0x7fffffffu - sprites ||
            scriptCount > 0x7fffffffu - scripts || entryCount == 0x7fffffffu)
            return false;
        spriteCount += sprites;
        scriptCount += scripts;
        const u32 compactSpan = (metadataSize + 3u) & ~3u;
        if (compactSize > 0xffffffffu - compactSpan)
            return false;
        compactSize += compactSpan;
        ++entryCount;
        if (entry->nextOffset == 0)
            break;
        sourceOffset += entry->nextOffset;
    }

    if (compactSize > sourceSize)
        return false;
    *outEntryCount = entryCount;
    *outSpriteCount = spriteCount;
    *outScriptCount = scriptCount;

    PspAnmCompactState &state = g_PspAnmCompact[anmIdx];
    state.sourceSize = sourceSize;
    state.compactSize = compactSize;
    state.entryCount = entryCount;
    state.spriteCount = spriteCount;
    state.scriptCount = scriptCount;

    u8 *compactBase = static_cast<u8 *>(g_ZunMemory.Alloc(compactSize, "anm compact"));
    if (compactBase == NULL)
    {
        state.compactSize = 0;
        fprintf(stderr,
                "TH08PSP ANM_COMPACT phase=candidate_oom anm=%ld source=%lu requested=%lu\n",
                static_cast<long>(anmIdx), static_cast<unsigned long>(sourceSize),
                static_cast<unsigned long>(compactSize));
        return true;
    }

    sourceOffset = 0;
    u32 compactOffset = 0;
    for (u32 index = 0; index < entryCount; ++index)
    {
        AnmRawEntry *sourceEntry = reinterpret_cast<AnmRawEntry *>(
            reinterpret_cast<u8 *>(sourceBase) + sourceOffset);
        const u32 entrySpan = sourceEntry->nextOffset != 0
                                  ? sourceEntry->nextOffset
                                  : sourceSize - sourceOffset;
        const u32 metadataSize = sourceEntry->hasData ? sourceEntry->textureOffset
                                                       : entrySpan;
        const u32 compactSpan = (metadataSize + 3u) & ~3u;
        AnmRawEntry *compactEntry = reinterpret_cast<AnmRawEntry *>(
            compactBase + compactOffset);
        memcpy(compactEntry, sourceEntry, metadataSize);
        if (compactSpan > metadataSize)
            memset(compactBase + compactOffset + metadataSize, 0,
                   compactSpan - metadataSize);
        compactEntry->nextOffset = index + 1 < entryCount ? compactSpan : 0;
        compactOffset += compactSpan;
        if (sourceEntry->nextOffset != 0)
            sourceOffset += sourceEntry->nextOffset;
    }

    state.data = compactBase;
    fprintf(stderr,
            "TH08PSP ANM_COMPACT phase=prepared anm=%ld source=%lu compact=%lu entries=%lu scripts=%lu load_generation=%lu\n",
            static_cast<long>(anmIdx), static_cast<unsigned long>(sourceSize),
            static_cast<unsigned long>(compactSize), static_cast<unsigned long>(entryCount),
            static_cast<unsigned long>(scriptCount),
            static_cast<unsigned long>(state.loadGeneration));
    return true;
}

bool FinalizePspAnmCompact(AnmLoaded *anmLoaded)
{
    if (anmLoaded == NULL || anmLoaded->anmIdx < 0 ||
        anmLoaded->anmIdx >= static_cast<i32>(ARRAY_SIZE(g_PspAnmCompact)))
        return false;
    PspAnmCompactState &state = g_PspAnmCompact[anmLoaded->anmIdx];
    if (state.data == NULL || anmLoaded->rawData == NULL ||
        state.entryCount != static_cast<u32>(anmLoaded->totalEntries))
        return false;

    SetPspAnmPhase(anmLoaded->anmIdx, PspAnmPhase::Finalizing);
    u8 *sourceBase = reinterpret_cast<u8 *>(anmLoaded->rawData);
    u32 sourceOffset = 0;
    u32 compactOffset = 0;
    u32 scriptIndex = 0;

    // Pass 1 is validation only. Never expose a mixture of source and
    // candidate script pointers if a later offset is malformed.
    for (u32 entryIndex = 0; entryIndex < state.entryCount; ++entryIndex)
    {
        if (sourceOffset > state.sourceSize ||
            state.sourceSize - sourceOffset < sizeof(AnmRawEntry) ||
            compactOffset > state.compactSize ||
            state.compactSize - compactOffset < sizeof(AnmRawEntry))
            return false;
        AnmRawEntry *sourceEntry = reinterpret_cast<AnmRawEntry *>(sourceBase + sourceOffset);
        AnmRawEntry *compactEntry = reinterpret_cast<AnmRawEntry *>(state.data + compactOffset);
        const u32 entrySpan = sourceEntry->nextOffset != 0
                                  ? sourceEntry->nextOffset
                                  : state.sourceSize - sourceOffset;
        const u32 metadataSize = sourceEntry->hasData ? sourceEntry->textureOffset
                                                       : entrySpan;
        const u32 compactSpan = (metadataSize + 3u) & ~3u;
        if (sourceEntry->numSprites < 0 || sourceEntry->numScripts < 0 ||
            metadataSize < sizeof(AnmRawEntry) || metadataSize > entrySpan ||
            compactSpan > state.compactSize - compactOffset)
            return false;
        const u32 tableWords = static_cast<u32>(sourceEntry->numSprites) +
                               static_cast<u32>(sourceEntry->numScripts) * 2u;
        if (sizeof(AnmRawEntry) + tableWords * sizeof(u32) > metadataSize)
            return false;
        const u32 *offsetTable = reinterpret_cast<const u32 *>(
            reinterpret_cast<const u8 *>(compactEntry) + sizeof(AnmRawEntry));
        for (i32 script = 0; script < sourceEntry->numScripts; ++script)
        {
            const u32 scriptOffset = offsetTable[sourceEntry->numSprites + script * 2 + 1];
            if (scriptOffset >= metadataSize || scriptIndex >= state.scriptCount)
                return false;
            ++scriptIndex;
        }
        compactOffset += compactSpan;
        if (sourceEntry->nextOffset != 0)
            sourceOffset += sourceEntry->nextOffset;
    }
    if (scriptIndex != state.scriptCount)
        return false;

    // Pass 2 commits only after every entry and script offset was accepted.
    sourceOffset = 0;
    compactOffset = 0;
    scriptIndex = 0;
    for (u32 entryIndex = 0; entryIndex < state.entryCount; ++entryIndex)
    {
        AnmRawEntry *sourceEntry = reinterpret_cast<AnmRawEntry *>(sourceBase + sourceOffset);
        AnmRawEntry *compactEntry = reinterpret_cast<AnmRawEntry *>(state.data + compactOffset);
        const u32 entrySpan = sourceEntry->nextOffset != 0
                                  ? sourceEntry->nextOffset
                                  : state.sourceSize - sourceOffset;
        const u32 metadataSize = sourceEntry->hasData ? sourceEntry->textureOffset
                                                       : entrySpan;
        const u32 compactSpan = (metadataSize + 3u) & ~3u;
        const u32 *offsetTable = reinterpret_cast<const u32 *>(
            reinterpret_cast<const u8 *>(compactEntry) + sizeof(AnmRawEntry));
        for (i32 script = 0; script < sourceEntry->numScripts; ++script)
        {
            const u32 scriptOffset = offsetTable[sourceEntry->numSprites + script * 2 + 1];
            anmLoaded->scripts[scriptIndex++] = reinterpret_cast<AnmRawInstr *>(
                reinterpret_cast<u8 *>(compactEntry) + scriptOffset);
        }
        compactOffset += compactSpan;
        if (sourceEntry->nextOffset != 0)
            sourceOffset += sourceEntry->nextOffset;
    }

    AnmRawEntry *sourceData = anmLoaded->rawData;
    anmLoaded->rawData = reinterpret_cast<AnmRawEntry *>(state.data);
    const u32 sourceSize = state.sourceSize;
    const u32 compactSize = state.compactSize;
    state.data = NULL;
    bool sourceReleased = true;
    if (state.sourceIsScratch)
        sourceReleased = th08::psp::AnmScratchRelease(state.sourceLease);
    else
        g_ZunMemory.Free(sourceData);
    state.sourceIsScratch = false;
    state.sourceLease = th08::psp::AnmScratchLease{};
    if (!sourceReleased)
    {
        SetPspAnmPhase(anmLoaded->anmIdx, PspAnmPhase::Failed);
        return false;
    }
    SetPspAnmPhase(anmLoaded->anmIdx, PspAnmPhase::Ready);
    fprintf(stderr,
            "TH08PSP ANM_COMPACT phase=active anm=%ld source=%lu compact=%lu saved=%lu load_generation=%lu\n",
            static_cast<long>(anmLoaded->anmIdx), static_cast<unsigned long>(sourceSize),
            static_cast<unsigned long>(compactSize),
            static_cast<unsigned long>(sourceSize - compactSize),
            static_cast<unsigned long>(state.loadGeneration));
    return true;
}
} // namespace
#endif

// FUNCTION: th08 0x40b580
VertexDiffuseXyzrhw::VertexDiffuseXyzrhw()
{
}

DIFFABLE_STATIC(AnmManager *, g_AnmManager);
DIFFABLE_STATIC_ARRAY(VertexTex1DiffuseXyzrhw, 4, g_QuadVertices);
DIFFABLE_STATIC_ARRAY(VertexTex0Xyzrhw, 4, g_AnmManagerUntexturedQuadVertices);
DIFFABLE_STATIC_ARRAY(VertexTex0Xyzrhw, 4, g_BackgroundQuadVertices);

#if defined(PSP)
// Keep OFF/AUDIT/PRODUCT BSS geometry identical.  Only M0 writes these
// presentation counters; product hot code contains no telemetry increments.
alignas(4) PspBulletOnePass4VStats g_PspBulletOnePass4VStats
    __attribute__((used)){};

PspBulletOnePass4VStats PspPeekBulletOnePass4VStats()
{
    return g_PspBulletOnePass4VStats;
}

PspBulletOnePass4VStats PspTakeBulletOnePass4VStats()
{
    const PspBulletOnePass4VStats snapshot = g_PspBulletOnePass4VStats;
    memset(&g_PspBulletOnePass4VStats, 0,
           sizeof(g_PspBulletOnePass4VStats));
    return snapshot;
}

void PspResetBulletOnePass4VStats()
{
    memset(&g_PspBulletOnePass4VStats, 0,
           sizeof(g_PspBulletOnePass4VStats));
}
#endif

#if defined(PSP)
namespace
{
enum class PspSpritePairRunOwner : u8
{
    None,
    ItemTime,
    Item,
    Bullet,
};

enum PspItemTimeDrawPairCacheValidation : u32
{
    PSP_ITEM_TIME_DRAW_PAIR_CACHE_UNVALIDATED = 0,
    PSP_ITEM_TIME_DRAW_PAIR_CACHE_VALID,
    PSP_ITEM_TIME_DRAW_PAIR_CACHE_RANGE_INVALID,
    PSP_ITEM_TIME_DRAW_PAIR_CACHE_FINGERPRINT_INVALID,
};

struct PspItemTimeDrawPairCache
{
    AnmLoaded *owner;
    AnmRawEntry *rawData;
    AnmRawInstr **scripts;
    AnmLoadedSprite *sprites;
    AnmRawInstr *script68;
    u32 generation;
    i32 anmIdx;
    PspItemTimeDrawPairCacheValidation validation;
};

struct PspSpritePairRunState
{
    PspSpritePairRunOwner owner;
    u8 passActive;
    u8 runActive;
    u8 backendReady;
    u32 pairCount;
    u32 auditRunPairs;
    u32 passCandidates;
    u32 passVisible;
    u32 passEligible;
    u32 passRuns;
    AnmVm *representativeVm;
    IDirect3DTexture8 *texture;
    u32 mixColor;
    u8 blendMode;
    u8 zWriteDisabled;
    u8 useMixColor;
    u8 depthTestDisabled;
#if TH08_PSP_ANY_MIXED_QUADS_ENABLED
    u32 generalCount;
    u8 stickyGeneral;
    u8 stateRunActive;
    u8 reservedBulletBytes[2];
#endif
};

struct PspItemTimeDrawPairSidecar
{
    PspItemTimeDrawPairCache cache;
    ItemTimeDrawPairStats stats;
    PspSpritePairRunState run;
#if TH08_PSP_ANY_MIXED_QUADS_ENABLED
    BulletMixedQuadStats bulletStats;
    BulletMixedQuadStats itemStats;
    u32 rejectedItemPassDepth;
    u32 rejectedBulletPassDepth;
#endif
};

// The reservation exists in every PSP build, including OFF.  Audit/product
// flags therefore cannot move later BSS objects or invalidate FPS/memory A/B.
// The generous fixed envelope also leaves room for the generic Bullet owner
// without changing this reservation's address or size.
constexpr u32 kPspItemTimeDrawPairSidecarBytes = 512U;
static_assert(sizeof(void *) == 4U,
              "PSP sprite-pair sidecar requires the 32-bit ABI");
static_assert(sizeof(PspItemTimeDrawPairSidecar) <=
                  kPspItemTimeDrawPairSidecarBytes,
              "PSP sprite-pair sidecar exceeded its fixed reservation");
static_assert(alignof(PspItemTimeDrawPairSidecar) <= 4U,
              "PSP sprite-pair sidecar alignment changed");
alignas(4) u8 g_PspItemTimeDrawPairSidecarReservation
    [kPspItemTimeDrawPairSidecarBytes] __attribute__((used));

[[maybe_unused]] PspItemTimeDrawPairSidecar &
GetPspItemTimeDrawPairSidecar()
{
    return *reinterpret_cast<PspItemTimeDrawPairSidecar *>(
        g_PspItemTimeDrawPairSidecarReservation);
}
} // namespace
#endif

#if defined(PSP)
namespace
{
struct PspItemNaturalQuadStorage
{
    PspItemNaturalQuadStats stats;
    const u16 *indexAuthority;
    u32 validatedIndexCount;
    u32 batchTriggerQuads;
    u8 currentTarget;
    u8 currentTargetAppended;
    u8 batchMarked;
    u8 indexAuthorityRejected;
};

static_assert(sizeof(void *) == 4U,
              "PSP Item natural-quad sidecar requires the 32-bit ABI");
static_assert(sizeof(PspItemNaturalQuadStats) == 136U,
              "Unexpected Item natural-quad stats layout");
static_assert(offsetof(PspItemNaturalQuadStorage, indexAuthority) == 136U,
              "Item natural-quad authority offset changed");
static_assert(sizeof(PspItemNaturalQuadStorage) == 152U,
              "Item natural-quad OFF/ON reservation must remain 152 bytes");
static_assert(alignof(PspItemNaturalQuadStorage) == 4U,
              "Unexpected Item natural-quad reservation alignment");

alignas(4) PspItemNaturalQuadStorage g_PspItemNaturalQuadStorage
    __attribute__((used));

#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
void ResetPspItemNaturalQuadBatchMarker()
{
    g_PspItemNaturalQuadStorage.batchMarked = 0U;
    g_PspItemNaturalQuadStorage.batchTriggerQuads = 0U;
}

void AbandonPspItemNaturalQuadBatch(u32 quads)
{
    if (g_PspItemNaturalQuadStorage.batchMarked == 0U)
        return;
    ++g_PspItemNaturalQuadStorage.stats.abandonedBatches;
    g_PspItemNaturalQuadStorage.stats.abandonedQuads += quads;
    ResetPspItemNaturalQuadBatchMarker();
}

void MarkPspItemNaturalQuadCanonicalAppend()
{
    if (g_PspItemNaturalQuadStorage.currentTarget == 0U ||
        g_PspItemNaturalQuadStorage.currentTargetAppended != 0U)
    {
        return;
    }
    g_PspItemNaturalQuadStorage.currentTargetAppended = 1U;
    g_PspItemNaturalQuadStorage.batchMarked = 1U;
    ++g_PspItemNaturalQuadStorage.batchTriggerQuads;
    ++g_PspItemNaturalQuadStorage.stats.visibleItemTime;
    ++g_PspItemNaturalQuadStorage.stats.triggerQuads;
}
#endif
} // namespace

#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
void PspItemNaturalQuadNotePass()
{
    ++g_PspItemNaturalQuadStorage.stats.passes;
}

void PspItemNaturalQuadSetCurrentTarget(bool active)
{
    if (active)
    {
        ++g_PspItemNaturalQuadStorage.stats.itemTimeCandidates;
        g_PspItemNaturalQuadStorage.currentTarget = 1U;
        g_PspItemNaturalQuadStorage.currentTargetAppended = 0U;
        return;
    }
    if (g_PspItemNaturalQuadStorage.currentTarget != 0U &&
        g_PspItemNaturalQuadStorage.currentTargetAppended == 0U)
    {
        ++g_PspItemNaturalQuadStorage.stats.culledItemTime;
    }
    g_PspItemNaturalQuadStorage.currentTarget = 0U;
    g_PspItemNaturalQuadStorage.currentTargetAppended = 0U;
}

void PspQueryItemNaturalQuadStats(PspItemNaturalQuadStats *stats)
{
    if (stats != NULL)
        *stats = g_PspItemNaturalQuadStorage.stats;
}

void PspResetItemNaturalQuadStats()
{
    memset(&g_PspItemNaturalQuadStorage.stats, 0,
           sizeof(g_PspItemNaturalQuadStorage.stats));
}
#endif
#endif

#if defined(PSP) && defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
    TH08_PSP_BULLET_UNIFIED_QUADS
namespace
{
// BulletManager has 0x600 logical enemy-bullet slots. Four unique vertices
// per slot keep the largest possible callback far below the u16 index ceiling.
constexpr u32 kPspBulletUnifiedQuadCapacity = 0x600U;
constexpr u32 kPspBulletUnifiedQuadIndexCount =
    kPspBulletUnifiedQuadCapacity * 6U;
static_assert(kPspBulletUnifiedQuadCapacity * 4U - 1U <= 0xffffU,
              "Unified Bullet quad indices must remain 16-bit");

alignas(64) u16
    g_PspBulletUnifiedQuadIndices[kPspBulletUnifiedQuadIndexCount];
bool g_PspBulletUnifiedQuadIndicesReady = false;
bool g_PspBulletUnifiedQuadBatchActive = false;
// Fixed in every PSP unified-quad build so M1 OFF/ON preserves BSS geometry.
// Behavior remains compile-time gated at every use site.
[[maybe_unused]] bool g_PspBulletPackedVertexFastpathActive
    __attribute__((used)) = false;

enum class PspUnifiedQuadBatchOwner : u8
{
    None,
    Item,
    Bullet,
    Effect,
};

PspUnifiedQuadBatchOwner g_PspUnifiedQuadBatchOwner
    __attribute__((used)) =
    PspUnifiedQuadBatchOwner::None;

#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED
PspEffectIndexedQuadStats g_PspEffectIndexedQuadStats{};
bool g_PspEffectIndexedQuadPassOpen = false;
bool g_PspEffectIndexedQuadPassAbandoned = false;

void AbandonPspEffectIndexedQuadPass()
{
    if (!g_PspEffectIndexedQuadPassOpen ||
        g_PspEffectIndexedQuadPassAbandoned)
    {
        return;
    }
    ++g_PspEffectIndexedQuadStats.abandonedPasses;
    g_PspEffectIndexedQuadPassAbandoned = true;
}

void NotePspEffectIndexedQuadSuccess(u32 quads)
{
    ++g_PspEffectIndexedQuadStats.batches;
    g_PspEffectIndexedQuadStats.successfulOrdinaryQuads += quads;
    const u32 savedVertices = quads * 2U;
    g_PspEffectIndexedQuadStats.verticesSaved += savedVertices;
    g_PspEffectIndexedQuadStats.bytesSaved +=
        savedVertices * sizeof(VertexTex1DiffuseXyzrhw);
    if (g_PspEffectIndexedQuadStats.maxBatchQuads < quads)
        g_PspEffectIndexedQuadStats.maxBatchQuads = quads;
}

void NotePspEffectIndexedQuadFallback(u32 requestedQuads,
                                      u32 recoveredQuads)
{
    ++g_PspEffectIndexedQuadStats.fallbacks;
    g_PspEffectIndexedQuadStats.fallbackQuads += requestedQuads;
    if (recoveredQuads >= requestedQuads)
        return;
    g_PspEffectIndexedQuadStats.abandonedQuads +=
        requestedQuads - recoveredQuads;
    AbandonPspEffectIndexedQuadPass();
}
#endif

void InitializePspBulletUnifiedQuadIndices()
{
    if (g_PspBulletUnifiedQuadIndicesReady)
        return;
    for (u32 sprite = 0; sprite < kPspBulletUnifiedQuadCapacity; ++sprite)
    {
        const u16 base = static_cast<u16>(sprite * 4U);
        g_PspBulletUnifiedQuadIndices[sprite * 6U + 0U] = base;
        g_PspBulletUnifiedQuadIndices[sprite * 6U + 1U] =
            static_cast<u16>(base + 1U);
        g_PspBulletUnifiedQuadIndices[sprite * 6U + 2U] =
            static_cast<u16>(base + 2U);
        g_PspBulletUnifiedQuadIndices[sprite * 6U + 3U] =
            static_cast<u16>(base + 1U);
        g_PspBulletUnifiedQuadIndices[sprite * 6U + 4U] =
            static_cast<u16>(base + 2U);
        g_PspBulletUnifiedQuadIndices[sprite * 6U + 5U] =
            static_cast<u16>(base + 3U);
    }
    g_PspBulletUnifiedQuadIndicesReady = true;
}

bool PspBulletUnifiedQuadBufferCanAppendVertices(const AnmManager *manager,
                                                 u32 vertexCount)
{
    if (manager == NULL || manager->vertexBufferEndPtr == NULL ||
        vertexCount == 0U)
        return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(manager->vertexBuffer);
    const uintptr_t end = reinterpret_cast<uintptr_t>(
        manager->vertexBuffer + ARRAY_SIZE(manager->vertexBuffer));
    const uintptr_t cursor =
        reinterpret_cast<uintptr_t>(manager->vertexBufferEndPtr);
    const uintptr_t bytes = static_cast<uintptr_t>(vertexCount) *
        sizeof(VertexTex1DiffuseXyzrhw);
    return cursor >= begin && cursor <= end && end - cursor >= bytes &&
           (cursor - begin) % sizeof(VertexTex1DiffuseXyzrhw) == 0U;
}

bool PspBulletUnifiedQuadBufferCanAppend(const AnmManager *manager)
{
    return PspBulletUnifiedQuadBufferCanAppendVertices(manager, 4U);
}

#if TH08_PSP_ANY_MIXED_QUADS_ENABLED
void RestorePspMixedQuadOwnerTokensIfQuiescent()
{
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();

    // A rejected nested pass always runs canonically with every unified/native
    // token suspended. Restore exactly one previous owner only after both
    // nesting counters have drained; ItemTime deliberately owns no unified
    // token and therefore remains on its independent pair/canonical path.
    g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::None;
    g_PspBulletUnifiedQuadBatchActive = false;
#if defined(TH08_PSP_BULLET_DIRECT_GE) && TH08_PSP_BULLET_DIRECT_GE
    th08_psp_bullet_direct_ge_set_batch(false);
#endif
#if TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED
    th08_psp_item_mixed_ge_set_batch(false);
#endif
    if (sidecar.rejectedItemPassDepth != 0U ||
        sidecar.rejectedBulletPassDepth != 0U ||
        sidecar.run.passActive == 0U)
    {
        return;
    }

    if (sidecar.run.owner == PspSpritePairRunOwner::ItemTime)
        return;

    if (sidecar.run.owner == PspSpritePairRunOwner::Bullet)
    {
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
        g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::Bullet;
        g_PspBulletUnifiedQuadBatchActive = true;
#if defined(TH08_PSP_BULLET_DIRECT_GE) && TH08_PSP_BULLET_DIRECT_GE
        th08_psp_bullet_direct_ge_set_batch(true);
#endif
#endif
        return;
    }

#if TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED
    if (sidecar.run.owner == PspSpritePairRunOwner::Item)
    {
        g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::Item;
        g_PspBulletUnifiedQuadBatchActive = true;
        th08_psp_item_mixed_ge_set_batch(true);
        return;
    }
#endif

    // Item AUDIT is canonical, while ItemTime uses a separate frontend. Both
    // intentionally leave the unified/native owner tokens disabled.
}

u32 PspBulletMixedFloatBits(f32 value)
{
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool PspBulletMixedFloatBitsEqual(f32 left, f32 right)
{
    return PspBulletMixedFloatBits(left) == PspBulletMixedFloatBits(right);
}

PspBulletMixedQuadRejectReason ClassifyPspBulletMixedQuad(
    const VertexTex1DiffuseXyzrhw *quad)
{
    if (quad == NULL)
        return PSP_BULLET_MIXED_QUAD_REJECT_NONFINITE;
    for (u32 vertex = 0U; vertex < 4U; ++vertex)
    {
        if (!std::isfinite(quad[vertex].pos.x) ||
            !std::isfinite(quad[vertex].pos.y) ||
            !std::isfinite(quad[vertex].pos.z) ||
            !std::isfinite(quad[vertex].w) ||
            !std::isfinite(quad[vertex].textureUV.x) ||
            !std::isfinite(quad[vertex].textureUV.y))
        {
            return PSP_BULLET_MIXED_QUAD_REJECT_NONFINITE;
        }
    }

    if (!PspBulletMixedFloatBitsEqual(quad[0].pos.z, quad[1].pos.z) ||
        !PspBulletMixedFloatBitsEqual(quad[0].pos.z, quad[2].pos.z) ||
        !PspBulletMixedFloatBitsEqual(quad[0].pos.z, quad[3].pos.z) ||
        !PspBulletMixedFloatBitsEqual(quad[0].w, quad[1].w) ||
        !PspBulletMixedFloatBitsEqual(quad[0].w, quad[2].w) ||
        !PspBulletMixedFloatBitsEqual(quad[0].w, quad[3].w) ||
        PspBulletMixedFloatBits(quad[0].w) !=
            PspBulletMixedFloatBits(1.0f))
    {
        return PSP_BULLET_MIXED_QUAD_REJECT_Z_OR_W;
    }
    if (quad[0].diffuse != quad[1].diffuse ||
        quad[0].diffuse != quad[2].diffuse ||
        quad[0].diffuse != quad[3].diffuse)
    {
        return PSP_BULLET_MIXED_QUAD_REJECT_DIFFUSE;
    }
    if (!PspBulletMixedFloatBitsEqual(quad[0].pos.x, quad[2].pos.x) ||
        !PspBulletMixedFloatBitsEqual(quad[1].pos.x, quad[3].pos.x) ||
        !PspBulletMixedFloatBitsEqual(quad[0].pos.y, quad[1].pos.y) ||
        !PspBulletMixedFloatBitsEqual(quad[2].pos.y, quad[3].pos.y))
    {
        return PSP_BULLET_MIXED_QUAD_REJECT_AXIS;
    }
    if (!PspBulletMixedFloatBitsEqual(quad[0].textureUV.x,
                                      quad[2].textureUV.x) ||
        !PspBulletMixedFloatBitsEqual(quad[1].textureUV.x,
                                      quad[3].textureUV.x) ||
        !PspBulletMixedFloatBitsEqual(quad[0].textureUV.y,
                                      quad[1].textureUV.y) ||
        !PspBulletMixedFloatBitsEqual(quad[2].textureUV.y,
                                      quad[3].textureUV.y))
    {
        return PSP_BULLET_MIXED_QUAD_REJECT_UV;
    }

    // GE_SPRITES consumes the first and second vertices as the upper-left and
    // lower-right corners.  Strictly increasing geometry and UV axes reject
    // zero area and every geometric or texture mirror without guessing.
    if (!(quad[1].pos.x > quad[0].pos.x) ||
        !(quad[2].pos.y > quad[0].pos.y) ||
        !(quad[1].textureUV.x > quad[0].textureUV.x) ||
        !(quad[2].textureUV.y > quad[0].textureUV.y))
    {
        return PSP_BULLET_MIXED_QUAD_REJECT_AREA_OR_MIRROR;
    }
    return PSP_BULLET_MIXED_QUAD_ACCEPT;
}

BulletMixedQuadStats &PspMixedQuadStatsForOwner(
    PspSpritePairRunOwner owner)
{
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
    return owner == PspSpritePairRunOwner::Item
        ? sidecar.itemStats
        : sidecar.bulletStats;
}

void NotePspMixedReject(BulletMixedQuadStats &stats,
                        PspBulletMixedQuadRejectReason reason)
{
    switch (reason)
    {
    case PSP_BULLET_MIXED_QUAD_REJECT_NONFINITE:
        ++stats.nonfiniteFallbacks;
        break;
    case PSP_BULLET_MIXED_QUAD_REJECT_AXIS:
        ++stats.axisFallbacks;
        break;
    case PSP_BULLET_MIXED_QUAD_REJECT_AREA_OR_MIRROR:
        ++stats.areaOrMirrorFallbacks;
        break;
    case PSP_BULLET_MIXED_QUAD_REJECT_Z_OR_W:
        ++stats.zOrWFallbacks;
        break;
    case PSP_BULLET_MIXED_QUAD_REJECT_UV:
        ++stats.uvFallbacks;
        break;
    case PSP_BULLET_MIXED_QUAD_REJECT_DIFFUSE:
        ++stats.diffuseFallbacks;
        break;
    case PSP_BULLET_MIXED_QUAD_ACCEPT:
        break;
    }
}

void NotePspBulletMixedReject(PspBulletMixedQuadRejectReason reason)
{
    NotePspMixedReject(
        GetPspItemTimeDrawPairSidecar().bulletStats, reason);
}

void NotePspItemMixedReject(PspBulletMixedQuadRejectReason reason)
{
    NotePspMixedReject(
        GetPspItemTimeDrawPairSidecar().itemStats, reason);
}

bool PspMixedWouldUsePair(PspBulletMixedQuadRejectReason reason)
{
    const PspSpritePairRunState &run =
        GetPspItemTimeDrawPairSidecar().run;
    return reason == PSP_BULLET_MIXED_QUAD_ACCEPT &&
           run.stickyGeneral == 0U;
}

bool PspBulletMixedWouldUsePair(PspBulletMixedQuadRejectReason reason)
{
    return PspMixedWouldUsePair(reason);
}

bool PspItemMixedWouldUsePair(PspBulletMixedQuadRejectReason reason)
{
    return PspMixedWouldUsePair(reason);
}

void RecordPspMixedQuad(PspSpritePairRunOwner owner,
                        PspBulletMixedQuadRejectReason reason,
                        bool usePair)
{
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
    PspSpritePairRunState &run = sidecar.run;
    BulletMixedQuadStats &stats = PspMixedQuadStatsForOwner(owner);
    if (run.stateRunActive == 0U)
    {
        run.stateRunActive = 1U;
        run.stickyGeneral = 0U;
        ++stats.stateRuns;
    }
    run.runActive = 1U;
    ++stats.candidates;
    if (usePair)
    {
        ++run.pairCount;
        ++stats.eligiblePrefixQuads;
        // Bullet already enters this experiment as a 4V indexed quad; Item's
        // untouched authority is the canonical 6V triangle list.
        stats.frontendVerticesSaved +=
            owner == PspSpritePairRunOwner::Item ? 4U : 2U;
        stats.geVerticesSaved += 4U;
        return;
    }

    ++run.generalCount;
    ++stats.generalQuads;
    if (owner == PspSpritePairRunOwner::Item)
        stats.frontendVerticesSaved += 2U;
    if (reason == PSP_BULLET_MIXED_QUAD_ACCEPT)
        ++stats.stickyGeneralQuads;
    else
        NotePspMixedReject(stats, reason);
    run.stickyGeneral = 1U;
}

void RecordPspBulletMixedQuad(PspBulletMixedQuadRejectReason reason,
                              bool usePair)
{
    RecordPspMixedQuad(
        PspSpritePairRunOwner::Bullet, reason, usePair);
}

void RecordPspItemMixedQuad(PspBulletMixedQuadRejectReason reason,
                            bool usePair)
{
    RecordPspMixedQuad(
        PspSpritePairRunOwner::Item, reason, usePair);
}

void FinishPspMixedBatch(PspSpritePairRunOwner owner, bool submitted,
                         bool backendFallback,
                         u32 canonicalRecoveredQuads,
                         bool failClosed, bool recoveryDrawFailed)
{
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
    PspSpritePairRunState &run = sidecar.run;
    BulletMixedQuadStats &stats = PspMixedQuadStatsForOwner(owner);
    if (run.owner != owner ||
        !run.runActive ||
        (run.pairCount == 0U && run.generalCount == 0U))
    {
        return;
    }
    ++stats.batches;
    if (stats.maxPairPrefix < run.pairCount)
        stats.maxPairPrefix = run.pairCount;
    if (stats.maxGeneralSuffix < run.generalCount)
        stats.maxGeneralSuffix = run.generalCount;
    if (submitted)
    {
        ++stats.submittedBatches;
        stats.submittedPairQuads += run.pairCount;
        stats.submittedGeneralQuads += run.generalCount;
    }
    if (backendFallback)
        ++stats.backendFallbackBatches;
    const bool countsBounded =
        run.pairCount <= kPspBulletUnifiedQuadCapacity &&
        run.generalCount <= kPspBulletUnifiedQuadCapacity &&
        run.pairCount <=
            kPspBulletUnifiedQuadCapacity - run.generalCount;
    const u32 totalQuads =
        countsBounded ? run.pairCount + run.generalCount : 0U;
    // Partial replay is not presented as success.  The explicit failure
    // counter and fail-closed batch reveal the dropped tail instead.
    if (countsBounded && canonicalRecoveredQuads == totalQuads &&
        totalQuads != 0U)
        stats.canonicalRecoveryQuads += totalQuads;
    if (failClosed)
        ++stats.failClosedBatches;
    if (recoveryDrawFailed)
        ++stats.canonicalRecoveryDrawFailures;
    run.runActive = 0U;
    run.pairCount = 0U;
    run.generalCount = 0U;
    run.stateRunActive = 0U;
    run.stickyGeneral = 0U;
}

void FinishPspBulletMixedBatch(bool submitted, bool backendFallback,
                               u32 canonicalRecoveredQuads,
                               bool failClosed, bool recoveryDrawFailed)
{
    FinishPspMixedBatch(PspSpritePairRunOwner::Bullet, submitted,
                        backendFallback, canonicalRecoveredQuads,
                        failClosed, recoveryDrawFailed);
}

void FinishPspItemMixedBatch(bool submitted, bool backendFallback,
                             u32 canonicalRecoveredQuads,
                             bool failClosed, bool recoveryDrawFailed)
{
    FinishPspMixedBatch(PspSpritePairRunOwner::Item, submitted,
                        backendFallback, canonicalRecoveredQuads,
                        failClosed, recoveryDrawFailed);
}
#endif
} // namespace
#endif

#if defined(PSP) && TH08_PSP_BULLET_MIXED_QUADS_ENABLED
const BulletMixedQuadStats &GetBulletMixedQuadStats()
{
    return GetPspItemTimeDrawPairSidecar().bulletStats;
}

void AnmManager::ResetPspBulletMixedQuadStats()
{
    memset(&GetPspItemTimeDrawPairSidecar().bulletStats, 0,
           sizeof(BulletMixedQuadStats));
}
#endif

#if defined(PSP) && TH08_PSP_ITEM_MIXED_QUADS_ENABLED
const ItemMixedQuadStats &GetItemMixedQuadStats()
{
    return GetPspItemTimeDrawPairSidecar().itemStats;
}

void AnmManager::ResetPspItemMixedQuadStats()
{
    memset(&GetPspItemTimeDrawPairSidecar().itemStats, 0,
           sizeof(ItemMixedQuadStats));
}
#endif

D3DFORMAT g_TextureFormatD3D8Mapping[] = {D3DFMT_UNKNOWN, D3DFMT_A8R8G8B8, D3DFMT_A1R5G5B5,
                                          D3DFMT_R5G6B5,  D3DFMT_R8G8B8,   D3DFMT_A4R4G4B4};

u32 g_TextureFormatBytesPerPixel[] = {4, 4, 2, 2, 3, 2};

ZunResult AnmLoaded::SetSprite(AnmVm *vm, int spriteIdx)
{
    if (this->rawData == NULL || LoadPendingAnmEntries(this) != 0)
    {
        return ZUN_ERROR;
    }

    vm->anmFile = this;
    vm->activeSpriteIndex = spriteIdx;
    vm->loadedSprite = &this->sprites[spriteIdx];
    vm->spriteSize.x = vm->loadedSprite->widthPx;
    vm->spriteSize.y = vm->loadedSprite->heightPx;

    D3DXMatrixIdentity(&vm->matrix1);
    D3DXMatrixIdentity(&vm->matrix3);

    /* ZUN bloat: what does this do? */
    if (vm->loadedSprite->scaleFactor.x < 1.0f)
    {
        spriteIdx = 0;
    }

    vm->matrix1.m[0][0] = vm->spriteSize.x / 256.0f;
    vm->matrix1.m[1][1] = vm->spriteSize.y / 256.0f;

    vm->matrix3.m[0][0] = (vm->spriteSize.x / vm->loadedSprite->width) * vm->loadedSprite->scaleFactor.x;
    vm->matrix3.m[1][1] = (vm->spriteSize.y / vm->loadedSprite->height) * vm->loadedSprite->scaleFactor.y;

    vm->matrix2 = vm->matrix1;

    return ZUN_SUCCESS;
}

void AnmLoaded::SetAndExecuteScript(AnmVm *vm, AnmRawInstr *beginningOfScript)
{
    if (beginningOfScript == NULL || LoadPendingAnmEntries(this) != 0)
    {
        memset(vm, 0, sizeof(AnmVm));
    }
    else
    {
        vm->Initialize();
        vm->anmFileIndex = this->anmIdx;
        vm->anmFile = this;
        vm->flip = 0;
        vm->beginningOfScript = beginningOfScript;
        vm->currentInstruction = vm->beginningOfScript;
        vm->currentTimeInScript = 0;
        vm->visible = FALSE;
        g_AnmManager->ExecuteScript(vm);
        g_AnmManager->scriptsStartedThisFrame++;
    }
}

#if defined(PSP) && \
    ((defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT) && \
      TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT) || \
     (defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH) && \
      TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH) || \
     (defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) && \
      TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) || \
     (defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH) && \
      TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH))
u32 AnmLoaded::PspLoadGenerationForItemTimeSpawnInit() const
{
    return PspAnmLoadGeneration(this->anmIdx);
}

bool AnmLoaded::PspLoadReadyForItemTimeSpawnInit(
    u32 expectedGeneration) const
{
    return this->rawData != NULL && this->sprites != NULL &&
           this->scripts != NULL && LoadPendingAnmEntries(this) == 0 &&
           PspAnmLoadReady(this->anmIdx, expectedGeneration);
}

bool AnmLoaded::PspItemTimeSpawnInitTablesContain(
    u32 expectedGeneration, i32 scriptIndex, i32 spriteIndex) const
{
    if (!PspLoadReadyForItemTimeSpawnInit(expectedGeneration) ||
        scriptIndex < 0 || spriteIndex < 0 || this->anmIdx < 0 ||
        this->anmIdx >= static_cast<i32>(ARRAY_SIZE(g_PspAnmCompact)))
    {
        return false;
    }

    const PspAnmCompactState &state = g_PspAnmCompact[this->anmIdx];
    return static_cast<u32>(scriptIndex) < state.scriptCount &&
           static_cast<u32>(spriteIndex) < state.spriteCount;
}

bool AnmLoaded::PspItemTimeSpawnInitScriptRangeContains(
    u32 expectedGeneration, const void *script, u32 byteCount) const
{
    if (!PspLoadReadyForItemTimeSpawnInit(expectedGeneration) ||
        script == NULL || byteCount == 0 || this->anmIdx < 0 ||
        this->anmIdx >= static_cast<i32>(ARRAY_SIZE(g_PspAnmCompact)))
    {
        return false;
    }

    const PspAnmCompactState &state = g_PspAnmCompact[this->anmIdx];
    if (state.compactSize == 0 || byteCount > state.compactSize)
        return false;

    const uintptr_t base = reinterpret_cast<uintptr_t>(this->rawData);
    const uintptr_t address = reinterpret_cast<uintptr_t>(script);
    if (address < base || (address & (alignof(i32) - 1U)) != 0U)
        return false;
    const uintptr_t offset = address - base;
    return offset <= static_cast<uintptr_t>(state.compactSize - byteCount);
}
#endif

f32 AnmVm::GetFloatVar(f32 varId)
{
    switch ((int)varId)
    {
    case AnmVariable_I0:
        return this->intVar0;
    case AnmVariable_I1:
        return this->intVar1;
    case AnmVariable_I2:
        return this->intVar2;
    case AnmVariable_I3:
        return this->intVar3;
    case AnmVariable_F0:
        return this->floatVar0;
    case AnmVariable_F1:
        return this->floatVar1;
    case AnmVariable_F2:
        return this->floatVar2;
    case AnmVariable_F3:
        return this->floatVar3;
    case AnmVariable_IC0:
        return this->counterVar0;
    case AnmVariable_IC1:
        return this->counterVar1;
    default:
        return varId;
    }
}

i32 AnmVm::GetIntVar(i32 varId)
{
    switch (varId)
    {
    case AnmVariable_I0:
        return this->intVar0;
    case AnmVariable_I1:
        return this->intVar1;
    case AnmVariable_I2:
        return this->intVar2;
    case AnmVariable_I3:
        return this->intVar3;
    case AnmVariable_F0:
        return this->floatVar0;
    case AnmVariable_F1:
        return this->floatVar1;
    case AnmVariable_F2:
        return this->floatVar2;
    case AnmVariable_F3:
        return this->floatVar3;
    case AnmVariable_IC0:
        return this->counterVar0;
    case AnmVariable_IC1:
        return this->counterVar1;
    default:
        return varId;
    }
}

f32 *AnmVm::GetFloatVarPtr(f32 *varPtr, u16 varMask, u32 variableNumber)
{
    if ((varMask & (1 << variableNumber)) == 0)
    {
        return varPtr;
    }

    switch ((int)*varPtr)
    {
    case AnmVariable_F0:
        return &this->floatVar0;
    case AnmVariable_F1:
        return &this->floatVar1;
    case AnmVariable_F2:
        return &this->floatVar2;
    case AnmVariable_F3:
        return &this->floatVar3;
    }

    return varPtr;
}

i32 *AnmVm::GetIntVarPtr(i32 *varPtr, u16 varMask, u32 variableNumber)
{
    if ((varMask & (1 << variableNumber)) == 0)
    {
        return varPtr;
    }

    switch (*varPtr)
    {
    case AnmVariable_I0:
        return &this->intVar0;
    case AnmVariable_I1:
        return &this->intVar1;
    case AnmVariable_I2:
        return &this->intVar2;
    case AnmVariable_I3:
        return &this->intVar3;
    case AnmVariable_IC0:
        return &this->counterVar0;
    case AnmVariable_IC1:
        return &this->counterVar1;
    }

    return varPtr;
}

#pragma var_order(instruction, nextInstruction, i, interp)
ZunBool AnmManager::ExecuteScript(AnmVm *vm)
{
    AnmRawInstr *instruction;
    AnmRawInstr *nextInstruction;
    int i;
    float interp;

    if (vm->currentInstruction == NULL)
    {
        return TRUE;
    }

    if (vm->flag19 != 0)
    {
        return FALSE;
    }

    if (vm->pendingInterrupt != 0)
    {
        goto handleInterrupt;
    }

    while (instruction = vm->currentInstruction, instruction->time <= (int)vm->currentTimeInScript)
    {
#define GET_INT_VAR(argNumber)                                                                                         \
    ((instruction->varMask & (1 << argNumber)) ? vm->GetIntVar(instruction->intArgs[argNumber])                        \
                                               : instruction->intArgs[argNumber])
#define GET_FLOAT_VAR(argNumber)                                                                                       \
    ((instruction->varMask & (1 << argNumber)) ? vm->GetFloatVar(instruction->floatArgs[argNumber])                    \
                                               : instruction->floatArgs[argNumber])

#define GET_INT_VAR_PTR(idx) vm->GetIntVarPtr(&instruction->intArgs[idx], instruction->varMask, idx)

#define GET_FLOAT_VAR_PTR(idx) vm->GetFloatVarPtr(&instruction->floatArgs[idx], instruction->varMask, idx)

        switch (instruction->opcode)
        {
        case AnmOpcode_EndOfScript:
        case AnmOpcode_Delete:
            vm->visible = false;
        case AnmOpcode_Static:
            vm->currentInstruction = NULL;
            return TRUE;
        case AnmOpcode_Sprite:
            vm->visible = true;

            vm->anmFile->SetSprite(vm, GET_INT_VAR(0));
            vm->timeOfLastSpriteSet = (int)vm->currentTimeInScript;
            break;
        case AnmOpcode_Scale:
            vm->scale.x = GET_FLOAT_VAR(0);
            vm->scale.y = GET_FLOAT_VAR(1);

            vm->updateScale = true;
            break;
        case AnmOpcode_Alpha:
            vm->color1.a = GET_INT_VAR(0);
            break;
        case AnmOpcode_Color:
            vm->color1.r = GET_INT_VAR(0);
            vm->color1.g = GET_INT_VAR(1);
            vm->color1.b = GET_INT_VAR(2);
            break;
        case AnmOpcode_Alpha2:
            vm->color2.a = GET_INT_VAR(0);
            break;
        case AnmOpcode_Color2:
            vm->color2.r = GET_INT_VAR(0);
            vm->color2.g = GET_INT_VAR(1);
            vm->color2.b = GET_INT_VAR(2);
            break;
        case AnmOpcode_Jmp:
            vm->currentTimeInScript = instruction->intArgs[1];
            vm->currentInstruction = (AnmRawInstr *)(((u8 *)vm->beginningOfScript) + instruction->intArgs[0]);
            continue;
        case AnmOpcode_JmpDec:
            *GET_INT_VAR_PTR(0) -= 1;

            if (GET_INT_VAR(0) > 0)
            {
                vm->currentTimeInScript = instruction->intArgs[2];
                vm->currentInstruction = (AnmRawInstr *)(((u8 *)vm->beginningOfScript) + instruction->intArgs[1]);
                continue;
            }
            break;
        case AnmOpcode_FlipX:
            vm->flip ^= (1 << 0);
            vm->scale.x *= -1.0f;
            vm->updateScale = true;
            break;
        case AnmOpcode_PosMode:
            vm->usePosOffset = instruction->intArgs[0];
            break;
        case AnmOpcode_FlipY:
            vm->flip ^= (1 << 1);
            vm->scale.y *= -1.0f;
            vm->updateScale = true;
            break;
        case AnmOpcode_Rotate:
            vm->rotation.x = GET_FLOAT_VAR(0);
            vm->rotation.y = GET_FLOAT_VAR(1);
            vm->rotation.z = GET_FLOAT_VAR(2);

            vm->updateRotation = true;
            break;
        case AnmOpcode_AngularVelocity:
            vm->angleVel.x = GET_FLOAT_VAR(0);
            vm->angleVel.y = GET_FLOAT_VAR(1);
            vm->angleVel.z = GET_FLOAT_VAR(2);

            vm->updateRotation = true;
            break;
        case AnmOpcode_ScaleGrowth:
            vm->scaleGrowth.x = GET_FLOAT_VAR(0);
            vm->scaleGrowth.y = GET_FLOAT_VAR(1);
            break;
        case AnmOpcode_ScaleTimeLinear:
            vm->interpCurrentTimers[AnmInterp_Scale] = 0;

            vm->interpEndTimers[AnmInterp_Scale] = GET_INT_VAR(2);

            vm->interpModes[AnmInterp_Scale] = AnmInterpMode_Linear;
            vm->scaleInitial = vm->scale;

            vm->scaleFinal.x = GET_FLOAT_VAR(0);
            vm->scaleFinal.y = GET_FLOAT_VAR(1);
            break;
        case AnmOpcode_AlphaTimeLinear:
            vm->color1Initial.a = vm->color1.a;
            vm->color1Final.a = instruction->intArgs[0];

            vm->interpCurrentTimers[AnmInterp_Alpha1] = 0;
            vm->interpEndTimers[AnmInterp_Alpha1] = GET_INT_VAR(1);
            vm->interpModes[AnmInterp_Alpha1] = AnmInterpMode_Linear;
            break;
        case AnmOpcode_AdditiveBlendMode:
            vm->blendMode = instruction->intArgs[0] != 0;
            break;
        case AnmOpcode_BlendMode:
            vm->blendMode = instruction->intArgs[0];
            break;
        case AnmOpcode_Pos:
            if (!vm->usePosOffset)
            {
                vm->pos = Float3(GET_FLOAT_VAR(0), GET_FLOAT_VAR(1), GET_FLOAT_VAR(2));
            }
            else
            {
                vm->pos2 = Float3(GET_FLOAT_VAR(0), GET_FLOAT_VAR(1), GET_FLOAT_VAR(2));
            }
            break;
        case AnmOpcode_PosTimeDecel2:
            vm->interpModes[AnmInterp_Pos] = AnmInterpMode_EaseOutQuartic;
            goto posTime;
        case AnmOpcode_PosTimeDecel:
            vm->interpModes[AnmInterp_Pos] = AnmInterpMode_EaseOut;
            goto posTime;
        case AnmOpcode_PosTimeLinear:
            vm->interpModes[AnmInterp_Pos] = AnmInterpMode_Linear;
        posTime:
            if (!vm->usePosOffset)
            {
                vm->posInitial = vm->pos;
            }
            else
            {
                vm->posInitial = vm->pos2;
            }

            vm->posFinal = Float3(GET_FLOAT_VAR(0), GET_FLOAT_VAR(1), GET_FLOAT_VAR(2));

            vm->interpEndTimers[AnmInterp_Pos] = GET_INT_VAR(3);
            vm->interpCurrentTimers[AnmInterp_Pos] = 0;
            break;
        case AnmOpcode_Wait:
            if (vm->waitTimer == 0)
            {
                vm->waitTimer = GET_INT_VAR(0);
            }
            else
            {
                vm->waitTimer--;
            }

            if (vm->waitTimer <= 0)
            {
                vm->waitTimer = 0;
                break;
            }
            vm->currentTimeInScript--;
            goto stop;
        case AnmOpcode_StopHide:
            vm->visible = false;
        case AnmOpcode_Stop:
            if (vm->pendingInterrupt == 0)
            {
                vm->stopped = true;
                vm->currentTimeInScript--;
                goto stop;
            }

        handleInterrupt:
            nextInstruction = NULL;
            instruction = vm->beginningOfScript;
            while (
                !(instruction->opcode == AnmOpcode_InterruptLabel && vm->pendingInterrupt == instruction->intArgs[0]) &&
                instruction->opcode != AnmOpcode_EndOfScript)
            {
                if (instruction->opcode == AnmOpcode_InterruptLabel && instruction->intArgs[0] == -1)
                {
                    nextInstruction = instruction;
                }
                instruction = (AnmRawInstr *)((u8 *)instruction + instruction->instructionSize);
            }

            vm->pendingInterrupt = 0;
            vm->stopped = false;

            if (instruction->opcode != AnmOpcode_InterruptLabel)
            {
                if (nextInstruction == NULL)
                {
                    vm->currentTimeInScript--;
                    goto stop;
                }
                instruction = nextInstruction;
            }

            vm->interruptReturnTime = vm->currentTimeInScript;
            vm->interruptReturnInstruction = vm->currentInstruction;
            instruction = (AnmRawInstr *)((u8 *)instruction + instruction->instructionSize);
            vm->currentInstruction = instruction;
            vm->currentTimeInScript = vm->currentInstruction->time;
            vm->visible = true;
            continue;
        case AnmOpcode_ReturnFromInterrupt:
            vm->currentTimeInScript = vm->interruptReturnTime;
            vm->currentInstruction = vm->interruptReturnInstruction;
            continue;
        case AnmOpcode_Visible:
            vm->visible = instruction->intArgs[0];
            break;
        case AnmOpcode_AnchorTopLeft:
            vm->anchor = 3;
            break;
        case AnmOpcode_Ins25:
            vm->type = instruction->intArgs[0];
            break;
        case AnmOpcode_AddU:
            vm->uvScrollPos.x += GET_FLOAT_VAR(0);
            ;
            if (vm->uvScrollPos.x >= 1.0f)
            {
                vm->uvScrollPos.x -= 1.0f;
            }
            else
            {
                if (vm->uvScrollPos.x < 0.0f)
                {
                    vm->uvScrollPos.x += 1.0f;
                }
            }
            break;
        case AnmOpcode_AddV:
            vm->uvScrollPos.y += GET_FLOAT_VAR(0);
            if (vm->uvScrollPos.y >= 1.0f)
            {
                vm->uvScrollPos.y -= 1.0f;
            }
            else
            {
                if (vm->uvScrollPos.y < 0.0f)
                {
                    vm->uvScrollPos.y += 1.0f;
                }
            }
            break;
        case AnmOpcode_UScroll:
            vm->uvScrollVel.x = GET_FLOAT_VAR(0);
            break;
        case AnmOpcode_VScroll:
            vm->uvScrollVel.y = GET_FLOAT_VAR(0);
            break;
        case AnmOpcode_ZWriteDisable:
            vm->zWriteDisabled = instruction->intArgs[0];
            break;
        case AnmOpcode_Ins31:
            vm->flag15 = instruction->intArgs[0];
            break;
        case AnmOpcode_PosTime:
            vm->interpCurrentTimers[AnmInterp_Pos] = 0;
            vm->interpEndTimers[AnmInterp_Pos] = GET_INT_VAR(0);
            vm->interpModes[AnmInterp_Pos] = instruction->intArgs[1];

            if (!vm->usePosOffset)
            {
                vm->posInitial = vm->pos;
            }
            else
            {
                vm->posInitial = vm->pos2;
            }

            vm->posFinal.x = GET_FLOAT_VAR(2);
            vm->posFinal.y = GET_FLOAT_VAR(3);
            vm->posFinal.z = GET_FLOAT_VAR(4);
            break;
        case AnmOpcode_ColorTime:
            vm->interpCurrentTimers[AnmInterp_RGB1] = 0;

            vm->interpEndTimers[AnmInterp_RGB1] = GET_INT_VAR(0);

            vm->interpModes[AnmInterp_RGB1] = instruction->intArgs[1];
            vm->color1Initial.r = vm->color1.r;
            vm->color1Initial.g = vm->color1.g;
            vm->color1Initial.b = vm->color1.b;

            vm->color1Final.r = GET_INT_VAR(2);
            vm->color1Final.g = GET_INT_VAR(3);
            vm->color1Final.b = GET_INT_VAR(4);
            break;
        case AnmOpcode_AlphaTime:
            vm->interpCurrentTimers[AnmInterp_Alpha1] = 0;
            vm->interpEndTimers[AnmInterp_Alpha1] = GET_INT_VAR(0);
            vm->interpModes[AnmInterp_Alpha1] = instruction->intArgs[1];

            vm->color1Initial.a = vm->color1.a;
            vm->color1Final.a = GET_INT_VAR(2);
            break;
        case AnmOpcode_Color2Time:
            vm->interpCurrentTimers[AnmInterp_RGB2] = 0;

            vm->interpEndTimers[AnmInterp_RGB2] = GET_INT_VAR(0);

            vm->interpModes[AnmInterp_RGB2] = instruction->intArgs[1];
            vm->color2Initial.r = vm->color2.r;
            vm->color2Initial.g = vm->color2.g;
            vm->color2Initial.b = vm->color2.b;

            vm->color2Final.r = GET_INT_VAR(2);
            vm->color2Final.g = GET_INT_VAR(3);
            vm->color2Final.b = GET_INT_VAR(4);
            break;
        case AnmOpcode_Alpha2Time:
            vm->interpCurrentTimers[AnmInterp_Alpha2] = 0;
            vm->interpEndTimers[AnmInterp_Alpha2] = GET_INT_VAR(0);
            vm->interpModes[AnmInterp_Alpha2] = instruction->intArgs[1];

            vm->color2Initial.a = vm->color2.a;
            vm->color2Final.a = GET_INT_VAR(2);
            break;
        case AnmOpcode_RotateTime:
            vm->interpCurrentTimers[AnmInterp_Rotate] = 0;

            vm->interpEndTimers[AnmInterp_Rotate] = GET_INT_VAR(0);

            vm->interpModes[AnmInterp_Rotate] = instruction->intArgs[1];
            vm->rotateInitial = vm->rotation;

            vm->rotateFinal.x = GET_FLOAT_VAR(2);
            vm->rotateFinal.y = GET_FLOAT_VAR(3);
            vm->rotateFinal.z = GET_FLOAT_VAR(4);

            vm->updateRotation = true;
            break;
        case AnmOpcode_ScaleTime:
            vm->interpCurrentTimers[AnmInterp_Scale] = 0;
            vm->interpEndTimers[AnmInterp_Scale] = GET_INT_VAR(0);

            vm->interpModes[AnmInterp_Scale] = instruction->intArgs[1];
            vm->scaleInitial = vm->scale;

            vm->scaleFinal.x = GET_FLOAT_VAR(2);
            vm->scaleFinal.y = GET_FLOAT_VAR(3);
            vm->updateScale = true;
            break;
        case AnmOpcode_Ins83:
            vm->playerBulletHitAnimationType = instruction->intArgs[0];
            break;
        case AnmOpcode_ISet:
            *GET_INT_VAR_PTR(0) = GET_INT_VAR(1);
            break;
        case AnmOpcode_FSet:
            *GET_FLOAT_VAR_PTR(0) = GET_FLOAT_VAR(1);
            break;
        case AnmOpcode_ISetAdd:
            *GET_INT_VAR_PTR(0) = GET_INT_VAR(1) + GET_INT_VAR(2);
            break;
        case AnmOpcode_FSetAdd:
            *GET_FLOAT_VAR_PTR(0) = GET_FLOAT_VAR(1) + GET_FLOAT_VAR(2);
            break;
        case AnmOpcode_ISetSub:
            *GET_INT_VAR_PTR(0) = GET_INT_VAR(1) - GET_INT_VAR(2);
            break;
        case AnmOpcode_FSetSub:
            *GET_FLOAT_VAR_PTR(0) = GET_FLOAT_VAR(1) - GET_FLOAT_VAR(2);
            break;
        case AnmOpcode_ISetMul:
            *GET_INT_VAR_PTR(0) = GET_INT_VAR(1) * GET_INT_VAR(2);
            break;
        case AnmOpcode_FSetMul:
            *GET_FLOAT_VAR_PTR(0) = GET_FLOAT_VAR(1) * GET_FLOAT_VAR(2);
            break;
        case AnmOpcode_ISetDiv:
            *GET_INT_VAR_PTR(0) = GET_INT_VAR(1) / GET_INT_VAR(2);
            break;
        case AnmOpcode_FSetDiv:
            *GET_FLOAT_VAR_PTR(0) = GET_FLOAT_VAR(1) / GET_FLOAT_VAR(2);
            break;
        case AnmOpcode_ISetMod:
            *GET_INT_VAR_PTR(0) = GET_INT_VAR(1) % GET_INT_VAR(2);
            break;
        case AnmOpcode_FSetMod:
            *GET_FLOAT_VAR_PTR(0) = fmodf(GET_FLOAT_VAR(1), GET_FLOAT_VAR(2));
            break;
        case AnmOpcode_IAdd:
            *GET_INT_VAR_PTR(0) += GET_INT_VAR(1);
            break;
        case AnmOpcode_FAdd:
            *GET_FLOAT_VAR_PTR(0) += GET_FLOAT_VAR(1);
            break;
        case AnmOpcode_ISub:
            *GET_INT_VAR_PTR(0) -= GET_INT_VAR(1);
            break;
        case AnmOpcode_FSub:
            *GET_FLOAT_VAR_PTR(0) -= GET_FLOAT_VAR(1);
            break;
        case AnmOpcode_IMul:
            *GET_INT_VAR_PTR(0) *= GET_INT_VAR(1);
            break;
        case AnmOpcode_FMul:
            *GET_FLOAT_VAR_PTR(0) *= GET_FLOAT_VAR(1);
            break;
        case AnmOpcode_IDiv:
            *GET_INT_VAR_PTR(0) /= GET_INT_VAR(1);
            break;
        case AnmOpcode_FDiv:
            *GET_FLOAT_VAR_PTR(0) /= GET_FLOAT_VAR(1);
            break;
        case AnmOpcode_IMod:
            *GET_INT_VAR_PTR(0) %= GET_INT_VAR(1);
            break;
        case AnmOpcode_FMod:
            *GET_FLOAT_VAR_PTR(0) = fmodf(GET_FLOAT_VAR(0), GET_FLOAT_VAR(1));
            break;
        case AnmOpcode_ISetRand:
            *GET_INT_VAR_PTR(0) = g_Rng.GetRandomU32InRange(GET_INT_VAR(1));
            break;
        case AnmOpcode_FSetRand:
            *GET_FLOAT_VAR_PTR(0) = g_Rng.GetRandomF32InRange(GET_FLOAT_VAR(1));
            break;
        case AnmOpcode_FSin:
            *GET_FLOAT_VAR_PTR(0) = sinf(GET_FLOAT_VAR(1));
            break;
        case AnmOpcode_FCos:
            *GET_FLOAT_VAR_PTR(0) = cosf(GET_FLOAT_VAR(1));
            break;
        case AnmOpcode_FTan:
            *GET_FLOAT_VAR_PTR(0) = tanf(GET_FLOAT_VAR(1));
            break;
        case AnmOpcode_FAcos:
            *GET_FLOAT_VAR_PTR(0) = acosf(GET_FLOAT_VAR(1));
            break;
        case AnmOpcode_FAtan:
            *GET_FLOAT_VAR_PTR(0) = atanf(GET_FLOAT_VAR(1));
            break;
        case AnmOpcode_NormalizeAngle:
            *GET_FLOAT_VAR_PTR(0) = AddNormalizeAngle(GET_FLOAT_VAR(0), 0);
            break;
        case AnmOpcode_IJmpEq:
            if (GET_INT_VAR(0) == GET_INT_VAR(1))
            {
                goto jump;
            }
            break;
        case AnmOpcode_FJmpEq:
            if (GET_FLOAT_VAR(0) == GET_FLOAT_VAR(1))
            {
                goto jump;
            }
            break;
        case AnmOpcode_IJmpNeq:
            if (GET_INT_VAR(0) != GET_INT_VAR(1))
            {
                goto jump;
            }
            break;
        case AnmOpcode_FJmpNeq:
            if (GET_FLOAT_VAR(0) != GET_FLOAT_VAR(1))
            {
                goto jump;
            }
            break;
        case AnmOpcode_IJmpLess:
            if (GET_INT_VAR(0) < GET_INT_VAR(1))
            {
                goto jump;
            }
            break;
        case AnmOpcode_FJmpLess:
            if (GET_FLOAT_VAR(0) < GET_FLOAT_VAR(1))
            {
                goto jump;
            }
            break;
        case AnmOpcode_IJmpLessOrEq:
            if (GET_INT_VAR(0) <= GET_INT_VAR(1))
            {
                goto jump;
            }
            break;
        case AnmOpcode_FJmpLessOrEq:
            if (GET_FLOAT_VAR(0) <= GET_FLOAT_VAR(1))
            {
                goto jump;
            }
            break;
        case AnmOpcode_IJmpGreater:
            if (GET_INT_VAR(0) > GET_INT_VAR(1))
            {
                goto jump;
            }
            break;
        case AnmOpcode_FJmpGreater:
            if (GET_FLOAT_VAR(0) > GET_FLOAT_VAR(1))
            {
                goto jump;
            }
            break;
        case AnmOpcode_IJmpGreaterOrEq:
            if (GET_INT_VAR(0) >= GET_INT_VAR(1))
            {
                goto jump;
            }
            break;
        case AnmOpcode_FJmpGreaterOrEq:
            if (GET_FLOAT_VAR(0) >= GET_FLOAT_VAR(1))
            {
                goto jump;
            }
            break;
        case AnmOpcode_Ins88:
            vm->flag17 = instruction->byteArgs[1];
            break;
        jump:
            vm->currentTimeInScript = instruction->intArgs[3];
            vm->currentInstruction = (AnmRawInstr *)(((u8 *)vm->beginningOfScript) + instruction->intArgs[2]);
            continue;
        default:
            break;
        }
#undef GET_FLOAT_VAR_PTR
#undef GET_INT_VAR_PTR
#undef GET_FLOAT_VAR
#undef GET_INT_VAR

        vm->currentInstruction = (AnmRawInstr *)((u8 *)instruction + instruction->instructionSize);
    }
stop:
    if (vm->angleVel.x != 0.0f)
    {
        vm->rotation.x = AddNormalizeAngle(vm->rotation.x, g_Supervisor.framerateMultiplier * vm->angleVel.x);
        vm->updateRotation = true;
    }

    if (vm->angleVel.y != 0.0f)
    {
        vm->rotation.y = AddNormalizeAngle(vm->rotation.y, g_Supervisor.framerateMultiplier * vm->angleVel.y);
        vm->updateRotation = true;
    }

    if (vm->angleVel.z != 0.0f)
    {
        vm->rotation.z = AddNormalizeAngle(vm->rotation.z, g_Supervisor.framerateMultiplier * vm->angleVel.z);
        vm->updateRotation = true;
    }

    for (i = 0; i < AnmInterp_Last; i++)
    {
        if (vm->interpEndTimers[i] > 0)
        {
            vm->interpCurrentTimers[i]++;
            if (vm->interpCurrentTimers[i] >= (int)vm->interpEndTimers[i])
            {
                interp = 1.0f;
                vm->interpEndTimers[i] = 0;
            }
            else
            {
                interp = (float)vm->interpCurrentTimers[i] / (float)vm->interpEndTimers[i];
            }

            switch (vm->interpModes[i])
            {
            case AnmInterpMode_EaseIn:
                interp = interp * interp;
                break;
            case AnmInterpMode_EaseInCubic:
                interp = interp * interp * interp;
                break;
            case AnmInterpMode_EaseInQuartic:
                interp = interp * interp;
                interp = interp * interp;
                break;
            case AnmInterpMode_EaseOut:
                interp = (1.0f - interp);
                interp *= interp;
                interp = (1.0f - interp);
                break;
            case AnmInterpMode_EaseOutCubic:
                interp = (1.0f - interp);
                interp = interp * interp * interp;
                interp = (1.0f - interp);
                break;
            case AnmInterpMode_EaseOutQuartic:
                interp = (1.0f - interp);
                interp = interp * interp;
                interp = interp * interp;
                interp = (1.0f - interp);
                break;
            }

            switch (i)
            {
            case AnmInterp_Pos:
                if (!vm->usePosOffset)
                {
                    vm->pos.x = interp * (vm->posFinal.x - vm->posInitial.x) + vm->posInitial.x;
                    vm->pos.y = interp * (vm->posFinal.y - vm->posInitial.y) + vm->posInitial.y;
                    vm->pos.z = interp * (vm->posFinal.z - vm->posInitial.z) + vm->posInitial.z;
                }
                else
                {
                    vm->pos2.x = interp * (vm->posFinal.x - vm->posInitial.x) + vm->posInitial.x;
                    vm->pos2.y = interp * (vm->posFinal.y - vm->posInitial.y) + vm->posInitial.y;
                    vm->pos2.z = interp * (vm->posFinal.z - vm->posInitial.z) + vm->posInitial.z;
                }
                break;
            case AnmInterp_RGB1:
                vm->color1.r = interp * ((float)vm->color1Final.r - vm->color1Initial.r) + vm->color1Initial.r;
                vm->color1.g = interp * ((float)vm->color1Final.g - vm->color1Initial.g) + vm->color1Initial.g;
                vm->color1.b = interp * ((float)vm->color1Final.b - vm->color1Initial.b) + vm->color1Initial.b;
                break;
            case AnmInterp_Alpha1:
                vm->color1.a = interp * ((float)vm->color1Final.a - vm->color1Initial.a) + vm->color1Initial.a;
                break;
            case AnmInterp_RGB2:
                vm->color2.r = interp * ((float)vm->color2Final.r - vm->color2Initial.r) + vm->color2Initial.r;
                vm->color2.g = interp * ((float)vm->color2Final.g - vm->color2Initial.g) + vm->color2Initial.g;
                vm->color2.b = interp * ((float)vm->color2Final.b - vm->color2Initial.b) + vm->color2Initial.b;
                break;
            case AnmInterp_Alpha2:
                vm->color2.a = interp * ((float)vm->color2Final.a - vm->color2Initial.a) + vm->color2Initial.a;
                break;
            case AnmInterp_Rotate:
                vm->rotation.x =
                    AddNormalizeAngle((vm->rotateFinal.x - vm->rotateInitial.x) * interp, vm->rotateInitial.x);
                vm->rotation.y =
                    AddNormalizeAngle((vm->rotateFinal.y - vm->rotateInitial.y) * interp, vm->rotateInitial.y);
                vm->rotation.z =
                    AddNormalizeAngle((vm->rotateFinal.z - vm->rotateInitial.z) * interp, vm->rotateInitial.z);
                vm->updateRotation = true;
                break;
            case AnmInterp_Scale:
                vm->scale.x = interp * (vm->scaleFinal.x - vm->scaleInitial.x) + vm->scaleInitial.x;
                vm->scale.y = interp * (vm->scaleFinal.y - vm->scaleInitial.y) + vm->scaleInitial.y;
                vm->updateScale = true;
                break;
            }
        }
    }

    if (vm->scaleGrowth.y != 0.0f)
    {
        vm->scale.y += g_Supervisor.framerateMultiplier * vm->scaleGrowth.y;
        vm->updateScale = true;
    }

    if (vm->scaleGrowth.x != 0.0f)
    {
        vm->scale.x += g_Supervisor.framerateMultiplier * vm->scaleGrowth.x;
        vm->updateScale = true;
        vm->updateRotation = true;
    }

    vm->uvScrollPos.x += vm->uvScrollVel.x;

    if (vm->uvScrollPos.x >= 1.0f)
    {
        vm->uvScrollPos.x -= 1.0f;
    }
    else
    {
        if (vm->uvScrollPos.x < 0.0f)
        {
            vm->uvScrollPos.x += 1.0f;
        }
    }

    vm->uvScrollPos.y += vm->uvScrollVel.y;
    if (vm->uvScrollPos.y >= 1.0f)
    {
        vm->uvScrollPos.y -= 1.0f;
    }
    else
    {
        if (vm->uvScrollPos.y < 0.0f)
        {
            vm->uvScrollPos.y += 1.0f;
        }
    }

    vm->currentTimeInScript++;
    this->scriptsExecutedThisFrame++;

    return FALSE;
}

void AnmManager::SetInterruptArray(AnmVm *vm, int count, i16 interrupt)
{
    while (count != 0)
    {
        if (g_AnmManager->SpriteHasTexture(vm))
        {
            vm->SetInterrupt(interrupt);
        }
        vm++;
        count--;
    }
}

// FUNCTION: th08 0x004622C0
inline ZunBool AnmManager::SpriteHasTexture(AnmVm *vm)
{
    if (vm->loadedSprite == NULL)
    {
        return FALSE;
    }

    if (vm->loadedSprite->anmIdx < 0)
    {
        return FALSE;
    }

    return this->anmFiles[vm->loadedSprite->anmIdx].textures != NULL;
}

void AnmManager::ExecuteScriptArray(AnmVm *sprite, int count)
{
    while (count != 0)
    {
        if (sprite->scriptIndex >= 0)
        {
            g_AnmManager->ExecuteScript(sprite);
        }
        sprite++;
        count--;
    }
}

void AnmLoaded::ExecuteAnmIdxArray(AnmVm *vm, i32 scriptIdx, i32 count)
{
    while (count != 0)
    {
        this->ExecuteAnmIdx(vm, scriptIdx);
        vm->baseSpriteIndex = vm->activeSpriteIndex;

        scriptIdx++;
        vm++;
        count--;
    }
}

u8 MixColors(u8 color1, u8 color2);

// FUNCTION: th08 0x004623c0
#pragma var_order(color, this)
void AnmManager::SetRenderStateForVm3D(AnmVm *vm)
{
    ZunColor color;

    if (this->currentBlendMode != vm->blendMode)
    {
        this->FlushVertexBuffer();
        this->currentBlendMode = vm->blendMode;
        switch (this->currentBlendMode)
        {
        case AnmBlendMode_Normal:
            g_Supervisor.d3dDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            break;
        case AnmBlendMode_Additive:
            g_Supervisor.d3dDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            break;
        }
    }

    color.d3dColor = vm->flag17 ? vm->color2.d3dColor : vm->color1.d3dColor;

    if (this->needsTextureFactorSetup)
    {
        this->needsTextureFactorSetup = 0;
        if (!g_Supervisor.IsVertexBufferDisabled())
        {
            this->FlushVertexBuffer();
            g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
            g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        }
    }

    if (!g_Supervisor.IsVertexBufferDisabled())
    {
        if (this->useMixColor)
        {
            color.r = MixColors(color.r, this->color.r);
            color.g = MixColors(color.g, this->color.g);
            color.b = MixColors(color.b, this->color.b);
            color.a = MixColors(color.a, this->color.a);
        }

        if (this->currentTextureFactor != color.d3dColor)
        {
            this->FlushVertexBuffer();
            this->currentTextureFactor = color.d3dColor;
            g_Supervisor.d3dDevice->SetRenderState(D3DRS_TEXTUREFACTOR, this->currentTextureFactor);
        }
    }
    else
    {
        if (this->useMixColor)
        {
            color.r = MixColors(color.r, this->color.r);
            color.g = MixColors(color.g, this->color.g);
            color.b = MixColors(color.b, this->color.b);
            color.a = MixColors(color.a, this->color.a);
        }

        g_QuadVertices[0].diffuse = color.d3dColor;
        g_QuadVertices[1].diffuse = color.d3dColor;
        g_QuadVertices[2].diffuse = color.d3dColor;
        g_QuadVertices[3].diffuse = color.d3dColor;
        *reinterpret_cast<D3DCOLOR *>(&g_BackgroundQuadVertices[0].w) = color.d3dColor;
        *reinterpret_cast<D3DCOLOR *>(&g_BackgroundQuadVertices[1].w) = color.d3dColor;
        *reinterpret_cast<D3DCOLOR *>(&g_BackgroundQuadVertices[2].w) = color.d3dColor;
        *reinterpret_cast<D3DCOLOR *>(&g_BackgroundQuadVertices[3].w) = color.d3dColor;
    }

    if (!g_Supervisor.IsDepthTestDisabled() && this->disableZWrite != vm->zWriteDisabled)
    {
        this->FlushVertexBuffer();
        this->disableZWrite = vm->zWriteDisabled;
        if (!this->disableZWrite)
        {
            g_Supervisor.d3dDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        }
        else
        {
            g_Supervisor.d3dDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        }
    }

    if (this->cameraMode != vm->flag15)
    {
        this->FlushVertexBuffer();
        this->cameraMode = vm->flag15;
        if (!this->cameraMode)
        {
            g_Background.SetCamera1();
            g_Supervisor.d3dDevice->SetViewport(&g_Supervisor.viewport);
        }
        else
        {
            g_Background.SetCamera2();
            g_Supervisor.d3dDevice->SetViewport(&g_Supervisor.viewport);
        }
    }

    this->renderStateChangesThisFrame++;
}

u8 MixColors(u8 color1, u8 color2)
{
    u32 color = ((color1 * color2) / 128U);

    if (color >= 256)
    {
        color = 255;
    }

    return color;
}

void AnmManager::SetRenderStateForVm(AnmVm *vm)
{
    if (this->currentBlendMode != vm->blendMode)
    {
        this->FlushVertexBuffer();
        this->currentBlendMode = vm->blendMode;

        switch (this->currentBlendMode)
        {
        case AnmBlendMode_Normal:
            g_Supervisor.d3dDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            break;
        case AnmBlendMode_Additive:
            g_Supervisor.d3dDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            break;
        }
    }

    if (!g_Supervisor.IsDepthTestDisabled() && this->disableZWrite != vm->zWriteDisabled)
    {
        this->disableZWrite = vm->zWriteDisabled;
        if (!this->disableZWrite)
        {
            g_Supervisor.SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        }
        else
        {
            g_Supervisor.SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        }
    }

    this->renderStateChangesThisFrame++;
}

static const f32 g_ZeroPointFive = 0.5;

#pragma var_order(triangleY1, triangleY2, triangleX2, triangleX1, color)
ZunResult AnmManager::DrawInner(AnmVm *vm, i32 flags)
{
    ZunColor color;
    float triangleX1, triangleX2, triangleY1, triangleY2;

    g_QuadVertices[0].pos.x += this->screenShakeOffset.x;
    g_QuadVertices[0].pos.y += this->screenShakeOffset.y;
    g_QuadVertices[1].pos.x += this->screenShakeOffset.x;
    g_QuadVertices[1].pos.y += this->screenShakeOffset.y;
    g_QuadVertices[2].pos.x += this->screenShakeOffset.x;
    g_QuadVertices[2].pos.y += this->screenShakeOffset.y;
    g_QuadVertices[3].pos.x += this->screenShakeOffset.x;
    g_QuadVertices[3].pos.y += this->screenShakeOffset.y;

    if (flags & 1)
    {
        /* same as in EoSD. */
#ifdef TH08_MODERN_PORT
        triangleX1 = nearbyintf(g_QuadVertices[0].pos.x) - g_ZeroPointFive;
        triangleX2 = nearbyintf(g_QuadVertices[1].pos.x) - g_ZeroPointFive;
        triangleY1 = nearbyintf(g_QuadVertices[0].pos.y) - g_ZeroPointFive;
        triangleY2 = nearbyintf(g_QuadVertices[2].pos.y) - g_ZeroPointFive;
        g_QuadVertices[2].pos.y = g_QuadVertices[3].pos.y = triangleY2;
        g_QuadVertices[0].pos.y = g_QuadVertices[1].pos.y = triangleY1;
        g_QuadVertices[1].pos.x = g_QuadVertices[3].pos.x = triangleX2;
        g_QuadVertices[0].pos.x = g_QuadVertices[2].pos.x = triangleX1;
#else
        __asm
        {
            fld g_QuadVertices[0 * TYPE g_QuadVertices].pos.x
            frndint
            fsub g_ZeroPointFive
            fld g_QuadVertices[1 * TYPE g_QuadVertices].pos.x
            frndint
            fsub g_ZeroPointFive
            fld g_QuadVertices[0 * TYPE g_QuadVertices].pos.y
            frndint
            fsub g_ZeroPointFive
            fld g_QuadVertices[2 * TYPE g_QuadVertices].pos.y
            frndint
            fsub g_ZeroPointFive
            fst g_QuadVertices[2 * TYPE g_QuadVertices].pos.y
            fstp g_QuadVertices[3 * TYPE g_QuadVertices].pos.y
            fst g_QuadVertices[0 * TYPE g_QuadVertices].pos.y
            fstp g_QuadVertices[1 * TYPE g_QuadVertices].pos.y
            fst g_QuadVertices[1 * TYPE g_QuadVertices].pos.x
            fstp g_QuadVertices[3 * TYPE g_QuadVertices].pos.x
            fst g_QuadVertices[0 * TYPE g_QuadVertices].pos.x
            fstp g_QuadVertices[2 * TYPE g_QuadVertices].pos.x
        }
#endif
    }

    g_QuadVertices[0].textureUV.x = g_QuadVertices[2].textureUV.x = vm->loadedSprite->uvStart.x + vm->uvScrollPos.x;
    g_QuadVertices[1].textureUV.x = g_QuadVertices[3].textureUV.x = vm->loadedSprite->uvEnd.x + vm->uvScrollPos.x;
    g_QuadVertices[0].textureUV.y = g_QuadVertices[1].textureUV.y = vm->loadedSprite->uvStart.y + vm->uvScrollPos.y;
    g_QuadVertices[2].textureUV.y = g_QuadVertices[3].textureUV.y = vm->loadedSprite->uvEnd.y + vm->uvScrollPos.y;

    triangleX1 = ZUN_MAX(g_QuadVertices[0].pos.x, g_QuadVertices[1].pos.x);
    triangleX1 = ZUN_MAX(g_QuadVertices[2].pos.x, triangleX1);
    triangleX1 = ZUN_MAX(g_QuadVertices[3].pos.x, triangleX1);

    triangleY1 = ZUN_MAX(g_QuadVertices[0].pos.y, g_QuadVertices[1].pos.y);
    triangleY1 = ZUN_MAX(g_QuadVertices[2].pos.y, triangleY1);
    triangleY1 = ZUN_MAX(g_QuadVertices[3].pos.y, triangleY1);

    triangleX2 = ZUN_MIN(g_QuadVertices[0].pos.x, g_QuadVertices[1].pos.x);
    triangleX2 = ZUN_MIN(g_QuadVertices[2].pos.x, triangleX2);
    triangleX2 = ZUN_MIN(g_QuadVertices[3].pos.x, triangleX2);

    triangleY2 = ZUN_MIN(g_QuadVertices[0].pos.y, g_QuadVertices[1].pos.y);
    triangleY2 = ZUN_MIN(g_QuadVertices[2].pos.y, triangleY2);
    triangleY2 = ZUN_MIN(g_QuadVertices[3].pos.y, triangleY2);

    if (triangleX1 < g_Supervisor.viewport.X || triangleY1 < g_Supervisor.viewport.Y ||
        triangleX2 > (g_Supervisor.viewport.X + g_Supervisor.viewport.Width) ||
        triangleY2 > (g_Supervisor.viewport.Y + g_Supervisor.viewport.Height))
    {
        return ZUN_SUCCESS;
    }

    if (this->currentTexture != vm->loadedSprite->texture)
    {
        this->currentTexture = vm->loadedSprite->texture;
        this->FlushVertexBuffer();
        g_Supervisor.d3dDevice->SetTexture(0, this->currentTexture);
    }

    if (this->currentVertexShader != 1)
    {
        this->FlushVertexBuffer();
        this->currentVertexShader = 1;
    }

    if ((flags & 2) == 0)
    {
        color.d3dColor = vm->flag17 ? vm->color2.d3dColor : vm->color1.d3dColor;

        if (this->useMixColor)
        {
            color.r = MixColors(color.r, this->color.r);
            color.g = MixColors(color.g, this->color.g);
            color.b = MixColors(color.b, this->color.b);
            color.a = MixColors(color.a, this->color.a);
        }

        g_QuadVertices[0].diffuse = color.d3dColor;
        g_QuadVertices[1].diffuse = color.d3dColor;
        g_QuadVertices[2].diffuse = color.d3dColor;
        g_QuadVertices[3].diffuse = color.d3dColor;
    }

    this->SetRenderStateForVm(vm);
    this->AddSpriteToDrawBuffer(g_QuadVertices);

    return ZUN_SUCCESS;
}

#if TH08_PSP_ANY_MIXED_QUADS_PRODUCT_ENABLED
namespace
{
void ReconstructPspBulletMixedPairQuad(
    const VertexTex1DiffuseXyzrhw *pair,
    VertexTex1DiffuseXyzrhw *quad)
{
    quad[0] = pair[0];
    quad[3] = pair[1];
    quad[1] = pair[0];
    quad[1].pos.x = pair[1].pos.x;
    quad[1].textureUV.x = pair[1].textureUV.x;
    quad[2] = pair[0];
    quad[2].pos.y = pair[1].pos.y;
    quad[2].textureUV.y = pair[1].textureUV.y;
}

bool DrawPspBulletMixedCanonicalQuad(
    const VertexTex1DiffuseXyzrhw *quad)
{
    if (quad == NULL || g_Supervisor.d3dDevice == NULL)
        return false;
    VertexTex1DiffuseXyzrhw triangles[6];
    triangles[0] = quad[0];
    triangles[1] = quad[1];
    triangles[2] = quad[2];
    triangles[3] = quad[1];
    triangles[4] = quad[2];
    triangles[5] = quad[3];
    return SUCCEEDED(g_Supervisor.d3dDevice->DrawPrimitiveUP(
        D3DPT_TRIANGLELIST, 2U, triangles,
        sizeof(VertexTex1DiffuseXyzrhw)));
}

bool FlushPspMixedQuadBatch(AnmManager *manager,
                            PspSpritePairRunOwner owner)
{
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
    PspSpritePairRunState &run = sidecar.run;
    if (manager == NULL ||
        (owner != PspSpritePairRunOwner::Item &&
         owner != PspSpritePairRunOwner::Bullet) ||
        run.owner != owner)
    {
        return false;
    }
    BulletMixedQuadStats &stats = PspMixedQuadStatsForOwner(owner);
    const char *const ownerLabel = owner == PspSpritePairRunOwner::Item
        ? "ITEM_MIXED"
        : "BULLET_MIXED";
    const auto disarmAndResetStaging = [manager, &run, owner]() {
#if TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED
        if (owner == PspSpritePairRunOwner::Item)
            th08_psp_item_mixed_ge_set_batch(false);
#endif
#if TH08_PSP_BULLET_MIXED_QUADS_PRODUCT_ENABLED
        if (owner == PspSpritePairRunOwner::Bullet)
            th08_psp_bullet_direct_ge_set_batch(false);
#endif
        g_PspBulletUnifiedQuadBatchActive = false;
        g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::None;
        manager->spritesToDraw = 0U;
        manager->vertexBufferStartPtr = manager->vertexBuffer;
        manager->vertexBufferEndPtr = manager->vertexBuffer;
        memset(&run, 0, sizeof(run));
    };
    if (!run.runActive)
    {
        // A non-empty mixed staging range without its representation counts
        // cannot be decoded safely.  Treat this as presentation corruption:
        // never guess whether the first vertices are pairs or quads.
        fprintf(stderr,
                "TH08PSP %s recovery=fail_closed "
                "reason=missing_run sprites=%lu\n",
                ownerLabel,
                static_cast<unsigned long>(manager->spritesToDraw));
        ++stats.batches;
        ++stats.backendFallbackBatches;
        ++stats.failClosedBatches;
        ++stats.missingRunBatches;
        disarmAndResetStaging();
        return true;
    }

    const u32 pairCount = run.pairCount;
    const u32 generalCount = run.generalCount;
    // Prove both terms and the sum before performing any count arithmetic.
    // Otherwise a corrupted UINT32_MAX+1 pair/general combination could wrap
    // to a plausible spritesToDraw value and authorize unsafe pointer math.
    const bool countsBounded =
        pairCount <= kPspBulletUnifiedQuadCapacity &&
        generalCount <= kPspBulletUnifiedQuadCapacity &&
        pairCount <= kPspBulletUnifiedQuadCapacity - generalCount;
    const u32 totalQuads =
        countsBounded ? pairCount + generalCount : 0U;
    const uintptr_t vertexBytes = sizeof(VertexTex1DiffuseXyzrhw);
    const uintptr_t expectedVertices =
        countsBounded
            ? static_cast<uintptr_t>(pairCount) * 2U +
                  static_cast<uintptr_t>(generalCount) * 4U
            : 0U;
    const uintptr_t bufferBegin =
        reinterpret_cast<uintptr_t>(manager->vertexBuffer);
    const uintptr_t bufferLimit = reinterpret_cast<uintptr_t>(
        manager->vertexBuffer + ARRAY_SIZE(manager->vertexBuffer));
    const uintptr_t batchBegin =
        reinterpret_cast<uintptr_t>(manager->vertexBufferStartPtr);
    const uintptr_t batchEnd =
        reinterpret_cast<uintptr_t>(manager->vertexBufferEndPtr);
    const bool orderedInBuffer =
        batchBegin >= bufferBegin && batchBegin <= bufferLimit &&
        batchEnd >= batchBegin && batchEnd <= bufferLimit;
    const uintptr_t batchOffset =
        orderedInBuffer ? batchBegin - bufferBegin : 0U;
    const uintptr_t batchSpan =
        orderedInBuffer ? batchEnd - batchBegin : 0U;
    const bool validRange =
        countsBounded && orderedInBuffer &&
        batchOffset % vertexBytes == 0U &&
        totalQuads == manager->spritesToDraw && totalQuads != 0U &&
        expectedVertices <= ARRAY_SIZE(manager->vertexBuffer) &&
        batchSpan == expectedVertices * vertexBytes;

    bool submitted = false;
    bool canonicalReplayComplete = false;
    bool canonicalReplayDrawFailed = false;
    if (validRange)
    {
        VertexTex1DiffuseXyzrhw *const pairVertices = pairCount != 0U
            ? manager->vertexBufferStartPtr
            : NULL;
        VertexTex1DiffuseXyzrhw *const generalVertices = generalCount != 0U
            ? manager->vertexBufferStartPtr + pairCount * 2U
            : NULL;
        const u16 *const indices = generalCount != 0U
            ? g_PspBulletUnifiedQuadIndices
            : NULL;
#if TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED
        if (owner == PspSpritePairRunOwner::Item)
        {
            submitted = th08_psp_draw_item_mixed_quads(
                g_Supervisor.d3dDevice, pairVertices, pairCount,
                generalVertices, generalCount,
                sizeof(VertexTex1DiffuseXyzrhw), indices,
                generalCount * 6U);
        }
#endif
#if TH08_PSP_BULLET_MIXED_QUADS_PRODUCT_ENABLED
        if (owner == PspSpritePairRunOwner::Bullet)
        {
            submitted = th08_psp_draw_bullet_mixed_quads(
                g_Supervisor.d3dDevice, pairVertices, pairCount,
                generalVertices, generalCount,
                sizeof(VertexTex1DiffuseXyzrhw), indices,
                generalCount * 6U);
        }
#endif
        if (!submitted)
        {
            // The mixed hook and its D3D bridge reject before the first PRIM.
            // Reconstruct each proven rectangle from its two exact endpoints,
            // then replay the untouched four-vertex suffix in original order.
            u32 recoveredQuads = 0U;
            for (u32 pair = 0U; pair < pairCount; ++pair)
            {
                VertexTex1DiffuseXyzrhw quad[4];
                ReconstructPspBulletMixedPairQuad(
                    pairVertices + pair * 2U, quad);
                if (!DrawPspBulletMixedCanonicalQuad(quad))
                {
                    canonicalReplayDrawFailed = true;
                    break;
                }
                ++recoveredQuads;
            }
            for (u32 general = 0U;
                 !canonicalReplayDrawFailed && general < generalCount;
                 ++general)
            {
                if (!DrawPspBulletMixedCanonicalQuad(
                        generalVertices + general * 4U))
                {
                    canonicalReplayDrawFailed = true;
                    break;
                }
                ++recoveredQuads;
            }
            canonicalReplayComplete = recoveredQuads == totalQuads;
            if (canonicalReplayDrawFailed)
            {
                fprintf(stderr,
                        "TH08PSP %s recovery=fail_closed "
                        "reason=canonical_draw_failed recovered=%lu "
                        "total=%lu\n",
                        ownerLabel,
                        static_cast<unsigned long>(recoveredQuads),
                        static_cast<unsigned long>(totalQuads));
            }
        }
    }
    else
    {
        // Never dereference an unproved staging range.  Disarm only this
        // presentation path; simulation, bucket links and replay state are
        // untouched.
        fprintf(stderr,
                "TH08PSP %s recovery=fail_closed "
                "pairs=%lu general=%lu sprites=%lu span=%lu expected=%lu\n",
                ownerLabel,
                static_cast<unsigned long>(pairCount),
                static_cast<unsigned long>(generalCount),
                static_cast<unsigned long>(manager->spritesToDraw),
                static_cast<unsigned long>(batchSpan),
                static_cast<unsigned long>(expectedVertices * vertexBytes));
        ++stats.invalidRangeBatches;
    }

    FinishPspMixedBatch(
        owner, submitted, !submitted,
        canonicalReplayComplete ? totalQuads : 0U,
        !validRange || canonicalReplayDrawFailed,
        canonicalReplayDrawFailed);
    if (!validRange || canonicalReplayDrawFailed)
        disarmAndResetStaging();
    return true;
}

#if TH08_PSP_BULLET_MIXED_QUADS_PRODUCT_ENABLED
bool FlushPspBulletMixedQuadBatch(AnmManager *manager)
{
    return FlushPspMixedQuadBatch(
        manager, PspSpritePairRunOwner::Bullet);
}
#endif

#if TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED
bool FlushPspItemMixedQuadBatch(AnmManager *manager)
{
    return FlushPspMixedQuadBatch(
        manager, PspSpritePairRunOwner::Item);
}
#endif
} // namespace
#endif

#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED
void PspQueryEffectIndexedQuadStats(PspEffectIndexedQuadStats *stats)
{
    if (stats != NULL)
        *stats = g_PspEffectIndexedQuadStats;
}

void PspResetEffectIndexedQuadStats()
{
    memset(&g_PspEffectIndexedQuadStats, 0,
           sizeof(g_PspEffectIndexedQuadStats));
}
#endif

#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
namespace
{
enum class PspItemNaturalFlushResult : u8
{
    NotMarked,
    Submitted,
    CanonicalFallback,
};

void NotePspItemNaturalFallback(u32 *specific)
{
    ++g_PspItemNaturalQuadStorage.stats.fallbackBatches;
    if (specific != NULL)
        ++*specific;
}

bool ValidatePspItemNaturalIndexAuthority(u32 quadCount)
{
    InitializePspBulletUnifiedQuadIndices();
    PspItemNaturalQuadStorage &storage = g_PspItemNaturalQuadStorage;
    if (storage.indexAuthorityRejected != 0U)
        return false;
    if (storage.indexAuthority == NULL)
        storage.indexAuthority = g_PspBulletUnifiedQuadIndices;
    if (storage.indexAuthority != g_PspBulletUnifiedQuadIndices)
        return false;

    const u32 indexCount = quadCount * 6U;
    static const u16 kCorners[6] = {0U, 1U, 2U, 1U, 2U, 3U};
    for (u32 ordinal = storage.validatedIndexCount;
         ordinal < indexCount; ++ordinal)
    {
        const u32 expected = (ordinal / 6U) * 4U +
            kCorners[ordinal % 6U];
        if (storage.indexAuthority[ordinal] != expected)
        {
            storage.indexAuthorityRejected = 1U;
            return false;
        }
    }
    if (storage.validatedIndexCount < indexCount)
        storage.validatedIndexCount = indexCount;
    return true;
}

PspItemNaturalFlushResult TrySubmitPspItemNaturalQuadsAtCanonicalBoundary(
    AnmManager *manager)
{
    PspItemNaturalQuadStorage &storage = g_PspItemNaturalQuadStorage;
    PspItemNaturalQuadStats &stats = storage.stats;
    if (storage.batchMarked == 0U)
        return PspItemNaturalFlushResult::NotMarked;

    ++stats.canonicalBatches;
    ++stats.triggerBatches;
    const u32 quadCount = manager != NULL ? manager->spritesToDraw : 0U;
    const u32 triggerQuads = storage.batchTriggerQuads;
    const uintptr_t bufferBegin = manager != NULL
        ? reinterpret_cast<uintptr_t>(manager->vertexBuffer)
        : 0U;
    const uintptr_t bufferLimit = manager != NULL
        ? reinterpret_cast<uintptr_t>(
              manager->vertexBuffer + ARRAY_SIZE(manager->vertexBuffer))
        : 0U;
    const uintptr_t batchBegin = manager != NULL
        ? reinterpret_cast<uintptr_t>(manager->vertexBufferStartPtr)
        : 0U;
    const uintptr_t batchEnd = manager != NULL
        ? reinterpret_cast<uintptr_t>(manager->vertexBufferEndPtr)
        : 0U;

    if (g_PspBulletUnifiedQuadBatchActive ||
        g_PspUnifiedQuadBatchOwner != PspUnifiedQuadBatchOwner::None)
    {
        NotePspItemNaturalFallback(&stats.stateFallbacks);
        ResetPspItemNaturalQuadBatchMarker();
        return PspItemNaturalFlushResult::CanonicalFallback;
    }

    if (manager == NULL || manager->vertexBufferStartPtr == NULL ||
        manager->vertexBufferEndPtr == NULL || batchBegin < bufferBegin ||
        batchBegin > bufferLimit || batchEnd < batchBegin ||
        batchEnd > bufferLimit ||
        (batchBegin - bufferBegin) %
                sizeof(VertexTex1DiffuseXyzrhw) != 0U)
    {
        NotePspItemNaturalFallback(&stats.pointerFallbacks);
        ResetPspItemNaturalQuadBatchMarker();
        return PspItemNaturalFlushResult::CanonicalFallback;
    }
    if (quadCount == 0U || quadCount > kPspBulletUnifiedQuadCapacity ||
        triggerQuads == 0U || triggerQuads > quadCount)
    {
        NotePspItemNaturalFallback(&stats.capacityFallbacks);
        ResetPspItemNaturalQuadBatchMarker();
        return PspItemNaturalFlushResult::CanonicalFallback;
    }

    const uintptr_t expectedSpan =
        static_cast<uintptr_t>(quadCount) * 6U *
        sizeof(VertexTex1DiffuseXyzrhw);
    if (batchEnd - batchBegin != expectedSpan)
    {
        NotePspItemNaturalFallback(&stats.spanFallbacks);
        ResetPspItemNaturalQuadBatchMarker();
        return PspItemNaturalFlushResult::CanonicalFallback;
    }

    ++stats.topologyChecks;
    const VertexTex1DiffuseXyzrhw *const source =
        manager->vertexBufferStartPtr;
    for (u32 quad = 0U; quad < quadCount; ++quad)
    {
        const VertexTex1DiffuseXyzrhw *const vertices = source + quad * 6U;
        if (memcmp(&vertices[1], &vertices[3], sizeof(vertices[1])) != 0 ||
            memcmp(&vertices[2], &vertices[4], sizeof(vertices[2])) != 0)
        {
            NotePspItemNaturalFallback(&stats.topologyFallbacks);
            ++stats.extraTopologyBatches;
            ResetPspItemNaturalQuadBatchMarker();
            return PspItemNaturalFlushResult::CanonicalFallback;
        }
    }
    stats.topologyCheckedQuads += quadCount;

    if (!ValidatePspItemNaturalIndexAuthority(quadCount))
    {
        NotePspItemNaturalFallback(&stats.indexFallbacks);
        ResetPspItemNaturalQuadBatchMarker();
        return PspItemNaturalFlushResult::CanonicalFallback;
    }

    const PspItemNaturalQuadSubmitResult result =
        th08_psp_draw_item_natural_quads(
            g_Supervisor.d3dDevice, source, quadCount,
            sizeof(VertexTex1DiffuseXyzrhw),
            g_PspBulletUnifiedQuadIndices, quadCount * 6U);
    if (result != PSP_ITEM_NATURAL_SUBMIT_NATIVE &&
        result != PSP_ITEM_NATURAL_SUBMIT_CLIENT &&
        result != PSP_ITEM_NATURAL_SUBMIT_CLIENT_AFTER_NATIVE_REJECT)
    {
        switch (result)
        {
        case PSP_ITEM_NATURAL_REJECT_CAPACITY:
            NotePspItemNaturalFallback(&stats.capacityFallbacks);
            break;
        case PSP_ITEM_NATURAL_REJECT_INDEX:
            NotePspItemNaturalFallback(&stats.indexFallbacks);
            break;
        case PSP_ITEM_NATURAL_REJECT_DEVICE:
        case PSP_ITEM_NATURAL_REJECT_STATE:
        default:
            NotePspItemNaturalFallback(&stats.stateFallbacks);
            break;
        }
        ResetPspItemNaturalQuadBatchMarker();
        return PspItemNaturalFlushResult::CanonicalFallback;
    }

    stats.coalescedQuads += quadCount - triggerQuads;
    stats.eligibleQuads += quadCount;
    ++stats.submittedBatches;
    stats.submittedQuads += quadCount;
    stats.canonicalInputVertices += quadCount * 6U;
    stats.packedOutputVertices += quadCount * 4U;
    stats.duplicateVerticesAvoided += quadCount * 2U;
    if (stats.maxBatchQuads < quadCount)
        stats.maxBatchQuads = quadCount;
    if (result == PSP_ITEM_NATURAL_SUBMIT_NATIVE)
    {
        ++stats.nativeSubmits;
        stats.nativeSubmittedQuads += quadCount;
    }
    else
    {
        if (result ==
            PSP_ITEM_NATURAL_SUBMIT_CLIENT_AFTER_NATIVE_REJECT)
        {
            ++stats.nativeFallbacks;
        }
        ++stats.clientFallbackSubmits;
        stats.clientFallbackQuads += quadCount;
    }
    ResetPspItemNaturalQuadBatchMarker();
    return PspItemNaturalFlushResult::Submitted;
}
} // namespace
#endif

void AnmManager::ClearVertexBuffer()
{
#if defined(PSP)
    // A queued FPS-overlay glyph belongs only to the current shared sprite
    // batch.  Clearing without a submit means it was not rendered and must not
    // leak into the next batch's attribution scope.
    th08::psp::RenderPerfDiscardQueuedFpsOverlayVertices();
#endif
#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
    AbandonPspItemNaturalQuadBatch(this->spritesToDraw);
#endif
#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED
    if (g_PspBulletUnifiedQuadBatchActive &&
        g_PspUnifiedQuadBatchOwner == PspUnifiedQuadBatchOwner::Effect)
    {
        g_PspEffectIndexedQuadStats.abandonedQuads += this->spritesToDraw;
        AbandonPspEffectIndexedQuadPass();
    }
#endif
    this->spritesToDraw = 0;
    this->vertexBufferStartPtr = this->vertexBufferEndPtr = this->vertexBuffer;
#if defined(PSP) && defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
    TH08_PSP_BULLET_UNIFIED_QUADS
#if defined(TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH) && \
    TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH
    if (g_Supervisor.d3dDevice != NULL)
        th08_psp_bullet_packed_vertex_end(g_Supervisor.d3dDevice);
    g_PspBulletPackedVertexFastpathActive = false;
#endif
#if defined(TH08_PSP_BULLET_DIRECT_GE) && TH08_PSP_BULLET_DIRECT_GE
    th08_psp_bullet_direct_ge_set_batch(false);
#endif
#if defined(TH08_PSP_ITEM_DIRECT_GE) && TH08_PSP_ITEM_DIRECT_GE
    th08_psp_item_direct_ge_set_batch(false);
#endif
#if TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED
    th08_psp_item_mixed_ge_set_batch(false);
#endif
    g_PspBulletUnifiedQuadBatchActive = false;
    g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::None;
#if TH08_PSP_ANY_MIXED_QUADS_ENABLED
    PspSpritePairRunState &run = GetPspItemTimeDrawPairSidecar().run;
    if (run.owner == PspSpritePairRunOwner::Item ||
        run.owner == PspSpritePairRunOwner::Bullet)
        memset(&run, 0, sizeof(run));
#endif
#endif
}

void AnmManager::FlushVertexBuffer()
{
    if (this->spritesToDraw == 0)
    {
#if defined(PSP)
        th08::psp::RenderPerfDiscardQueuedFpsOverlayVertices();
#endif
#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
        AbandonPspItemNaturalQuadBatch(0U);
#endif
        return;
    }

#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED
    const bool pspEffectIndexedQuadFlush =
        g_PspBulletUnifiedQuadBatchActive &&
        g_PspUnifiedQuadBatchOwner == PspUnifiedQuadBatchOwner::Effect;
    const u32 pspEffectIndexedQuadFlushQuads =
        pspEffectIndexedQuadFlush ? this->spritesToDraw : 0U;
    if (pspEffectIndexedQuadFlush)
        ++g_PspEffectIndexedQuadStats.flushes;
#endif

#if defined(PSP)
    // RenderPerfNoteDraw reports logical submitted vertices.  Every sprite in
    // this shared batch has the canonical six-index workload even when its
    // PSP storage is packed to 2V/4V.  The nested-safe scope commits only the
    // FPS/replay-diagnostic subset after the complete batch is submitted.
    th08::psp::RenderPerfFpsOverlayDrawScope fpsOverlayDrawScope(
        this->spritesToDraw * 6U);
#endif

    g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_Supervisor.d3dDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
    if (TrySubmitPspItemNaturalQuadsAtCanonicalBoundary(this) ==
        PspItemNaturalFlushResult::Submitted)
    {
        this->vertexBufferStartPtr = this->vertexBufferEndPtr;
        this->spritesToDraw = 0U;
        ++this->flushesThisFrame;
        return;
    }
#endif
#if defined(PSP) && defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
    TH08_PSP_BULLET_UNIFIED_QUADS
    if (g_PspBulletUnifiedQuadBatchActive)
    {
#if defined(TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH) && \
    TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH
        if (g_PspBulletPackedVertexFastpathActive &&
            g_PspUnifiedQuadBatchOwner ==
                PspUnifiedQuadBatchOwner::Bullet)
        {
            // This is the same canonical state boundary and the same single
            // indexed draw as the accepted 4V path.  Only its source storage
            // is already-final 24B PSP data, so no 28B staging range exists.
            InitializePspBulletUnifiedQuadIndices();
            const bool submitted =
                th08_psp_bullet_packed_vertex_submit(
                    g_Supervisor.d3dDevice, this->spritesToDraw,
                    g_PspBulletUnifiedQuadIndices,
                    this->spritesToDraw * 6U);
            if (!submitted)
            {
                fprintf(stderr,
                        "TH08PSP BULLET_PACKED_VERTEX_FASTPATH "
                        "recovery=fail_closed reason=submit_contract "
                        "quads=%lu\n",
                        static_cast<unsigned long>(this->spritesToDraw));
                th08_psp_bullet_packed_vertex_end(
                    g_Supervisor.d3dDevice);
                g_PspBulletPackedVertexFastpathActive = false;
                th08_psp_bullet_direct_ge_set_batch(false);
                g_PspBulletUnifiedQuadBatchActive = false;
                g_PspUnifiedQuadBatchOwner =
                    PspUnifiedQuadBatchOwner::None;
            }
            this->vertexBufferStartPtr = this->vertexBufferEndPtr;
            this->spritesToDraw = 0U;
            this->flushesThisFrame++;
            return;
        }
#endif
#if TH08_PSP_ANY_MIXED_QUADS_PRODUCT_ENABLED
        bool mixedProductFlushed = false;
#if TH08_PSP_BULLET_MIXED_QUADS_PRODUCT_ENABLED
        if (g_PspUnifiedQuadBatchOwner == PspUnifiedQuadBatchOwner::Bullet &&
            GetPspItemTimeDrawPairSidecar().run.owner ==
                PspSpritePairRunOwner::Bullet)
        {
            FlushPspBulletMixedQuadBatch(this);
            mixedProductFlushed = true;
        }
#endif
#if TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED
        if (!mixedProductFlushed &&
            g_PspUnifiedQuadBatchOwner == PspUnifiedQuadBatchOwner::Item &&
            GetPspItemTimeDrawPairSidecar().run.owner ==
                PspSpritePairRunOwner::Item)
        {
            FlushPspItemMixedQuadBatch(this);
            mixedProductFlushed = true;
        }
#endif
        if (!mixedProductFlushed)
#endif
        {
        const uintptr_t bufferBegin =
            reinterpret_cast<uintptr_t>(this->vertexBuffer);
        const uintptr_t bufferLimit = reinterpret_cast<uintptr_t>(
            this->vertexBuffer + ARRAY_SIZE(this->vertexBuffer));
        const uintptr_t batchBegin =
            reinterpret_cast<uintptr_t>(this->vertexBufferStartPtr);
        const uintptr_t batchEnd =
            reinterpret_cast<uintptr_t>(this->vertexBufferEndPtr);
        const uintptr_t vertexBytes = sizeof(VertexTex1DiffuseXyzrhw);
        const uintptr_t quadBytes = 4U * vertexBytes;
        const bool orderedInBuffer =
            batchBegin >= bufferBegin && batchBegin <= bufferLimit &&
            batchEnd >= batchBegin && batchEnd <= bufferLimit;
        const uintptr_t batchOffset =
            orderedInBuffer ? batchBegin - bufferBegin : 0U;
        const uintptr_t batchSpan =
            orderedInBuffer ? batchEnd - batchBegin : 0U;
        // The pre-bullet Item/Laser flush may leave the shared staging cursor
        // after an odd number of canonical 6-vertex sprites.  That is still a
        // valid vertex boundary (6 mod 4 == 2); only the bullet span itself
        // must contain complete 4-vertex quads.  Requiring the absolute start
        // offset to be quad-aligned rejected valid batches before they ever
        // reached the indexed/direct-GE submit path.
        const bool canonicalQuadRange =
            orderedInBuffer && batchOffset % vertexBytes == 0U &&
            batchSpan % quadBytes == 0U;
        const uintptr_t expectedBytes =
            this->spritesToDraw <= kPspBulletUnifiedQuadCapacity
                ? static_cast<uintptr_t>(this->spritesToDraw) * quadBytes
                : 0U;
        const bool validRange =
            this->spritesToDraw <= kPspBulletUnifiedQuadCapacity &&
            canonicalQuadRange && batchSpan == expectedBytes;
        if (!validRange)
        {
            // Preserve every complete quad proven to be inside the canonical
            // 4V staging range. A corrupt or unaligned pointer is never
            // dereferenced; that case is logged and failed closed.
            const u32 requestedQuads = this->spritesToDraw;
            const u32 rangeQuads = canonicalQuadRange
                                       ? static_cast<u32>(batchSpan / quadBytes)
                                       : 0U;
            const u32 recoverableQuads =
                requestedQuads < rangeQuads ? requestedQuads : rangeQuads;
            u32 recoveredQuads = 0U;
            [[maybe_unused]] bool recoveryDrawFailed = false;
            if (!canonicalQuadRange)
            {
                fprintf(stderr,
                        "TH08PSP BULLET_UQ_RECOVERY result=fail_closed "
                        "reason=unsafe_pointer_or_alignment sprites=%lu "
                        "begin=0x%08lx end=0x%08lx buffer=0x%08lx "
                        "limit=0x%08lx\n",
                        static_cast<unsigned long>(requestedQuads),
                        static_cast<unsigned long>(batchBegin),
                        static_cast<unsigned long>(batchEnd),
                        static_cast<unsigned long>(bufferBegin),
                        static_cast<unsigned long>(bufferLimit));
            }
            else
            {
                for (; recoveredQuads < recoverableQuads; ++recoveredQuads)
                {
                    const VertexTex1DiffuseXyzrhw *quad =
                        this->vertexBufferStartPtr + recoveredQuads * 4U;
                    VertexTex1DiffuseXyzrhw fallback[6];
                    fallback[0] = quad[0];
                    fallback[1] = quad[1];
                    fallback[2] = quad[2];
                    fallback[3] = quad[1];
                    fallback[4] = quad[2];
                    fallback[5] = quad[3];
                    const HRESULT recoveryResult =
                        g_Supervisor.d3dDevice->DrawPrimitiveUP(
                            D3DPT_TRIANGLELIST, 2U, fallback,
                            sizeof(VertexTex1DiffuseXyzrhw));
                    if (FAILED(recoveryResult))
                    {
                        recoveryDrawFailed = true;
                        fprintf(stderr,
                                "TH08PSP BULLET_UQ_RECOVERY "
                                "result=fail_closed reason=draw_failed "
                                "requested=%lu range=%lu recovered=%lu\n",
                                static_cast<unsigned long>(requestedQuads),
                                static_cast<unsigned long>(rangeQuads),
                                static_cast<unsigned long>(recoveredQuads));
                        break;
                    }
                }
                fprintf(stderr,
                        "TH08PSP BULLET_UQ_RECOVERY result=%s "
                        "requested=%lu range=%lu recovered=%lu dropped=%lu\n",
                        recoveredQuads == requestedQuads ? "recovered"
                                                        : "fail_closed",
                        static_cast<unsigned long>(requestedQuads),
                        static_cast<unsigned long>(rangeQuads),
                        static_cast<unsigned long>(recoveredQuads),
                        static_cast<unsigned long>(requestedQuads -
                                                   recoveredQuads));
            }
#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED
            if (pspEffectIndexedQuadFlush)
            {
                NotePspEffectIndexedQuadFallback(
                    pspEffectIndexedQuadFlushQuads, recoveredQuads);
            }
#endif
#if defined(TH08_PSP_BULLET_DIRECT_GE) && TH08_PSP_BULLET_DIRECT_GE
            th08_psp_bullet_direct_ge_set_batch(false);
#endif
#if defined(TH08_PSP_ITEM_DIRECT_GE) && TH08_PSP_ITEM_DIRECT_GE
            th08_psp_item_direct_ge_set_batch(false);
#endif
#if TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED
            th08_psp_item_mixed_ge_set_batch(false);
#endif
            this->spritesToDraw = 0;
            this->vertexBufferStartPtr =
                this->vertexBufferEndPtr = this->vertexBuffer;
            g_PspBulletUnifiedQuadBatchActive = false;
            g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::None;
#if TH08_PSP_ANY_MIXED_QUADS_ENABLED
            const PspSpritePairRunOwner failedOwner =
                GetPspItemTimeDrawPairSidecar().run.owner;
            if (failedOwner == PspSpritePairRunOwner::Item ||
                failedOwner == PspSpritePairRunOwner::Bullet)
            {
                ++PspMixedQuadStatsForOwner(failedOwner)
                      .invalidRangeBatches;
                FinishPspMixedBatch(
                    failedOwner, false, true,
                    recoveredQuads == requestedQuads ? requestedQuads : 0U,
                    recoveredQuads != requestedQuads,
                    recoveryDrawFailed);
            }
#endif
            this->flushesThisFrame++;
            return;
        }

        InitializePspBulletUnifiedQuadIndices();
        const HRESULT indexedResult =
            g_Supervisor.d3dDevice->DrawIndexedPrimitiveUP(
                D3DPT_TRIANGLELIST, 0U, this->spritesToDraw * 4U,
                this->spritesToDraw * 2U,
                g_PspBulletUnifiedQuadIndices, D3DFMT_INDEX16,
                this->vertexBufferStartPtr,
                sizeof(VertexTex1DiffuseXyzrhw));
        u32 indexedRecoveryQuads = 0U;
        [[maybe_unused]] bool indexedRecoveryDrawFailed = false;
        if (FAILED(indexedResult))
        {
            // The indexed backend validates before submission. Its failure is
            // therefore safe to recover with the exact canonical six-vertex
            // topology, one quad at a time and without another allocation.
            for (u32 sprite = 0; sprite < this->spritesToDraw; ++sprite)
            {
                const VertexTex1DiffuseXyzrhw *quad =
                    this->vertexBufferStartPtr + sprite * 4U;
                VertexTex1DiffuseXyzrhw fallback[6];
                fallback[0] = quad[0];
                fallback[1] = quad[1];
                fallback[2] = quad[2];
                fallback[3] = quad[1];
                fallback[4] = quad[2];
                fallback[5] = quad[3];
                const HRESULT recoveryResult =
                    g_Supervisor.d3dDevice->DrawPrimitiveUP(
                        D3DPT_TRIANGLELIST, 2U, fallback,
                        sizeof(VertexTex1DiffuseXyzrhw));
                if (FAILED(recoveryResult))
                {
                    indexedRecoveryDrawFailed = true;
                    break;
                }
                ++indexedRecoveryQuads;
            }
        }
#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED
        if (pspEffectIndexedQuadFlush)
        {
            if (SUCCEEDED(indexedResult))
            {
                NotePspEffectIndexedQuadSuccess(
                    pspEffectIndexedQuadFlushQuads);
            }
            else
            {
                NotePspEffectIndexedQuadFallback(
                    pspEffectIndexedQuadFlushQuads,
                    indexedRecoveryQuads);
            }
        }
#endif
#if TH08_PSP_ANY_MIXED_QUADS_ENABLED
        const PspSpritePairRunOwner auditOwner =
            GetPspItemTimeDrawPairSidecar().run.owner;
        if (auditOwner == PspSpritePairRunOwner::Item ||
            auditOwner == PspSpritePairRunOwner::Bullet)
        {
            // AUDIT keeps its accepted renderer byte-for-byte unchanged;
            // PRODUCT already consumed and reset the run above.
            FinishPspMixedBatch(
                auditOwner, false, FAILED(indexedResult),
                FAILED(indexedResult) &&
                        indexedRecoveryQuads == this->spritesToDraw
                    ? indexedRecoveryQuads
                    : 0U,
                indexedRecoveryDrawFailed,
                indexedRecoveryDrawFailed);
            if (indexedRecoveryDrawFailed)
                this->ClearVertexBuffer();
        }
#endif
        }
    }
    else
#endif
    {
    g_Supervisor.d3dDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, this->spritesToDraw * 2, this->vertexBufferStartPtr,
                                            sizeof(VertexTex1DiffuseXyzrhw));
#if defined(PSP) && defined(TH08_PSP_ITEM_MIXED_QUADS_AUDIT) && \
    TH08_PSP_ITEM_MIXED_QUADS_AUDIT
    if (GetPspItemTimeDrawPairSidecar().run.owner ==
        PspSpritePairRunOwner::Item)
    {
        // Audit observes the final canonical quad but retains the original
        // six-vertex DrawPrimitiveUP submission byte-for-byte.
        FinishPspItemMixedBatch(false, false, 0U, false, false);
    }
#endif
    }

    this->vertexBufferStartPtr = this->vertexBufferEndPtr;
    this->spritesToDraw = 0;
    this->flushesThisFrame++;
}

#if defined(PSP) && defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
    TH08_PSP_BULLET_UNIFIED_QUADS
void AnmManager::BeginPspBulletUnifiedQuadBatch()
{
    // Item and Laser precede the six enemy-bullet buckets. Their canonical
    // six-vertex stream must be submitted before the representation changes.
#if TH08_PSP_ANY_MIXED_QUADS_ENABLED
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
#if TH08_PSP_ITEM_TIME_DRAW_PAIR_ENABLED
    if (sidecar.run.owner == PspSpritePairRunOwner::ItemTime)
        this->PspItemTimeDrawPairBoundary();
#endif
#endif
    this->FlushVertexBuffer();
    InitializePspBulletUnifiedQuadIndices();
#if defined(TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH) && \
    TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH
    g_PspBulletPackedVertexFastpathActive = false;
#endif
#if TH08_PSP_ANY_MIXED_QUADS_ENABLED
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
    ++sidecar.bulletStats.passes;
#endif
    if (sidecar.run.owner != PspSpritePairRunOwner::None)
    {
        // A generic Item pair run still owns the sidecar.  Never erase its VM
        // replay authority to start a Bullet representation; this pass uses
        // the canonical 6V renderer instead.
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
        ++sidecar.bulletStats.ownerConflictPasses;
#else
        ++sidecar.itemStats.ownerConflictPasses;
#endif
        ++sidecar.rejectedBulletPassDepth;
        g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::None;
        g_PspBulletUnifiedQuadBatchActive = false;
#if defined(TH08_PSP_BULLET_DIRECT_GE) && TH08_PSP_BULLET_DIRECT_GE
        th08_psp_bullet_direct_ge_set_batch(false);
#endif
#if TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED
        // Item -> Bullet nesting suspends the Item token explicitly. The
        // symmetric End helper may restore it only after both depths drain.
        if (sidecar.run.owner == PspSpritePairRunOwner::Item)
            th08_psp_item_mixed_ge_set_batch(false);
#endif
        return;
    }
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
    memset(&sidecar.run, 0, sizeof(sidecar.run));
    sidecar.run.owner = PspSpritePairRunOwner::Bullet;
    sidecar.run.passActive = 1U;
    sidecar.run.backendReady = g_Supervisor.d3dDevice != NULL ? 1U : 0U;
#endif
#endif
    g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::Bullet;
    g_PspBulletUnifiedQuadBatchActive = true;
#if defined(TH08_PSP_BULLET_DIRECT_GE) && TH08_PSP_BULLET_DIRECT_GE
    th08_psp_bullet_direct_ge_set_batch(true);
#endif
#if defined(TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH) && \
    TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH
    // Append's pure color evaluator models the canonical Flush-time ARG2=
    // DIFFUSE override without mutating D3D state here. Requested/emitted
    // state-call topology therefore remains exactly OFF-identical.
    g_PspBulletPackedVertexFastpathActive =
        th08_psp_bullet_packed_vertex_begin(
            g_Supervisor.d3dDevice);
#endif
    if (!PspBulletUnifiedQuadBufferCanAppend(this))
    {
        // Previous draws were synchronously consumed by the D3D compatibility
        // layer, so the per-frame staging array can be safely recycled.
        this->vertexBufferStartPtr =
            this->vertexBufferEndPtr = this->vertexBuffer;
    }
}

void AnmManager::EndPspBulletUnifiedQuadBatch()
{
#if TH08_PSP_ANY_MIXED_QUADS_ENABLED
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
    if (sidecar.rejectedBulletPassDepth != 0U)
    {
        // The rejected pass was deliberately isolated as canonical 6V after
        // its previous owner was submitted. Commit it before that owner may
        // resume and repurpose the shared staging front. The helper requires
        // sidecar.rejectedItemPassDepth == 0U before restoring either token.
        this->FlushVertexBuffer();
        --sidecar.rejectedBulletPassDepth;
        RestorePspMixedQuadOwnerTokensIfQuiescent();
        return;
    }
#endif
    if (g_PspBulletUnifiedQuadBatchActive)
    {
        // Submit every bullet before EffectManager resumes the canonical 6V
        // path. FlushVertexBuffer may itself fail closed and disarm the mode.
        this->FlushVertexBuffer();
    }
#if defined(TH08_PSP_BULLET_DIRECT_GE) && TH08_PSP_BULLET_DIRECT_GE
    th08_psp_bullet_direct_ge_set_batch(false);
#endif
#if defined(TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH) && \
    TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH
    th08_psp_bullet_packed_vertex_end(g_Supervisor.d3dDevice);
    g_PspBulletPackedVertexFastpathActive = false;
#endif
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
    PspSpritePairRunState &run = GetPspItemTimeDrawPairSidecar().run;
    if (run.owner == PspSpritePairRunOwner::Bullet)
    {
        run.passActive = 0U;
        run.runActive = 0U;
        run.stateRunActive = 0U;
        run.stickyGeneral = 0U;
        run.pairCount = 0U;
        run.generalCount = 0U;
        run.owner = PspSpritePairRunOwner::None;
    }
#endif
    g_PspBulletUnifiedQuadBatchActive = false;
    g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::None;
}

#if defined(TH08_PSP_EFFECT_INDEXED_QUADS) && \
    TH08_PSP_EFFECT_INDEXED_QUADS
void AnmManager::BeginPspEffectIndexedQuadBatch()
{
    // Effect draw groups are presentation-only passes outside the Item and
    // Bullet owners. Commit the preceding canonical/foreign representation
    // before selecting four unique vertices per ordinary Effect sprite.
    this->FlushVertexBuffer();
    InitializePspBulletUnifiedQuadIndices();

    ++g_PspEffectIndexedQuadStats.passes;
    if (g_PspEffectIndexedQuadPassOpen ||
        g_PspBulletUnifiedQuadBatchActive ||
        g_PspUnifiedQuadBatchOwner != PspUnifiedQuadBatchOwner::None)
    {
        ++g_PspEffectIndexedQuadStats.ownerConflicts;
        AbandonPspEffectIndexedQuadPass();
    }
    g_PspEffectIndexedQuadPassOpen = true;
    g_PspEffectIndexedQuadPassAbandoned = false;

    g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::Effect;
    g_PspBulletUnifiedQuadBatchActive = true;
    if (!PspBulletUnifiedQuadBufferCanAppend(this))
    {
        // The PSP D3D compatibility layer consumes every submitted range
        // synchronously, so an empty pass may safely recycle its staging
        // front exactly as the established Bullet path does.
        this->vertexBufferStartPtr =
            this->vertexBufferEndPtr = this->vertexBuffer;
    }
}

void AnmManager::EndPspEffectIndexedQuadBatch()
{
    if (g_PspBulletUnifiedQuadBatchActive &&
        g_PspUnifiedQuadBatchOwner == PspUnifiedQuadBatchOwner::Effect)
    {
        // Preserve the final Effect sprite/state boundary before the next
        // draw-chain client resumes the canonical six-vertex writer.
        this->FlushVertexBuffer();
    }
    if (!g_PspBulletUnifiedQuadBatchActive ||
        g_PspUnifiedQuadBatchOwner != PspUnifiedQuadBatchOwner::Effect)
    {
        AbandonPspEffectIndexedQuadPass();
    }
    g_PspBulletUnifiedQuadBatchActive = false;
    g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::None;
    g_PspEffectIndexedQuadPassOpen = false;
    g_PspEffectIndexedQuadPassAbandoned = false;
}
#endif

#if TH08_PSP_ITEM_MIXED_QUADS_ENABLED
void AnmManager::BeginPspItemMixedQuadBatch()
{
    // ItemManager owns one complete linked-list traversal.  Commit geometry
    // inherited from the previous draw-chain owner before selecting either
    // the audit-only canonical stream or the product mixed representation.
    this->FlushVertexBuffer();
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
    ++sidecar.itemStats.passes;
    if (sidecar.run.owner != PspSpritePairRunOwner::None)
    {
        // Never erase another owner's replay authority.  This Item pass stays
        // on the canonical six-vertex path and End commits it independently.
        ++sidecar.itemStats.ownerConflictPasses;
        ++sidecar.rejectedItemPassDepth;
        g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::None;
        g_PspBulletUnifiedQuadBatchActive = false;
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED && \
    defined(TH08_PSP_BULLET_DIRECT_GE) && TH08_PSP_BULLET_DIRECT_GE
        if (sidecar.run.owner == PspSpritePairRunOwner::Bullet)
            th08_psp_bullet_direct_ge_set_batch(false);
#endif
#if TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED
        th08_psp_item_mixed_ge_set_batch(false);
#endif
        return;
    }

    memset(&sidecar.run, 0, sizeof(sidecar.run));
    sidecar.run.owner = PspSpritePairRunOwner::Item;
    sidecar.run.passActive = 1U;
    sidecar.run.backendReady =
        g_Supervisor.d3dDevice != NULL ? 1U : 0U;

#if TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED
    InitializePspBulletUnifiedQuadIndices();
    g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::Item;
    g_PspBulletUnifiedQuadBatchActive = true;
    th08_psp_item_mixed_ge_set_batch(true);
    if (!PspBulletUnifiedQuadBufferCanAppend(this))
    {
        // Every previous draw was consumed synchronously before the owner
        // token changed, so the shared staging front is safe to recycle.
        this->vertexBufferStartPtr =
            this->vertexBufferEndPtr = this->vertexBuffer;
    }
#else
    // AUDIT observes the final quad bytes but deliberately retains the exact
    // accepted six-vertex DrawPrimitiveUP path.
    g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::None;
    g_PspBulletUnifiedQuadBatchActive = false;
#endif
}

void AnmManager::EndPspItemMixedQuadBatch()
{
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
    if (sidecar.rejectedItemPassDepth != 0U)
    {
        this->FlushVertexBuffer();
        --sidecar.rejectedItemPassDepth;
        RestorePspMixedQuadOwnerTokensIfQuiescent();
        return;
    }
    if (sidecar.run.owner != PspSpritePairRunOwner::Item)
        return;

    // Product submits the mixed prefix/suffix; audit submits the untouched
    // canonical stream.  Both close before Laser may change state or order.
    this->FlushVertexBuffer();
#if TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED
    th08_psp_item_mixed_ge_set_batch(false);
#endif
    g_PspBulletUnifiedQuadBatchActive = false;
    g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::None;
    memset(&sidecar.run, 0, sizeof(sidecar.run));
}
#endif

#if defined(TH08_PSP_ITEM_DIRECT_GE) && TH08_PSP_ITEM_DIRECT_GE
void AnmManager::BeginPspItemUnifiedQuadBatch()
{
    // Item is the first sub-pass owned by BulletManager::OnDraw.  Commit any
    // geometry inherited from the previous draw-chain owner before changing
    // the staging representation from canonical 6V sprites to indexed 4V
    // quads.  Item list order and every state-triggered Flush remain intact.
    this->FlushVertexBuffer();
    InitializePspBulletUnifiedQuadIndices();
    g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::Item;
    g_PspBulletUnifiedQuadBatchActive = true;
    th08_psp_item_direct_ge_set_batch(true);
    if (!PspBulletUnifiedQuadBufferCanAppend(this))
    {
        this->vertexBufferStartPtr =
            this->vertexBufferEndPtr = this->vertexBuffer;
    }
}

void AnmManager::EndPspItemUnifiedQuadBatch()
{
    if (g_PspBulletUnifiedQuadBatchActive &&
        g_PspUnifiedQuadBatchOwner == PspUnifiedQuadBatchOwner::Item)
    {
        // Items must be committed before the laser pass restores the ordinary
        // six-vertex writer.  PSPGL keeps the submitted texture/list owners
        // pinned; only the immutable packed Item arena outlives this call.
        this->FlushVertexBuffer();
    }
    th08_psp_item_direct_ge_set_batch(false);
    g_PspBulletUnifiedQuadBatchActive = false;
    g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::None;
}
#endif
#endif

/* This function copies 4 vertices creating a quad into 6 vertices
 * (2 triangles) for rendering.
 */
ZunResult AnmManager::AddSpriteToDrawBuffer(VertexTex1DiffuseXyzrhw *vertices)
{
#if defined(PSP) && defined(TH08_PSP_ITEM_MIXED_QUADS_AUDIT) && \
    TH08_PSP_ITEM_MIXED_QUADS_AUDIT
    PspSpritePairRunState &itemAuditRun =
        GetPspItemTimeDrawPairSidecar().run;
    if (itemAuditRun.owner == PspSpritePairRunOwner::Item &&
        itemAuditRun.passActive != 0U)
    {
        const PspBulletMixedQuadRejectReason reason =
            ClassifyPspBulletMixedQuad(vertices);
        RecordPspItemMixedQuad(reason, PspItemMixedWouldUsePair(reason));
        // Observation only: the final four quad bytes remain untouched and
        // continue into the exact canonical six-vertex writer below.
    }
#endif
#if defined(PSP) && defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
    TH08_PSP_BULLET_UNIFIED_QUADS
    if (g_PspBulletUnifiedQuadBatchActive)
    {
#if defined(TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH) && \
    TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH
        if (g_PspBulletPackedVertexFastpathActive &&
            g_PspUnifiedQuadBatchOwner ==
                PspUnifiedQuadBatchOwner::Bullet)
        {
            if (this->spritesToDraw >= kPspBulletUnifiedQuadCapacity)
                this->FlushVertexBuffer();
            if (g_PspBulletPackedVertexFastpathActive &&
                th08_psp_bullet_packed_vertex_append(
                    g_Supervisor.d3dDevice, vertices,
                    sizeof(VertexTex1DiffuseXyzrhw)))
            {
                ++this->spritesToDraw;
                return ZUN_SUCCESS;
            }

            // A first-quad rejection retains the complete untouched quad and
            // may select the accepted 28B canonical path below.  A rejection
            // after prior packed quads would have no 28B replay authority; do
            // not invent an extra flush/draw or silently change ordering.
            const u32 packedQuadsBeforeFallback = this->spritesToDraw;
            const bool safeCanonicalFallback =
                packedQuadsBeforeFallback == 0U;
            if (!safeCanonicalFallback)
            {
                // Unexpected owner/state/capacity loss after Begin's full
                // 0x600-slot reservation must not drop already-packed bullets.
                // Commit that prefix at the current canonical state boundary,
                // record the resulting NO-GO split, then let this untouched
                // current quad and the suffix use the canonical writer below.
                this->FlushVertexBuffer();
                th08_psp_bullet_packed_vertex_note_recovery_split(
                    g_Supervisor.d3dDevice,
                    packedQuadsBeforeFallback);
            }
            th08_psp_bullet_packed_vertex_end(
                g_Supervisor.d3dDevice);
            g_PspBulletPackedVertexFastpathActive = false;
            if (!safeCanonicalFallback)
            {
                fprintf(stderr,
                        "TH08PSP BULLET_PACKED_VERTEX_FASTPATH "
                        "recovery=split_to_canonical reason=mid_run_append "
                        "packed_quads=%lu\n",
                        static_cast<unsigned long>(
                            packedQuadsBeforeFallback));
            }
        }
#endif
#if TH08_PSP_ANY_MIXED_QUADS_ENABLED
        PspSpritePairRunState &mixedRun =
            GetPspItemTimeDrawPairSidecar().run;
        const bool bulletMixedOwner =
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
            g_PspUnifiedQuadBatchOwner == PspUnifiedQuadBatchOwner::Bullet &&
            mixedRun.owner == PspSpritePairRunOwner::Bullet;
#else
            false;
#endif
        const bool itemMixedOwner =
#if TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED
            g_PspUnifiedQuadBatchOwner == PspUnifiedQuadBatchOwner::Item &&
            mixedRun.owner == PspSpritePairRunOwner::Item;
#else
            false;
#endif
        if ((bulletMixedOwner || itemMixedOwner) &&
            mixedRun.passActive != 0U)
        {
            const PspSpritePairRunOwner mixedOwner = itemMixedOwner
                ? PspSpritePairRunOwner::Item
                : PspSpritePairRunOwner::Bullet;
            const PspBulletMixedQuadRejectReason reason =
                ClassifyPspBulletMixedQuad(vertices);
            const bool usePair = PspMixedWouldUsePair(reason);
            const bool productOwner =
#if TH08_PSP_BULLET_MIXED_QUADS_PRODUCT_ENABLED
                (mixedOwner == PspSpritePairRunOwner::Bullet) ||
#endif
#if TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED
                (mixedOwner == PspSpritePairRunOwner::Item) ||
#endif
                false;
            // Bullet AUDIT deliberately retains its already accepted indexed
            // 4V renderer. Item AUDIT never activates this unified branch.
            const u32 requiredVertices = productOwner
                ? (usePair ? 2U : 4U)
                : 4U;
#if !TH08_PSP_ANY_MIXED_QUADS_PRODUCT_ENABLED
            // AUDIT classifies the final quad but deliberately retains the
            // already accepted four-vertex indexed renderer.
            (void)productOwner;
#endif
            if (this->spritesToDraw >= kPspBulletUnifiedQuadCapacity ||
                !PspBulletUnifiedQuadBufferCanAppendVertices(
                    this, requiredVertices))
            {
                // Capacity alone is not a state boundary.  If the first
                // general quad already appeared, the next physical submit in
                // this same state run must remain a 4V suffix as well.
                const u8 stickyGeneral = mixedRun.stickyGeneral;
                const u8 stateRunActive = mixedRun.stateRunActive;
                this->FlushVertexBuffer();
                if (g_PspBulletUnifiedQuadBatchActive &&
                    mixedRun.owner == mixedOwner)
                {
                    mixedRun.stickyGeneral = stickyGeneral;
                    mixedRun.stateRunActive = stateRunActive;
                }
            }
            if (g_PspBulletUnifiedQuadBatchActive &&
                !PspBulletUnifiedQuadBufferCanAppendVertices(
                    this, requiredVertices))
            {
                this->vertexBufferStartPtr =
                    this->vertexBufferEndPtr = this->vertexBuffer;
            }
            if (g_PspBulletUnifiedQuadBatchActive &&
                PspBulletUnifiedQuadBufferCanAppendVertices(
                    this, requiredVertices))
            {
                RecordPspMixedQuad(mixedOwner, reason, usePair);
                if (productOwner && usePair)
                {
                    this->vertexBufferEndPtr[0] = vertices[0];
                    this->vertexBufferEndPtr[1] = vertices[3];
                    this->vertexBufferEndPtr += 2;
                }
                else
                {
                    this->vertexBufferEndPtr[0] = vertices[0];
                    this->vertexBufferEndPtr[1] = vertices[1];
                    this->vertexBufferEndPtr[2] = vertices[2];
                    this->vertexBufferEndPtr[3] = vertices[3];
                    this->vertexBufferEndPtr += 4;
                }
                ++this->spritesToDraw;
                return ZUN_SUCCESS;
            }
            // A fail-closed mixed batch disarms the presentation token.  The
            // untouched final quad continues through canonical six-vertex
            // submission below; no simulation or linked-list state changes.
        }
        else
#endif
        {
        if (this->spritesToDraw >= kPspBulletUnifiedQuadCapacity ||
            !PspBulletUnifiedQuadBufferCanAppend(this))
        {
            this->FlushVertexBuffer();
        }
        if (g_PspBulletUnifiedQuadBatchActive &&
            !PspBulletUnifiedQuadBufferCanAppend(this))
        {
            this->vertexBufferStartPtr =
                this->vertexBufferEndPtr = this->vertexBuffer;
        }
        if (g_PspBulletUnifiedQuadBatchActive &&
            PspBulletUnifiedQuadBufferCanAppend(this))
        {
            this->vertexBufferEndPtr[0] = vertices[0];
            this->vertexBufferEndPtr[1] = vertices[1];
            this->vertexBufferEndPtr[2] = vertices[2];
            this->vertexBufferEndPtr[3] = vertices[3];
            this->vertexBufferEndPtr += 4;
            this->spritesToDraw++;
            return ZUN_SUCCESS;
        }
        }
        // An invalid indexed range is disarmed by FlushVertexBuffer. Continue
        // through the untouched six-vertex writer for this and later sprites.
    }
#endif
#if defined(PSP) && TH08_PSP_ANY_MIXED_QUADS_ENABLED
    // A rejected mixed batch may arrive here from a cursor only two vertices
    // below the shared array limit.  Canonical fallback needs six complete
    // vertices, so prove that space independently of the former 2V/4V mode.
    if (!PspBulletUnifiedQuadBufferCanAppendVertices(this, 6U))
    {
        this->FlushVertexBuffer();
        if (!PspBulletUnifiedQuadBufferCanAppendVertices(this, 6U))
        {
            // A zero-sprite or just-consumed stream has no live frontend
            // owner. Recycle the synchronously consumed staging array.
            this->vertexBufferStartPtr = this->vertexBuffer;
            this->vertexBufferEndPtr = this->vertexBuffer;
        }
    }
    if (!PspBulletUnifiedQuadBufferCanAppendVertices(this, 6U))
    {
        fprintf(stderr,
                "TH08PSP MIXED_QUADS recovery=fail_closed "
                "reason=canonical_6v_capacity\n");
        const PspSpritePairRunOwner owner =
            GetPspItemTimeDrawPairSidecar().run.owner;
        if (owner == PspSpritePairRunOwner::Item ||
            owner == PspSpritePairRunOwner::Bullet)
        {
            ++PspMixedQuadStatsForOwner(owner).failClosedBatches;
        }
        this->ClearVertexBuffer();
        return ZUN_ERROR;
    }
#endif
    this->vertexBufferEndPtr[0] = vertices[0];
    this->vertexBufferEndPtr[1] = vertices[1];
    this->vertexBufferEndPtr[2] = vertices[2];
    this->vertexBufferEndPtr[3] = vertices[1];
    this->vertexBufferEndPtr[4] = vertices[2];
    this->vertexBufferEndPtr[5] = vertices[3];

    this->vertexBufferEndPtr += 6;
    this->spritesToDraw++;

#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
    // Only this point proves DrawInner survived culling/state setup and the
    // untouched canonical 0,1,2/1,2,3 six-vertex write completed.  The
    // ITEM_TIME wrapper never marks a culled or failed Draw2D candidate.
    MarkPspItemNaturalQuadCanonicalAppend();
#endif

    return ZUN_SUCCESS;
}

#pragma var_order(spriteHalfWidth, spriteHalfHeight)
ZunResult AnmManager::DrawNoRotation(AnmVm *vm)
{
    float spriteHalfWidth;
    float spriteHalfHeight;

    if (!vm->IsVisible())
    {
        return ZUN_ERROR;
    }

    if (!vm->flag1)
    {
        return ZUN_ERROR;
    }

    if (vm->color1.a == 0)
    {
        return ZUN_ERROR;
    }

    spriteHalfWidth = (vm->spriteSize.x * vm->scale.x) / 2.0f;
    spriteHalfHeight = (vm->spriteSize.y * vm->scale.y) / 2.0f;

    if ((vm->anchor & 1) == 0)
    {
        g_QuadVertices[0].pos.x = g_QuadVertices[2].pos.x = vm->pos.x - spriteHalfWidth;
        g_QuadVertices[1].pos.x = g_QuadVertices[3].pos.x = spriteHalfWidth + vm->pos.x;
    }
    else
    {
        g_QuadVertices[0].pos.x = g_QuadVertices[2].pos.x = vm->pos.x;
        g_QuadVertices[1].pos.x = g_QuadVertices[3].pos.x = spriteHalfWidth + vm->pos.x + spriteHalfWidth;
    }

    if ((vm->anchor & 2) == 0)
    {
        g_QuadVertices[0].pos.y = g_QuadVertices[1].pos.y = vm->pos.y - spriteHalfHeight;
        g_QuadVertices[2].pos.y = g_QuadVertices[3].pos.y = spriteHalfHeight + vm->pos.y;
    }
    else
    {
        g_QuadVertices[0].pos.y = g_QuadVertices[1].pos.y = vm->pos.y;
        g_QuadVertices[2].pos.y = g_QuadVertices[3].pos.y = spriteHalfHeight + vm->pos.y + spriteHalfHeight;
    }

    g_QuadVertices[0].pos.z = g_QuadVertices[1].pos.z = g_QuadVertices[2].pos.z = g_QuadVertices[3].pos.z = vm->pos.z;

    return this->DrawInner(vm, 1);
}

#if TH08_PSP_ITEM_TIME_DRAW_PAIR_ENABLED
namespace
{
constexpr i32 kPspItemTimeDrawPairScriptIndex = 68;
constexpr i32 kPspItemTimeDrawPairInitialSpriteIndex = 179;
constexpr i32 kPspItemTimeDrawPairIndicatorSpriteIndex = 189;
constexpr u32 kPspItemTimeDrawPairFingerprintBytes = 68U;
constexpr u32 kPspItemTimeDrawPairCapacity = 2096U;
constexpr u32 kPspItemTimeDrawPairHashOffset = 2166136261U;
constexpr u32 kPspItemTimeDrawPairHashPrime = 16777619U;

bool ValidatePspItemTimeDrawPairScript68(const AnmRawInstr *sprite)
{
    if (sprite == NULL || sprite->opcode != AnmOpcode_Sprite ||
        sprite->instructionSize != 12 || sprite->time != 0 ||
        sprite->varMask != 0 ||
        sprite->intArgs[0] != kPspItemTimeDrawPairInitialSpriteIndex)
    {
        return false;
    }

    const AnmRawInstr *const color = reinterpret_cast<const AnmRawInstr *>(
        reinterpret_cast<const u8 *>(sprite) + sprite->instructionSize);
    if (color->opcode != AnmOpcode_ColorTime ||
        color->instructionSize != 28 || color->time != 30 ||
        color->varMask != 0 || color->intArgs[0] != 20 ||
        color->intArgs[1] != AnmInterpMode_Linear ||
        color->intArgs[2] != 128 || color->intArgs[3] != 128 ||
        color->intArgs[4] != 128)
    {
        return false;
    }

    const AnmRawInstr *const alpha = reinterpret_cast<const AnmRawInstr *>(
        reinterpret_cast<const u8 *>(color) + color->instructionSize);
    if (alpha->opcode != AnmOpcode_AlphaTime ||
        alpha->instructionSize != 20 || alpha->time != 30 ||
        alpha->varMask != 0 || alpha->intArgs[0] != 20 ||
        alpha->intArgs[1] != AnmInterpMode_Linear ||
        alpha->intArgs[2] != 192)
    {
        return false;
    }

    const AnmRawInstr *const stop = reinterpret_cast<const AnmRawInstr *>(
        reinterpret_cast<const u8 *>(alpha) + alpha->instructionSize);
    return stop->opcode == AnmOpcode_Static &&
           stop->instructionSize == 8 && stop->time == 50 &&
           stop->varMask == 0;
}

void NotePspItemTimeDrawPairReject(PspItemTimeDrawPairRejectReason reason,
                                   u32 count = 1U)
{
    ItemTimeDrawPairStats &stats =
        GetPspItemTimeDrawPairSidecar().stats;
    stats.canonicalFallbacks += count;
    switch (reason)
    {
    case PSP_ITEM_TIME_DRAW_PAIR_REJECT_OWNER:
        stats.ownerFallbacks += count;
        break;
    case PSP_ITEM_TIME_DRAW_PAIR_REJECT_LOAD:
        stats.loadFallbacks += count;
        break;
    case PSP_ITEM_TIME_DRAW_PAIR_REJECT_SCRIPT:
        stats.scriptFallbacks += count;
        break;
    case PSP_ITEM_TIME_DRAW_PAIR_REJECT_SPRITE:
        stats.spriteFallbacks += count;
        break;
    case PSP_ITEM_TIME_DRAW_PAIR_REJECT_VISIBILITY:
        stats.visibilityFallbacks += count;
        break;
    case PSP_ITEM_TIME_DRAW_PAIR_REJECT_ROTATION:
        stats.rotationFallbacks += count;
        break;
    case PSP_ITEM_TIME_DRAW_PAIR_REJECT_SCALE:
        stats.scaleFallbacks += count;
        break;
    case PSP_ITEM_TIME_DRAW_PAIR_REJECT_NONFINITE:
        stats.nonfiniteFallbacks += count;
        break;
    case PSP_ITEM_TIME_DRAW_PAIR_REJECT_TEXTURE:
        stats.textureFallbacks += count;
        break;
    case PSP_ITEM_TIME_DRAW_PAIR_REJECT_STATE:
        stats.stateFallbacks += count;
        break;
    case PSP_ITEM_TIME_DRAW_PAIR_REJECT_AXIS:
        stats.axisFallbacks += count;
        break;
    case PSP_ITEM_TIME_DRAW_PAIR_REJECT_ENDPOINT:
        stats.endpointFallbacks += count;
        break;
    case PSP_ITEM_TIME_DRAW_PAIR_REJECT_CAPACITY:
        stats.capacityFallbacks += count;
        break;
    case PSP_ITEM_TIME_DRAW_PAIR_REJECT_BACKEND:
        stats.backendFallbacks += count;
        break;
    case PSP_ITEM_TIME_DRAW_PAIR_ACCEPT:
        break;
    }
}

bool PspItemTimeDrawPairFloatBitsEqual(f32 left, f32 right)
{
    return memcmp(&left, &right, sizeof(left)) == 0;
}

void BuildPspItemTimeDrawPairQuad(VertexTex1DiffuseXyzrhw *quad,
                                  const AnmVm *vm,
                                  const Float2 &screenShake)
{
    // Preserve DrawNoRotation then DrawInner expression/store order.  The
    // global quad's persistent W/diffuse bytes are copied first because a
    // culled canonical draw does not rewrite diffuse.
    memcpy(quad, g_QuadVertices, sizeof(g_QuadVertices));
    const f32 spriteHalfWidth =
        (vm->spriteSize.x * vm->scale.x) / 2.0f;
    const f32 spriteHalfHeight =
        (vm->spriteSize.y * vm->scale.y) / 2.0f;

    if ((vm->anchor & 1) == 0)
    {
        quad[0].pos.x = quad[2].pos.x = vm->pos.x - spriteHalfWidth;
        quad[1].pos.x = quad[3].pos.x = spriteHalfWidth + vm->pos.x;
    }
    else
    {
        quad[0].pos.x = quad[2].pos.x = vm->pos.x;
        quad[1].pos.x = quad[3].pos.x =
            spriteHalfWidth + vm->pos.x + spriteHalfWidth;
    }

    if ((vm->anchor & 2) == 0)
    {
        quad[0].pos.y = quad[1].pos.y = vm->pos.y - spriteHalfHeight;
        quad[2].pos.y = quad[3].pos.y = spriteHalfHeight + vm->pos.y;
    }
    else
    {
        quad[0].pos.y = quad[1].pos.y = vm->pos.y;
        quad[2].pos.y = quad[3].pos.y =
            spriteHalfHeight + vm->pos.y + spriteHalfHeight;
    }
    quad[0].pos.z = quad[1].pos.z =
        quad[2].pos.z = quad[3].pos.z = vm->pos.z;

    quad[0].pos.x += screenShake.x;
    quad[0].pos.y += screenShake.y;
    quad[1].pos.x += screenShake.x;
    quad[1].pos.y += screenShake.y;
    quad[2].pos.x += screenShake.x;
    quad[2].pos.y += screenShake.y;
    quad[3].pos.x += screenShake.x;
    quad[3].pos.y += screenShake.y;

    const f32 triangleX1 = nearbyintf(quad[0].pos.x) - g_ZeroPointFive;
    const f32 triangleX2 = nearbyintf(quad[1].pos.x) - g_ZeroPointFive;
    const f32 triangleY1 = nearbyintf(quad[0].pos.y) - g_ZeroPointFive;
    const f32 triangleY2 = nearbyintf(quad[2].pos.y) - g_ZeroPointFive;
    quad[2].pos.y = quad[3].pos.y = triangleY2;
    quad[0].pos.y = quad[1].pos.y = triangleY1;
    quad[1].pos.x = quad[3].pos.x = triangleX2;
    quad[0].pos.x = quad[2].pos.x = triangleX1;

    quad[0].textureUV.x = quad[2].textureUV.x =
        vm->loadedSprite->uvStart.x + vm->uvScrollPos.x;
    quad[1].textureUV.x = quad[3].textureUV.x =
        vm->loadedSprite->uvEnd.x + vm->uvScrollPos.x;
    quad[0].textureUV.y = quad[1].textureUV.y =
        vm->loadedSprite->uvStart.y + vm->uvScrollPos.y;
    quad[2].textureUV.y = quad[3].textureUV.y =
        vm->loadedSprite->uvEnd.y + vm->uvScrollPos.y;
}

bool PspItemTimeDrawPairQuadVisible(
    const VertexTex1DiffuseXyzrhw *quad)
{
    const f32 viewportX = static_cast<f32>(g_Supervisor.viewport.X);
    const f32 viewportY = static_cast<f32>(g_Supervisor.viewport.Y);
    const f32 viewportWidth =
        static_cast<f32>(g_Supervisor.viewport.Width);
    const f32 viewportHeight =
        static_cast<f32>(g_Supervisor.viewport.Height);
    f32 triangleX1 = ZUN_MAX(quad[0].pos.x, quad[1].pos.x);
    triangleX1 = ZUN_MAX(quad[2].pos.x, triangleX1);
    triangleX1 = ZUN_MAX(quad[3].pos.x, triangleX1);
    f32 triangleY1 = ZUN_MAX(quad[0].pos.y, quad[1].pos.y);
    triangleY1 = ZUN_MAX(quad[2].pos.y, triangleY1);
    triangleY1 = ZUN_MAX(quad[3].pos.y, triangleY1);
    f32 triangleX2 = ZUN_MIN(quad[0].pos.x, quad[1].pos.x);
    triangleX2 = ZUN_MIN(quad[2].pos.x, triangleX2);
    triangleX2 = ZUN_MIN(quad[3].pos.x, triangleX2);
    f32 triangleY2 = ZUN_MIN(quad[0].pos.y, quad[1].pos.y);
    triangleY2 = ZUN_MIN(quad[2].pos.y, triangleY2);
    triangleY2 = ZUN_MIN(quad[3].pos.y, triangleY2);
    return !(triangleX1 < viewportX || triangleY1 < viewportY ||
             triangleX2 > viewportX + viewportWidth ||
             triangleY2 > viewportY + viewportHeight);
}

bool PspItemTimeDrawPairQuadFinite(
    const VertexTex1DiffuseXyzrhw *quad)
{
    for (i32 vertex = 0; vertex < 4; ++vertex)
    {
        if (!std::isfinite(quad[vertex].pos.x) ||
            !std::isfinite(quad[vertex].pos.y) ||
            !std::isfinite(quad[vertex].pos.z) ||
            !std::isfinite(quad[vertex].w) ||
            !std::isfinite(quad[vertex].textureUV.x) ||
            !std::isfinite(quad[vertex].textureUV.y))
        {
            return false;
        }
    }
    return true;
}

bool PspItemTimeDrawPairQuadAxisAligned(
    const VertexTex1DiffuseXyzrhw *quad, bool requireDiffuse)
{
    const bool positions =
        PspItemTimeDrawPairFloatBitsEqual(quad[0].pos.x, quad[2].pos.x) &&
        PspItemTimeDrawPairFloatBitsEqual(quad[1].pos.x, quad[3].pos.x) &&
        PspItemTimeDrawPairFloatBitsEqual(quad[0].pos.y, quad[1].pos.y) &&
        PspItemTimeDrawPairFloatBitsEqual(quad[2].pos.y, quad[3].pos.y) &&
        PspItemTimeDrawPairFloatBitsEqual(quad[0].pos.z, quad[1].pos.z) &&
        PspItemTimeDrawPairFloatBitsEqual(quad[0].pos.z, quad[2].pos.z) &&
        PspItemTimeDrawPairFloatBitsEqual(quad[0].pos.z, quad[3].pos.z) &&
        quad[0].pos.x < quad[3].pos.x &&
        quad[0].pos.y < quad[3].pos.y;
    const bool uv =
        PspItemTimeDrawPairFloatBitsEqual(quad[0].textureUV.x,
                                          quad[2].textureUV.x) &&
        PspItemTimeDrawPairFloatBitsEqual(quad[1].textureUV.x,
                                          quad[3].textureUV.x) &&
        PspItemTimeDrawPairFloatBitsEqual(quad[0].textureUV.y,
                                          quad[1].textureUV.y) &&
        PspItemTimeDrawPairFloatBitsEqual(quad[2].textureUV.y,
                                          quad[3].textureUV.y);
    const bool w =
        PspItemTimeDrawPairFloatBitsEqual(quad[0].w, 1.0f) &&
        PspItemTimeDrawPairFloatBitsEqual(quad[1].w, 1.0f) &&
        PspItemTimeDrawPairFloatBitsEqual(quad[2].w, 1.0f) &&
        PspItemTimeDrawPairFloatBitsEqual(quad[3].w, 1.0f);
    const bool diffuse =
        !requireDiffuse ||
        (quad[0].diffuse == quad[1].diffuse &&
         quad[0].diffuse == quad[2].diffuse &&
         quad[0].diffuse == quad[3].diffuse);
    return positions && uv && w && diffuse;
}

void SetPspItemTimeDrawPairDiffuse(AnmManager *manager, AnmVm *vm,
                                   VertexTex1DiffuseXyzrhw *quad)
{
    ZunColor color;
    color.d3dColor = vm->flag17 ? vm->color2.d3dColor
                                : vm->color1.d3dColor;
    if (manager->useMixColor)
    {
        color.r = MixColors(color.r, manager->color.r);
        color.g = MixColors(color.g, manager->color.g);
        color.b = MixColors(color.b, manager->color.b);
        color.a = MixColors(color.a, manager->color.a);
    }
    quad[0].diffuse = quad[1].diffuse =
        quad[2].diffuse = quad[3].diffuse = color.d3dColor;
}

PspItemTimeDrawPairRejectReason BuildAndValidatePspItemTimeDrawPair(
    AnmManager *manager, AnmVm *vm, VertexTex1DiffuseXyzrhw *quad,
    bool *visible)
{
    if (manager == NULL || vm == NULL || visible == NULL ||
        !vm->IsVisible() || !vm->flag1 || vm->color1.a == 0)
    {
        return PSP_ITEM_TIME_DRAW_PAIR_REJECT_VISIBILITY;
    }
    if (!std::isfinite(vm->rotation.x) ||
        !std::isfinite(vm->rotation.y) ||
        !std::isfinite(vm->rotation.z) ||
        !std::isfinite(vm->scale.x) ||
        !std::isfinite(vm->scale.y) ||
        !std::isfinite(vm->spriteSize.x) ||
        !std::isfinite(vm->spriteSize.y) ||
        !std::isfinite(vm->pos.x) || !std::isfinite(vm->pos.y) ||
        !std::isfinite(vm->pos.z) ||
        !std::isfinite(vm->uvScrollPos.x) ||
        !std::isfinite(vm->uvScrollPos.y) ||
        !std::isfinite(manager->screenShakeOffset.x) ||
        !std::isfinite(manager->screenShakeOffset.y))
    {
        return PSP_ITEM_TIME_DRAW_PAIR_REJECT_NONFINITE;
    }
    if (vm->rotation.x != 0.0f || vm->rotation.y != 0.0f ||
        vm->rotation.z != 0.0f)
    {
        return PSP_ITEM_TIME_DRAW_PAIR_REJECT_ROTATION;
    }
    // TH07's hardware path established the same fail-closed rule: GU_SPRITES
    // may not replace mirrored or negative-extents quads.
    if (vm->scale.x <= 0.0f || vm->scale.y <= 0.0f ||
        vm->spriteSize.x <= 0.0f || vm->spriteSize.y <= 0.0f)
    {
        return PSP_ITEM_TIME_DRAW_PAIR_REJECT_SCALE;
    }
    if (vm->loadedSprite == NULL || vm->loadedSprite->texture == NULL)
        return PSP_ITEM_TIME_DRAW_PAIR_REJECT_TEXTURE;
    if (vm->blendMode != AnmBlendMode_Normal &&
        vm->blendMode != AnmBlendMode_Additive)
    {
        return PSP_ITEM_TIME_DRAW_PAIR_REJECT_STATE;
    }
    if (!std::isfinite(vm->loadedSprite->uvStart.x) ||
        !std::isfinite(vm->loadedSprite->uvStart.y) ||
        !std::isfinite(vm->loadedSprite->uvEnd.x) ||
        !std::isfinite(vm->loadedSprite->uvEnd.y) ||
        vm->loadedSprite->uvEnd.x < vm->loadedSprite->uvStart.x ||
        vm->loadedSprite->uvEnd.y < vm->loadedSprite->uvStart.y)
    {
        return PSP_ITEM_TIME_DRAW_PAIR_REJECT_NONFINITE;
    }

    BuildPspItemTimeDrawPairQuad(quad, vm, manager->screenShakeOffset);
    if (!PspItemTimeDrawPairQuadFinite(quad))
        return PSP_ITEM_TIME_DRAW_PAIR_REJECT_NONFINITE;
    *visible = PspItemTimeDrawPairQuadVisible(quad);
    if (*visible)
        SetPspItemTimeDrawPairDiffuse(manager, vm, quad);
    if (!PspItemTimeDrawPairQuadAxisAligned(quad, *visible))
        return PSP_ITEM_TIME_DRAW_PAIR_REJECT_AXIS;
    return PSP_ITEM_TIME_DRAW_PAIR_ACCEPT;
}

PspItemTimeDrawPairRejectReason PspSpritePairRunKeyMismatch(
    const AnmManager *manager, const AnmVm *vm)
{
    const PspSpritePairRunState &run =
        GetPspItemTimeDrawPairSidecar().run;
    if (!run.runActive)
        return PSP_ITEM_TIME_DRAW_PAIR_ACCEPT;
    if (run.texture != vm->loadedSprite->texture)
        return PSP_ITEM_TIME_DRAW_PAIR_REJECT_TEXTURE;
    if (run.blendMode != vm->blendMode ||
        run.zWriteDisabled != vm->zWriteDisabled ||
        run.useMixColor != static_cast<u8>(manager->useMixColor != 0) ||
        run.mixColor != manager->color.d3dColor ||
        run.depthTestDisabled !=
            static_cast<u8>(g_Supervisor.IsDepthTestDisabled() != 0))
    {
        return PSP_ITEM_TIME_DRAW_PAIR_REJECT_STATE;
    }
    return PSP_ITEM_TIME_DRAW_PAIR_ACCEPT;
}

void SetPspSpritePairRunKey(AnmManager *manager, AnmVm *vm)
{
    PspSpritePairRunState &run =
        GetPspItemTimeDrawPairSidecar().run;
    run.texture = vm->loadedSprite->texture;
    run.blendMode = static_cast<u8>(vm->blendMode);
    run.zWriteDisabled = static_cast<u8>(vm->zWriteDisabled);
    run.useMixColor = static_cast<u8>(manager->useMixColor != 0);
    run.mixColor = manager->color.d3dColor;
    run.depthTestDisabled =
        static_cast<u8>(g_Supervisor.IsDepthTestDisabled() != 0);
    run.representativeVm = vm;
}

#if defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) && \
    TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT
void HashPspItemTimeDrawPair(const AnmVm *vm,
                             const VertexTex1DiffuseXyzrhw *quad)
{
    ItemTimeDrawPairStats &stats =
        GetPspItemTimeDrawPairSidecar().stats;
    // The VM packs several state values as bit-fields; copy those values into
    // fixed-width objects before taking addresses for the byte-wise audit.
    const i32 anmFileIndex = vm->anmFileIndex;
    const i32 activeSpriteIndex = vm->activeSpriteIndex;
    const u8 blendMode = static_cast<u8>(vm->blendMode);
    const u8 zWriteDisabled = static_cast<u8>(vm->zWriteDisabled);
    const void *pieces[] = {
        &anmFileIndex,
        &activeSpriteIndex,
        &blendMode,
        &zWriteDisabled,
        &quad[0],
        &quad[3],
    };
    const u32 sizes[] = {
        sizeof(anmFileIndex),
        sizeof(activeSpriteIndex),
        sizeof(blendMode),
        sizeof(zWriteDisabled),
        sizeof(quad[0]),
        sizeof(quad[3]),
    };
    u32 hash = stats.semanticHash;
    for (u32 piece = 0; piece < ARRAY_SIZE(pieces); ++piece)
    {
        const u8 *bytes = static_cast<const u8 *>(pieces[piece]);
        for (u32 index = 0; index < sizes[piece]; ++index)
        {
            hash ^= bytes[index];
            hash *= kPspItemTimeDrawPairHashPrime;
        }
    }
    stats.semanticHash = hash;
}
#endif

#if TH08_PSP_ITEM_TIME_DRAW_PAIR_PRODUCT_ENABLED
bool PspSpritePairRunCanStore(const AnmManager *manager, u32 pairIndex)
{
    if (manager == NULL || pairIndex >= kPspItemTimeDrawPairCapacity)
        return false;
    const uintptr_t bufferBegin =
        reinterpret_cast<uintptr_t>(manager->vertexBuffer);
    const uintptr_t bufferEnd = reinterpret_cast<uintptr_t>(
        manager->vertexBuffer + ARRAY_SIZE(manager->vertexBuffer));
    const uintptr_t pairEnd = bufferBegin +
        static_cast<uintptr_t>(pairIndex + 1U) * 2U *
            sizeof(VertexTex1DiffuseXyzrhw);
    const uintptr_t pointerBegin = bufferEnd -
        static_cast<uintptr_t>(pairIndex + 1U) * sizeof(AnmVm *);
    return pairEnd <= pointerBegin;
}

void StorePspSpritePairRunVm(AnmManager *manager, u32 pairIndex,
                             AnmVm *vm)
{
    u8 *const bufferEnd = reinterpret_cast<u8 *>(
        manager->vertexBuffer + ARRAY_SIZE(manager->vertexBuffer));
    memcpy(bufferEnd - static_cast<size_t>(pairIndex + 1U) * sizeof(vm),
           &vm, sizeof(vm));
}

AnmVm *LoadPspSpritePairRunVm(AnmManager *manager, u32 pairIndex)
{
    AnmVm *vm = NULL;
    const u8 *const bufferEnd = reinterpret_cast<const u8 *>(
        manager->vertexBuffer + ARRAY_SIZE(manager->vertexBuffer));
    memcpy(&vm,
           bufferEnd - static_cast<size_t>(pairIndex + 1U) * sizeof(vm),
           sizeof(vm));
    return vm;
}
#endif

#if defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) && \
    TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT
void FinishPspItemTimeDrawPairAuditRun()
{
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
    PspSpritePairRunState &run = sidecar.run;
    if (run.auditRunPairs == 0U)
        return;
    ++sidecar.stats.compatibleRuns;
    ++run.passRuns;
    if (sidecar.stats.maxRunLength < run.auditRunPairs)
        sidecar.stats.maxRunLength = run.auditRunPairs;
    run.auditRunPairs = 0U;
    run.runActive = false;
    run.representativeVm = NULL;
    run.texture = NULL;
}
#endif

#if TH08_PSP_ITEM_TIME_DRAW_PAIR_PRODUCT_ENABLED
void FlushPspSpritePairRun(AnmManager *manager)
{
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
    PspSpritePairRunState &run = sidecar.run;
    if (!run.runActive || run.owner != PspSpritePairRunOwner::ItemTime ||
        run.pairCount == 0U)
    {
        return;
    }

    const u32 pairCount = run.pairCount;
    AnmVm *const representative = run.representativeVm;
    // A later canonical-culled ITEM_TIME can update the persistent scratch
    // without joining this visible run.  If PSPGL rejects, replaying retained
    // visible VMs must not leave that scratch rewound to the run's last VM.
    VertexTex1DiffuseXyzrhw preservedQuad[4];
    memcpy(preservedQuad, g_QuadVertices, sizeof(preservedQuad));
    ++sidecar.stats.compatibleRuns;
    ++run.passRuns;
    if (sidecar.stats.maxRunLength < pairCount)
        sidecar.stats.maxRunLength = pairCount;

    bool submitted = run.backendReady && manager != NULL &&
                     representative != NULL &&
                     g_Supervisor.d3dDevice != NULL;
    bool logicalStateCountAdded = false;
    if (submitted)
    {
        if (manager->currentTexture != run.texture)
        {
            manager->currentTexture = run.texture;
            manager->FlushVertexBuffer();
            g_Supervisor.d3dDevice->SetTexture(0, manager->currentTexture);
        }
        if (manager->currentVertexShader != 1)
        {
            manager->FlushVertexBuffer();
            manager->currentVertexShader = 1;
        }
        manager->SetRenderStateForVm(representative);
        logicalStateCountAdded = true;
        if (pairCount > 1U)
            manager->renderStateChangesThisFrame += pairCount - 1U;
        g_Supervisor.d3dDevice->SetTextureStageState(
            0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        g_Supervisor.d3dDevice->SetTextureStageState(
            0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        g_Supervisor.d3dDevice->SetVertexShader(
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        submitted = th08_psp_draw_sprite_pairs_in_place(
            g_Supervisor.d3dDevice, manager->vertexBuffer, pairCount,
            sizeof(VertexTex1DiffuseXyzrhw));
    }

    run.runActive = false;
    run.pairCount = 0U;
    run.representativeVm = NULL;
    run.texture = NULL;
    manager->spritesToDraw = 0U;
    manager->vertexBufferStartPtr =
        manager->vertexBufferEndPtr = manager->vertexBuffer;

    if (submitted)
    {
        ++sidecar.stats.submittedRuns;
        sidecar.stats.submittedPairs += pairCount;
        sidecar.stats.verticesSaved += pairCount * 4U;
        sidecar.stats.frontendBytesSaved +=
            pairCount * 4U * sizeof(VertexTex1DiffuseXyzrhw);
        sidecar.stats.backendBytesSaved += pairCount * 4U * 24U;
        ++manager->flushesThisFrame;
        return;
    }

    // The D3D-side backend guards reject before its consuming in-place pack.
    // PSPGL's owned transient copy may still fail after that pack, but it
    // returns before PRIM.  The independent VM pointer tail is untouched by
    // the shrinking front conversion, so restore the logical counter and
    // replay every retained VM through canonical Draw2D in linked-list order.
    if (logicalStateCountAdded &&
        manager->renderStateChangesThisFrame >= pairCount)
        manager->renderStateChangesThisFrame -= pairCount;
    sidecar.stats.canonicalDraws += pairCount;
    NotePspItemTimeDrawPairReject(
        PSP_ITEM_TIME_DRAW_PAIR_REJECT_BACKEND, pairCount);
    for (u32 pair = 0U; pair < pairCount; ++pair)
    {
        AnmVm *const vm = LoadPspSpritePairRunVm(manager, pair);
        if (vm != NULL)
            manager->Draw2D(vm);
    }
    memcpy(g_QuadVertices, preservedQuad, sizeof(preservedQuad));
}
#endif

void UpdatePspItemTimeDrawPairPassPeaks()
{
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
    PspSpritePairRunState &run = sidecar.run;
    bool peakChanged = false;
    if (sidecar.stats.peakCandidatesPerPass < run.passCandidates)
    {
        sidecar.stats.peakCandidatesPerPass = run.passCandidates;
        peakChanged = true;
    }
    if (sidecar.stats.peakVisiblePerPass < run.passVisible)
    {
        sidecar.stats.peakVisiblePerPass = run.passVisible;
        peakChanged = true;
    }
    if (sidecar.stats.peakEligiblePerPass < run.passEligible)
    {
        sidecar.stats.peakEligiblePerPass = run.passEligible;
        peakChanged = true;
    }
    if (sidecar.stats.peakRunsPerPass < run.passRuns)
    {
        sidecar.stats.peakRunsPerPass = run.passRuns;
        peakChanged = true;
    }
    if (peakChanged)
        sidecar.stats.peakStageFrame = g_GameManager.stageActiveFrames;
}
} // namespace

const ItemTimeDrawPairStats &GetItemTimeDrawPairStats()
{
    return GetPspItemTimeDrawPairSidecar().stats;
}

void AnmManager::ResetPspItemTimeDrawPairStats()
{
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
    // Cache/stat reset is not permission to erase another presentation
    // owner's staged vertices.  In particular, preserve Bullet stats and a
    // live Bullet run in combined AUDIT configurations.
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
    if (sidecar.run.owner != PspSpritePairRunOwner::None)
        ++sidecar.bulletStats.ownerConflictPasses;
#endif
    memset(&sidecar.cache, 0, sizeof(sidecar.cache));
    memset(&sidecar.stats, 0, sizeof(sidecar.stats));
    if (sidecar.run.owner == PspSpritePairRunOwner::None)
    {
        memset(&sidecar.run, 0, sizeof(sidecar.run));
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
        sidecar.rejectedItemPassDepth = 0U;
        sidecar.rejectedBulletPassDepth = 0U;
#endif
    }
    sidecar.stats.peakStageFrame = 0xffffffffU;
    sidecar.stats.semanticHash = kPspItemTimeDrawPairHashOffset;
}

PspItemTimeDrawPairRejectReason
AnmManager::PspValidateItemTimeDrawPairIdentity(
    const AnmVm *vm, AnmLoaded *owner, i32 expectedSpriteIndex)
{
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
    PspItemTimeDrawPairCache &cache = sidecar.cache;
    if (vm == NULL || owner == NULL || owner->anmIdx < 0)
        return PSP_ITEM_TIME_DRAW_PAIR_REJECT_OWNER;
    if (expectedSpriteIndex != kPspItemTimeDrawPairInitialSpriteIndex &&
        expectedSpriteIndex != kPspItemTimeDrawPairIndicatorSpriteIndex)
    {
        return PSP_ITEM_TIME_DRAW_PAIR_REJECT_SPRITE;
    }

    const u32 generation =
        owner->PspLoadGenerationForItemTimeSpawnInit();
    if (generation == 0U ||
        !owner->PspLoadReadyForItemTimeSpawnInit(generation))
    {
        return PSP_ITEM_TIME_DRAW_PAIR_REJECT_LOAD;
    }

    const bool cacheMatch = cache.owner == owner &&
                            cache.generation == generation;
    if (cacheMatch)
    {
        if (cache.rawData != owner->rawData ||
            cache.scripts != owner->scripts ||
            cache.sprites != owner->sprites || cache.anmIdx != owner->anmIdx)
        {
            ++sidecar.stats.cacheValidationFailures;
            return PSP_ITEM_TIME_DRAW_PAIR_REJECT_LOAD;
        }
        if (cache.validation ==
            PSP_ITEM_TIME_DRAW_PAIR_CACHE_RANGE_INVALID)
        {
            return PSP_ITEM_TIME_DRAW_PAIR_REJECT_LOAD;
        }
        if (cache.validation ==
            PSP_ITEM_TIME_DRAW_PAIR_CACHE_FINGERPRINT_INVALID)
        {
            return PSP_ITEM_TIME_DRAW_PAIR_REJECT_SCRIPT;
        }
        if (cache.validation != PSP_ITEM_TIME_DRAW_PAIR_CACHE_VALID ||
            owner->scripts[kPspItemTimeDrawPairScriptIndex] != cache.script68)
        {
            ++sidecar.stats.cacheValidationFailures;
            return PSP_ITEM_TIME_DRAW_PAIR_REJECT_SCRIPT;
        }
        ++sidecar.stats.cacheHits;
    }
    else
    {
        if (cache.owner == owner && cache.generation != 0U &&
            cache.generation != generation)
        {
            ++sidecar.stats.cacheGenerationChanges;
        }
        ++sidecar.stats.cacheRevalidations;
        memset(&cache, 0, sizeof(cache));
        cache.owner = owner;
        cache.rawData = owner->rawData;
        cache.scripts = owner->scripts;
        cache.sprites = owner->sprites;
        cache.generation = generation;
        cache.anmIdx = owner->anmIdx;

        if (!owner->PspItemTimeSpawnInitTablesContain(
                generation, kPspItemTimeDrawPairScriptIndex,
                kPspItemTimeDrawPairInitialSpriteIndex) ||
            !owner->PspItemTimeSpawnInitTablesContain(
                generation, kPspItemTimeDrawPairScriptIndex,
                kPspItemTimeDrawPairIndicatorSpriteIndex))
        {
            cache.validation = PSP_ITEM_TIME_DRAW_PAIR_CACHE_RANGE_INVALID;
            ++sidecar.stats.cacheValidationFailures;
            return PSP_ITEM_TIME_DRAW_PAIR_REJECT_LOAD;
        }
        cache.script68 = owner->scripts[kPspItemTimeDrawPairScriptIndex];
        if (cache.script68 == NULL ||
            !owner->PspItemTimeSpawnInitScriptRangeContains(
                generation, cache.script68,
                kPspItemTimeDrawPairFingerprintBytes))
        {
            cache.validation = PSP_ITEM_TIME_DRAW_PAIR_CACHE_RANGE_INVALID;
            ++sidecar.stats.cacheValidationFailures;
            return PSP_ITEM_TIME_DRAW_PAIR_REJECT_LOAD;
        }
        if (!ValidatePspItemTimeDrawPairScript68(cache.script68))
        {
            cache.validation =
                PSP_ITEM_TIME_DRAW_PAIR_CACHE_FINGERPRINT_INVALID;
            ++sidecar.stats.cacheValidationFailures;
            return PSP_ITEM_TIME_DRAW_PAIR_REJECT_SCRIPT;
        }
        cache.validation = PSP_ITEM_TIME_DRAW_PAIR_CACHE_VALID;
    }

    if (vm->anmFile != owner || vm->anmFileIndex != owner->anmIdx ||
        vm->scriptIndex != kPspItemTimeDrawPairScriptIndex ||
        vm->beginningOfScript != cache.script68)
    {
        return PSP_ITEM_TIME_DRAW_PAIR_REJECT_SCRIPT;
    }
    if (vm->activeSpriteIndex != expectedSpriteIndex ||
        vm->loadedSprite != &owner->sprites[expectedSpriteIndex])
    {
        return PSP_ITEM_TIME_DRAW_PAIR_REJECT_SPRITE;
    }
    return PSP_ITEM_TIME_DRAW_PAIR_ACCEPT;
}

void AnmManager::BeginPspItemTimeDrawPairPass()
{
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
    if (sidecar.run.owner != PspSpritePairRunOwner::None)
    {
        if (sidecar.run.owner == PspSpritePairRunOwner::ItemTime)
            this->PspItemTimeDrawPairBoundary();
        // A Bullet owner is submitted through its active 2V/4V decoder;
        // canonical geometry left by an Item audit owner is submitted by the
        // same generic call. No old representation may remain in the front.
        this->FlushVertexBuffer();
#if defined(TH08_PSP_BULLET_DIRECT_GE) && TH08_PSP_BULLET_DIRECT_GE
        th08_psp_bullet_direct_ge_set_batch(false);
#endif
        g_PspBulletUnifiedQuadBatchActive = false;
        g_PspUnifiedQuadBatchOwner = PspUnifiedQuadBatchOwner::None;
        ++sidecar.bulletStats.ownerConflictPasses;
        ++sidecar.stats.passes;
        ++sidecar.rejectedItemPassDepth;
        return;
    }
#endif
#if TH08_PSP_ITEM_TIME_DRAW_PAIR_PRODUCT_ENABLED
    FlushPspSpritePairRun(this);
#else
    FinishPspItemTimeDrawPairAuditRun();
#endif
    memset(&sidecar.run, 0, sizeof(sidecar.run));
    sidecar.run.owner = PspSpritePairRunOwner::ItemTime;
    sidecar.run.passActive = true;
    sidecar.run.backendReady =
        g_Supervisor.d3dDevice != NULL ? 1U : 0U;
#if defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
    TH08_PSP_BULLET_UNIFIED_QUADS
    if (g_PspBulletUnifiedQuadBatchActive)
        sidecar.run.backendReady = 0U;
#endif
    ++sidecar.stats.passes;
}

void AnmManager::EndPspItemTimeDrawPairPass()
{
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
    if (sidecar.rejectedItemPassDepth != 0U)
    {
        // Canonical geometry from the isolated Item pass must not be mistaken
        // for a resumed Bullet pair/quad run.
        this->FlushVertexBuffer();
        --sidecar.rejectedItemPassDepth;
        // The shared helper is the sole restoration site for
        // g_PspBulletUnifiedQuadBatchActive = true, and also requires
        // sidecar.rejectedBulletPassDepth == 0U.
        RestorePspMixedQuadOwnerTokensIfQuiescent();
        return;
    }
    if (sidecar.run.owner != PspSpritePairRunOwner::ItemTime)
        return;
#endif
#if TH08_PSP_ITEM_TIME_DRAW_PAIR_PRODUCT_ENABLED
    FlushPspSpritePairRun(this);
#else
    FinishPspItemTimeDrawPairAuditRun();
#endif
    UpdatePspItemTimeDrawPairPassPeaks();
    sidecar.run.passActive = false;
    sidecar.run.owner = PspSpritePairRunOwner::None;
}

void AnmManager::PspItemTimeDrawPairBoundary()
{
#if TH08_PSP_ITEM_TIME_DRAW_PAIR_PRODUCT_ENABLED
    FlushPspSpritePairRun(this);
#else
    FinishPspItemTimeDrawPairAuditRun();
#endif
}

#if defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) && \
    TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT
ZunResult AnmManager::DrawPspItemTimePairAudit(
    AnmVm *vm, PspItemTimeDrawPairRejectReason identityReason)
{
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
    PspSpritePairRunState &run = sidecar.run;
    ++sidecar.stats.candidates;
    ++sidecar.stats.canonicalDraws;
    ++run.passCandidates;

    const ZunResult result = this->Draw2D(vm);
    if (identityReason != PSP_ITEM_TIME_DRAW_PAIR_ACCEPT)
    {
        FinishPspItemTimeDrawPairAuditRun();
        NotePspItemTimeDrawPairReject(identityReason);
        return result;
    }

    VertexTex1DiffuseXyzrhw candidate[4];
    bool visible = false;
    PspItemTimeDrawPairRejectReason reason =
        BuildAndValidatePspItemTimeDrawPair(this, vm, candidate, &visible);
    if (reason != PSP_ITEM_TIME_DRAW_PAIR_ACCEPT)
    {
        FinishPspItemTimeDrawPairAuditRun();
        NotePspItemTimeDrawPairReject(reason);
        return result;
    }
    if (!visible)
    {
        ++sidecar.stats.culledCandidates;
        // DrawInner returns before writing diffuse or render state when the
        // rounded quad misses the viewport.  Prove the entire persistent
        // scratch image anyway: Build copied its previous diffuse/W bytes and
        // then reproduced only the canonical position/UV writes.
        if (memcmp(candidate, g_QuadVertices, sizeof(candidate)) != 0)
        {
            ++sidecar.stats.endpointMismatches;
            FinishPspItemTimeDrawPairAuditRun();
            NotePspItemTimeDrawPairReject(
                PSP_ITEM_TIME_DRAW_PAIR_REJECT_ENDPOINT);
        }
        else
        {
            ++sidecar.stats.endpointMatches;
        }
        return result;
    }
    ++sidecar.stats.visibleCandidates;
    ++run.passVisible;

    if (!PspItemTimeDrawPairQuadAxisAligned(g_QuadVertices, true))
    {
        FinishPspItemTimeDrawPairAuditRun();
        NotePspItemTimeDrawPairReject(
            PSP_ITEM_TIME_DRAW_PAIR_REJECT_AXIS);
        return result;
    }
    const bool endpointsMatch =
        memcmp(&candidate[0], &g_QuadVertices[0],
               sizeof(candidate[0])) == 0 &&
        memcmp(&candidate[3], &g_QuadVertices[3],
               sizeof(candidate[3])) == 0;
    if (!endpointsMatch)
    {
        ++sidecar.stats.endpointMismatches;
        FinishPspItemTimeDrawPairAuditRun();
        NotePspItemTimeDrawPairReject(
            PSP_ITEM_TIME_DRAW_PAIR_REJECT_ENDPOINT);
        return result;
    }
    ++sidecar.stats.endpointMatches;

    reason = PspSpritePairRunKeyMismatch(this, vm);
    if (reason != PSP_ITEM_TIME_DRAW_PAIR_ACCEPT)
    {
        FinishPspItemTimeDrawPairAuditRun();
        NotePspItemTimeDrawPairReject(reason);
        return result;
    }
    if (!run.runActive)
    {
        run.runActive = true;
        SetPspSpritePairRunKey(this, vm);
    }
    ++run.auditRunPairs;
    ++sidecar.stats.eligiblePairs;
    ++run.passEligible;
    sidecar.stats.verticesSaved += 4U;
    sidecar.stats.frontendBytesSaved +=
        4U * sizeof(VertexTex1DiffuseXyzrhw);
    sidecar.stats.backendBytesSaved += 4U * 24U;
    HashPspItemTimeDrawPair(vm, candidate);
    return result;
}
#endif

#if TH08_PSP_ITEM_TIME_DRAW_PAIR_PRODUCT_ENABLED
bool AnmManager::TryDrawPspItemTimeSpritePair(
    AnmVm *vm, PspItemTimeDrawPairRejectReason identityReason)
{
    PspItemTimeDrawPairSidecar &sidecar =
        GetPspItemTimeDrawPairSidecar();
    PspSpritePairRunState &run = sidecar.run;
    ++sidecar.stats.candidates;
    ++run.passCandidates;

    if (!run.passActive || run.owner != PspSpritePairRunOwner::ItemTime ||
        !run.backendReady)
    {
        FlushPspSpritePairRun(this);
        ++sidecar.stats.canonicalDraws;
        NotePspItemTimeDrawPairReject(
            PSP_ITEM_TIME_DRAW_PAIR_REJECT_BACKEND);
        return false;
    }
    if (identityReason != PSP_ITEM_TIME_DRAW_PAIR_ACCEPT)
    {
        FlushPspSpritePairRun(this);
        ++sidecar.stats.canonicalDraws;
        NotePspItemTimeDrawPairReject(identityReason);
        return false;
    }

    VertexTex1DiffuseXyzrhw quad[4];
    bool visible = false;
    PspItemTimeDrawPairRejectReason reason =
        BuildAndValidatePspItemTimeDrawPair(this, vm, quad, &visible);
    if (reason != PSP_ITEM_TIME_DRAW_PAIR_ACCEPT)
    {
        FlushPspSpritePairRun(this);
        ++sidecar.stats.canonicalDraws;
        NotePspItemTimeDrawPairReject(reason);
        return false;
    }

    // Match canonical scratch state even when the exact cull means no GE
    // primitive and therefore does not split an otherwise compatible run.
    memcpy(g_QuadVertices, quad, sizeof(quad));
    if (!visible)
    {
        ++sidecar.stats.culledCandidates;
        return true;
    }
    ++sidecar.stats.visibleCandidates;
    ++run.passVisible;

    reason = PspSpritePairRunKeyMismatch(this, vm);
    if (reason != PSP_ITEM_TIME_DRAW_PAIR_ACCEPT)
    {
        FlushPspSpritePairRun(this);
        ++sidecar.stats.canonicalDraws;
        NotePspItemTimeDrawPairReject(reason);
        return false;
    }

    if (!run.runActive)
    {
        this->FlushVertexBuffer();
        this->spritesToDraw = 0U;
        this->vertexBufferStartPtr =
            this->vertexBufferEndPtr = this->vertexBuffer;
        run.runActive = true;
        SetPspSpritePairRunKey(this, vm);
    }
    if (!PspSpritePairRunCanStore(this, run.pairCount))
    {
        FlushPspSpritePairRun(this);
        ++sidecar.stats.canonicalDraws;
        NotePspItemTimeDrawPairReject(
            PSP_ITEM_TIME_DRAW_PAIR_REJECT_CAPACITY);
        return false;
    }

    VertexTex1DiffuseXyzrhw *const pair =
        this->vertexBuffer + run.pairCount * 2U;
    pair[0] = quad[0];
    pair[1] = quad[3];
    StorePspSpritePairRunVm(this, run.pairCount, vm);
    ++run.pairCount;
    this->vertexBufferEndPtr =
        this->vertexBuffer + run.pairCount * 2U;
    ++sidecar.stats.eligiblePairs;
    ++run.passEligible;
    return true;
}
#endif
#endif

#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_BATCH) && \
    TH08_PSP_ASCII_POPUP_BATCH
namespace
{
ZunResult RejectPspAsciiPopupBatch()
{
    th08::psp::RenderPerfNoteAsciiPopupBatchFallback();
    return ZUN_ERROR;
}

i32 PspAsciiPopupSpriteIndex(const AsciiManagerPopup &popup, u8 digit)
{
    i32 spriteIndex = digit;
    // TH08 applies the transition offset to glyph 10 as well.  TH07 did not;
    // carrying that exception across would change the PSP score animation.
    if (popup.timer.current >= 52)
        spriteIndex += popup.timer.current < 56 ? 11 : 21;
    return spriteIndex;
}

void BuildPspAsciiPopupQuad(VertexTex1DiffuseXyzrhw *quad, const AnmVm *vm,
                            const AnmLoadedSprite *sprite, f32 widthPx,
                            f32 scaleX, f32 scaleY,
                            const Float2 &screenShake)
{
    // Keep the retail expression order from DrawNoRotation and DrawInner.
    // In particular, screen shake is added before nearbyintf and the -0.5f
    // pixel-center correction; negative and half-way coordinates depend on it.
    const f32 spriteHalfWidth = (widthPx * scaleX) / 2.0f;
    const f32 spriteHalfHeight = (vm->spriteSize.y * scaleY) / 2.0f;

    if ((vm->anchor & 1) == 0)
    {
        quad[0].pos.x = quad[2].pos.x = vm->pos.x - spriteHalfWidth;
        quad[1].pos.x = quad[3].pos.x = spriteHalfWidth + vm->pos.x;
    }
    else
    {
        quad[0].pos.x = quad[2].pos.x = vm->pos.x;
        quad[1].pos.x = quad[3].pos.x =
            spriteHalfWidth + vm->pos.x + spriteHalfWidth;
    }

    if ((vm->anchor & 2) == 0)
    {
        quad[0].pos.y = quad[1].pos.y = vm->pos.y - spriteHalfHeight;
        quad[2].pos.y = quad[3].pos.y = spriteHalfHeight + vm->pos.y;
    }
    else
    {
        quad[0].pos.y = quad[1].pos.y = vm->pos.y;
        quad[2].pos.y = quad[3].pos.y =
            spriteHalfHeight + vm->pos.y + spriteHalfHeight;
    }

    quad[0].pos.z = quad[1].pos.z = quad[2].pos.z = quad[3].pos.z = vm->pos.z;
    quad[0].pos.x += screenShake.x;
    quad[0].pos.y += screenShake.y;
    quad[1].pos.x += screenShake.x;
    quad[1].pos.y += screenShake.y;
    quad[2].pos.x += screenShake.x;
    quad[2].pos.y += screenShake.y;
    quad[3].pos.x += screenShake.x;
    quad[3].pos.y += screenShake.y;

    const f32 triangleX1 = nearbyintf(quad[0].pos.x) - g_ZeroPointFive;
    const f32 triangleX2 = nearbyintf(quad[1].pos.x) - g_ZeroPointFive;
    const f32 triangleY1 = nearbyintf(quad[0].pos.y) - g_ZeroPointFive;
    const f32 triangleY2 = nearbyintf(quad[2].pos.y) - g_ZeroPointFive;
    quad[2].pos.y = quad[3].pos.y = triangleY2;
    quad[0].pos.y = quad[1].pos.y = triangleY1;
    quad[1].pos.x = quad[3].pos.x = triangleX2;
    quad[0].pos.x = quad[2].pos.x = triangleX1;

    quad[0].textureUV.x = quad[2].textureUV.x =
        sprite->uvStart.x + vm->uvScrollPos.x;
    quad[1].textureUV.x = quad[3].textureUV.x =
        sprite->uvEnd.x + vm->uvScrollPos.x;
    quad[0].textureUV.y = quad[1].textureUV.y =
        sprite->uvStart.y + vm->uvScrollPos.y;
    quad[2].textureUV.y = quad[3].textureUV.y =
        sprite->uvEnd.y + vm->uvScrollPos.y;
}

bool PspAsciiPopupQuadVisible(const VertexTex1DiffuseXyzrhw *quad)
{
    f32 triangleX1 = ZUN_MAX(quad[0].pos.x, quad[1].pos.x);
    triangleX1 = ZUN_MAX(quad[2].pos.x, triangleX1);
    triangleX1 = ZUN_MAX(quad[3].pos.x, triangleX1);
    f32 triangleY1 = ZUN_MAX(quad[0].pos.y, quad[1].pos.y);
    triangleY1 = ZUN_MAX(quad[2].pos.y, triangleY1);
    triangleY1 = ZUN_MAX(quad[3].pos.y, triangleY1);
    f32 triangleX2 = ZUN_MIN(quad[0].pos.x, quad[1].pos.x);
    triangleX2 = ZUN_MIN(quad[2].pos.x, triangleX2);
    triangleX2 = ZUN_MIN(quad[3].pos.x, triangleX2);
    f32 triangleY2 = ZUN_MIN(quad[0].pos.y, quad[1].pos.y);
    triangleY2 = ZUN_MIN(quad[2].pos.y, triangleY2);
    triangleY2 = ZUN_MIN(quad[3].pos.y, triangleY2);

    // Preserve DrawInner's inclusive viewport edges exactly.  The right and
    // bottom sums are formed in the viewport's integer domain before the one
    // float conversion, matching the canonical expression order.
    const f32 viewportLeft = static_cast<f32>(g_Supervisor.viewport.X);
    const f32 viewportTop = static_cast<f32>(g_Supervisor.viewport.Y);
    const f32 viewportRight = static_cast<f32>(
        g_Supervisor.viewport.X + g_Supervisor.viewport.Width);
    const f32 viewportBottom = static_cast<f32>(
        g_Supervisor.viewport.Y + g_Supervisor.viewport.Height);
    return !(triangleX1 < viewportLeft || triangleY1 < viewportTop ||
             triangleX2 > viewportRight || triangleY2 > viewportBottom);
}

bool PspAsciiPopupQuadFinite(const VertexTex1DiffuseXyzrhw *quad)
{
    for (i32 vertex = 0; vertex < 4; ++vertex)
    {
        if (!std::isfinite(quad[vertex].pos.x) ||
            !std::isfinite(quad[vertex].pos.y) ||
            !std::isfinite(quad[vertex].pos.z) ||
            !std::isfinite(quad[vertex].textureUV.x) ||
            !std::isfinite(quad[vertex].textureUV.y))
        {
            return false;
        }
    }
    return true;
}

#if defined(TH08_PSP_ASCII_POPUP_DIRECT_PAIR) && \
    TH08_PSP_ASCII_POPUP_DIRECT_PAIR
constexpr i32 kPspAsciiPopupDirectSpriteCount = 32;
constexpr f32 kPspAsciiPopupDirectSafeMagnitude = 1.0e30f;

struct PspAsciiPopupDirectSprite
{
    AnmLoadedSprite *sprite;
    f32 halfWidth;
    f32 uvLeft;
    f32 uvRight;
    f32 uvTop;
    f32 uvBottom;
};

bool PspAsciiPopupDirectMagnitudeSafe(f32 value)
{
    return std::isfinite(value) &&
           std::fabs(value) <= kPspAsciiPopupDirectSafeMagnitude;
}

void BuildPspAsciiPopupDirectPair(
    VertexTex1DiffuseXyzrhw *pair, const AnmVm *vm,
    const PspAsciiPopupDirectSprite &sprite, f32 roundedTop,
    f32 roundedBottom, f32 screenShakeX, f32 w0, f32 w3)
{
    // Preserve BuildPspAsciiPopupQuad's binary32 expression order.  Only the
    // redundant top-right/bottom-left corners are omitted.
    f32 rawLeft;
    f32 rawRight;
    if ((vm->anchor & 1) == 0)
    {
        rawLeft = vm->pos.x - sprite.halfWidth;
        rawRight = sprite.halfWidth + vm->pos.x;
    }
    else
    {
        rawLeft = vm->pos.x;
        rawRight = sprite.halfWidth + vm->pos.x + sprite.halfWidth;
    }

    pair[0].pos.x = nearbyintf(rawLeft + screenShakeX) - g_ZeroPointFive;
    pair[1].pos.x = nearbyintf(rawRight + screenShakeX) - g_ZeroPointFive;
    pair[0].pos.y = roundedTop;
    pair[1].pos.y = roundedBottom;
    pair[0].pos.z = pair[1].pos.z = vm->pos.z;
    // DrawNoRotation/DrawInner never write RHW.  Retain the same two global
    // corner values the accepted batch copied after canonical construction.
    pair[0].w = w0;
    pair[1].w = w3;
    pair[0].textureUV.x = sprite.uvLeft;
    pair[0].textureUV.y = sprite.uvTop;
    pair[1].textureUV.x = sprite.uvRight;
    pair[1].textureUV.y = sprite.uvBottom;
}

bool PspAsciiPopupDirectPairVisible(
    const VertexTex1DiffuseXyzrhw *pair)
{
    // Positive, finite scale/size is proven before generation, so corner 0
    // is the exact min and corner 1 is the exact max used by DrawInner.
    const f32 viewportLeft = static_cast<f32>(g_Supervisor.viewport.X);
    const f32 viewportTop = static_cast<f32>(g_Supervisor.viewport.Y);
    const f32 viewportRight = static_cast<f32>(
        g_Supervisor.viewport.X + g_Supervisor.viewport.Width);
    const f32 viewportBottom = static_cast<f32>(
        g_Supervisor.viewport.Y + g_Supervisor.viewport.Height);
    return !(pair[1].pos.x < viewportLeft ||
             pair[1].pos.y < viewportTop ||
             pair[0].pos.x > viewportRight ||
             pair[0].pos.y > viewportBottom);
}

void CommitPspAsciiPopupDirectFinalQuad(
    const VertexTex1DiffuseXyzrhw *lastPair, bool hasVisibleColor,
    D3DCOLOR lastVisibleColor)
{
    // The canonical loop leaves geometry/UV from its final digit, but leaves
    // diffuse at the final *visible* digit (or untouched when all are culled).
    // RHW is never written by DrawNoRotation/DrawInner and remains untouched.
    g_QuadVertices[0].pos = lastPair[0].pos;
    g_QuadVertices[1].pos.x = lastPair[1].pos.x;
    g_QuadVertices[1].pos.y = lastPair[0].pos.y;
    g_QuadVertices[1].pos.z = lastPair[1].pos.z;
    g_QuadVertices[2].pos.x = lastPair[0].pos.x;
    g_QuadVertices[2].pos.y = lastPair[1].pos.y;
    g_QuadVertices[2].pos.z = lastPair[0].pos.z;
    g_QuadVertices[3].pos = lastPair[1].pos;

    g_QuadVertices[0].textureUV = lastPair[0].textureUV;
    g_QuadVertices[1].textureUV.x = lastPair[1].textureUV.x;
    g_QuadVertices[1].textureUV.y = lastPair[0].textureUV.y;
    g_QuadVertices[2].textureUV.x = lastPair[0].textureUV.x;
    g_QuadVertices[2].textureUV.y = lastPair[1].textureUV.y;
    g_QuadVertices[3].textureUV = lastPair[1].textureUV;

    if (hasVisibleColor)
    {
        g_QuadVertices[0].diffuse = lastVisibleColor;
        g_QuadVertices[1].diffuse = lastVisibleColor;
        g_QuadVertices[2].diffuse = lastVisibleColor;
        g_QuadVertices[3].diffuse = lastVisibleColor;
    }
}
#endif
} // namespace

ZunResult AnmManager::DrawPspAsciiPopupBatch(
    AnmVm *vm, AnmLoaded *asciiAnm, AsciiManagerPopup *popups,
    i32 popupCount, f32 playerX, f32 playerY, f32 popupScaleX,
    f32 popupScaleY)
{
    constexpr i32 kScorePopupCount =
        ASCII_MAX_SCORE_POPUPS + ASCII_MAX_PLAYER_POPUPS;
    // This experiment deliberately cannot consume the separate time-popup
    // array.  Score/time ordering and their independent VMs stay canonical.
    if (vm != &g_AsciiManager.smallScoreText ||
        asciiAnm != g_AsciiManager.asciiAnm ||
        popups != g_AsciiManager.scorePopups ||
        popupCount != kScorePopupCount || asciiAnm == NULL ||
        asciiAnm->sprites == NULL || g_Supervisor.d3dDevice == NULL ||
        !vm->IsVisible() || !vm->flag1 ||
        !std::isfinite(playerX) || !std::isfinite(playerY) ||
        !std::isfinite(popupScaleX) || popupScaleX <= 0.0f ||
        !std::isfinite(popupScaleY) || popupScaleY <= 0.0f ||
        !std::isfinite(vm->spriteSize.y) || vm->spriteSize.y <= 0.0f ||
        !std::isfinite(vm->pos.z) ||
        !std::isfinite(vm->uvScrollPos.x) ||
        !std::isfinite(vm->uvScrollPos.y) ||
        !std::isfinite(this->screenShakeOffset.x) ||
        !std::isfinite(this->screenShakeOffset.y))
    {
        return RejectPspAsciiPopupBatch();
    }
#if defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
    TH08_PSP_BULLET_UNIFIED_QUADS
    if (g_PspBulletUnifiedQuadBatchActive)
        return RejectPspAsciiPopupBatch();
#endif

    u32 digitCount = 0U;
    u32 visibleCount = 0U;
    IDirect3DTexture8 *batchTexture = NULL;

#if defined(TH08_PSP_ASCII_POPUP_DIRECT_PAIR) && \
    TH08_PSP_ASCII_POPUP_DIRECT_PAIR
    // Metadata-only pass.  Every failure occurs before VM, renderer and the
    // shared quad are touched.  The 32-entry table is the complete image of
    // digit 0..10 plus TH08's +11/+21 transition mapping, and lives only for
    // this call so it cannot outlive the authoritative ANM owner.
    PspAsciiPopupDirectSprite directSprites[
        kPspAsciiPopupDirectSpriteCount] = {};
    bool directSpriteValidated[kPspAsciiPopupDirectSpriteCount] = {};
    u32 activePopupCount = 0U;
    const f32 spriteHalfHeight =
        (vm->spriteSize.y * popupScaleY) / 2.0f;
    if (!PspAsciiPopupDirectMagnitudeSafe(spriteHalfHeight) ||
        !PspAsciiPopupDirectMagnitudeSafe(this->screenShakeOffset.x) ||
        !PspAsciiPopupDirectMagnitudeSafe(this->screenShakeOffset.y))
    {
        return RejectPspAsciiPopupBatch();
    }

    for (i32 popupIndex = 0; popupIndex < popupCount; ++popupIndex)
    {
        const AsciiManagerPopup &popup = popups[popupIndex];
        if (!popup.inUse)
            continue;
        if (popup.characterCount == 0U ||
            popup.characterCount > sizeof(popup.text) ||
            !PspAsciiPopupDirectMagnitudeSafe(popup.position.x) ||
            !PspAsciiPopupDirectMagnitudeSafe(popup.position.y))
        {
            return RejectPspAsciiPopupBatch();
        }

        const f32 dx = playerX - popup.position.x;
        const f32 dy = playerY - popup.position.y;
        const f32 distanceSquared = dx * dx + dy * dy;
        if (!std::isfinite(distanceSquared) || distanceSquared < 0.0f ||
            distanceSquared > 2147483520.0f)
        {
            return RejectPspAsciiPopupBatch();
        }

        const f32 firstX =
            popup.position.x - static_cast<f32>(popup.characterCount * 4);
        if (!PspAsciiPopupDirectMagnitudeSafe(firstX))
            return RejectPspAsciiPopupBatch();

        ++activePopupCount;
        const u8 *digit = reinterpret_cast<const u8 *>(
            &popup.text[popup.characterCount - 1]);
        for (i32 remaining = popup.characterCount; remaining > 0;
             --remaining, --digit)
        {
            if (*digit > 10U)
                return RejectPspAsciiPopupBatch();
            const i32 spriteIndex = PspAsciiPopupSpriteIndex(popup, *digit);
            if (spriteIndex < 0 ||
                spriteIndex >= kPspAsciiPopupDirectSpriteCount)
            {
                return RejectPspAsciiPopupBatch();
            }

            if (!directSpriteValidated[spriteIndex])
            {
                AnmLoadedSprite *const sprite =
                    asciiAnm->GetSprite(spriteIndex);
                if (sprite == NULL || sprite->texture == NULL ||
                    (batchTexture != NULL &&
                     batchTexture != sprite->texture) ||
                    !std::isfinite(sprite->widthPx) ||
                    sprite->widthPx <= 0.0f ||
                    !std::isfinite(sprite->uvStart.x) ||
                    !std::isfinite(sprite->uvStart.y) ||
                    !std::isfinite(sprite->uvEnd.x) ||
                    !std::isfinite(sprite->uvEnd.y) ||
                    sprite->uvEnd.x < sprite->uvStart.x ||
                    sprite->uvEnd.y < sprite->uvStart.y)
                {
                    return RejectPspAsciiPopupBatch();
                }

                PspAsciiPopupDirectSprite &cached =
                    directSprites[spriteIndex];
                cached.sprite = sprite;
                cached.halfWidth =
                    (sprite->widthPx * popupScaleX) / 2.0f;
                cached.uvLeft = sprite->uvStart.x + vm->uvScrollPos.x;
                cached.uvRight = sprite->uvEnd.x + vm->uvScrollPos.x;
                cached.uvTop = sprite->uvStart.y + vm->uvScrollPos.y;
                cached.uvBottom = sprite->uvEnd.y + vm->uvScrollPos.y;
                if (!PspAsciiPopupDirectMagnitudeSafe(cached.halfWidth) ||
                    !std::isfinite(cached.uvLeft) ||
                    !std::isfinite(cached.uvRight) ||
                    !std::isfinite(cached.uvTop) ||
                    !std::isfinite(cached.uvBottom))
                {
                    return RejectPspAsciiPopupBatch();
                }

                batchTexture = sprite->texture;
                directSpriteValidated[spriteIndex] = true;
            }
            ++digitCount;
        }
    }

    if (digitCount == 0U)
        return ZUN_SUCCESS;
    // Reserve the worst-case visible count before VM mutation.  This replaces
    // the old validation geometry/cull pass; scorePopups' fixed 723x12 bound
    // is far below AnmManager's existing 0x18000-vertex staging capacity.
    if (digitCount > ARRAY_SIZE(this->vertexBuffer) / 2U ||
        !th08_psp_reserve_ascii_popup_sprite_pairs(
            g_Supervisor.d3dDevice, digitCount))
    {
        return RejectPspAsciiPopupBatch();
    }

    const f32 preservedW0 = g_QuadVertices[0].w;
    const f32 preservedW3 = g_QuadVertices[3].w;
    VertexTex1DiffuseXyzrhw lastPair[2] = {
        g_QuadVertices[0], g_QuadVertices[3],
    };
    bool hasVisibleColor = false;
    D3DCOLOR lastVisibleColor = g_QuadVertices[0].diffuse;

    for (i32 popupIndex = 0; popupIndex < popupCount; ++popupIndex)
    {
        AsciiManagerPopup &popup = popups[popupIndex];
        if (!popup.inUse)
            continue;
        th08::psp::RenderPerfNotePopup(popup.characterCount);

        vm->pos.x =
            popup.position.x - static_cast<f32>(popup.characterCount * 4);
        vm->pos.y = popup.position.y;
        vm->color1.d3dColor = popup.color;

        f32 dx = playerX - popup.position.x;
        f32 dy = playerY - popup.position.y;
        i32 alpha = static_cast<i32>(dx * dx + dy * dy);
        if (alpha > 4096)
            alpha = 208;
        else if (alpha > 1024)
            alpha = ((alpha - 1024) << 7) / 3072 + 80;
        else
            alpha = 80;

        vm->scale.x = popupScaleX;
        vm->scale.y = popupScaleY;

        f32 rawTop;
        f32 rawBottom;
        if ((vm->anchor & 2) == 0)
        {
            rawTop = vm->pos.y - spriteHalfHeight;
            rawBottom = spriteHalfHeight + vm->pos.y;
        }
        else
        {
            rawTop = vm->pos.y;
            rawBottom = spriteHalfHeight + vm->pos.y + spriteHalfHeight;
        }
        const f32 roundedTop = nearbyintf(
            rawTop + this->screenShakeOffset.y) - g_ZeroPointFive;
        const f32 roundedBottom = nearbyintf(
            rawBottom + this->screenShakeOffset.y) - g_ZeroPointFive;

        u8 *digit = reinterpret_cast<u8 *>(
            &popup.text[popup.characterCount - 1]);
        for (i32 remaining = popup.characterCount; remaining > 0;
             --remaining, --digit)
        {
            const i32 spriteIndex = PspAsciiPopupSpriteIndex(popup, *digit);
            const PspAsciiPopupDirectSprite &cached =
                directSprites[spriteIndex];
            vm->loadedSprite = cached.sprite;
            vm->color1.a = static_cast<u8>(alpha);
            // TH08's score loop updates only X. Y deliberately remains the
            // VM's initialized height rather than each glyph's heightPx.
            vm->spriteSize.x = cached.sprite->widthPx;

            VertexTex1DiffuseXyzrhw pair[2] = {
                g_QuadVertices[0], g_QuadVertices[3],
            };
            BuildPspAsciiPopupDirectPair(
                pair, vm, cached, roundedTop, roundedBottom,
                this->screenShakeOffset.x, preservedW0, preservedW3);
            lastPair[0] = pair[0];
            lastPair[1] = pair[1];

            if (PspAsciiPopupDirectPairVisible(pair))
            {
                ZunColor batchColor;
                batchColor.d3dColor = vm->flag17
                                          ? vm->color2.d3dColor
                                          : vm->color1.d3dColor;
                if (this->useMixColor)
                {
                    batchColor.r = MixColors(batchColor.r, this->color.r);
                    batchColor.g = MixColors(batchColor.g, this->color.g);
                    batchColor.b = MixColors(batchColor.b, this->color.b);
                    batchColor.a = MixColors(batchColor.a, this->color.a);
                }
                pair[0].diffuse = batchColor.d3dColor;
                pair[1].diffuse = batchColor.d3dColor;

                if (visibleCount == 0U)
                {
                    // The first actual write owns the front of the fixed
                    // staging array; commit older geometry before overwrite.
                    this->FlushVertexBuffer();
                }
                VertexTex1DiffuseXyzrhw *const output =
                    this->vertexBuffer + visibleCount * 2U;
                output[0] = pair[0];
                output[1] = pair[1];
                ++visibleCount;
                hasVisibleColor = true;
                lastVisibleColor = batchColor.d3dColor;
            }
            vm->pos.x += 8.0f;
        }
    }

    CommitPspAsciiPopupDirectFinalQuad(
        lastPair, hasVisibleColor, lastVisibleColor);
#else
    // Pass 1 validates every active popup, sprite, coordinate, texture, cull
    // edge and capacity. Nothing in renderer, VM, g_QuadVertices, or popup
    // state changes until this complete-frame proof and backend reserve pass.
    VertexTex1DiffuseXyzrhw validationQuad[4] = {
        g_QuadVertices[0], g_QuadVertices[1],
        g_QuadVertices[2], g_QuadVertices[3],
    };
    AnmVm validationVm = *vm;
    validationVm.scale.x = popupScaleX;
    validationVm.scale.y = popupScaleY;
    for (i32 popupIndex = 0; popupIndex < popupCount; ++popupIndex)
    {
        const AsciiManagerPopup &popup = popups[popupIndex];
        if (!popup.inUse)
            continue;
        if (popup.characterCount == 0U ||
            popup.characterCount > sizeof(popup.text) ||
            !std::isfinite(popup.position.x) ||
            !std::isfinite(popup.position.y))
        {
            return RejectPspAsciiPopupBatch();
        }

        const f32 dx = playerX - popup.position.x;
        const f32 dy = playerY - popup.position.y;
        const f32 distanceSquared = dx * dx + dy * dy;
        if (!std::isfinite(distanceSquared) || distanceSquared < 0.0f ||
            distanceSquared > 2147483520.0f)
        {
            return RejectPspAsciiPopupBatch();
        }

        validationVm.pos.x =
            popup.position.x - static_cast<f32>(popup.characterCount * 4);
        validationVm.pos.y = popup.position.y;
        const u8 *digit = reinterpret_cast<const u8 *>(
            &popup.text[popup.characterCount - 1]);
        for (i32 remaining = popup.characterCount; remaining > 0;
             --remaining, --digit)
        {
            if (*digit > 10U)
                return RejectPspAsciiPopupBatch();
            const i32 spriteIndex = PspAsciiPopupSpriteIndex(popup, *digit);
            AnmLoadedSprite *sprite = asciiAnm->GetSprite(spriteIndex);
            if (sprite == NULL || sprite->texture == NULL ||
                (batchTexture != NULL && batchTexture != sprite->texture) ||
                !std::isfinite(sprite->widthPx) || sprite->widthPx <= 0.0f ||
                !std::isfinite(sprite->uvStart.x) ||
                !std::isfinite(sprite->uvStart.y) ||
                !std::isfinite(sprite->uvEnd.x) ||
                !std::isfinite(sprite->uvEnd.y) ||
                sprite->uvEnd.x < sprite->uvStart.x ||
                sprite->uvEnd.y < sprite->uvStart.y)
            {
                return RejectPspAsciiPopupBatch();
            }
            batchTexture = sprite->texture;
            validationVm.loadedSprite = sprite;
            validationVm.spriteSize.x = sprite->widthPx;
            BuildPspAsciiPopupQuad(validationQuad, &validationVm, sprite,
                                   sprite->widthPx, popupScaleX,
                                   popupScaleY, this->screenShakeOffset);
            if (!PspAsciiPopupQuadFinite(validationQuad))
                return RejectPspAsciiPopupBatch();
            if (PspAsciiPopupQuadVisible(validationQuad))
                ++visibleCount;
            ++digitCount;
            validationVm.pos.x += 8.0f;
        }
    }

    if (digitCount == 0U)
        return ZUN_SUCCESS;
    if (visibleCount > ARRAY_SIZE(this->vertexBuffer) / 2U ||
        !th08_psp_reserve_ascii_popup_sprite_pairs(
            g_Supervisor.d3dDevice, visibleCount))
    {
        return RejectPspAsciiPopupBatch();
    }

    if (visibleCount != 0U)
    {
        // The direct source uses the front of the manager's existing fixed
        // staging array. Commit all older geometry before overwriting it.
        this->FlushVertexBuffer();
    }

    u32 generatedVisible = 0U;
    for (i32 popupIndex = 0; popupIndex < popupCount; ++popupIndex)
    {
        AsciiManagerPopup &popup = popups[popupIndex];
        if (!popup.inUse)
            continue;
        th08::psp::RenderPerfNotePopup(popup.characterCount);

        vm->pos.x = popup.position.x - static_cast<f32>(popup.characterCount * 4);
        vm->pos.y = popup.position.y;
        vm->color1.d3dColor = popup.color;

        f32 dx = playerX - popup.position.x;
        f32 dy = playerY - popup.position.y;
        i32 alpha = static_cast<i32>(dx * dx + dy * dy);
        if (alpha > 4096)
            alpha = 208;
        else if (alpha > 1024)
            alpha = ((alpha - 1024) << 7) / 3072 + 80;
        else
            alpha = 80;

        vm->scale.x = popupScaleX;
        vm->scale.y = popupScaleY;
        u8 *digit = reinterpret_cast<u8 *>(
            &popup.text[popup.characterCount - 1]);
        for (i32 remaining = popup.characterCount; remaining > 0;
             --remaining, --digit)
        {
            const i32 spriteIndex = PspAsciiPopupSpriteIndex(popup, *digit);
            vm->loadedSprite = asciiAnm->GetSprite(spriteIndex);
            vm->color1.a = static_cast<u8>(alpha);
            // TH08's score loop updates only X. Y deliberately remains the
            // VM's initialized height rather than each candidate's heightPx.
            vm->spriteSize.x = vm->loadedSprite->widthPx;
            BuildPspAsciiPopupQuad(g_QuadVertices, vm, vm->loadedSprite,
                                   vm->spriteSize.x, vm->scale.x, vm->scale.y,
                                   this->screenShakeOffset);

            if (PspAsciiPopupQuadVisible(g_QuadVertices))
            {
                ZunColor batchColor;
                batchColor.d3dColor = vm->flag17
                                          ? vm->color2.d3dColor
                                          : vm->color1.d3dColor;
                if (this->useMixColor)
                {
                    batchColor.r = MixColors(batchColor.r, this->color.r);
                    batchColor.g = MixColors(batchColor.g, this->color.g);
                    batchColor.b = MixColors(batchColor.b, this->color.b);
                    batchColor.a = MixColors(batchColor.a, this->color.a);
                }
                g_QuadVertices[0].diffuse = batchColor.d3dColor;
                g_QuadVertices[1].diffuse = batchColor.d3dColor;
                g_QuadVertices[2].diffuse = batchColor.d3dColor;
                g_QuadVertices[3].diffuse = batchColor.d3dColor;

                VertexTex1DiffuseXyzrhw *pair =
                    this->vertexBuffer + generatedVisible * 2U;
                pair[0] = g_QuadVertices[0];
                pair[1] = g_QuadVertices[3];
                ++generatedVisible;
            }
            vm->pos.x += 8.0f;
        }
    }

    // Pass 1 and pass 2 share the same pure geometry/cull helper, so this can
    // differ only after memory corruption. The backend still rejects before
    // emission and AsciiManager then executes the complete canonical loop.
    if (generatedVisible != visibleCount)
        return RejectPspAsciiPopupBatch();
#endif

    if (visibleCount != 0U)
    {
        if (this->currentTexture != batchTexture)
        {
            this->currentTexture = batchTexture;
            this->FlushVertexBuffer();
            g_Supervisor.d3dDevice->SetTexture(0, this->currentTexture);
        }
        if (this->currentVertexShader != 1)
        {
            this->FlushVertexBuffer();
            this->currentVertexShader = 1;
        }
        this->SetRenderStateForVm(vm);
        if (visibleCount > 1U)
            this->renderStateChangesThisFrame += visibleCount - 1U;
        g_Supervisor.d3dDevice->SetTextureStageState(
            0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        g_Supervisor.d3dDevice->SetTextureStageState(
            0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        g_Supervisor.d3dDevice->SetVertexShader(
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        if (!th08_psp_draw_ascii_popup_sprite_pairs(
                g_Supervisor.d3dDevice, this->vertexBuffer, visibleCount,
                sizeof(VertexTex1DiffuseXyzrhw)))
        {
            return RejectPspAsciiPopupBatch();
        }
        ++this->flushesThisFrame;
    }

    th08::psp::RenderPerfNoteAsciiPopupBatchSuccess(digitCount, visibleCount);
#if defined(TH08_PSP_ASCII_POPUP_DIRECT_PAIR) && \
    TH08_PSP_ASCII_POPUP_DIRECT_PAIR
    th08::psp::RenderPerfNoteAsciiPopupDirectPairSavings(
        digitCount, activePopupCount);
#endif
    return ZUN_SUCCESS;
}
#endif

void AnmManager::TranslateRotation(VertexTex1DiffuseXyzrhw *vertex, float x, float y, float sine, float cosine,
                                   float xOffset, float yOffset)
{
    vertex->pos.x = x * cosine - y * sine + xOffset;
    vertex->pos.y = x * sine + y * cosine + yOffset;
}


#pragma var_order(sine, rotation, cosine, x, y, yOffset, xOffset)
ZunResult AnmManager::Draw2D(AnmVm *vm)
{
    float sine, cosine, rotation, xOffset, yOffset, x, y;

    if (vm->rotation.z == 0.0f)
    {
        return this->DrawNoRotation(vm);
    }

    if (!vm->IsVisible())
    {
        return ZUN_ERROR;
    }

    if (!vm->flag1)
    {
        return ZUN_ERROR;
    }

    if (vm->color1.a == 0)
    {
        return ZUN_ERROR;
    }

    rotation = vm->rotation.z;

#if defined(PSP)
    th08::psp::RenderSinCos(rotation, &sine, &cosine);
#else
    sincos(rotation, sine, cosine);
#endif

    xOffset = vm->pos.x;
    yOffset = vm->pos.y;

    x = (vm->spriteSize.x * vm->scale.x) / 2.0f;
    y = (vm->spriteSize.y * vm->scale.y) / 2.0f;

    this->TranslateRotation(&g_QuadVertices[0], -x, -y, sine, cosine, xOffset, yOffset);
    this->TranslateRotation(&g_QuadVertices[1], x, -y, sine, cosine, xOffset, yOffset);
    this->TranslateRotation(&g_QuadVertices[2], -x, y, sine, cosine, xOffset, yOffset);
    this->TranslateRotation(&g_QuadVertices[3], x, y, sine, cosine, xOffset, yOffset);

    g_QuadVertices[0].pos.z = g_QuadVertices[1].pos.z = g_QuadVertices[2].pos.z = g_QuadVertices[3].pos.z = vm->pos.z;

    if (vm->anchor & 1)
    {
        g_QuadVertices[0].pos.x += x;
        g_QuadVertices[1].pos.x += x;
        g_QuadVertices[2].pos.x += x;
        g_QuadVertices[3].pos.x += x;
    }

    if (vm->anchor & 2)
    {
        g_QuadVertices[0].pos.y += y;
        g_QuadVertices[1].pos.y += y;
        g_QuadVertices[2].pos.y += y;
        g_QuadVertices[3].pos.y += y;
    }

    return this->DrawInner(vm, 0);
}

ZunResult AnmManager::Draw2DWithPrecomputedRotation(AnmVm *vm, f32 sine,
                                                     f32 cosine)
{
    // Preserve Draw2D's exact zero-rotation branch, including its half-pixel
    // rounding flag.  The cached values are only consumed by the rotated path.
    if (vm->rotation.z == 0.0f)
        return this->DrawNoRotation(vm);

    if (!vm->IsVisible())
        return ZUN_ERROR;
    if (!vm->flag1)
        return ZUN_ERROR;
    if (vm->color1.a == 0)
        return ZUN_ERROR;

    const f32 xOffset = vm->pos.x;
    const f32 yOffset = vm->pos.y;
    const f32 x = (vm->spriteSize.x * vm->scale.x) / 2.0f;
    const f32 y = (vm->spriteSize.y * vm->scale.y) / 2.0f;

    this->TranslateRotation(&g_QuadVertices[0], -x, -y, sine, cosine,
                            xOffset, yOffset);
    this->TranslateRotation(&g_QuadVertices[1], x, -y, sine, cosine,
                            xOffset, yOffset);
    this->TranslateRotation(&g_QuadVertices[2], -x, y, sine, cosine,
                            xOffset, yOffset);
    this->TranslateRotation(&g_QuadVertices[3], x, y, sine, cosine,
                            xOffset, yOffset);

    g_QuadVertices[0].pos.z = g_QuadVertices[1].pos.z =
        g_QuadVertices[2].pos.z = g_QuadVertices[3].pos.z = vm->pos.z;

    if (vm->anchor & 1)
    {
        g_QuadVertices[0].pos.x += x;
        g_QuadVertices[1].pos.x += x;
        g_QuadVertices[2].pos.x += x;
        g_QuadVertices[3].pos.x += x;
    }

    if (vm->anchor & 2)
    {
        g_QuadVertices[0].pos.y += y;
        g_QuadVertices[1].pos.y += y;
        g_QuadVertices[2].pos.y += y;
        g_QuadVertices[3].pos.y += y;
    }

    return this->DrawInner(vm, 0);
}

#if TH08_PSP_BULLET_ONEPASS_4V_ENABLED
namespace
{
bool PspBulletOnePass4VInputSupported(const AnmManager *manager,
                                      const AnmVm *vm)
{
    if (manager == NULL || vm == NULL || vm->loadedSprite == NULL ||
        vm->loadedSprite->texture == NULL || vm->rotation.z == 0.0f ||
        !vm->visible || !vm->flag1 || vm->color1.a == 0)
    {
        return false;
    }

    return true;
}

bool PspBulletOnePass4VQuadFinite(
    const VertexTex1DiffuseXyzrhw *quad)
{
    u32 carry = 0U;
    carry |= th08::psp::BulletOnePassFiniteCarry(quad[0].pos.x);
    carry |= th08::psp::BulletOnePassFiniteCarry(quad[0].pos.y);
    carry |= th08::psp::BulletOnePassFiniteCarry(quad[1].pos.x);
    carry |= th08::psp::BulletOnePassFiniteCarry(quad[1].pos.y);
    carry |= th08::psp::BulletOnePassFiniteCarry(quad[2].pos.x);
    carry |= th08::psp::BulletOnePassFiniteCarry(quad[2].pos.y);
    carry |= th08::psp::BulletOnePassFiniteCarry(quad[3].pos.x);
    carry |= th08::psp::BulletOnePassFiniteCarry(quad[3].pos.y);
    carry |= th08::psp::BulletOnePassFiniteCarry(quad[0].pos.z);
    carry |= th08::psp::BulletOnePassFiniteCarry(quad[0].textureUV.x);
    carry |= th08::psp::BulletOnePassFiniteCarry(quad[1].textureUV.x);
    carry |= th08::psp::BulletOnePassFiniteCarry(quad[0].textureUV.y);
    carry |= th08::psp::BulletOnePassFiniteCarry(quad[2].textureUV.y);
    return (carry & 0x80000000U) == 0U;
}

bool PspBulletOnePass4VOwnerReady()
{
    return g_PspBulletUnifiedQuadBatchActive &&
           g_PspUnifiedQuadBatchOwner == PspUnifiedQuadBatchOwner::Bullet;
}

bool PspBulletOnePass4VStateReady(const AnmManager *manager,
                                  const AnmVm *vm)
{
    return manager->currentTexture == vm->loadedSprite->texture &&
           manager->currentVertexShader == 1 &&
           manager->currentBlendMode == vm->blendMode &&
           (g_Supervisor.cfg.opts.disableDepthTest ||
            manager->disableZWrite == vm->zWriteDisabled);
}

#if defined(TH08_PSP_BULLET_ONEPASS_4V_AUDIT) && \
    TH08_PSP_BULLET_ONEPASS_4V_AUDIT
bool PspBulletOnePass4VCapacityReady(const AnmManager *manager)
{
    if (manager == NULL ||
        manager->spritesToDraw >= kPspBulletUnifiedQuadCapacity ||
        !PspBulletUnifiedQuadBufferCanAppend(manager) ||
        manager->vertexBufferStartPtr == NULL)
    {
        return false;
    }

    const uintptr_t begin =
        reinterpret_cast<uintptr_t>(manager->vertexBuffer);
    const uintptr_t limit = reinterpret_cast<uintptr_t>(
        manager->vertexBuffer + ARRAY_SIZE(manager->vertexBuffer));
    const uintptr_t start =
        reinterpret_cast<uintptr_t>(manager->vertexBufferStartPtr);
    const uintptr_t cursor =
        reinterpret_cast<uintptr_t>(manager->vertexBufferEndPtr);
    if (start < begin || start > cursor || cursor > limit)
        return false;

    const uintptr_t expectedBytes =
        static_cast<uintptr_t>(manager->spritesToDraw) * 4U *
        sizeof(VertexTex1DiffuseXyzrhw);
    return cursor - start == expectedBytes;
}
#endif

#if defined(TH08_PSP_BULLET_ONEPASS_4V_FASTPATH) && \
    TH08_PSP_BULLET_ONEPASS_4V_FASTPATH
bool PspBulletOnePass4VProductCapacityReady(const AnmManager *manager)
{
    if (manager == NULL || manager->vertexBufferEndPtr == NULL ||
        manager->spritesToDraw >= kPspBulletUnifiedQuadCapacity)
    {
        return false;
    }

    // BeginPspBulletUnifiedQuadBatch and every accepted canonical/unified
    // append establish the exact start/count/cursor relationship.  M0 checks
    // that complete invariant on every proposed acceptance.  Product needs
    // only prove that this typed cursor still owns four complete slots; it
    // does not repeat the expensive divide/modulo and count multiplication.
    const uintptr_t begin =
        reinterpret_cast<uintptr_t>(manager->vertexBuffer);
    const uintptr_t limit = reinterpret_cast<uintptr_t>(
        manager->vertexBuffer + ARRAY_SIZE(manager->vertexBuffer));
    const uintptr_t cursor =
        reinterpret_cast<uintptr_t>(manager->vertexBufferEndPtr);
    const uintptr_t bytes = 4U * sizeof(VertexTex1DiffuseXyzrhw);
    return cursor >= begin && cursor <= limit && limit - cursor >= bytes;
}
#endif

void BuildPspBulletOnePass4V(VertexTex1DiffuseXyzrhw *quad,
                             const AnmManager *manager, const AnmVm *vm,
                             f32 sine, f32 cosine)
{
    th08::psp::BuildBulletOnePassRotatedQuad4V(
        quad, *vm, sine, cosine, manager->screenShakeOffset.x,
        manager->screenShakeOffset.y);
}

bool PspBulletOnePass4VVisible(const VertexTex1DiffuseXyzrhw *quad)
{
    return th08::psp::BulletOnePassQuad4VVisible(
        quad, static_cast<f32>(g_Supervisor.viewport.X),
        static_cast<f32>(g_Supervisor.viewport.Y),
        static_cast<f32>(g_Supervisor.viewport.X +
                         g_Supervisor.viewport.Width),
        static_cast<f32>(g_Supervisor.viewport.Y +
                         g_Supervisor.viewport.Height));
}

void SetPspBulletOnePass4VDiffuse(AnmManager *manager, AnmVm *vm,
                                  VertexTex1DiffuseXyzrhw *quad)
{
    ZunColor color;
    color.d3dColor = vm->flag17 ? vm->color2.d3dColor
                                : vm->color1.d3dColor;
    if (manager->useMixColor)
    {
        color.r = MixColors(color.r, manager->color.r);
        color.g = MixColors(color.g, manager->color.g);
        color.b = MixColors(color.b, manager->color.b);
        color.a = MixColors(color.a, manager->color.a);
    }
    quad[0].diffuse = quad[1].diffuse =
        quad[2].diffuse = quad[3].diffuse = color.d3dColor;
}

#if defined(TH08_PSP_BULLET_ONEPASS_4V_AUDIT) && \
    TH08_PSP_BULLET_ONEPASS_4V_AUDIT
void NotePspBulletOnePass4VMismatch(const char *kind)
{
    const u32 mismatches = g_PspBulletOnePass4VStats.quadMismatches +
        g_PspBulletOnePass4VStats.bufferMismatches +
        g_PspBulletOnePass4VStats.vmMismatches +
        g_PspBulletOnePass4VStats.stateMismatches;
    if (mismatches <= 4U)
    {
        fprintf(stderr,
                "TH08PSP BULLET_ONEPASS_4V_M0 mismatch=%s ordinal=%lu\n",
                kind, static_cast<unsigned long>(
                          g_PspBulletOnePass4VStats.attempts));
    }
}
#endif
} // namespace

#if defined(TH08_PSP_BULLET_ONEPASS_4V_AUDIT) && \
    TH08_PSP_BULLET_ONEPASS_4V_AUDIT
ZunResult AnmManager::DrawPspBulletOnePass4VAudit(AnmVm *vm, f32 sine,
                                                   f32 cosine)
{
    PspBulletOnePass4VStats &stats = g_PspBulletOnePass4VStats;
    ++stats.attempts;
    ++stats.canonicalDraws;

    if (!PspBulletOnePass4VInputSupported(this, vm))
    {
        ++stats.inputFallbacks;
        return this->Draw2DWithPrecomputedRotation(vm, sine, cosine);
    }
    if (!PspBulletOnePass4VOwnerReady())
    {
        ++stats.ownerFallbacks;
        return this->Draw2DWithPrecomputedRotation(vm, sine, cosine);
    }

    VertexTex1DiffuseXyzrhw candidate[4];
    memcpy(candidate, g_QuadVertices, sizeof(candidate));
    BuildPspBulletOnePass4V(candidate, this, vm, sine, cosine);
    ++stats.builtQuads;
    if (!PspBulletOnePass4VQuadFinite(candidate))
    {
        ++stats.inputFallbacks;
        return this->Draw2DWithPrecomputedRotation(vm, sine, cosine);
    }
    const bool visible = PspBulletOnePass4VVisible(candidate);
    if (visible)
    {
        ++stats.visibleQuads;
        SetPspBulletOnePass4VDiffuse(this, vm, candidate);
    }
    else
    {
        ++stats.culledQuads;
    }

    const bool stateReady = PspBulletOnePass4VStateReady(this, vm);
    const bool capacityReady = PspBulletOnePass4VCapacityReady(this);
    if (!stateReady)
        ++stats.stateFallbacks;
    else if (!capacityReady)
        ++stats.capacityFallbacks;
    const bool wouldAccept = stateReady && capacityReady;
    if (wouldAccept)
        ++stats.wouldAccept;

    // Draw2D is not allowed to mutate any VM byte.  Keep a complete raw M0
    // witness rather than checking only the fields used by this frontend.
    alignas(4) u8 vmBefore[sizeof(AnmVm)];
    memcpy(vmBefore, vm, sizeof(vmBefore));
    VertexTex1DiffuseXyzrhw *const endBefore = this->vertexBufferEndPtr;
    VertexTex1DiffuseXyzrhw *const startBefore = this->vertexBufferStartPtr;
    const u32 spritesBefore = this->spritesToDraw;
    const u32 renderStateChangesBefore = this->renderStateChangesThisFrame;
    const u32 flushesBefore = this->flushesThisFrame;
    IDirect3DTexture8 *const textureBefore = this->currentTexture;
    const u8 blendBefore = this->currentBlendMode;
    const u8 shaderBefore = this->currentVertexShader;
    const u8 zWriteBefore = this->disableZWrite;

    const ZunResult result =
        this->Draw2DWithPrecomputedRotation(vm, sine, cosine);

    if (memcmp(candidate, g_QuadVertices, sizeof(candidate)) == 0)
    {
        ++stats.quadMatches;
    }
    else
    {
        ++stats.quadMismatches;
        NotePspBulletOnePass4VMismatch("quad_bytes");
    }

    if (memcmp(vmBefore, vm, sizeof(vmBefore)) == 0)
    {
        ++stats.vmMatches;
    }
    else
    {
        ++stats.vmMismatches;
        NotePspBulletOnePass4VMismatch("vm_post_state");
    }

    if (wouldAccept)
    {
        const bool bufferMatches = visible
            ? result == ZUN_SUCCESS &&
                  this->vertexBufferStartPtr == startBefore &&
                  this->vertexBufferEndPtr == endBefore + 4 &&
                  this->spritesToDraw == spritesBefore + 1U &&
                  memcmp(endBefore, candidate, sizeof(candidate)) == 0
            : result == ZUN_SUCCESS &&
                  this->vertexBufferStartPtr == startBefore &&
                  this->vertexBufferEndPtr == endBefore &&
                  this->spritesToDraw == spritesBefore;
        if (bufferMatches)
        {
            ++stats.bufferMatches;
        }
        else
        {
            ++stats.bufferMismatches;
            NotePspBulletOnePass4VMismatch("buffer_or_count");
        }

        const u32 expectedStateChanges =
            renderStateChangesBefore + (visible ? 1U : 0U);
        const bool stateMatches =
            this->renderStateChangesThisFrame == expectedStateChanges &&
            this->flushesThisFrame == flushesBefore &&
            this->currentTexture == textureBefore &&
            this->currentBlendMode == blendBefore &&
            this->currentVertexShader == shaderBefore &&
            this->disableZWrite == zWriteBefore;
        if (stateMatches)
        {
            ++stats.stateMatches;
        }
        else
        {
            ++stats.stateMismatches;
            NotePspBulletOnePass4VMismatch("renderer_state");
        }
    }
    return result;
}
#endif

#if defined(TH08_PSP_BULLET_ONEPASS_4V_FASTPATH) && \
    TH08_PSP_BULLET_ONEPASS_4V_FASTPATH
bool AnmManager::TryDrawPspBulletOnePass4V(AnmVm *vm, f32 sine,
                                            f32 cosine, ZunResult *result)
{
    if (result == NULL ||
        !PspBulletOnePass4VInputSupported(this, vm) ||
        !PspBulletOnePass4VOwnerReady() ||
        !PspBulletOnePass4VStateReady(this, vm) ||
        !PspBulletOnePass4VProductCapacityReady(this))
    {
        return false;
    }

    // Structural/state/capacity rejection precedes every write.  Build the
    // same persistent scratch image as rotated Draw2D, then cheaply validate
    // all generated endpoints.  A nonfinite rejection is safe after this
    // scratch write: the canonical fallback deterministically rebuilds the
    // complete position/UV image before consuming it.
    BuildPspBulletOnePass4V(g_QuadVertices, this, vm, sine, cosine);
    if (!PspBulletOnePass4VQuadFinite(g_QuadVertices))
        return false;
    if (!PspBulletOnePass4VVisible(g_QuadVertices))
    {
        *result = ZUN_SUCCESS;
        return true;
    }

    SetPspBulletOnePass4VDiffuse(this, vm, g_QuadVertices);
    ++this->renderStateChangesThisFrame;
    this->vertexBufferEndPtr[0] = g_QuadVertices[0];
    this->vertexBufferEndPtr[1] = g_QuadVertices[1];
    this->vertexBufferEndPtr[2] = g_QuadVertices[2];
    this->vertexBufferEndPtr[3] = g_QuadVertices[3];
    this->vertexBufferEndPtr += 4;
    ++this->spritesToDraw;
    *result = ZUN_SUCCESS;
    return true;
}
#endif
#endif

// FUNCTION: th08 0x00463470
#pragma var_order(sine, rotation, cosine, halfWidth, halfHeight, yOffset, xOffset, zeroHalfWidth, zeroHalfHeight, this)
ZunResult AnmManager::Draw2DRotatedOrAxisAligned(AnmVm *vm)
{
    f32 rotation;
    f32 sine;
    f32 cosine;
    f32 halfWidth;
    f32 halfHeight;
    f32 xOffset;
    f32 yOffset;
    f32 zeroHalfWidth;
    f32 zeroHalfHeight;

    if (!vm->IsVisible())
        return ZUN_ERROR;
    if (!vm->flag1)
        return ZUN_ERROR;
    if (vm->color1.a == 0)
        return ZUN_ERROR;

    rotation = vm->rotation.z;
    if (rotation != 0.0f)
    {
#if defined(PSP)
        th08::psp::RenderSinCos(rotation, &sine, &cosine);
#else
        sincos(rotation, sine, cosine);
#endif
        xOffset = vm->pos.x;
        yOffset = vm->pos.y;
        halfWidth = vm->spriteSize.x * vm->scale.x / 2.0f;
        halfHeight = vm->spriteSize.y * vm->scale.y / 2.0f;

        this->TranslateRotation(&g_QuadVertices[0], -halfWidth, -halfHeight, sine, cosine, xOffset, yOffset);
        this->TranslateRotation(&g_QuadVertices[1], halfWidth, -halfHeight, sine, cosine, xOffset, yOffset);
        this->TranslateRotation(&g_QuadVertices[2], -halfWidth, halfHeight, sine, cosine, xOffset, yOffset);
        this->TranslateRotation(&g_QuadVertices[3], halfWidth, halfHeight, sine, cosine, xOffset, yOffset);

        g_QuadVertices[3].pos.z = vm->pos.z;
        g_QuadVertices[2].pos.z = g_QuadVertices[3].pos.z;
        g_QuadVertices[1].pos.z = g_QuadVertices[2].pos.z;
        g_QuadVertices[0].pos.z = g_QuadVertices[1].pos.z;

        if (vm->anchor & 1)
        {
            g_QuadVertices[0].pos.x += halfWidth;
            g_QuadVertices[1].pos.x += halfWidth;
            g_QuadVertices[2].pos.x += halfWidth;
            g_QuadVertices[3].pos.x += halfWidth;
        }
        if (vm->anchor & 2)
        {
            g_QuadVertices[0].pos.y += halfHeight;
            g_QuadVertices[1].pos.y += halfHeight;
            g_QuadVertices[2].pos.y += halfHeight;
            g_QuadVertices[3].pos.y += halfHeight;
        }
    }
    else
    {
        zeroHalfWidth = vm->spriteSize.x * vm->scale.x / 2.0f;
        zeroHalfHeight = vm->spriteSize.y * vm->scale.y / 2.0f;

        if ((vm->anchor & 1) == 0)
        {
            g_QuadVertices[2].pos.x = vm->pos.x - zeroHalfWidth;
            g_QuadVertices[0].pos.x = g_QuadVertices[2].pos.x;
            g_QuadVertices[3].pos.x = zeroHalfWidth + vm->pos.x;
            g_QuadVertices[1].pos.x = g_QuadVertices[3].pos.x;
        }
        else
        {
            g_QuadVertices[2].pos.x = vm->pos.x;
            g_QuadVertices[0].pos.x = g_QuadVertices[2].pos.x;
            g_QuadVertices[3].pos.x = zeroHalfWidth + vm->pos.x + zeroHalfWidth;
            g_QuadVertices[1].pos.x = g_QuadVertices[3].pos.x;
        }

        if ((vm->anchor & 2) == 0)
        {
            g_QuadVertices[1].pos.y = vm->pos.y - zeroHalfHeight;
            g_QuadVertices[0].pos.y = g_QuadVertices[1].pos.y;
            g_QuadVertices[3].pos.y = zeroHalfHeight + vm->pos.y;
            g_QuadVertices[2].pos.y = g_QuadVertices[3].pos.y;
        }
        else
        {
            g_QuadVertices[1].pos.y = vm->pos.y;
            g_QuadVertices[0].pos.y = g_QuadVertices[1].pos.y;
            g_QuadVertices[3].pos.y = zeroHalfHeight + vm->pos.y + zeroHalfHeight;
            g_QuadVertices[2].pos.y = g_QuadVertices[3].pos.y;
        }
    }

    return this->DrawInner(vm, 0);
}

/* This is identical to DrawNoRotation except for 0 being passed to DrawInner,
 * which doesn't round and subtract 0.5 from each vertex.
 */
#pragma var_order(spriteHalfWidth, spriteHalfHeight)
ZunResult AnmManager::DrawNoRotationNoRound(AnmVm *vm)
{
    float spriteHalfWidth;
    float spriteHalfHeight;

    if (!vm->IsVisible())
    {
        return ZUN_ERROR;
    }

    if (!vm->flag1)
    {
        return ZUN_ERROR;
    }

    if (vm->color1.a == 0)
    {
        return ZUN_ERROR;
    }

    spriteHalfWidth = (vm->spriteSize.x * vm->scale.x) / 2.0f;
    spriteHalfHeight = (vm->spriteSize.y * vm->scale.y) / 2.0f;

    if ((vm->anchor & 1) == 0)
    {
        g_QuadVertices[0].pos.x = g_QuadVertices[2].pos.x = vm->pos.x - spriteHalfWidth;
        g_QuadVertices[1].pos.x = g_QuadVertices[3].pos.x = spriteHalfWidth + vm->pos.x;
    }
    else
    {
        g_QuadVertices[0].pos.x = g_QuadVertices[2].pos.x = vm->pos.x;
        g_QuadVertices[1].pos.x = g_QuadVertices[3].pos.x = spriteHalfWidth + vm->pos.x + spriteHalfWidth;
    }

    if ((vm->anchor & 2) == 0)
    {
        g_QuadVertices[0].pos.y = g_QuadVertices[1].pos.y = vm->pos.y - spriteHalfHeight;
        g_QuadVertices[2].pos.y = g_QuadVertices[3].pos.y = spriteHalfHeight + vm->pos.y;
    }
    else
    {
        g_QuadVertices[0].pos.y = g_QuadVertices[1].pos.y = vm->pos.y;
        g_QuadVertices[2].pos.y = g_QuadVertices[3].pos.y = spriteHalfHeight + vm->pos.y + spriteHalfHeight;
    }

    g_QuadVertices[0].pos.z = g_QuadVertices[1].pos.z = g_QuadVertices[2].pos.z = g_QuadVertices[3].pos.z = vm->pos.z;

    return this->DrawInner(vm, 0);
}

// FUNCTION: th08 0x4639e0
#pragma var_order(halfWidth, halfHeight, yOffset, xOffset, sine, worldMatrix, rotation, projectedReference, projectedPosition, delta, cosine, origin, this)
ZunResult AnmManager::ProjectCameraFacingQuad(AnmVm *vm)
{
    f32 rotation;
    f32 sine;
    f32 cosine;
    f32 xOffset;
    f32 yOffset;
    f32 halfHeight;
    f32 halfWidth;

    rotation = vm->rotation.z;
#if defined(PSP)
    th08::psp::RenderSinCos(rotation, &sine, &cosine);
#else
    sincos(rotation, sine, cosine);
#endif

    D3DXMATRIX worldMatrix;
    Float3 projectedPosition;
    Float3 projectedReference;
    Float3 delta;
    Float3 origin(0.0f, 0.0f, 0.0f);

    D3DXMatrixIdentity(&worldMatrix);
    worldMatrix._41 = vm->pos.operator float *()[0];
    worldMatrix._42 = vm->pos.operator float *()[1];
    worldMatrix._43 = vm->pos.operator float *()[2];

    D3DXVec3Project(reinterpret_cast<D3DXVECTOR3 *>(&projectedPosition),
                    reinterpret_cast<D3DXVECTOR3 *>(&origin), &g_Supervisor.viewport,
                    &g_Supervisor.projectionMatrix, &g_Supervisor.viewMatrix, &worldMatrix);
    if (projectedPosition.z < 0.0f || projectedPosition.z > 1.0f)
        return ZUN_ERROR;

    D3DXVec3Project(reinterpret_cast<D3DXVECTOR3 *>(&projectedReference),
                    reinterpret_cast<D3DXVECTOR3 *>(&g_Background.cameraCurrent.right),
                    &g_Supervisor.viewport, &g_Supervisor.projectionMatrix,
                    &g_Supervisor.viewMatrix, &worldMatrix);
    delta = projectedReference - projectedPosition;
    xOffset = D3DXVec3Length(reinterpret_cast<D3DXVECTOR3 *>(&delta)) * 0.5f;
    halfWidth = xOffset * vm->spriteSize.x * vm->scale.x;
    halfHeight = xOffset * vm->spriteSize.y * vm->scale.y;
    xOffset = projectedPosition.x;
    yOffset = projectedPosition.y;

    this->TranslateRotation(&g_QuadVertices[0], -halfWidth, -halfHeight, sine, cosine, xOffset, yOffset);
    this->TranslateRotation(&g_QuadVertices[1], halfWidth, -halfHeight, sine, cosine, xOffset, yOffset);
    this->TranslateRotation(&g_QuadVertices[2], -halfWidth, halfHeight, sine, cosine, xOffset, yOffset);
    this->TranslateRotation(&g_QuadVertices[3], halfWidth, halfHeight, sine, cosine, xOffset, yOffset);

    g_QuadVertices[3].pos.z = projectedPosition.z;
    g_QuadVertices[2].pos.z = g_QuadVertices[3].pos.z;
    g_QuadVertices[1].pos.z = g_QuadVertices[2].pos.z;
    g_QuadVertices[0].pos.z = g_QuadVertices[1].pos.z;

    if (vm->anchor & 1)
    {
        g_QuadVertices[0].pos.x += halfWidth;
        g_QuadVertices[1].pos.x += halfWidth;
        g_QuadVertices[2].pos.x += halfWidth;
        g_QuadVertices[3].pos.x += halfWidth;
    }
    if (vm->anchor & 2)
    {
        g_QuadVertices[0].pos.y += halfHeight;
        g_QuadVertices[1].pos.y += halfHeight;
        g_QuadVertices[2].pos.y += halfHeight;
        g_QuadVertices[3].pos.y += halfHeight;
    }
    return ZUN_SUCCESS;
}

// FUNCTION: th08 0x463cf0
ZunResult AnmManager::DrawCameraFacingQuad(AnmVm *vm)
{
    if (!vm->IsVisible())
        return ZUN_ERROR;
    if (!vm->flag1)
        return ZUN_ERROR;
    if (vm->color1.a == 0)
        return ZUN_ERROR;
    if (this->ProjectCameraFacingQuad(vm) != ZUN_SUCCESS)
        return ZUN_ERROR;
    return this->DrawInner(vm, 0);
}

// FUNCTION: th08 0x463d60
#pragma var_order(rotationMatrix, worldTransformMatrix, this)
void AnmManager::Project3DQuad(AnmVm *vm)
{
    D3DXMATRIX worldTransformMatrix;
    D3DXMATRIX rotationMatrix;

    if (!vm->flag16 && (vm->updateScale || vm->updateRotation))
    {
        vm->matrix2 = vm->matrix1;
        vm->matrix2._11 *= vm->scale.x;
        vm->matrix2._22 *= vm->scale.y;
        vm->updateScale = 0;

        if (vm->rotation.x != 0.0)
        {
            D3DXMatrixRotationX(&rotationMatrix, vm->rotation.x);
            D3DXMatrixMultiply(&vm->matrix2, &vm->matrix2, &rotationMatrix);
        }
        if (vm->rotation.y != 0.0)
        {
            D3DXMatrixRotationY(&rotationMatrix, vm->rotation.y);
            D3DXMatrixMultiply(&vm->matrix2, &vm->matrix2, &rotationMatrix);
        }
        if (vm->rotation.z != 0.0)
        {
            D3DXMatrixRotationZ(&rotationMatrix, vm->rotation.z);
            D3DXMatrixMultiply(&vm->matrix2, &vm->matrix2, &rotationMatrix);
        }
        vm->updateRotation = 0;
    }

    worldTransformMatrix = vm->matrix2;
    if ((vm->anchor & 1) == 0)
        worldTransformMatrix._41 = vm->pos.x;
    else
        worldTransformMatrix._41 = fabsf(vm->spriteSize.x * vm->scale.x / 2.0f) + vm->pos.x;

    if ((vm->anchor & 2) == 0)
        worldTransformMatrix._42 = vm->pos.y;
    else
        worldTransformMatrix._42 = fabsf(vm->spriteSize.y * vm->scale.y / 2.0f) + vm->pos.y;

    worldTransformMatrix._43 = vm->pos.z;

    D3DXVec3Project(reinterpret_cast<D3DXVECTOR3 *>(&g_QuadVertices[0].pos), reinterpret_cast<D3DXVECTOR3 *>(&this->untexturedVector[0].pos), &g_Supervisor.viewport,
                    &g_Supervisor.projectionMatrix, &g_Supervisor.viewMatrix, &worldTransformMatrix);
    D3DXVec3Project(reinterpret_cast<D3DXVECTOR3 *>(&g_QuadVertices[1].pos), reinterpret_cast<D3DXVECTOR3 *>(&this->untexturedVector[1].pos), &g_Supervisor.viewport,
                    &g_Supervisor.projectionMatrix, &g_Supervisor.viewMatrix, &worldTransformMatrix);
    D3DXVec3Project(reinterpret_cast<D3DXVECTOR3 *>(&g_QuadVertices[2].pos), reinterpret_cast<D3DXVECTOR3 *>(&this->untexturedVector[2].pos), &g_Supervisor.viewport,
                    &g_Supervisor.projectionMatrix, &g_Supervisor.viewMatrix, &worldTransformMatrix);
    D3DXVec3Project(reinterpret_cast<D3DXVECTOR3 *>(&g_QuadVertices[3].pos), reinterpret_cast<D3DXVECTOR3 *>(&this->untexturedVector[3].pos), &g_Supervisor.viewport,
                    &g_Supervisor.projectionMatrix, &g_Supervisor.viewMatrix, &worldTransformMatrix);

    this->cachedWorldMatrix = worldTransformMatrix;
}

// FUNCTION: th08 0x464070
ZunResult AnmManager::DrawProjected3DQuad(AnmVm *vm)
{
    if (!vm->IsVisible())
        return ZUN_ERROR;
    if (!vm->flag1)
        return ZUN_ERROR;
    if (vm->color1.a == 0)
        return ZUN_ERROR;
    this->Project3DQuad(vm);
    return this->DrawInner(vm, 0);
}

// FUNCTION: th08 0x4640e0
#pragma var_order(halfWidth, halfHeight, yOffset, xOffset, sine, worldMatrix, rotation, projectedReference, projectedPosition, delta, cosine, origin, this)
ZunResult AnmManager::ProjectCameraFacingQuadWithCallback(
    AnmVm *vm, AnmProjectedPositionCallback callback)
{
    f32 rotation;
    f32 sine;
    f32 cosine;
    f32 xOffset;
    f32 yOffset;
    f32 halfHeight;
    f32 halfWidth;

    rotation = vm->rotation.z;
#if defined(PSP)
    th08::psp::RenderSinCos(rotation, &sine, &cosine);
#else
    sincos(rotation, sine, cosine);
#endif

    D3DXMATRIX worldMatrix;
    Float3 projectedPosition;
    Float3 projectedReference;
    Float3 delta;
    Float3 origin(0.0f, 0.0f, 0.0f);

    D3DXMatrixIdentity(&worldMatrix);
    worldMatrix._41 = vm->pos.operator float *()[0];
    worldMatrix._42 = vm->pos.operator float *()[1];
    worldMatrix._43 = vm->pos.operator float *()[2];

    D3DXVec3Project(reinterpret_cast<D3DXVECTOR3 *>(&projectedPosition),
                    reinterpret_cast<D3DXVECTOR3 *>(&origin), &g_Supervisor.viewport,
                    &g_Supervisor.projectionMatrix, &g_Supervisor.viewMatrix, &worldMatrix);
    if (projectedPosition.z < 0.0f || projectedPosition.z > 1.0f)
        return ZUN_ERROR;

    D3DXVec3Project(reinterpret_cast<D3DXVECTOR3 *>(&projectedReference),
                    reinterpret_cast<D3DXVECTOR3 *>(&g_Background.cameraCurrent.right),
                    &g_Supervisor.viewport, &g_Supervisor.projectionMatrix,
                    &g_Supervisor.viewMatrix, &worldMatrix);
    delta = projectedReference - projectedPosition;
    xOffset = D3DXVec3Length(reinterpret_cast<D3DXVECTOR3 *>(&delta)) * 0.5f;
    halfWidth = xOffset * vm->spriteSize.x * vm->scale.x;
    halfHeight = xOffset * vm->spriteSize.y * vm->scale.y;

    if (callback != NULL)
        callback(vm, reinterpret_cast<D3DXVECTOR3 *>(&projectedPosition));

    xOffset = projectedPosition.x;
    yOffset = projectedPosition.y;
    this->TranslateRotation(&g_QuadVertices[0], -halfWidth, -halfHeight, sine, cosine, xOffset, yOffset);
    this->TranslateRotation(&g_QuadVertices[1], halfWidth, -halfHeight, sine, cosine, xOffset, yOffset);
    this->TranslateRotation(&g_QuadVertices[2], -halfWidth, halfHeight, sine, cosine, xOffset, yOffset);
    this->TranslateRotation(&g_QuadVertices[3], halfWidth, halfHeight, sine, cosine, xOffset, yOffset);

    g_QuadVertices[3].pos.z = projectedPosition.z;
    g_QuadVertices[2].pos.z = g_QuadVertices[3].pos.z;
    g_QuadVertices[1].pos.z = g_QuadVertices[2].pos.z;
    g_QuadVertices[0].pos.z = g_QuadVertices[1].pos.z;

    if (vm->anchor & 1)
    {
        g_QuadVertices[0].pos.x += halfWidth;
        g_QuadVertices[1].pos.x += halfWidth;
        g_QuadVertices[2].pos.x += halfWidth;
        g_QuadVertices[3].pos.x += halfWidth;
    }
    if (vm->anchor & 2)
    {
        g_QuadVertices[0].pos.y += halfHeight;
        g_QuadVertices[1].pos.y += halfHeight;
        g_QuadVertices[2].pos.y += halfHeight;
        g_QuadVertices[3].pos.y += halfHeight;
    }
    return ZUN_SUCCESS;
}

// FUNCTION: th08 0x464400
ZunResult AnmManager::DrawWithCallback(
    AnmVm *vm, AnmProjectedPositionCallback callback)
{
    if (!vm->IsVisible())
        return ZUN_ERROR;
    if (!vm->flag1)
        return ZUN_ERROR;
    if (vm->color1.a == 0)
        return ZUN_ERROR;
    if (this->ProjectCameraFacingQuadWithCallback(vm, callback) != ZUN_SUCCESS)
        return ZUN_ERROR;
    return this->DrawInner(vm, 0);
}

// FUNCTION: th08 0x00464470
#pragma var_order(textureMatrix, rotationMatrix, worldTransformMatrix, this)
ZunResult AnmManager::Draw3D(AnmVm *vm)
{
    D3DMATRIX textureMatrix;
    D3DXMATRIX rotationMatrix;
    D3DXMATRIX worldTransformMatrix;

    if (!vm->IsVisible())
    {
        return ZUN_ERROR;
    }
    if (!vm->flag1)
    {
        return ZUN_ERROR;
    }
    if (vm->color1.a == 0)
    {
        return ZUN_ERROR;
    }

    if (this->spritesToDraw != 0)
    {
        this->FlushVertexBuffer();
    }

    if (!vm->flag16 && (vm->updateScale || vm->updateRotation))
    {
        vm->matrix2 = vm->matrix1;
        vm->matrix2._11 *= vm->scale.x;
        vm->matrix2._22 *= vm->scale.y;
        vm->updateScale = 0;

        if (vm->rotation.x != 0.0)
        {
            D3DXMatrixRotationX(&rotationMatrix, vm->rotation.x);
            D3DXMatrixMultiply(&vm->matrix2, &vm->matrix2, &rotationMatrix);
        }
        if (vm->rotation.y != 0.0)
        {
            D3DXMatrixRotationY(&rotationMatrix, vm->rotation.y);
            D3DXMatrixMultiply(&vm->matrix2, &vm->matrix2, &rotationMatrix);
        }
        if (vm->rotation.z != 0.0)
        {
            D3DXMatrixRotationZ(&rotationMatrix, vm->rotation.z);
            D3DXMatrixMultiply(&vm->matrix2, &vm->matrix2, &rotationMatrix);
        }
        vm->updateRotation = 0;
    }

    worldTransformMatrix = vm->matrix2;
    if ((vm->anchor & 1) == 0)
    {
        worldTransformMatrix._41 = vm->pos.x;
    }
    else
    {
        worldTransformMatrix._41 = fabsf(vm->spriteSize.x * vm->scale.x / 2.0f) + vm->pos.x;
    }
    if ((vm->anchor & 2) == 0)
    {
        worldTransformMatrix._42 = vm->pos.y;
    }
    else
    {
        worldTransformMatrix._42 = fabsf(vm->spriteSize.y * vm->scale.y / 2.0f) + vm->pos.y;
    }
    worldTransformMatrix._41 += this->screenShakeOffset.x;
    worldTransformMatrix._42 += this->screenShakeOffset.y;

    this->SetRenderStateForVm3D(vm);
    worldTransformMatrix._43 = vm->pos.z;
    g_Supervisor.d3dDevice->SetTransform(D3DTS_WORLD, &worldTransformMatrix);

    if (this->currentSprite != vm->loadedSprite || vm->uvScrollPos.x != 0.0f || vm->uvScrollPos.x != 0.0f)
    {
        this->currentSprite = vm->loadedSprite;
        textureMatrix = vm->matrix3;
        textureMatrix._31 = vm->loadedSprite->uvStart.x + vm->uvScrollPos.x;
        textureMatrix._32 = vm->loadedSprite->uvStart.y + vm->uvScrollPos.y;
        g_Supervisor.d3dDevice->SetTransform(D3DTS_TEXTURE0, &textureMatrix);

        if (this->currentTexture != vm->loadedSprite->texture)
        {
            this->currentTexture = vm->loadedSprite->texture;
            g_Supervisor.d3dDevice->SetTexture(0, this->currentTexture);
        }
    }

    if (this->currentVertexShader != 2)
    {
        if (!g_Supervisor.IsVertexBufferDisabled())
        {
            g_Supervisor.d3dDevice->SetVertexShader(D3DFVF_XYZ | D3DFVF_TEX1);
            g_Supervisor.d3dDevice->SetStreamSource(0, this->quadVertexBuffer, sizeof(VertexDiffuseXyzrhw));
        }
        else
        {
            g_Supervisor.d3dDevice->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        }
        g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
        g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        this->currentVertexShader = 2;
    }

    if (!g_Supervisor.IsVertexBufferDisabled())
    {
        g_Supervisor.d3dDevice->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
    }
    else
    {
        g_Supervisor.d3dDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, g_BackgroundQuadVertices,
                                                 sizeof(VertexTex0Xyzrhw));
    }
    return ZUN_SUCCESS;
}

// FUNCTION: th08 0x4649a0
#pragma var_order(y, i, vertex, x, currentX, step, xSpan)
ZunResult AnmManager::InitializeHorizontalTextureStrip(AnmVm *vm, VertexTex1DiffuseXyzrhw *vertices, i32 vertexCount)
{
    f32 y;
    i32 i;
    VertexTex1DiffuseXyzrhw *vertex;
    f32 x;
    f32 currentX;
    f32 step;
    f32 xSpan;

    if (vertexCount < 3)
        return ZUN_ERROR;

    x = vm->loadedSprite->uvEnd.x + vm->uvScrollPos.x;
    xSpan = vm->loadedSprite->uvEnd.x - vm->loadedSprite->uvStart.x;
    y = vm->loadedSprite->uvStart.y + vm->uvScrollPos.y;
    vertex = vertices;
    step = xSpan / ((vertexCount + 1) / 2 - 1);
    i = 0;
    currentX = x;
    for (; i < vertexCount; i += 2, vertex += 2, currentX -= step)
    {
        vertex->textureUV.x = currentX;
        vertex->textureUV.y = y;
        vertex->diffuse = vm->color1.d3dColor;
        vertex->w = 1.0f;
    }

    y = vm->loadedSprite->uvEnd.y + vm->uvScrollPos.y;
    vertex = vertices + 1;
    i = 1;
    currentX = x;
    for (; i < vertexCount; i += 2, vertex += 2, currentX -= step)
    {
        vertex->textureUV.x = currentX;
        vertex->textureUV.y = y;
        vertex->diffuse = vm->color1.d3dColor;
        vertex->w = 1.0f;
    }
    return ZUN_SUCCESS;
}

// FUNCTION: th08 0x464b00
#pragma var_order(x, i, vertex, y, currentY, step, ySpan)
ZunResult AnmManager::InitializeVerticalTextureStrip(AnmVm *vm, VertexTex1DiffuseXyzrhw *vertices, i32 vertexCount)
{
    f32 x;
    i32 i;
    VertexTex1DiffuseXyzrhw *vertex;
    f32 y;
    f32 currentY;
    f32 step;
    f32 ySpan;

    if (vertexCount < 3)
        return ZUN_ERROR;

    y = vm->loadedSprite->uvEnd.y + vm->uvScrollPos.y;
    ySpan = vm->loadedSprite->uvEnd.y - vm->loadedSprite->uvStart.y;
    x = vm->loadedSprite->uvStart.x + vm->uvScrollPos.x;
    vertex = vertices;
    step = ySpan / ((vertexCount + 1) / 2 - 1);
    i = 0;
    currentY = y;
    for (; i < vertexCount; i += 2, vertex += 2, currentY -= step)
    {
        vertex->textureUV.y = currentY;
        vertex->textureUV.x = x;
        vertex->diffuse = vm->color1.d3dColor;
        vertex->w = 1.0f;
    }

    x = vm->loadedSprite->uvEnd.x + vm->uvScrollPos.x;
    vertex = vertices + 1;
    i = 1;
    currentY = y;
    for (; i < vertexCount; i += 2, vertex += 2, currentY -= step)
    {
        vertex->textureUV.y = currentY;
        vertex->textureUV.x = x;
        vertex->diffuse = vm->color1.d3dColor;
        vertex->w = 1.0f;
    }
    return ZUN_SUCCESS;
}

// FUNCTION: th08 0x00464c60
ZunResult AnmManager::DrawVertices(AnmVm *vm, VertexTex1DiffuseXyzrhw *vertices, i32 vertexCount)
{
    if (!vm->IsVisible())
        return ZUN_ERROR;
    if (!vm->flag1)
        return ZUN_ERROR;
    if (vm->color1.a == 0)
        return ZUN_ERROR;

    if (this->spritesToDraw != 0)
        this->FlushVertexBuffer();

    if (this->currentTexture != vm->loadedSprite->texture)
    {
        this->currentTexture = vm->loadedSprite->texture;
        g_Supervisor.d3dDevice->SetTexture(0, this->currentTexture);
    }

    if (this->currentVertexShader != 3)
    {
        g_Supervisor.d3dDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        this->currentVertexShader = 3;
    }

    this->SetRenderStateForVm(vm);

    if (!this->needsTextureFactorSetup)
    {
        this->needsTextureFactorSetup = 1;
        if (!g_Supervisor.IsVertexBufferDisabled())
        {
            g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        }
    }

    g_Supervisor.d3dDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, vertexCount - 2, vertices,
                                             sizeof(VertexTex1DiffuseXyzrhw));
    return ZUN_SUCCESS;
}

// FUNCTION: th08 0x00464dd0
ZunResult AnmManager::QueueSpriteQuad(AnmVm *vm, VertexTex1DiffuseXyzrhw *vertices)
{
    if (!vm->IsVisible())
    {
        return ZUN_ERROR;
    }
    if (!vm->flag1)
    {
        return ZUN_ERROR;
    }
    if (vm->color1.a == 0)
    {
        return ZUN_ERROR;
    }

    if (this->currentTexture != vm->loadedSprite->texture)
    {
        this->currentTexture = vm->loadedSprite->texture;
        this->FlushVertexBuffer();
        g_Supervisor.d3dDevice->SetTexture(0, this->currentTexture);
    }

    if (this->currentVertexShader != 1)
    {
        this->FlushVertexBuffer();
        this->currentVertexShader = 1;
    }

    this->SetRenderStateForVm(vm);
    this->AddSpriteToDrawBuffer(vertices);
    return ZUN_SUCCESS;
}

ZunResult AnmManager::DrawTriangleFan(AnmVm *vm, VertexDiffuseXyzrhw *vertices, i32 vertexCount)
{
    if (this->spritesToDraw != 0)
    {
        this->FlushVertexBuffer();
    }

    if (this->currentVertexShader != 4)
    {
        g_Supervisor.d3dDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        this->currentVertexShader = 4;
    }

    this->SetRenderStateForVm(vm);

    if (!g_Supervisor.IsColorCompositingDisabled())
    {
        g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    }

    g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);

    g_Supervisor.SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

    g_Supervisor.d3dDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, vertexCount - 2, vertices, sizeof(VertexDiffuseXyzrhw));

    g_AnmManager->ClearVertexShader();
    g_AnmManager->ClearColorOp();
    g_AnmManager->ClearBlendMode();
    g_AnmManager->ClearZWrite();

    if (!g_Supervisor.IsColorCompositingDisabled())
    {
        g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    }

    g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_Supervisor.d3dDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);

    return ZUN_SUCCESS;
}


// FUNCTION: th08 0x465070
AnmManager::AnmManager()
{
    memset((void *)this, 0, sizeof(AnmManager));

    g_AnmManagerUntexturedQuadVertices[0].w = g_AnmManagerUntexturedQuadVertices[1].w =
        g_AnmManagerUntexturedQuadVertices[2].w = g_AnmManagerUntexturedQuadVertices[3].w = 1.0f;
    g_AnmManagerUntexturedQuadVertices[0].textureUV.x = 0.0f;
    g_AnmManagerUntexturedQuadVertices[0].textureUV.y = 0.0f;
    g_AnmManagerUntexturedQuadVertices[1].textureUV.x = 1.0f;
    g_AnmManagerUntexturedQuadVertices[1].textureUV.y = 0.0f;
    g_AnmManagerUntexturedQuadVertices[2].textureUV.x = 0.0f;
    g_AnmManagerUntexturedQuadVertices[2].textureUV.y = 1.0f;
    g_AnmManagerUntexturedQuadVertices[3].textureUV.x = 1.0f;
    g_AnmManagerUntexturedQuadVertices[3].textureUV.y = 1.0f;

    g_QuadVertices[0].w = g_QuadVertices[1].w = g_QuadVertices[2].w = g_QuadVertices[3].w = 1.0f;
    g_QuadVertices[0].textureUV.x = 0.0f;
    g_QuadVertices[0].textureUV.y = 0.0f;
    g_QuadVertices[1].textureUV.x = 1.0f;
    g_QuadVertices[1].textureUV.y = 0.0f;
    g_QuadVertices[2].textureUV.x = 0.0f;
    g_QuadVertices[2].textureUV.y = 1.0f;
    g_QuadVertices[3].textureUV.x = 1.0f;
    g_QuadVertices[3].textureUV.y = 1.0f;

    this->quadVertexBuffer = NULL;
    this->currentTexture = NULL;
    this->currentBlendMode = 0;
    this->currentColorOp = 0;
    this->currentTextureFactor = 1;
    this->currentVertexShader = 0;
    this->cameraMode = (AnmCameraMode)0xff;
    this->disableZWrite = (AnmZWriteMode)0;
    this->captureAnmIdx = -1;
    this->captureSurfaceIdx = -1;
}

// FUNCTION: th08 0x465250
void AnmManager::SetupVertexBuffer()
{
    void *lockedVertexBuffer;

    this->untexturedVector[2].pos.x = -128.0f;
    this->untexturedVector[0].pos.x = -128.0f;
    this->untexturedVector[3].pos.x = 128.0f;
    this->untexturedVector[1].pos.x = 128.0f;
    this->untexturedVector[1].pos.y = -128.0f;
    this->untexturedVector[0].pos.y = -128.0f;
    this->untexturedVector[3].pos.y = 128.0f;
    this->untexturedVector[2].pos.y = 128.0f;
    this->untexturedVector[3].pos.z = 0.0f;
    this->untexturedVector[2].pos.z = 0.0f;
    this->untexturedVector[1].pos.z = 0.0f;
    this->untexturedVector[0].pos.z = 0.0f;
    this->untexturedVector[2].w = 0.0f;
    this->untexturedVector[0].w = 0.0f;
    this->untexturedVector[3].w = 1.0f;
    this->untexturedVector[1].w = 1.0f;
    *(u32 *)&this->untexturedVector[1].diffuse = 0;
    *(u32 *)&this->untexturedVector[0].diffuse = 0;
    *(u32 *)&this->untexturedVector[3].diffuse = 0x3f800000;
    *(u32 *)&this->untexturedVector[2].diffuse = 0x3f800000;

    g_BackgroundQuadVertices[0].pos = this->untexturedVector[0].pos;
    g_BackgroundQuadVertices[1].pos = this->untexturedVector[1].pos;
    g_BackgroundQuadVertices[2].pos = this->untexturedVector[2].pos;
    g_BackgroundQuadVertices[3].pos = this->untexturedVector[3].pos;
    *(u32 *)&g_BackgroundQuadVertices[0].textureUV.x = *(u32 *)&this->untexturedVector[0].w;
    *(u32 *)&g_BackgroundQuadVertices[0].textureUV.y = *(u32 *)&this->untexturedVector[0].diffuse;
    *(u32 *)&g_BackgroundQuadVertices[1].textureUV.x = *(u32 *)&this->untexturedVector[1].w;
    *(u32 *)&g_BackgroundQuadVertices[1].textureUV.y = *(u32 *)&this->untexturedVector[1].diffuse;
    *(u32 *)&g_BackgroundQuadVertices[2].textureUV.x = *(u32 *)&this->untexturedVector[2].w;
    *(u32 *)&g_BackgroundQuadVertices[2].textureUV.y = *(u32 *)&this->untexturedVector[2].diffuse;
    *(u32 *)&g_BackgroundQuadVertices[3].textureUV.x = *(u32 *)&this->untexturedVector[3].w;
    *(u32 *)&g_BackgroundQuadVertices[3].textureUV.y = *(u32 *)&this->untexturedVector[3].diffuse;

    if (!g_Supervisor.IsVertexBufferDisabled())
    {
        g_Supervisor.d3dDevice->CreateVertexBuffer(sizeof(this->untexturedVector), 0, D3DFVF_XYZ | D3DFVF_TEX1,
                                                   D3DPOOL_MANAGED, &this->quadVertexBuffer);
        this->quadVertexBuffer->Lock(0, 0, (BYTE **)&lockedVertexBuffer, 0);
        memcpy(lockedVertexBuffer, this->untexturedVector, sizeof(this->untexturedVector));
        this->quadVertexBuffer->Unlock();
        g_Supervisor.d3dDevice->SetStreamSource(0, g_AnmManager->quadVertexBuffer, sizeof(VertexDiffuseXyzrhw));
    }
}

i32 GetAnmFormat(i32 format)
{
    if (g_Supervisor.Is16bitColorMode() != 0)
    {
        if ((g_TextureFormatD3D8Mapping[format] == D3DFMT_A8R8G8B8) ||
            (g_TextureFormatD3D8Mapping[format] == D3DFMT_UNKNOWN))
        {
            format = 5;
        }
        else if (g_TextureFormatD3D8Mapping[format] == D3DFMT_R8G8B8)
        {
            format = 3;
        }
    }

    return format;
}

// FUNCTION: th08 0x465570
ZunResult AnmManager::CreateTextureFromFile(AnmEntry *entry, i32 format, i32 colorKey)
{
    format = GetAnmFormat(format);
    if (D3DXCreateTextureFromFileInMemoryEx(g_Supervisor.d3dDevice, entry->rawData, entry->size, 0, 0, 0, 0,
                                            g_TextureFormatD3D8Mapping[format], D3DPOOL_MANAGED, 3, -1, colorKey,
                                            NULL, NULL, &entry->texture) != D3D_OK)
    {
        return ZUN_ERROR;
    }
    return ZUN_SUCCESS;
}

#pragma var_order(surface, textureSurfaceLevel, header, lockedRect, currentY, textureSrc, textureDest)
ZunResult AnmManager::CreateTextureFromAnm(IDirect3DTexture8 **outTexture, AnmTextureHeader *textureHeader, i32 format)
{
    IDirect3DSurface8 *surface;
    IDirect3DSurface8 *textureSurfaceLevel;
    AnmTextureHeader *header;
    const void *textureSrc;
    void *textureDest;
    D3DLOCKED_RECT lockedRect;
    int currentY;

    surface = NULL;
    textureSurfaceLevel = NULL;
    format = GetAnmFormat(format);
    header = textureHeader;

#if defined(PSP)
    // Embedded ANM textures are immutable. The desktop D3DX route creates a
    // source image surface and a destination texture surface simultaneously;
    // a 512x512x16 image therefore needs two contiguous 512 KiB blocks. Feed
    // the archive pixels to the persistent PSP texture in small strips instead.
    if (D3DXCreateTexture(g_Supervisor.d3dDevice, header->width, header->height, 1, 0,
                          g_TextureFormatD3D8Mapping[format], D3DPOOL_MANAGED,
                          outTexture) == D3D_OK)
    {
        const BYTE *sourcePixels = reinterpret_cast<const BYTE *>(textureHeader) +
                                   sizeof(AnmTextureHeader);
        const D3DFORMAT sourceFormat = g_TextureFormatD3D8Mapping[header->format];
        const UINT sourcePitch = static_cast<UINT>(header->width) *
                                 g_TextureFormatBytesPerPixel[header->format];
        if (th08_linux_texture_upload_static(*outTexture, sourcePixels, sourcePitch,
                                             sourceFormat))
            return ZUN_SUCCESS;
        (*outTexture)->Release();
        *outTexture = NULL;
    }
#endif

    g_Supervisor.d3dDevice->CreateImageSurface(header->width, header->height,
                                               g_TextureFormatD3D8Mapping[header->format], &surface);

    surface->LockRect(&lockedRect, NULL, 0);

    for (currentY = 0; currentY < header->height; currentY++)
    {
        textureDest = (u8 *)lockedRect.pBits + currentY * lockedRect.Pitch;
        textureSrc = ((u8 *)textureHeader) + sizeof(AnmTextureHeader) +
                     (currentY * header->width * g_TextureFormatBytesPerPixel[header->format]);
        memcpy(textureDest, textureSrc, header->width * g_TextureFormatBytesPerPixel[header->format]);
    }

    surface->UnlockRect();

    if (D3DXCreateTexture(g_Supervisor.d3dDevice, header->width, header->height, 1, 0,
                          g_TextureFormatD3D8Mapping[format], D3DPOOL_MANAGED, outTexture) != D3D_OK)
    {
        goto err;
    }

    (*outTexture)->GetSurfaceLevel(0, &textureSurfaceLevel);

    if (D3DXLoadSurfaceFromSurface(textureSurfaceLevel, NULL, NULL, surface, NULL, NULL, 3, 0) != D3D_OK)
    {
        goto err;
    }

    if (surface != NULL)
    {
        surface->Release();
        surface = NULL;
    }
    if (textureSurfaceLevel != NULL)
    {
        textureSurfaceLevel->Release();
        textureSurfaceLevel = NULL;
    }

    return ZUN_SUCCESS;

err:
    if (surface != NULL)
    {
        surface->Release();
        surface = NULL;
    }
    if (textureSurfaceLevel != NULL)
    {
        textureSurfaceLevel->Release();
        textureSurfaceLevel = NULL;
    }

    return ZUN_ERROR;
}

ZunResult AnmManager::CreateEmptyTexture(IDirect3DTexture8 **outTexture, i32 width, i32 height, i32 format)
{
    D3DXCreateTexture(g_Supervisor.d3dDevice, width, height, 1, 0, g_TextureFormatD3D8Mapping[format], D3DPOOL_MANAGED,
                      outTexture);

    return ZUN_SUCCESS;
}

AnmLoaded *AnmManager::LoadAnm(i32 anmIdx, const char *filename)
{
    utils::DebugPrint("::loadAnim : %s\n", filename);
    AnmLoaded *anmLoaded = this->ReadAnmEntries(anmIdx, filename);
    if (anmLoaded != NULL)
    {
#if defined(PSP)
        const u32 loadGeneration = PspAnmLoadGeneration(anmIdx);
#endif
        StorePendingAnmEntries(anmLoaded, 1);

#if defined(FIX_REALLY_BAD_BUGS) || defined(PSP)
        while (anmLoaded != NULL && LoadPendingAnmEntries(anmLoaded) != 0)
#else
        /* ZUN bug: no NULL check! */
        while (LoadPendingAnmEntries(anmLoaded) != 0)
#endif
        {
            anmLoaded = this->PostloadAnmEntry(anmLoaded);
        }
#if defined(PSP)
        if (anmLoaded == NULL || !PspAnmLoadReady(anmIdx, loadGeneration))
            return NULL;
#endif
    }

    return anmLoaded;
}

#pragma var_order(curEntryNum, totalSprites, totalEntries, anmLoaded, entry, result, totalScripts, curEntry)
AnmLoaded *AnmManager::ReadAnmEntries(int anmIdx, const char *filename)
{
    i32 result;
    i32 fileSize = 0;

    utils::DebugPrint("::preloadAnim : %s\n", filename);

    if (anmIdx < 0 || anmIdx >= 25)
    {
        g_GameErrorContext.Fatal(TH_ERR_ANMMANAGER_NO_TEXTURE_STORAGE);
        return NULL;
    }

#if defined(PSP)
    const PspAnmPhase previousPhase = GetPspAnmPhase(anmIdx);
    if (previousPhase == PspAnmPhase::Loading ||
        previousPhase == PspAnmPhase::Finalizing)
    {
        fprintf(stderr,
                "TH08PSP ANM_LOAD phase=busy_reject anm=%ld file=%s\n",
                static_cast<long>(anmIdx), filename);
        return NULL;
    }
#endif

    this->ReleaseAnm(anmIdx);

#if defined(PSP)
    if (!BeginPspAnmLoad(anmIdx))
        return NULL;

    AnmRawEntry *entry = NULL;
    th08::psp::AnmScratchLease sourceLease{};
    const DWORD archiveSize = FileSystem::GetArchiveEntrySize(filename);
    if (archiveSize != 0 &&
        th08::psp::AnmScratchTryAcquire(static_cast<size_t>(archiveSize), anmIdx,
                                        filename, &sourceLease))
    {
        entry = reinterpret_cast<AnmRawEntry *>(FileSystem::OpenArchiveFileInto(
            filename, &fileSize, static_cast<LPBYTE>(sourceLease.base),
            th08::psp::AnmScratchCapacity()));
        if (entry == NULL)
        {
            th08::psp::AnmScratchRelease(sourceLease);
            ClearPspAnmState(anmIdx);
            return NULL;
        }
        PspAnmCompactState &state = g_PspAnmCompact[anmIdx];
        state.sourceIsScratch = true;
        state.sourceLease = sourceLease;
    }
    else
    {
        entry = reinterpret_cast<AnmRawEntry *>(
            FileSystem::OpenFile(filename, &fileSize, 0));
    }
#else
    AnmRawEntry *entry = (AnmRawEntry *)FileSystem::OpenFile(filename, &fileSize, 0);
#endif
    i32 totalEntries = 0;
    i32 totalScripts = 0;
    i32 totalSprites = 0;
    i32 curEntryNum = 0;

    AnmLoaded *anmLoaded = this->anmFiles + anmIdx;
    if (entry == NULL)
    {
#if defined(PSP)
        ClearPspAnmState(anmIdx);
#endif
        return NULL;
    }

    anmLoaded->anmIdx = anmIdx;
    anmLoaded->rawData = entry;
    AnmRawEntry *curEntry = entry;

#if defined(PSP)
    u32 validatedEntries = 0;
    u32 validatedSprites = 0;
    u32 validatedScripts = 0;
    if (fileSize <= 0 ||
        !PreparePspAnmCompact(anmIdx, entry, static_cast<u32>(fileSize),
                              &validatedEntries, &validatedSprites,
                              &validatedScripts) ||
        (g_PspAnmCompact[anmIdx].sourceIsScratch &&
         g_PspAnmCompact[anmIdx].data == NULL))
    {
        fprintf(stderr,
                "TH08PSP ANM_COMPACT phase=required_prepare_failed anm=%ld source=%ld scratch=%d\n",
                static_cast<long>(anmIdx), static_cast<long>(fileSize),
                g_PspAnmCompact[anmIdx].sourceIsScratch ? 1 : 0);
        SetPspAnmPhase(anmIdx, PspAnmPhase::Failed);
        this->ReleaseAnm(anmIdx);
        return NULL;
    }
    totalEntries = static_cast<i32>(validatedEntries);
    totalSprites = static_cast<i32>(validatedSprites);
    totalScripts = static_cast<i32>(validatedScripts);
#else
    while (true)
    {
        totalEntries++;
        totalScripts += curEntry->numScripts;
        totalSprites += curEntry->numSprites;

        if (curEntry->nextOffset == 0)
        {
            break;
        }

        curEntry = (AnmRawEntry *)(((u8 *)curEntry) + curEntry->nextOffset);
    }
#endif

    anmLoaded->totalEntries = totalEntries;

    anmLoaded->textures = static_cast<AnmEntry *>(
        g_ZunMemory.Alloc(static_cast<size_t>(totalEntries) * sizeof(AnmEntry),
                          "anm texture table"));
    anmLoaded->sprites = totalSprites > 0
                             ? static_cast<AnmLoadedSprite *>(g_ZunMemory.Alloc(
                                   static_cast<size_t>(totalSprites) * sizeof(AnmLoadedSprite),
                                   "anm sprite table"))
                             : NULL;
    anmLoaded->scripts = totalScripts > 0
                             ? static_cast<AnmRawInstr **>(g_ZunMemory.Alloc(
                                   static_cast<size_t>(totalScripts) * sizeof(AnmRawInstr *),
                                   "anm script table"))
                             : NULL;
    if (anmLoaded->textures == NULL ||
        (totalSprites > 0 && anmLoaded->sprites == NULL) ||
        (totalScripts > 0 && anmLoaded->scripts == NULL))
    {
#if defined(PSP)
        SetPspAnmPhase(anmIdx, PspAnmPhase::Failed);
        this->ReleaseAnm(anmIdx);
#endif
        return NULL;
    }
    memset(anmLoaded->textures, 0,
           sizeof(AnmEntry) * static_cast<size_t>(totalEntries));

    curEntry = entry;
    totalEntries = 0;
    totalSprites = 0;
    totalScripts = 0;

    while (true)
    {
        result = this->LoadExternalTextureData(anmLoaded, curEntryNum, &totalSprites, &totalScripts, curEntry);
        if (result < ZUN_SUCCESS)
        {
#if defined(PSP)
            this->ReleaseAnm(anmIdx);
#endif
            return NULL;
        }

        curEntryNum++;

        if (curEntry->nextOffset == 0)
        {
            break;
        }

        curEntry = (AnmRawEntry *)(((u8 *)curEntry) + curEntry->nextOffset);
    }

    return anmLoaded;
}

AnmLoaded *AnmManager::PreloadAnm(i32 anmIdx, const char *filename)
{
    AnmLoaded *anmLoaded = this->ReadAnmEntries(anmIdx, filename);
    if (anmLoaded == NULL)
    {
        return NULL;
    }
#if defined(PSP)
    const u32 loadGeneration = PspAnmLoadGeneration(anmIdx);
#endif

    /* AnmManager::ServicePreloadedAnims, called on the main thread every
     * frame through Supervisor::OnUpdate will process one entry on each
     * loading file until loading is finished
     */
    StorePendingAnmEntries(anmLoaded, 1);
    while (LoadPendingAnmEntries(anmLoaded) != 0 && !g_Supervisor.subthreadCloseRequestActive)
    {
        Sleep(1);
    }
    utils::DebugPrint("::preloadAnimEnd : %s\n", filename);

    if (g_Supervisor.subthreadCloseRequestActive)
        return NULL;
#if defined(PSP)
    return PspAnmLoadReady(anmIdx, loadGeneration) ? anmLoaded : NULL;
#else
    return anmLoaded;
#endif
}

// FUNCTION: th08 0x465ac0
#pragma var_order(result, startOfEntry, path, fileSize, fileData)
i32 AnmManager::LoadExternalTextureData(AnmLoaded *anmLoaded, i32 entryNumber, i32 *sprites, i32 *scripts,
                                        AnmRawEntry *rawEntry)
{
    i32 result = 0;
    AnmRawEntry *startOfEntry;
    const char *path = NULL;
    i32 fileSize;
    u8 *fileData;

    if (rawEntry == NULL)
    {
        g_GameErrorContext.Fatal(TH_ERR_ANMMANAGER_ANIMATION_CORRUPTED);
        return ZUN_ERROR;
    }

    startOfEntry = rawEntry;
    if (startOfEntry->version != 3)
    {
#if defined(PSP)
        const u32 *headerWords = reinterpret_cast<const u32 *>(startOfEntry);
        th08::psp::BootLog(
            "ANM bad_version phase=external anm=%d entry=%d ptr=0x%08lx "
            "words=%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,"
            "%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx\n",
            anmLoaded != NULL ? anmLoaded->anmIdx : -1,
            entryNumber,
            static_cast<unsigned long>(reinterpret_cast<uintptr_t>(startOfEntry)),
            static_cast<unsigned long>(headerWords[0]),
            static_cast<unsigned long>(headerWords[1]),
            static_cast<unsigned long>(headerWords[2]),
            static_cast<unsigned long>(headerWords[3]),
            static_cast<unsigned long>(headerWords[4]),
            static_cast<unsigned long>(headerWords[5]),
            static_cast<unsigned long>(headerWords[6]),
            static_cast<unsigned long>(headerWords[7]),
            static_cast<unsigned long>(headerWords[8]),
            static_cast<unsigned long>(headerWords[9]),
            static_cast<unsigned long>(headerWords[10]),
            static_cast<unsigned long>(headerWords[11]),
            static_cast<unsigned long>(headerWords[12]),
            static_cast<unsigned long>(headerWords[13]),
            static_cast<unsigned long>(headerWords[14]),
            static_cast<unsigned long>(headerWords[15]));
#endif
        g_GameErrorContext.Fatal(TH_ERR_ANMMANAGER_ANIMATION_WRONG_VERSION);
        return ZUN_ERROR;
    }

    if (!startOfEntry->hasData)
    {
        path = (const char *)((u8 *)startOfEntry + startOfEntry->nameOffset);
        if (path[0] != '@')
        {
            fileData = FileSystem::OpenFile(path, &fileSize, TRUE);
            if (fileData == NULL)
            {
                g_GameErrorContext.Fatal(TH_ERR_ANMMANAGER_EXTERN_TEXTURE_CORRUPTED, path);
                return ZUN_ERROR;
            }
            anmLoaded->textures[entryNumber].size = fileSize;
            anmLoaded->textures[entryNumber].rawData = fileData;
        }
    }

    return result + 1;
}

#pragma var_order(currentEntryNumber, currentNumSprites, entryLoadNumber, data, result, currentNumScripts, rawEntry)
AnmLoaded *AnmManager::PostloadAnmEntry(AnmLoaded *anmLoaded)
{
    i32 result;

    utils::DebugPrint("::postloadAnim : %d, %d\n", anmLoaded->anmIdx,
                      LoadPendingAnmEntries(anmLoaded));

    AnmRawEntry *rawData = anmLoaded->rawData;

    i32 entryLoadNumber = 0;
    i32 currentNumScripts = 0;
    i32 currentNumSprites = 0;
    i32 currentEntryNumber = 0;

    // Preserve the first entry as the loaded file owner while rawEntry walks
    // the continuation chain below.
    anmLoaded->rawData = rawData;
    AnmRawEntry *rawEntry = rawData;

    while (true)
    {
        if (entryLoadNumber == LoadPendingAnmEntries(anmLoaded) - 1 &&
            (result = this->LoadTextureData(anmLoaded, currentEntryNumber, currentNumSprites, currentNumScripts,
                                            rawEntry)) < ZUN_SUCCESS)
        {
#if defined(PSP)
            const i32 failedAnmIdx = anmLoaded->anmIdx;
            SetPspAnmPhase(failedAnmIdx, PspAnmPhase::Failed);
            this->ReleaseAnm(failedAnmIdx);
#endif
            StorePendingAnmEntries(anmLoaded, 0);
            return NULL;
        }

        currentNumSprites += rawEntry->numSprites;
        currentNumScripts += rawEntry->numScripts;
        currentEntryNumber++;

        if (rawEntry->nextOffset == 0)
        {
            break;
        }

        rawEntry = (AnmRawEntry *)(((u8 *)rawEntry) + rawEntry->nextOffset);
        entryLoadNumber++;

        if (entryLoadNumber == LoadPendingAnmEntries(anmLoaded))
        {
            StorePendingAnmEntries(anmLoaded, entryLoadNumber + 1);
            return anmLoaded;
        }
    }

#if defined(PSP)
    const bool scratchSource = g_PspAnmCompact[anmLoaded->anmIdx].sourceIsScratch;
    if (!FinalizePspAnmCompact(anmLoaded))
    {
        const i32 failedAnmIdx = anmLoaded->anmIdx;
        fprintf(stderr,
                "TH08PSP ANM_COMPACT phase=finalize_failed anm=%ld scratch=%d\n",
                static_cast<long>(failedAnmIdx), scratchSource ? 1 : 0);
        ReleasePspAnmCandidate(failedAnmIdx);
        if (scratchSource)
        {
            SetPspAnmPhase(failedAnmIdx, PspAnmPhase::Failed);
            this->ReleaseAnm(failedAnmIdx);
            StorePendingAnmEntries(anmLoaded, 0);
            return NULL;
        }
        // Heap-backed sources may safely remain in their original full form;
        // pass 1 made no script-pointer mutations.
        SetPspAnmPhase(failedAnmIdx, PspAnmPhase::Ready);
    }
#endif

    // Publish completion only after script pointers have been rebound, the
    // compact raw-data owner has been installed, and the source lease has
    // been released. The preload thread observes this with acquire ordering.
    StorePendingAnmEntries(anmLoaded, 0);

    return anmLoaded;
}

#pragma var_order(result, startOfEntry, surfaceDesc, path, rawSprite, i, currentOffset, loadedSprite)
int AnmManager::LoadTextureData(AnmLoaded *anmLoaded, i32 entryNumber, i32 currentSpriteNumber, i32 currentScriptNumber,
                                AnmRawEntry *rawEntry)
{
    int result = 0;
    AnmLoadedSprite loadedSprite;
    int i;
    const char *path = NULL;

    if (rawEntry == NULL)
    {
        g_GameErrorContext.Fatal(TH_ERR_ANMMANAGER_ANIMATION_CORRUPTED);
        return ZUN_ERROR;
    }

    AnmRawEntry *startOfEntry = rawEntry;

#if defined(PSP)
    LogPspAnmMemory("begin", anmLoaded, entryNumber, startOfEntry);
#endif

    if (startOfEntry->version != 3)
    {
#if defined(PSP)
        const u32 *headerWords = reinterpret_cast<const u32 *>(startOfEntry);
        th08::psp::BootLog(
            "ANM bad_version phase=texture anm=%d entry=%d ptr=0x%08lx "
            "words=%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,"
            "%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx\n",
            anmLoaded != NULL ? anmLoaded->anmIdx : -1,
            entryNumber,
            static_cast<unsigned long>(reinterpret_cast<uintptr_t>(startOfEntry)),
            static_cast<unsigned long>(headerWords[0]),
            static_cast<unsigned long>(headerWords[1]),
            static_cast<unsigned long>(headerWords[2]),
            static_cast<unsigned long>(headerWords[3]),
            static_cast<unsigned long>(headerWords[4]),
            static_cast<unsigned long>(headerWords[5]),
            static_cast<unsigned long>(headerWords[6]),
            static_cast<unsigned long>(headerWords[7]),
            static_cast<unsigned long>(headerWords[8]),
            static_cast<unsigned long>(headerWords[9]),
            static_cast<unsigned long>(headerWords[10]),
            static_cast<unsigned long>(headerWords[11]),
            static_cast<unsigned long>(headerWords[12]),
            static_cast<unsigned long>(headerWords[13]),
            static_cast<unsigned long>(headerWords[14]),
            static_cast<unsigned long>(headerWords[15]));
#endif
        g_GameErrorContext.Fatal(TH_ERR_ANMMANAGER_ANIMATION_WRONG_VERSION);
        return ZUN_ERROR;
    }

    if (!startOfEntry->hasData)
    {
        path = (const char *)(((u8 *)startOfEntry) + startOfEntry->nameOffset);

        if (path[0] == '@')
        {
            this->CreateEmptyTexture(&anmLoaded->textures[entryNumber].texture, startOfEntry->width,
                                     startOfEntry->height, startOfEntry->format);
        }
        else
        {
            if (this->CreateTextureFromFile(&anmLoaded->textures[entryNumber], startOfEntry->format,
                                            startOfEntry->colorKey) != ZUN_SUCCESS)
            {
                g_GameErrorContext.Fatal(TH_ERR_ANMMANAGER_EXTERN_TEXTURE_CORRUPTED, path);
                return ZUN_ERROR;
            }
        }
    }
    else
    {
        if (this->CreateTextureFromAnm(&anmLoaded->textures[entryNumber].texture,
                                       (AnmTextureHeader *)(((u8 *)startOfEntry) + startOfEntry->textureOffset),
                                       startOfEntry->format) != ZUN_SUCCESS)
        {
            g_GameErrorContext.Fatal(TH_ERR_ANMMANAGER_TEXTURE_CORRUPTED);
            return ZUN_ERROR;
        }
    }

#if defined(PSP)
    LogPspAnmMemory("created", anmLoaded, entryNumber, startOfEntry);
#endif

    anmLoaded->textures[entryNumber].texture->SetPriority(startOfEntry->priority);
#if defined(PSP)
    if (startOfEntry->hasData || (path != NULL && path[0] != '@'))
    {
        th08_linux_texture_mark_static(anmLoaded->textures[entryNumber].texture);
    }
#endif
    anmLoaded->textures[entryNumber].texture->PreLoad();
#if defined(PSP)
    LogPspAnmMemory("uploaded", anmLoaded, entryNumber, startOfEntry);
#endif

    D3DSURFACE_DESC surfaceDesc;

    anmLoaded->textures[entryNumber].texture->GetLevelDesc(0, &surfaceDesc);

    u32 *currentOffset = (u32 *)((u8 *)startOfEntry + sizeof(AnmRawEntry));

    AnmRawSprite *rawSprite;

    for (i = 0; i < startOfEntry->numSprites; i++, currentOffset++)
    {
        rawSprite = (AnmRawSprite *)((u8 *)startOfEntry + *currentOffset);

        loadedSprite.anmIdx = anmLoaded->anmIdx;
        loadedSprite.texture = anmLoaded->textures[entryNumber].texture;
        loadedSprite.scaleFactor.x = surfaceDesc.Width / (float)startOfEntry->width;
        loadedSprite.scaleFactor.y = surfaceDesc.Height / (float)startOfEntry->height;

        loadedSprite.startPixelInclusive.x = rawSprite->x * loadedSprite.scaleFactor.x;
        loadedSprite.startPixelInclusive.y = rawSprite->y * loadedSprite.scaleFactor.y;
        loadedSprite.endPixelInclusive.x = (rawSprite->x + rawSprite->width) * loadedSprite.scaleFactor.x;
        loadedSprite.endPixelInclusive.y = (rawSprite->y + rawSprite->height) * loadedSprite.scaleFactor.y;
        loadedSprite.width = surfaceDesc.Width;
        loadedSprite.height = surfaceDesc.Height;

        anmLoaded->LoadSprite(currentSpriteNumber, &loadedSprite);

        currentSpriteNumber++;
    }

    for (i = 0; i < startOfEntry->numScripts; i++, currentOffset += 2)
    {
        anmLoaded->scripts[currentScriptNumber] = (AnmRawInstr *)(((u8 *)startOfEntry) + currentOffset[1]);
        currentScriptNumber++;
    }

    return result + 1;
}

ZunResult AnmManager::ServicePreloadedAnims()
{
    for (int i = 0; i < ARRAY_SIZE(this->anmFiles); i++)
    {
        if (LoadPendingAnmEntries(this->anmFiles + i) != 0 &&
            this->PostloadAnmEntry(this->anmFiles + i) == NULL)
        {
            return ZUN_ERROR;
        }
    }

    return ZUN_SUCCESS;
}

void AnmManager::ReleaseAnm(i32 anmIdx)
{
    if (anmIdx < 0 || anmIdx >= ARRAY_SIZE(this->anmFiles))
    {
        return;
    }

    AnmLoaded &anm = this->anmFiles[anmIdx];
    if (anm.textures != NULL)
    {
        for (int i = 0; i < anm.totalEntries; i++)
        {
            this->ReleaseAnmEntry(&anm.textures[i]);
        }
        g_ZunMemory.Free(anm.textures);
        anm.textures = NULL;
    }
    if (anm.sprites != NULL)
    {
        g_ZunMemory.Free(anm.sprites);
        anm.sprites = NULL;
    }
    if (anm.scripts != NULL)
    {
        g_ZunMemory.Free(anm.scripts);
        anm.scripts = NULL;
    }

#if defined(PSP)
    ReleasePspAnmCandidate(anmIdx);
#endif

    if (anm.rawData != NULL)
    {
#if defined(PSP)
        if (!ReleasePspAnmScratchSource(anmIdx, anm.rawData))
#endif
            g_ZunMemory.Free(anm.rawData);
        anm.rawData = NULL;
    }

    memset(&anm, 0, sizeof(AnmLoaded));
#if defined(PSP)
    ClearPspAnmState(anmIdx);
#endif
}

void AnmManager::ReleaseAnmEntry(AnmEntry *entry)
{
    if (entry->texture != NULL)
    {
        entry->texture->Release();
        entry->texture = NULL;
    }
    if (entry->rawData != NULL)
    {
        g_ZunMemory.Free(entry->rawData);
        entry->rawData = NULL;
    }
}

void AnmLoaded::LoadSprite(i32 spriteIdx, AnmLoadedSprite *loadedSprite)
{
    this->sprites[spriteIdx] = *loadedSprite;

#if defined(PSP)
    // Keep linear filtering, but sample inside the requested atlas cell.
    // Integer UV edges blend with neighbouring cells after the 640x480 ->
    // 480x272 shrink; front.anm's repeated 32px frame tile then exposes a
    // horizontal HUD grid.  This is the same texel-centre rule used by the
    // hardware-tested TH07 PSP renderer.
    this->sprites[spriteIdx].uvStart.x =
        (this->sprites[spriteIdx].startPixelInclusive.x + 0.5f) /
        this->sprites[spriteIdx].width;
    this->sprites[spriteIdx].uvEnd.x =
        (this->sprites[spriteIdx].endPixelInclusive.x - 0.5f) /
        this->sprites[spriteIdx].width;
    this->sprites[spriteIdx].uvStart.y =
        (this->sprites[spriteIdx].startPixelInclusive.y + 0.5f) /
        this->sprites[spriteIdx].height;
    this->sprites[spriteIdx].uvEnd.y =
        (this->sprites[spriteIdx].endPixelInclusive.y - 0.5f) /
        this->sprites[spriteIdx].height;
#else
    this->sprites[spriteIdx].uvStart.x =
        this->sprites[spriteIdx].startPixelInclusive.x / (this->sprites[spriteIdx].width);
    this->sprites[spriteIdx].uvEnd.x = this->sprites[spriteIdx].endPixelInclusive.x / (this->sprites[spriteIdx].width);
    this->sprites[spriteIdx].uvStart.y =
        this->sprites[spriteIdx].startPixelInclusive.y / (this->sprites[spriteIdx].height);
    this->sprites[spriteIdx].uvEnd.y = this->sprites[spriteIdx].endPixelInclusive.y / (this->sprites[spriteIdx].height);
#endif
    this->sprites[spriteIdx].widthPx =
        (this->sprites[spriteIdx].endPixelInclusive.x - this->sprites[spriteIdx].startPixelInclusive.x) /
        (loadedSprite->scaleFactor.x);
    this->sprites[spriteIdx].heightPx =
        (this->sprites[spriteIdx].endPixelInclusive.y - this->sprites[spriteIdx].startPixelInclusive.y) /
        (loadedSprite->scaleFactor.y);
}

void AnmManager::DrawTextInner(IDirect3DTexture8 *outTexture, i32 x, i32 y, i32 width, i32 height, i32 fontWidth,
                               i32 fontHeight, COLORREF textColor, COLORREF outlineColor, const char *buffer,
                               float scaleFactorX, float scaleFactorY)
{
    if (fontWidth <= 0)
    {
        fontWidth = 15;
    }

    if (fontHeight <= 0)
    {
        fontHeight = 15;
    }

    if (fontWidth > 8)
    {
        TextHelper::RenderTextToTextureBold(x, y, width, height, fontWidth * scaleFactorX, fontHeight * scaleFactorY,
                                            textColor, outlineColor, buffer, outTexture);
    }
    else
    {
        TextHelper::RenderTextToTexture(x, y, width, height, 8, 8, textColor, outlineColor, buffer, outTexture);
    }
}

#pragma var_order(buf, fontWidth)
void AnmManager::DrawTextLeft(AnmVm *vm, COLORREF textColor, COLORREF shadowColor, const char *fmt, ...)
{
    char buf[128];
    int fontWidth = vm->fontWidth;

    va_list args;

    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);

    this->DrawTextInner(vm->loadedSprite->texture, vm->loadedSprite->startPixelInclusive.x,
                        vm->loadedSprite->startPixelInclusive.y, vm->loadedSprite->width, vm->loadedSprite->height,
                        fontWidth, vm->fontHeight, textColor, shadowColor, buf, vm->loadedSprite->scaleFactor.x,
                        vm->loadedSprite->scaleFactor.y);

    vm->visible = true;
}

// FUNCTION: th08 0x4664a0
#pragma var_order(buf, fontWidth)
void AnmManager::DrawTextRight(AnmVm *vm, COLORREF textColor, COLORREF shadowColor, const char *fmt, ...)
{
    char buf[128];
    int x;
    int fontWidth = vm->fontWidth <= 0 ? 15 : vm->fontWidth;

    va_list args;

    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);

    x = vm->loadedSprite->startPixelInclusive.x + vm->loadedSprite->widthPx * vm->loadedSprite->scaleFactor.x -
        (f32)strlen(buf) * fontWidth * vm->loadedSprite->scaleFactor.x / 2.0f;
    this->DrawTextInner(vm->loadedSprite->texture, x, vm->loadedSprite->startPixelInclusive.y,
                        vm->loadedSprite->width, vm->loadedSprite->height, fontWidth, vm->fontHeight, textColor,
                        shadowColor, buf, vm->loadedSprite->scaleFactor.x, vm->loadedSprite->scaleFactor.y);

    vm->visible = true;
}

// FUNCTION: th08 0x466650
#pragma var_order(buf, fontWidth)
void AnmManager::DrawTextCentered(AnmVm *vm, COLORREF textColor, COLORREF shadowColor, const char *fmt, ...)
{
    char buf[72];
    int x;
    int fontWidth = vm->fontWidth <= 0 ? 15 : vm->fontWidth;

    va_list args;

    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);

    x = vm->loadedSprite->startPixelInclusive.x + vm->loadedSprite->widthPx * vm->loadedSprite->scaleFactor.x / 2.0f -
        (f32)strlen(buf) * fontWidth * vm->loadedSprite->scaleFactor.x / 4.0f;
    this->DrawTextInner(vm->loadedSprite->texture, x, vm->loadedSprite->startPixelInclusive.y,
                        vm->loadedSprite->width, vm->loadedSprite->height, fontWidth, vm->fontHeight, textColor,
                        shadowColor, buf, vm->loadedSprite->scaleFactor.x, vm->loadedSprite->scaleFactor.y);

    vm->visible = true;
}

#pragma var_order(surface, fileSize, fileData)
ZunResult AnmManager::LoadSurface(i32 surfaceIdx, const char *filename)
{
    u8 *fileData;
    i32 fileSize;

#if defined(PSP)
    // TitleSetupThread owns only compressed I/O.  It must not release the old
    // framebuffer/texture-backed surface through PSPGL's process-global GL
    // context. Detach its preload first; the old GL owner stays valid until a
    // replacement has decoded successfully on the main render thread.
    fileData = this->surfaceData[surfaceIdx];
    fileSize = this->surfaceDataSizes[surfaceIdx];
    this->surfaceData[surfaceIdx] = NULL;
    this->surfaceDataSizes[surfaceIdx] = 0;
#endif

#if !defined(PSP)
    if (this->surfaces[surfaceIdx] != NULL || this->surfacesBis[surfaceIdx] != NULL)
    {
        this->ReleaseSurface(surfaceIdx);
    }
#endif

#if defined(PSP)
    if (fileData == NULL)
#else
    if (surfaceData[surfaceIdx] == NULL)
#endif
    {
#if defined(PSP) && defined(TH08_PSP_STAGE_POOL_ARENA)
        // Full-screen frontend images are decoded and released before a stage
        // can bind the gameplay pools.  Read the compressed source into the
        // retained, currently-idle stage arena so a fragmented heap is never
        // asked for a 600+ KiB contiguous PNG buffer on title return.
        fileData = NULL;
        const DWORD archiveSize = FileSystem::GetArchiveEntrySize(filename);
        if (archiveSize != 0)
        {
            fileData = static_cast<u8 *>(th08::psp::StagePoolArenaAcquireIdleTransient(
                static_cast<size_t>(archiveSize), filename));
            if (fileData != NULL &&
                FileSystem::OpenArchiveFileInto(filename, &fileSize, fileData,
                                                static_cast<size_t>(archiveSize)) == NULL)
            {
                g_ZunMemory.Free(fileData);
                fileData = NULL;
            }
        }
        if (fileData == NULL)
            fileData = FileSystem::OpenFile(filename, &fileSize, 0);
#else
        fileData = FileSystem::OpenFile(filename, &fileSize, 0);
#endif
        if (fileData == NULL)
        {
            g_GameErrorContext.Fatal(TH_ERR_CANNOT_BE_LOADED, filename);
            return ZUN_ERROR;
        }
    }
    else
    {
#if !defined(PSP)
        fileData = this->surfaceData[surfaceIdx];
        fileSize = this->surfaceDataSizes[surfaceIdx];
        this->surfaceData[surfaceIdx] = NULL;
#endif
    }

#if defined(PSP)
#if defined(TH08_PSP_SURFACE_SOURCE_DUMP) && TH08_PSP_SURFACE_SOURCE_DUMP
    // Opt-in bring-up evidence only. Product builds must not rewrite a 600+
    // KiB source beside the EBOOT on every title return; the durable decoder
    // breadcrumb below identifies the failing phase without Memory Stick I/O.
    if (surfaceIdx == 0 || surfaceIdx == 8)
    {
        const char *dumpName = surfaceIdx == 0 ? "TH08PSP_SURFACE0_IMAGE.bin"
                                               : "TH08PSP_SURFACE8_IMAGE.bin";
        FILE *dump = fopen(dumpName, "wb");
        if (dump != NULL)
        {
            fwrite(fileData, 1, static_cast<size_t>(fileSize), dump);
            fclose(dump);
        }
    }
#endif
    UINT decodedWidth = 0;
    UINT decodedHeight = 0;
    IDirect3DSurface8 *decodedSurface = NULL;
    if (!th08_linux_surface_load_image_memory(
            g_Supervisor.d3dDevice, fileData, static_cast<UINT>(fileSize),
            &decodedSurface, &decodedWidth, &decodedHeight))
    {
        // A transient scope/contention failure must not destroy the last good
        // picture or the only compressed retry source.
        this->surfaceData[surfaceIdx] = fileData;
        this->surfaceDataSizes[surfaceIdx] = fileSize;
        th08::psp::BootLog(
            "TITLE_SURFACE replace=ROLLED_BACK index=%ld source_retained=1 old_live=%d\n",
            static_cast<long>(surfaceIdx),
            (this->surfaces[surfaceIdx] != NULL ||
             this->surfacesBis[surfaceIdx] != NULL) ? 1 : 0);
        return ZUN_ERROR;
    }

    if (this->surfaces[surfaceIdx] != NULL || this->surfacesBis[surfaceIdx] != NULL)
        this->ReleaseSurface(surfaceIdx);
    this->surfacesBis[surfaceIdx] = decodedSurface;

    // The original path keeps a 640x1024 decode target plus two identical
    // exact-size surfaces.  On PSP the picture is immutable: retain one
    // RGB565 surface, publish its exact metadata without the mismatched
    // 28-byte D3DXIMAGE_INFO write, then let the persistent GE cache discard
    this->surfaceInfo[surfaceIdx].Width = decodedWidth;
    this->surfaceInfo[surfaceIdx].Height = decodedHeight;
    this->surfaceInfo[surfaceIdx].Depth = 1;
    this->surfaceInfo[surfaceIdx].MipLevels = 1;
    this->surfaceInfo[surfaceIdx].Format = D3DFMT_R5G6B5;
    th08_linux_surface_mark_static(this->surfacesBis[surfaceIdx]);
    fprintf(stderr, "TH08PSP SURFACE ready index=%ld size=%lux%lu format=RGB565\n",
            static_cast<long>(surfaceIdx), static_cast<unsigned long>(decodedWidth),
            static_cast<unsigned long>(decodedHeight));
    g_ZunMemory.Free(fileData);
    return ZUN_SUCCESS;
#else
    IDirect3DSurface8 *surface = NULL;
    if (g_Supervisor.d3dDevice->CreateImageSurface(640, 1024, g_Supervisor.presentParameters.BackBufferFormat,
                                                   &surface) != D3D_OK)
    {
        return ZUN_ERROR;
    }

    if (D3DXLoadSurfaceFromFileInMemory(surface, NULL, NULL, fileData, fileSize, NULL, 1, 0,
                                        (D3DXIMAGE_INFO *)&surfaceInfo[surfaceIdx]) != D3D_OK)
    {
        goto err;
    }

    if (g_Supervisor.d3dDevice->CreateRenderTarget(this->surfaceInfo[surfaceIdx].Width, surfaceInfo[surfaceIdx].Height,
                                                   g_Supervisor.presentParameters.BackBufferFormat, D3DMULTISAMPLE_NONE,
                                                   1, &this->surfaces[surfaceIdx]) != D3D_OK)
    {
        if (g_Supervisor.d3dDevice->CreateImageSurface(
                this->surfaceInfo[surfaceIdx].Width, this->surfaceInfo[surfaceIdx].Height,
                g_Supervisor.presentParameters.BackBufferFormat, &this->surfaces[surfaceIdx]) != D3D_OK)
        {
            goto err;
        }
    }

    if (g_Supervisor.d3dDevice->CreateImageSurface(
            this->surfaceInfo[surfaceIdx].Width, this->surfaceInfo[surfaceIdx].Height,
            g_Supervisor.presentParameters.BackBufferFormat, &this->surfacesBis[surfaceIdx]) != D3D_OK)
    {
        goto err;
    }

    if (D3DXLoadSurfaceFromSurface(this->surfaces[surfaceIdx], NULL, NULL, surface, NULL, NULL, D3DX_FILTER_NONE, 0) !=
        D3D_OK)
    {
        goto err;
    }

    if (D3DXLoadSurfaceFromSurface(this->surfacesBis[surfaceIdx], NULL, NULL, surface, NULL, NULL, D3DX_FILTER_NONE,
                                   0) != D3D_OK)
    {
        goto err;
    }

    if (surface != NULL)
    {
        surface->Release();
        surface = NULL;
    }
    g_ZunMemory.Free(fileData);

    return ZUN_SUCCESS;
err:
    if (surface != NULL)
    {
        surface->Release();
        surface = NULL;
    }
    g_ZunMemory.Free(fileData);

    return ZUN_ERROR;
#endif
}

#pragma var_order(fileSize, fileData)
ZunResult AnmManager::PreloadSurface(i32 surfaceIdx, const char *path)
{
    u32 fileSize;
    u8 *fileData;

#if defined(PSP)
    // This runs on TitleSetupThread. Keep all GL/proxy destruction on the
    // render thread; LoadSurface retires the old surface after atomically
    // detaching the compressed preload above. Only replace an abandoned I/O
    // preload here, which has no graphics ownership.
    if (this->surfaceData[surfaceIdx] != NULL)
    {
        g_ZunMemory.Free(this->surfaceData[surfaceIdx]);
        this->surfaceData[surfaceIdx] = NULL;
        this->surfaceDataSizes[surfaceIdx] = 0;
    }
    if (this->surfaces[surfaceIdx] != NULL || this->surfacesBis[surfaceIdx] != NULL)
    {
        th08::psp::BootLog(
            "TITLE_SURFACE preload=DEFERRED_RELEASE index=%ld main_thread_owner=1\n",
            static_cast<long>(surfaceIdx));
    }
#else
    if (this->surfaces[surfaceIdx] != NULL || this->surfacesBis[surfaceIdx] != NULL)
    {
        this->ReleaseSurface(surfaceIdx);
    }
#endif

#if defined(PSP) && defined(TH08_PSP_STAGE_POOL_ARENA)
    // TitleSetupThread preloads the compressed full-screen image while the
    // result/score lifetime may still be active. Give it its own idle-arena
    // loan instead of requiring another large contiguous heap block. The
    // subsequent decoder consumes and frees this exact pointer.
    fileData = NULL;
    const DWORD archiveSize = FileSystem::GetArchiveEntrySize(path);
    if (archiveSize != 0)
    {
        fileData = static_cast<u8 *>(th08::psp::StagePoolArenaAcquireIdleTransient(
            static_cast<size_t>(archiveSize), path));
        if (fileData != NULL &&
            FileSystem::OpenArchiveFileInto(path, reinterpret_cast<i32 *>(&fileSize),
                                            fileData,
                                            static_cast<size_t>(archiveSize)) == NULL)
        {
            g_ZunMemory.Free(fileData);
            fileData = NULL;
        }
    }
    if (fileData == NULL)
        fileData = FileSystem::OpenFile(path, reinterpret_cast<i32 *>(&fileSize), 0);
#else
    fileData = FileSystem::OpenFile(path, (i32 *)&fileSize, 0);
#endif
    if (fileData == NULL)
    {
        g_GameErrorContext.Fatal(TH_ERR_CANNOT_BE_LOADED, path);
        return ZUN_ERROR;
    }

    this->surfaceData[surfaceIdx] = fileData;
    this->surfaceDataSizes[surfaceIdx] = fileSize;

    return ZUN_SUCCESS;
}

void AnmManager::ReleaseSurface(i32 surfaceIdx)
{
    if (this->surfaces[surfaceIdx] != NULL)
    {
        this->surfaces[surfaceIdx]->Release();
        this->surfaces[surfaceIdx] = NULL;
    }
    if (this->surfacesBis[surfaceIdx] != NULL)
    {
        this->surfacesBis[surfaceIdx]->Release();
        this->surfacesBis[surfaceIdx] = NULL;
    }
    if (this->surfaceData[surfaceIdx] != NULL)
    {
        g_ZunMemory.Free(this->surfaceData[surfaceIdx]);
    }
    this->surfaceData[surfaceIdx] = NULL;
}

/* completely identical to EoSD. */
void AnmManager::CopySurfaceToBackbuffer(i32 surfaceIdx, i32 left, i32 top, i32 x, i32 y)
{
    if (this->surfacesBis[surfaceIdx] == NULL)
    {
        return;
    }

    IDirect3DSurface8 *destSurface;
    if (g_Supervisor.d3dDevice->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &destSurface) != D3D_OK)
    {
        return;
    }
#if !defined(PSP)
    if (this->surfaces[surfaceIdx] == NULL)
    {
        if (g_Supervisor.d3dDevice->CreateRenderTarget(
                this->surfaceInfo[surfaceIdx].Width, this->surfaceInfo[surfaceIdx].Height,
                g_Supervisor.presentParameters.BackBufferFormat, D3DMULTISAMPLE_NONE, TRUE,
                &this->surfaces[surfaceIdx]) != D3D_OK)
        {
            if (g_Supervisor.d3dDevice->CreateImageSurface(
                    this->surfaceInfo[surfaceIdx].Width, this->surfaceInfo[surfaceIdx].Height,
                    g_Supervisor.presentParameters.BackBufferFormat, &this->surfaces[surfaceIdx]) != D3D_OK)
            {
                destSurface->Release();
                return;
            }
        }
        if (D3DXLoadSurfaceFromSurface(this->surfaces[surfaceIdx], NULL, NULL, this->surfacesBis[surfaceIdx], NULL,
                                       NULL, D3DX_FILTER_NONE, 0) != D3D_OK)
        {
            destSurface->Release();
            return;
        }
    }
#endif

    RECT sourceRect;
    POINT destPoint;
    sourceRect.left = left;
    sourceRect.top = top;
    sourceRect.right = this->surfaceInfo[surfaceIdx].Width;
    sourceRect.bottom = this->surfaceInfo[surfaceIdx].Height;
    destPoint.x = x;
    destPoint.y = y;
    g_Supervisor.d3dDevice->CopyRects(
#if defined(PSP)
        this->surfacesBis[surfaceIdx],
#else
        this->surfaces[surfaceIdx],
#endif
        &sourceRect, 1, destSurface, &destPoint);
    destSurface->Release();
}

void AnmManager::CopySurfaceToBackbuffer2(i32 surfaceIdx, i32 rectX, i32 rectY, i32 rectLeft, i32 rectTop, i32 width,
                                          i32 height)
{
    if (this->surfacesBis[surfaceIdx] == NULL)
    {
        return;
    }

    IDirect3DSurface8 *backbuffer;
    if (g_Supervisor.d3dDevice->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backbuffer) != D3D_OK)
    {
#if defined(PSP)
        th08::psp::AnmScratchReleaseTransition();
#endif
        return;
    }

#if !defined(PSP)
    if (this->surfaces[surfaceIdx] == NULL)
    {
        if (g_Supervisor.d3dDevice->CreateRenderTarget(
                this->surfaceInfo[surfaceIdx].Width, this->surfaceInfo[surfaceIdx].Height,
                g_Supervisor.presentParameters.BackBufferFormat, D3DMULTISAMPLE_NONE, TRUE,
                &this->surfaces[surfaceIdx]) != D3D_OK)
        {
            if (g_Supervisor.d3dDevice->CreateImageSurface(
                    this->surfaceInfo[surfaceIdx].Width, this->surfaceInfo[surfaceIdx].Height,
                    g_Supervisor.presentParameters.BackBufferFormat, &this->surfaces[surfaceIdx]) != D3D_OK)
            {
                backbuffer->Release();
                return;
            }
        }

        if (D3DXLoadSurfaceFromSurface(this->surfaces[surfaceIdx], NULL, NULL, this->surfacesBis[surfaceIdx], NULL,
                                       NULL, D3DX_FILTER_NONE, 0) != D3D_OK)
        {
            backbuffer->Release();
            return;
        }
    }
#endif

    RECT rect;
    POINT point;
    rect.left = rectLeft;
    rect.top = rectTop;
    rect.right = rectLeft + width;
    rect.bottom = rectTop + height;
    point.x = rectX;
    point.y = rectY;
    g_Supervisor.d3dDevice->CopyRects(
#if defined(PSP)
        this->surfacesBis[surfaceIdx],
#else
        this->surfaces[surfaceIdx],
#endif
        &rect, 1, backbuffer, &point);
    backbuffer->Release();
}

// FUNCTION: th08 0x466f20
#pragma var_order(srcRect, textureSurface, backbuffer, dstRect, this)
void AnmManager::CaptureToTexture(i32 captureAnmIdx, i32 srcX, i32 srcY, i32 srcW, i32 srcH, i32 dstX, i32 dstY,
                                  i32 dstW, i32 dstH)
{
    IDirect3DSurface8 *backbuffer;
    IDirect3DSurface8 *textureSurface;
    RECT srcRect;
    RECT dstRect;

    if (this->anmFiles[captureAnmIdx].textures->texture == NULL)
    {
        return;
    }

    this->FlushVertexBuffer();

    if (g_Supervisor.d3dDevice->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backbuffer) != D3D_OK)
    {
        return;
    }

    if (this->anmFiles[captureAnmIdx].textures->texture->GetSurfaceLevel(0, &textureSurface) != D3D_OK)
    {
        backbuffer->Release();
        return;
    }

    srcRect.left = srcX;
    srcRect.top = srcY;
    srcRect.right = srcX + srcW;
    srcRect.bottom = srcY + srcH;
    dstRect.left = dstX;
    dstRect.top = dstY;
    dstRect.right = dstX + dstW;
    dstRect.bottom = dstY + dstH;

    if (D3DXLoadSurfaceFromSurface(textureSurface, NULL, &dstRect, backbuffer, NULL, &srcRect, -1, 0) != D3D_OK)
    {
        textureSurface->Release();
        backbuffer->Release();
        return;
    }

    textureSurface->Release();
    backbuffer->Release();
}

// FUNCTION: th08 0x467040
#pragma var_order(destSurface, srcSurface, this)
void AnmManager::CopyTextureRect(i32 dstAnmIdx, i32 dstEntryIdx, i32 srcAnmIdx, i32 srcEntryIdx, RECT *dstRect,
                                 RECT *srcRect)
{
    IDirect3DSurface8 *destSurface;
    IDirect3DSurface8 *srcSurface;

    if (this->anmFiles[dstAnmIdx].textures[dstEntryIdx].texture == NULL)
        return;
    if (this->anmFiles[srcAnmIdx].textures[srcEntryIdx].texture == NULL)
        return;

    this->FlushVertexBuffer();

    if (this->anmFiles[dstAnmIdx].textures[dstEntryIdx].texture->GetSurfaceLevel(0, &destSurface) != D3D_OK)
        return;
    if (this->anmFiles[srcAnmIdx].textures[srcEntryIdx].texture->GetSurfaceLevel(0, &srcSurface) != D3D_OK)
    {
        destSurface->Release();
        return;
    }

    if (D3DXLoadSurfaceFromSurface(destSurface, NULL, dstRect, srcSurface, NULL, srcRect, -1, 0) != D3D_OK)
    {
        destSurface->Release();
        srcSurface->Release();
        return;
    }

    destSurface->Release();
    srcSurface->Release();
}

#pragma var_order(srcRect, backbuffer, dstRect)
void AnmManager::CaptureToSurface(i32 captureSurfaceIdx, i32 srcX, i32 srcY, i32 srcW, i32 srcH, i32 dstX, i32 dstY,
                                  i32 dstW, i32 dstH, bool readDisplayedFrame)
{
    IDirect3DSurface8 *backbuffer;
    RECT srcRect;
    RECT dstRect;

    this->FlushVertexBuffer();

    if (this->surfaces[captureSurfaceIdx] != NULL ||
        this->surfacesBis[captureSurfaceIdx] != NULL)
    {
        this->ReleaseSurface(captureSurfaceIdx);
    }

    srcRect.left = srcX;
    srcRect.top = srcY;
    srcRect.right = srcX + srcW;
    srcRect.bottom = srcY + srcH;

    dstRect.left = dstX;
    dstRect.top = dstY;
    dstRect.right = dstX + dstW;
    dstRect.bottom = dstY + dstH;

    if (g_Supervisor.d3dDevice->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backbuffer) != D3D_OK)
    {
        return;
    }

    this->surfaceInfo[captureSurfaceIdx].Width = dstW;
    this->surfaceInfo[captureSurfaceIdx].Height = dstH;

#if defined(PSP)
    // Capture the physical framebuffer exactly once into a 512x512 RGB565 GE
    // texture. The original logical 640x480 dimensions remain published for
    // transition rectangles, but no 600 KiB CPU shadow survives this call.
    // Native readback and padding borrow the otherwise-idle ANM source scratch
    // for this phase only; persistent texture backing stays in the renderer
    // arena.
    this->surfaceInfo[captureSurfaceIdx].Format = D3DFMT_R5G6B5;
    if (!th08_linux_surface_capture_native(
            this->surfaceInfo[captureSurfaceIdx].Width,
            this->surfaceInfo[captureSurfaceIdx].Height,
            readDisplayedFrame,
            &this->surfacesBis[captureSurfaceIdx]))
    {
        goto out;
    }
#else
    if (g_Supervisor.d3dDevice->CreateRenderTarget(this->surfaceInfo[captureSurfaceIdx].Width,
                                                   this->surfaceInfo[captureSurfaceIdx].Height,
                                                   g_Supervisor.presentParameters.BackBufferFormat, D3DMULTISAMPLE_NONE,
                                                   1, &this->surfaces[captureSurfaceIdx]) != D3D_OK)
    {
        if (g_Supervisor.d3dDevice->CreateImageSurface(
                this->surfaceInfo[captureSurfaceIdx].Width, this->surfaceInfo[captureSurfaceIdx].Height,
                g_Supervisor.presentParameters.BackBufferFormat, &this->surfaces[captureSurfaceIdx]) != D3D_OK)
        {
            goto out;
        }
    }

    if (g_Supervisor.d3dDevice->CreateImageSurface(
            this->surfaceInfo[captureSurfaceIdx].Width, this->surfaceInfo[captureSurfaceIdx].Height,
            g_Supervisor.presentParameters.BackBufferFormat, &this->surfacesBis[captureSurfaceIdx]) != D3D_OK)
    {
        goto out;
    }

    if (D3DXLoadSurfaceFromSurface(this->surfaces[captureSurfaceIdx], NULL, &dstRect, backbuffer, NULL, &srcRect, -1,
                                   0) != D3D_OK)
    {
        goto out;
    }

    D3DXLoadSurfaceFromSurface(this->surfacesBis[captureSurfaceIdx], NULL, NULL, this->surfaces[captureSurfaceIdx],
                               NULL, NULL, -1, 0);
#endif

out:
#if defined(PSP)
    // The native capture helper normally releases the reservation itself. If
    // setup failed before entering it, make this path idempotently clean.
    if (th08::psp::AnmScratchTransitionActive())
        th08::psp::AnmScratchReleaseTransition();
    // The CPU shadow is only needed while copying this one capture.  Recreate
    // it on demand for the next transition instead of pinning it in the heap.
    th08_linux_surface_discard_readback(backbuffer);
#endif
    SAFE_RELEASE(backbuffer);
}


// FUNCTION: th08 0x45e960
void AnmManager::DrawPlayerBullet(AnmVm *vm)
{
    switch (vm->playerBulletHitAnimationType)
    {
    case 0:
        this->DrawNoRotation(vm);
        break;
    case 1:
        this->DrawNoRotationNoRound(vm);
        break;
    case 2:
        this->Draw2D(vm);
        break;
    case 3:
        this->Draw2DRotatedOrAxisAligned(vm);
        break;
    case 4:
        this->DrawCameraFacingQuad(vm);
        break;
    case 5:
        this->DrawProjected3DQuad(vm);
        break;
    }
}


#if TH08_PSP_GUI_BORDER_STATS_ENABLED
// Accessors for psp/gui_border_replay.cpp: the shared-batch state lives in
// this translation unit's anonymous namespace.
namespace psp
{
bool GuiBorderBatchActive()
{
    return g_PspBulletUnifiedQuadBatchActive;
}

bool GuiBorderBatchCanAppend(const AnmManager *manager, u32 vertexCount)
{
    return PspBulletUnifiedQuadBufferCanAppendVertices(manager, vertexCount);
}
} // namespace psp
#endif
}; // Namespace th08
