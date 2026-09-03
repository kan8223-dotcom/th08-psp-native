#include "th_pch.h"

#include "BulletManager.hpp"
#include "EclManager.hpp"
#include "GameManager.hpp"
#include "Gui.hpp"
#include "ItemManager.hpp"
#include "Player.hpp"
#include "ReplaySyncAudit.hpp"
#include "ReplayManager.hpp"
#include "Spellcard.hpp"

#if defined(PSP)
#include "item_atan2_audit.hpp"
#include "item_sincos_audit.hpp"
#include "item_update_subprofile.hpp"
#include "perf_attribution.hpp"
#include "render_perf_telemetry.hpp"
#if TH08_PSP_ITEM_TIME_INLINE_DRAW_ENABLED
#include <cmath>
#endif
#endif

#if defined(TH08_PSP_STAGE_POOL_ARENA)
#include "stage_pool_arena.hpp"
#endif

namespace th08
{

DIFFABLE_STATIC(ItemManager, g_ItemManager);
#if defined(PSP)
// Retail address 0x0164cf9c is Hscr::numPointItemsCollected inside
// g_GameManager, not the similarly named web-port field 0x100 bytes later.
#define g_MaxValuePointItemsCollected g_GameManager.hscr.numPointItemsCollected
#else
DIFFABLE_STATIC(i32, g_MaxValuePointItemsCollected);
#endif
DIFFABLE_STATIC_ARRAY_ASSIGN(i32, 6, g_PowerUpThresholds) = {8, 24, 48, 80, 128, 999};

#if defined(PSP)
namespace
{
// Every Item autocollect velocity computation goes through here.  The
// canonical FromAngleMagnitude call is unchanged unless the sin/cos product
// switch is on, in which case the double-float fast path supplies the
// identical binary32 pair when it accepts and FromAngleMagnitude runs
// otherwise.  The audit switch keeps the canonical call and shadow-compares.
inline void ItemAutocollectVelocityCompute(Item *item, f32 angle, f32 speed)
{
#if TH08_PSP_ITEM_SINCOS_FASTPATH_PRODUCT_ENABLED
    f32 velocityX = 0.0f;
    f32 velocityY = 0.0f;
    th08::psp::ItemSinCosFastpathReason reason =
        th08::psp::ItemSinCosFastpathReason::Count;
    if (th08::psp::ItemSinCosFastpathTry(angle, speed, &velocityX,
                                         &velocityY, &reason))
    {
        th08::psp::ItemSinCosProductNote(reason);
        item->startPositionOrVelocity.x = velocityX;
        item->startPositionOrVelocity.y = velocityY;
        return;
    }
    th08::psp::ItemSinCosProductNote(reason);
    item->startPositionOrVelocity.FromAngleMagnitude(angle, speed);
#else
#if TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED
    std::uint64_t sinCosStartUs = 0U;
    const bool sinCosSampled =
        th08::psp::ItemSinCosAuditBeginCall(&sinCosStartUs);
#endif
    item->startPositionOrVelocity.FromAngleMagnitude(angle, speed);
#if TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED
    th08::psp::ItemSinCosAuditAfterCanonical(
        angle, speed, item->startPositionOrVelocity.x,
        item->startPositionOrVelocity.y, sinCosSampled, sinCosStartUs);
#endif
#endif
}
} // namespace
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT) && \
    TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT
namespace
{
struct ItemAutocollectCacheAuditEntry
{
    u32 angleBits;
    u32 speedBits;
    u32 velocityXBits;
    u32 velocityYBits;
};

ItemAutocollectCacheAuditEntry
    g_ItemAutocollectCacheAuditEntries[MAX_ITEMS];
u32 g_ItemAutocollectCacheAuditValidBits[(MAX_ITEMS + 31) / 32];
ItemAutocollectCacheAuditStats g_ItemAutocollectCacheAuditStats;

u32 ItemAutocollectAuditFloatBits(f32 value)
{
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void ResetItemAutocollectCacheAudit()
{
    memset(g_ItemAutocollectCacheAuditEntries, 0,
           sizeof(g_ItemAutocollectCacheAuditEntries));
    memset(g_ItemAutocollectCacheAuditValidBits, 0,
           sizeof(g_ItemAutocollectCacheAuditValidBits));
    memset(&g_ItemAutocollectCacheAuditStats, 0,
           sizeof(g_ItemAutocollectCacheAuditStats));
}

void ObserveItemAutocollectCanonicalResult(ItemManager *manager, Item *item,
                                           f32 angle, f32 speed)
{
    ++g_ItemAutocollectCacheAuditStats.canonicalCalls;

    const i32 itemIndex = static_cast<i32>(item - manager->items);
    if (itemIndex < 0 || itemIndex >= MAX_ITEMS)
    {
        ++g_ItemAutocollectCacheAuditStats.invalidSlotObservations;
        return;
    }

    const u32 angleBits = ItemAutocollectAuditFloatBits(angle);
    const u32 speedBits = ItemAutocollectAuditFloatBits(speed);
    const u32 velocityXBits =
        ItemAutocollectAuditFloatBits(item->startPositionOrVelocity.x);
    const u32 velocityYBits =
        ItemAutocollectAuditFloatBits(item->startPositionOrVelocity.y);
    const u32 validMask = 1U << (itemIndex & 31);
    ItemAutocollectCacheAuditEntry &entry =
        g_ItemAutocollectCacheAuditEntries[itemIndex];

    if ((g_ItemAutocollectCacheAuditValidBits[itemIndex >> 5] & validMask) !=
            0U &&
        entry.angleBits == angleBits && entry.speedBits == speedBits)
    {
        ++g_ItemAutocollectCacheAuditStats.exactInputRepeats;
        if (entry.velocityXBits == velocityXBits &&
            entry.velocityYBits == velocityYBits)
        {
            ++g_ItemAutocollectCacheAuditStats.exactOutputMatches;
        }
        else
        {
            ++g_ItemAutocollectCacheAuditStats.exactOutputMismatches;
        }
    }

    entry.angleBits = angleBits;
    entry.speedBits = speedBits;
    entry.velocityXBits = velocityXBits;
    entry.velocityYBits = velocityYBits;
    g_ItemAutocollectCacheAuditValidBits[itemIndex >> 5] |= validMask;
}
} // namespace

const ItemAutocollectCacheAuditStats &GetItemAutocollectCacheAuditStats()
{
    return g_ItemAutocollectCacheAuditStats;
}
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_AUTOCOLLECT_CACHE) && \
    TH08_PSP_ITEM_AUTOCOLLECT_CACHE
namespace
{
constexpr u32 kItemAutocollectCacheCapacity = 1024U;
constexpr u32 kItemAutocollectCacheMask =
    kItemAutocollectCacheCapacity - 1U;
constexpr u8 kItemAutocollectCacheInvalidTag = 0xffU;

struct ItemAutocollectCacheEntry
{
    u32 angleBits;
    u32 speedBits;
    u32 velocityXBits;
    u32 velocityYBits;
};

ItemAutocollectCacheEntry
    g_ItemAutocollectCacheEntries[kItemAutocollectCacheCapacity];
u8 g_ItemAutocollectCacheSlotTags[kItemAutocollectCacheCapacity];
ItemAutocollectCacheStats g_ItemAutocollectCacheStats;

u32 ItemAutocollectCacheFloatBits(f32 value)
{
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void ResetItemAutocollectCache()
{
    // Entries need no initialization: an invalid tag makes every first access
    // canonical and initializes the complete entry before it can be read.
    memset(g_ItemAutocollectCacheSlotTags,
           kItemAutocollectCacheInvalidTag,
           sizeof(g_ItemAutocollectCacheSlotTags));
    memset(&g_ItemAutocollectCacheStats, 0,
           sizeof(g_ItemAutocollectCacheStats));
}

void SetItemAutocollectVelocity(ItemManager *manager, Item *item,
                                f32 angle, f32 speed)
{
#if defined(TH08_REPLAY_SYNC_AUDIT) && TH08_REPLAY_SYNC_AUDIT
    ++g_ItemAutocollectCacheStats.lookups;
#endif

    const i32 itemIndex = static_cast<i32>(item - manager->items);
    if (itemIndex < 0 || itemIndex >= MAX_ITEMS)
    {
#if defined(TH08_REPLAY_SYNC_AUDIT) && TH08_REPLAY_SYNC_AUDIT
        ++g_ItemAutocollectCacheStats.misses;
        ++g_ItemAutocollectCacheStats.invalidSlotLookups;
#endif
        ItemAutocollectVelocityCompute(item, angle, speed);
        return;
    }

    const u32 slot = static_cast<u32>(itemIndex);
    const u32 cacheIndex = slot & kItemAutocollectCacheMask;
    const u8 slotTag = static_cast<u8>(slot >> 10U);
    const u32 angleBits = ItemAutocollectCacheFloatBits(angle);
    const u32 speedBits = ItemAutocollectCacheFloatBits(speed);
    ItemAutocollectCacheEntry &entry =
        g_ItemAutocollectCacheEntries[cacheIndex];

    if (g_ItemAutocollectCacheSlotTags[cacheIndex] == slotTag &&
        entry.angleBits == angleBits && entry.speedBits == speedBits)
    {
        memcpy(&item->startPositionOrVelocity.x, &entry.velocityXBits,
               sizeof(entry.velocityXBits));
        memcpy(&item->startPositionOrVelocity.y, &entry.velocityYBits,
               sizeof(entry.velocityYBits));
#if defined(TH08_REPLAY_SYNC_AUDIT) && TH08_REPLAY_SYNC_AUDIT
        ++g_ItemAutocollectCacheStats.hits;
#endif
        return;
    }

#if defined(TH08_REPLAY_SYNC_AUDIT) && TH08_REPLAY_SYNC_AUDIT
    ++g_ItemAutocollectCacheStats.misses;
    if (g_ItemAutocollectCacheSlotTags[cacheIndex] !=
            kItemAutocollectCacheInvalidTag &&
        g_ItemAutocollectCacheSlotTags[cacheIndex] != slotTag)
    {
        ++g_ItemAutocollectCacheStats.conflicts;
    }
#endif

    ItemAutocollectVelocityCompute(item, angle, speed);
    entry.angleBits = angleBits;
    entry.speedBits = speedBits;
    entry.velocityXBits =
        ItemAutocollectCacheFloatBits(item->startPositionOrVelocity.x);
    entry.velocityYBits =
        ItemAutocollectCacheFloatBits(item->startPositionOrVelocity.y);
    g_ItemAutocollectCacheSlotTags[cacheIndex] = slotTag;
}
} // namespace

const ItemAutocollectCacheStats &GetItemAutocollectCacheStats()
{
    return g_ItemAutocollectCacheStats;
}
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT) && \
    TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT
namespace
{
constexpr i32 kItemTimeScriptIndex = 68;
constexpr i32 kItemTimeInitialSpriteIndex = 179;
constexpr u32 kItemTimeScriptFingerprintBytes = 68;

ItemTimeSpawnInitAuditStats g_ItemTimeSpawnInitAuditStats;

void ResetItemTimeSpawnInitAudit()
{
    memset(&g_ItemTimeSpawnInitAuditStats, 0,
           sizeof(g_ItemTimeSpawnInitAuditStats));
    g_ItemTimeSpawnInitAuditStats.observedStageFrame = 0xffffffffU;
    g_ItemTimeSpawnInitAuditStats.peakCandidatesStageFrame = 0xffffffffU;
    g_ItemTimeSpawnInitAuditStats.peakEligibleStageFrame = 0xffffffffU;
}

void BeginItemTimeSpawnInitAuditCandidate()
{
    const u32 stageFrame = g_GameManager.stageActiveFrames;
    if (g_ItemTimeSpawnInitAuditStats.observedStageFrame != stageFrame)
    {
        g_ItemTimeSpawnInitAuditStats.observedStageFrame = stageFrame;
        g_ItemTimeSpawnInitAuditStats.currentFrameCandidates = 0;
        g_ItemTimeSpawnInitAuditStats.currentFrameEligible = 0;
    }
    ++g_ItemTimeSpawnInitAuditStats.currentFrameCandidates;
    if (g_ItemTimeSpawnInitAuditStats.currentFrameCandidates >
        g_ItemTimeSpawnInitAuditStats.peakCandidatesPerFrame)
    {
        g_ItemTimeSpawnInitAuditStats.peakCandidatesPerFrame =
            g_ItemTimeSpawnInitAuditStats.currentFrameCandidates;
        g_ItemTimeSpawnInitAuditStats.peakCandidatesStageFrame = stageFrame;
    }
}

void NoteItemTimeSpawnInitAuditEligible()
{
    ++g_ItemTimeSpawnInitAuditStats.currentFrameEligible;
    if (g_ItemTimeSpawnInitAuditStats.currentFrameEligible >
        g_ItemTimeSpawnInitAuditStats.peakEligiblePerFrame)
    {
        g_ItemTimeSpawnInitAuditStats.peakEligiblePerFrame =
            g_ItemTimeSpawnInitAuditStats.currentFrameEligible;
        g_ItemTimeSpawnInitAuditStats.peakEligibleStageFrame =
            g_ItemTimeSpawnInitAuditStats.observedStageFrame;
    }
}

bool ValidateItemTimeSpawnScript68(const AnmRawInstr *sprite)
{
    if (sprite == NULL || sprite->opcode != AnmOpcode_Sprite ||
        sprite->instructionSize != 12 || sprite->time != 0 ||
        sprite->varMask != 0 ||
        sprite->intArgs[0] != kItemTimeInitialSpriteIndex)
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

// Construct exactly the state produced by SetAndExecuteScriptIdx(script 68)
// without running the generic opcode dispatcher.  The caller supplies a full
// copy of the pre-call VM: only AnmVmBase is cleared because that is the exact
// boundary used by AnmVmBase::Initialize.  Unwritten tail bytes therefore keep
// their canonical stale-slot semantics and remain part of the 0x2a4-byte
// comparison.
ZunBool InitializeItemTimeScript68Shadow(
    AnmVm *vm, AnmLoaded *owner, AnmRawInstr *script68,
    u32 *scriptsStarted, u32 *scriptsExecuted)
{
    memset(static_cast<AnmVmBase *>(vm), 0, sizeof(AnmVmBase));
    vm->scale.x = 1.0f;
    vm->scale.y = 1.0f;
    vm->color1.d3dColor = COLOR_WHITE;
    D3DXMatrixIdentity(&vm->matrix1);
    vm->flags = 7;
    vm->currentTimeInScript.Initialize();

    // SetAndExecuteScriptIdx writes these tail fields before entering
    // SetAndExecuteScript.  Initialize clears only the base, not scriptIndex.
    vm->scriptIndex = kItemTimeScriptIndex;
    vm->anmFileIndex = owner->anmIdx;
    vm->anmFile = owner;
    vm->flip = 0;
    vm->beginningOfScript = script68;
    vm->currentInstruction = script68;
    vm->currentTimeInScript = 0;
    vm->visible = FALSE;

    // The sole time-zero instruction is Sprite 179.  Mirror SetSprite's store
    // order and arithmetic after the ready/generation gate has proved that the
    // immutable sprite table belongs to this exact ANM generation.
    vm->visible = TRUE;
    vm->anmFile = owner;
    vm->activeSpriteIndex = kItemTimeInitialSpriteIndex;
    vm->loadedSprite = &owner->sprites[kItemTimeInitialSpriteIndex];
    vm->spriteSize.x = vm->loadedSprite->widthPx;
    vm->spriteSize.y = vm->loadedSprite->heightPx;
    D3DXMatrixIdentity(&vm->matrix1);
    D3DXMatrixIdentity(&vm->matrix3);
    vm->matrix1.m[0][0] = vm->spriteSize.x / 256.0f;
    vm->matrix1.m[1][1] = vm->spriteSize.y / 256.0f;
    vm->matrix3.m[0][0] =
        (vm->spriteSize.x / vm->loadedSprite->width) *
        vm->loadedSprite->scaleFactor.x;
    vm->matrix3.m[1][1] =
        (vm->spriteSize.y / vm->loadedSprite->height) *
        vm->loadedSprite->scaleFactor.y;
    vm->matrix2 = vm->matrix1;
    vm->timeOfLastSpriteSet = static_cast<i32>(vm->currentTimeInScript);
    vm->currentInstruction = reinterpret_cast<AnmRawInstr *>(
        reinterpret_cast<u8 *>(script68) + script68->instructionSize);

    // ExecuteScript's common tail is observable even though every dynamic
    // source was zeroed.  Keep its exact operation order and timer helper.
    vm->uvScrollPos.x += vm->uvScrollVel.x;
    if (vm->uvScrollPos.x >= 1.0f)
        vm->uvScrollPos.x -= 1.0f;
    else if (vm->uvScrollPos.x < 0.0f)
        vm->uvScrollPos.x += 1.0f;
    vm->uvScrollPos.y += vm->uvScrollVel.y;
    if (vm->uvScrollPos.y >= 1.0f)
        vm->uvScrollPos.y -= 1.0f;
    else if (vm->uvScrollPos.y < 0.0f)
        vm->uvScrollPos.y += 1.0f;
    vm->currentTimeInScript++;
    ++*scriptsExecuted;
    ++*scriptsStarted;
    return FALSE;
}

bool ItemTimeSpawnPointerFieldsMatch(const AnmVm &canonical,
                                     const AnmVm &shadow)
{
    return canonical.anmFile == shadow.anmFile &&
           canonical.beginningOfScript == shadow.beginningOfScript &&
           canonical.currentInstruction == shadow.currentInstruction &&
           canonical.loadedSprite == shadow.loadedSprite &&
           canonical.interruptReturnInstruction ==
               shadow.interruptReturnInstruction;
}

bool ItemTimeSpawnSpriteFieldsMatch(const AnmVm &canonical,
                                    const AnmVm &shadow)
{
    return canonical.activeSpriteIndex == shadow.activeSpriteIndex &&
           memcmp(&canonical.spriteSize, &shadow.spriteSize,
                  sizeof(canonical.spriteSize)) == 0 &&
           memcmp(&canonical.matrix1, &shadow.matrix1,
                  sizeof(canonical.matrix1)) == 0 &&
           memcmp(&canonical.matrix2, &shadow.matrix2,
                  sizeof(canonical.matrix2)) == 0 &&
           memcmp(&canonical.matrix3, &shadow.matrix3,
                  sizeof(canonical.matrix3)) == 0;
}

bool ItemTimeSpawnNamedFieldsMatch(const AnmVm &canonical,
                                   const AnmVm &shadow,
                                   bool pointersMatch, bool spriteMatch)
{
    return pointersMatch && spriteMatch &&
           canonical.anmFileIndex == shadow.anmFileIndex &&
           canonical.scriptIndex == shadow.scriptIndex &&
           canonical.timeOfLastSpriteSet == shadow.timeOfLastSpriteSet &&
           canonical.flagsWord == shadow.flagsWord &&
           memcmp(&canonical.currentTimeInScript,
                  &shadow.currentTimeInScript,
                  sizeof(canonical.currentTimeInScript)) == 0;
}

void NoteItemTimeSpawnInitFallback(u32 *specificFallback)
{
    ++g_ItemTimeSpawnInitAuditStats.canonicalFallbacks;
    ++*specificFallback;
    th08::psp::RenderPerfNoteItemTimeSpawnInitAudit(false, false, false,
                                                     true);
}

void SetAndAuditItemTimeSpawnScript(Item *item, AnmLoaded *owner,
                                    i32 scriptIndex)
{
    if (item->itemType != ITEM_TIME || scriptIndex != kItemTimeScriptIndex)
    {
        owner->SetAndExecuteScriptIdx(&item->sprite, scriptIndex);
        return;
    }

    ++g_ItemTimeSpawnInitAuditStats.candidates;
    BeginItemTimeSpawnInitAuditCandidate();
    AnmVm before;
    memcpy(&before, &item->sprite, sizeof(before));

    AnmRawInstr *scriptBefore = NULL;
    u32 generationBefore = 0;
    bool readyBefore = false;
    bool tablesBefore = false;
    if (owner != NULL)
    {
        generationBefore = owner->PspLoadGenerationForSpawnInitAudit();
        readyBefore = generationBefore != 0 &&
                      owner->PspLoadReadyForSpawnInitAudit(generationBefore);
        tablesBefore = readyBefore &&
                       owner->PspItemTimeSpawnInitTablesContain(
                           generationBefore, kItemTimeScriptIndex,
                           kItemTimeInitialSpriteIndex);
        if (tablesBefore)
            scriptBefore = owner->scripts[kItemTimeScriptIndex];
    }

    const u32 startedBefore = g_AnmManager->scriptsStartedThisFrame;
    const u32 executedBefore = g_AnmManager->scriptsExecutedThisFrame;

    // This is the only authoritative call.  No mismatch can affect gameplay,
    // draw order, counters, or replay state because the candidate below lives
    // solely in a stack copy.
    owner->SetAndExecuteScriptIdx(&item->sprite, scriptIndex);

    const u32 startedAfter = g_AnmManager->scriptsStartedThisFrame;
    const u32 executedAfter = g_AnmManager->scriptsExecutedThisFrame;
    const u32 generationAfter =
        owner->PspLoadGenerationForSpawnInitAudit();
    const bool readyAfter =
        generationAfter != 0 &&
        owner->PspLoadReadyForSpawnInitAudit(generationAfter);
    const bool tablesAfter =
        readyAfter && owner->PspItemTimeSpawnInitTablesContain(
                          generationAfter, kItemTimeScriptIndex,
                          kItemTimeInitialSpriteIndex);

    if (!readyBefore || !readyAfter ||
        generationBefore != generationAfter)
    {
        ++g_ItemTimeSpawnInitAuditStats.loadGenerationMismatches;
        NoteItemTimeSpawnInitFallback(
            &g_ItemTimeSpawnInitAuditStats.loadStateFallbacks);
        return;
    }

    if (!tablesBefore || !tablesAfter)
    {
        NoteItemTimeSpawnInitFallback(
            &g_ItemTimeSpawnInitAuditStats.compactRangeFallbacks);
        return;
    }

    if (scriptBefore == NULL ||
        owner->scripts[kItemTimeScriptIndex] != scriptBefore)
    {
        NoteItemTimeSpawnInitFallback(
            &g_ItemTimeSpawnInitAuditStats.scriptFingerprintFallbacks);
        return;
    }

    if (!owner->PspItemTimeSpawnInitScriptRangeContains(
            generationAfter, scriptBefore,
            kItemTimeScriptFingerprintBytes))
    {
        NoteItemTimeSpawnInitFallback(
            &g_ItemTimeSpawnInitAuditStats.compactRangeFallbacks);
        return;
    }

    if (!ValidateItemTimeSpawnScript68(scriptBefore))
    {
        NoteItemTimeSpawnInitFallback(
            &g_ItemTimeSpawnInitAuditStats.scriptFingerprintFallbacks);
        return;
    }

    AnmRawInstr *const nextInstruction = reinterpret_cast<AnmRawInstr *>(
        reinterpret_cast<u8 *>(scriptBefore) + scriptBefore->instructionSize);
    if (item->sprite.anmFile != owner ||
        item->sprite.scriptIndex != kItemTimeScriptIndex ||
        item->sprite.beginningOfScript != scriptBefore ||
        item->sprite.currentInstruction != nextInstruction ||
        item->sprite.activeSpriteIndex != kItemTimeInitialSpriteIndex ||
        item->sprite.loadedSprite !=
            &owner->sprites[kItemTimeInitialSpriteIndex])
    {
        NoteItemTimeSpawnInitFallback(
            &g_ItemTimeSpawnInitAuditStats.canonicalStateFallbacks);
        return;
    }

    AnmVm shadow;
    memcpy(&shadow, &before, sizeof(shadow));
    u32 shadowStarted = startedBefore;
    u32 shadowExecuted = executedBefore;
    const ZunBool shadowReturn = InitializeItemTimeScript68Shadow(
        &shadow, owner, scriptBefore, &shadowStarted, &shadowExecuted);

    const u32 generationFinal =
        owner->PspLoadGenerationForSpawnInitAudit();
    if (generationFinal != generationAfter ||
        !owner->PspLoadReadyForSpawnInitAudit(generationFinal) ||
        !owner->PspItemTimeSpawnInitTablesContain(
            generationFinal, kItemTimeScriptIndex,
            kItemTimeInitialSpriteIndex) ||
        !owner->PspItemTimeSpawnInitScriptRangeContains(
            generationFinal, scriptBefore,
            kItemTimeScriptFingerprintBytes))
    {
        ++g_ItemTimeSpawnInitAuditStats.loadGenerationMismatches;
        NoteItemTimeSpawnInitFallback(
            &g_ItemTimeSpawnInitAuditStats.loadStateFallbacks);
        return;
    }

    ++g_ItemTimeSpawnInitAuditStats.eligible;
    NoteItemTimeSpawnInitAuditEligible();
    ++g_ItemTimeSpawnInitAuditStats.loadGenerationMatches;

    const bool fullVmMatch =
        memcmp(&item->sprite, &shadow, sizeof(AnmVm)) == 0;
    if (fullVmMatch)
        ++g_ItemTimeSpawnInitAuditStats.fullVmMatches;
    else
        ++g_ItemTimeSpawnInitAuditStats.fullVmMismatches;

    const bool pointersMatch =
        ItemTimeSpawnPointerFieldsMatch(item->sprite, shadow);
    if (pointersMatch)
        ++g_ItemTimeSpawnInitAuditStats.pointerMatches;
    else
        ++g_ItemTimeSpawnInitAuditStats.pointerMismatches;

    const bool spriteMatch =
        ItemTimeSpawnSpriteFieldsMatch(item->sprite, shadow);
    if (spriteMatch)
        ++g_ItemTimeSpawnInitAuditStats.spriteMatches;
    else
        ++g_ItemTimeSpawnInitAuditStats.spriteMismatches;

    const bool fieldsMatch = ItemTimeSpawnNamedFieldsMatch(
        item->sprite, shadow, pointersMatch, spriteMatch);
    if (fieldsMatch)
        ++g_ItemTimeSpawnInitAuditStats.fieldMatches;
    else
        ++g_ItemTimeSpawnInitAuditStats.fieldMismatches;

    // SetAndExecuteScript discards the canonical ExecuteScript return, so no
    // canonical return value is observable here.  This only witnesses that a
    // validated candidate retains the required FALSE continuation contract.
    if (shadowReturn == FALSE)
        ++g_ItemTimeSpawnInitAuditStats.candidateReturnContractMatches;
    else
        ++g_ItemTimeSpawnInitAuditStats.candidateReturnContractMismatches;

    const bool countersMatch =
        startedAfter == shadowStarted && executedAfter == shadowExecuted;
    if (countersMatch)
        ++g_ItemTimeSpawnInitAuditStats.counterMatches;
    else
        ++g_ItemTimeSpawnInitAuditStats.counterMismatches;

    th08::psp::RenderPerfNoteItemTimeSpawnInitAudit(
        true, !fullVmMatch, !fieldsMatch, false);
}
} // namespace

const ItemTimeSpawnInitAuditStats &GetItemTimeSpawnInitAuditStats()
{
    return g_ItemTimeSpawnInitAuditStats;
}
#endif

#if defined(PSP)
namespace
{
enum ItemTimeSpawnCacheValidation
{
    ITEM_TIME_SPAWN_CACHE_UNVALIDATED = 0,
    ITEM_TIME_SPAWN_CACHE_VALID,
    ITEM_TIME_SPAWN_CACHE_RANGE_INVALID,
    ITEM_TIME_SPAWN_CACHE_FINGERPRINT_INVALID,
};

struct ItemTimeSpawnInitCache
{
    AnmLoaded *owner;
    AnmRawEntry *rawData;
    AnmRawInstr **scripts;
    AnmLoadedSprite *sprites;
    AnmRawInstr *script68;
    u32 generation;
    i32 anmIdx;
    ItemTimeSpawnCacheValidation validation;
};

// Preserve one exact typed BSS reservation in every PSP build.  Product OFF,
// product ON, and the mutually exclusive shadow audit therefore keep the same
// stage-pool/heap geometry; only code and reads of this storage are gated.
struct ItemTimeSpawnInitFastpathStorage
{
    ItemTimeSpawnInitCache cache;
    ItemTimeSpawnInitFastpathStats stats;
};

static_assert(sizeof(void *) == 4, "PSP item spawn cache requires 32-bit ABI");
static_assert(sizeof(ItemTimeSpawnInitCache) == 32,
              "Unexpected PSP ITEM_TIME spawn cache size");
static_assert(alignof(ItemTimeSpawnInitCache) == 4,
              "Unexpected PSP ITEM_TIME spawn cache alignment");
static_assert(sizeof(ItemTimeSpawnInitFastpathStats) == 56,
              "Unexpected PSP ITEM_TIME spawn stats size");
static_assert(alignof(ItemTimeSpawnInitFastpathStats) == 4,
              "Unexpected PSP ITEM_TIME spawn stats alignment");
static_assert(offsetof(ItemTimeSpawnInitFastpathStorage, cache) == 0,
              "ITEM_TIME spawn cache must lead the fixed reservation");
static_assert(offsetof(ItemTimeSpawnInitFastpathStorage, stats) == 32,
              "ITEM_TIME spawn stats offset changed");
static_assert(sizeof(ItemTimeSpawnInitFastpathStorage) == 88,
              "ITEM_TIME spawn OFF/ON reservation must remain 88 bytes");
static_assert(alignof(ItemTimeSpawnInitFastpathStorage) == 4,
              "Unexpected ITEM_TIME spawn reservation alignment");

alignas(4) ItemTimeSpawnInitFastpathStorage
    g_ItemTimeSpawnInitFastpathStorage __attribute__((used));
} // namespace
#endif

#if defined(PSP) && TH08_PSP_ITEM_TIME_SPAWN_INIT_PRODUCT_ENABLED
#define g_ItemTimeSpawnInitCache \
    (g_ItemTimeSpawnInitFastpathStorage.cache)
#define g_ItemTimeSpawnInitFastpathStats \
    (g_ItemTimeSpawnInitFastpathStorage.stats)
namespace
{
constexpr i32 kItemTimeSpawnFastpathScriptIndex = 68;
constexpr i32 kItemTimeSpawnFastpathSpriteIndex = 179;
constexpr u32 kItemTimeSpawnFastpathFingerprintBytes = 68;

enum ItemTimeSpawnFallbackReason
{
    ITEM_TIME_SPAWN_FALLBACK_OWNER,
    ITEM_TIME_SPAWN_FALLBACK_GENERATION,
    ITEM_TIME_SPAWN_FALLBACK_READINESS,
    ITEM_TIME_SPAWN_FALLBACK_SCRIPT,
    ITEM_TIME_SPAWN_FALLBACK_RANGE,
    ITEM_TIME_SPAWN_FALLBACK_FINGERPRINT,
};

void ResetItemTimeSpawnInitFastpath()
{
    memset(&g_ItemTimeSpawnInitCache, 0,
           sizeof(g_ItemTimeSpawnInitCache));
    memset(&g_ItemTimeSpawnInitFastpathStats, 0,
           sizeof(g_ItemTimeSpawnInitFastpathStats));
    g_ItemTimeSpawnInitFastpathStats.cacheResets = 1;
}

void NoteItemTimeSpawnInitFastpathFallback(
    ItemTimeSpawnFallbackReason reason)
{
    ++g_ItemTimeSpawnInitFastpathStats.canonicalFallbacks;
    switch (reason)
    {
    case ITEM_TIME_SPAWN_FALLBACK_OWNER:
        ++g_ItemTimeSpawnInitFastpathStats.ownerFallbacks;
        break;
    case ITEM_TIME_SPAWN_FALLBACK_GENERATION:
        ++g_ItemTimeSpawnInitFastpathStats.generationFallbacks;
        break;
    case ITEM_TIME_SPAWN_FALLBACK_READINESS:
        ++g_ItemTimeSpawnInitFastpathStats.readinessFallbacks;
        break;
    case ITEM_TIME_SPAWN_FALLBACK_SCRIPT:
        ++g_ItemTimeSpawnInitFastpathStats.scriptFallbacks;
        break;
    case ITEM_TIME_SPAWN_FALLBACK_RANGE:
        ++g_ItemTimeSpawnInitFastpathStats.rangeFallbacks;
        break;
    case ITEM_TIME_SPAWN_FALLBACK_FINGERPRINT:
        ++g_ItemTimeSpawnInitFastpathStats.fingerprintFallbacks;
        break;
    }
}

bool ValidateItemTimeSpawnFastpathScript68(const AnmRawInstr *sprite)
{
    if (sprite == NULL || sprite->opcode != AnmOpcode_Sprite ||
        sprite->instructionSize != 12 || sprite->time != 0 ||
        sprite->varMask != 0 ||
        sprite->intArgs[0] != kItemTimeSpawnFastpathSpriteIndex)
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

bool ResolveItemTimeSpawnFastpathScript(AnmLoaded *owner,
                                        AnmRawInstr **outScript68)
{
    if (outScript68 == NULL || owner == NULL || g_AnmManager == NULL ||
        owner != g_BulletManager.bulletAnm)
    {
        NoteItemTimeSpawnInitFastpathFallback(
            ITEM_TIME_SPAWN_FALLBACK_OWNER);
        return false;
    }

    const u32 generation =
        owner->PspLoadGenerationForItemTimeSpawnInit();
    if (generation == 0)
    {
        NoteItemTimeSpawnInitFastpathFallback(
            ITEM_TIME_SPAWN_FALLBACK_GENERATION);
        return false;
    }
    if (!owner->PspLoadReadyForItemTimeSpawnInit(generation))
    {
        NoteItemTimeSpawnInitFastpathFallback(
            ITEM_TIME_SPAWN_FALLBACK_READINESS);
        return false;
    }

    const bool cacheOwnerGenerationMatch =
        g_ItemTimeSpawnInitCache.owner == owner &&
        g_ItemTimeSpawnInitCache.generation == generation;
    if (cacheOwnerGenerationMatch)
    {
        if (g_ItemTimeSpawnInitCache.rawData != owner->rawData ||
            g_ItemTimeSpawnInitCache.scripts != owner->scripts ||
            g_ItemTimeSpawnInitCache.sprites != owner->sprites ||
            g_ItemTimeSpawnInitCache.anmIdx != owner->anmIdx)
        {
            NoteItemTimeSpawnInitFastpathFallback(
                ITEM_TIME_SPAWN_FALLBACK_SCRIPT);
            return false;
        }

        if (g_ItemTimeSpawnInitCache.validation ==
            ITEM_TIME_SPAWN_CACHE_RANGE_INVALID)
        {
            NoteItemTimeSpawnInitFastpathFallback(
                ITEM_TIME_SPAWN_FALLBACK_RANGE);
            return false;
        }
        if (g_ItemTimeSpawnInitCache.validation ==
            ITEM_TIME_SPAWN_CACHE_FINGERPRINT_INVALID)
        {
            NoteItemTimeSpawnInitFastpathFallback(
                ITEM_TIME_SPAWN_FALLBACK_FINGERPRINT);
            return false;
        }
        if (g_ItemTimeSpawnInitCache.validation !=
            ITEM_TIME_SPAWN_CACHE_VALID)
        {
            NoteItemTimeSpawnInitFastpathFallback(
                ITEM_TIME_SPAWN_FALLBACK_SCRIPT);
            return false;
        }

        // Table and byte bounds were validated for this immutable generation.
        // Verify the table-owner pointers above before reading the cached
        // script slot; a same-generation identity mutation fails closed.
        AnmRawInstr *const script68 =
            owner->scripts[kItemTimeSpawnFastpathScriptIndex];
        if (g_ItemTimeSpawnInitCache.script68 != script68)
        {
            NoteItemTimeSpawnInitFastpathFallback(
                ITEM_TIME_SPAWN_FALLBACK_SCRIPT);
            return false;
        }
        ++g_ItemTimeSpawnInitFastpathStats.cacheHits;
        *outScript68 = script68;
        return true;
    }

    // Only a new immutable Ready+compact ANM load-generation cache entry pays
    // the complete table/range proof and four-instruction fingerprint walk.
    // A replay hash covers gameplay state; the copied draw fields still need
    // a separate same-frame pixel gate before this product can be accepted.
    if (!owner->PspItemTimeSpawnInitTablesContain(
            generation, kItemTimeSpawnFastpathScriptIndex,
            kItemTimeSpawnFastpathSpriteIndex))
    {
        NoteItemTimeSpawnInitFastpathFallback(
            ITEM_TIME_SPAWN_FALLBACK_RANGE);
        return false;
    }

    AnmRawInstr *const script68 =
        owner->scripts[kItemTimeSpawnFastpathScriptIndex];
    if (script68 == NULL)
    {
        NoteItemTimeSpawnInitFastpathFallback(
            ITEM_TIME_SPAWN_FALLBACK_SCRIPT);
        return false;
    }

    if (g_ItemTimeSpawnInitCache.owner == owner &&
        g_ItemTimeSpawnInitCache.generation != 0 &&
        g_ItemTimeSpawnInitCache.generation != generation)
    {
        ++g_ItemTimeSpawnInitFastpathStats.cacheGenerationChanges;
    }
    ++g_ItemTimeSpawnInitFastpathStats.cacheRevalidations;
    memset(&g_ItemTimeSpawnInitCache, 0,
           sizeof(g_ItemTimeSpawnInitCache));
    g_ItemTimeSpawnInitCache.owner = owner;
    g_ItemTimeSpawnInitCache.rawData = owner->rawData;
    g_ItemTimeSpawnInitCache.scripts = owner->scripts;
    g_ItemTimeSpawnInitCache.sprites = owner->sprites;
    g_ItemTimeSpawnInitCache.script68 = script68;
    g_ItemTimeSpawnInitCache.generation = generation;
    g_ItemTimeSpawnInitCache.anmIdx = owner->anmIdx;

    if (!owner->PspItemTimeSpawnInitScriptRangeContains(
            generation, script68,
            kItemTimeSpawnFastpathFingerprintBytes))
    {
        g_ItemTimeSpawnInitCache.validation =
            ITEM_TIME_SPAWN_CACHE_RANGE_INVALID;
        ++g_ItemTimeSpawnInitFastpathStats.cacheValidationFailures;
        NoteItemTimeSpawnInitFastpathFallback(
            ITEM_TIME_SPAWN_FALLBACK_RANGE);
        return false;
    }
    if (!ValidateItemTimeSpawnFastpathScript68(script68))
    {
        g_ItemTimeSpawnInitCache.validation =
            ITEM_TIME_SPAWN_CACHE_FINGERPRINT_INVALID;
        ++g_ItemTimeSpawnInitFastpathStats.cacheValidationFailures;
        NoteItemTimeSpawnInitFastpathFallback(
            ITEM_TIME_SPAWN_FALLBACK_FINGERPRINT);
        return false;
    }

    g_ItemTimeSpawnInitCache.validation = ITEM_TIME_SPAWN_CACHE_VALID;
    *outScript68 = script68;
    return true;
}

// r038 proved this construction byte-for-byte over the complete 0x2a4-byte
// AnmVm for all 4,972 Stage-5 candidates.  Preserve the canonical lifetime
// boundary: only AnmVmBase is cleared, while stale slot bytes in
// [0x208, 0x2a4) survive unless the stock path explicitly writes them.
void InitializeItemTimeScript68Fastpath(AnmVm *vm, AnmLoaded *owner,
                                        AnmRawInstr *script68)
{
    memset(static_cast<AnmVmBase *>(vm), 0, sizeof(AnmVmBase));
    vm->scale.x = 1.0f;
    vm->scale.y = 1.0f;
    vm->color1.d3dColor = COLOR_WHITE;
    D3DXMatrixIdentity(&vm->matrix1);
    vm->flags = 7;
    vm->currentTimeInScript.Initialize();

    vm->scriptIndex = kItemTimeSpawnFastpathScriptIndex;
    vm->anmFileIndex = owner->anmIdx;
    vm->anmFile = owner;
    vm->flip = 0;
    vm->beginningOfScript = script68;
    vm->currentInstruction = script68;
    vm->currentTimeInScript = 0;
    vm->visible = FALSE;

    vm->visible = TRUE;
    vm->anmFile = owner;
    vm->activeSpriteIndex = kItemTimeSpawnFastpathSpriteIndex;
    vm->loadedSprite = &owner->sprites[kItemTimeSpawnFastpathSpriteIndex];
    vm->spriteSize.x = vm->loadedSprite->widthPx;
    vm->spriteSize.y = vm->loadedSprite->heightPx;
    D3DXMatrixIdentity(&vm->matrix1);
    D3DXMatrixIdentity(&vm->matrix3);
    vm->matrix1.m[0][0] = vm->spriteSize.x / 256.0f;
    vm->matrix1.m[1][1] = vm->spriteSize.y / 256.0f;
    vm->matrix3.m[0][0] =
        (vm->spriteSize.x / vm->loadedSprite->width) *
        vm->loadedSprite->scaleFactor.x;
    vm->matrix3.m[1][1] =
        (vm->spriteSize.y / vm->loadedSprite->height) *
        vm->loadedSprite->scaleFactor.y;
    vm->matrix2 = vm->matrix1;
    vm->timeOfLastSpriteSet = static_cast<i32>(vm->currentTimeInScript);
    vm->currentInstruction = reinterpret_cast<AnmRawInstr *>(
        reinterpret_cast<u8 *>(script68) + script68->instructionSize);

    vm->uvScrollPos.x += vm->uvScrollVel.x;
    if (vm->uvScrollPos.x >= 1.0f)
        vm->uvScrollPos.x -= 1.0f;
    else if (vm->uvScrollPos.x < 0.0f)
        vm->uvScrollPos.x += 1.0f;
    vm->uvScrollPos.y += vm->uvScrollVel.y;
    if (vm->uvScrollPos.y >= 1.0f)
        vm->uvScrollPos.y -= 1.0f;
    else if (vm->uvScrollPos.y < 0.0f)
        vm->uvScrollPos.y += 1.0f;
    vm->currentTimeInScript++;
    ++g_AnmManager->scriptsExecutedThisFrame;
    ++g_AnmManager->scriptsStartedThisFrame;
}

bool TryInitializeItemTimeSpawnScript68Fastpath(
    Item *item, AnmLoaded *owner, i32 scriptIndex)
{
    if (item == NULL || item->itemType != ITEM_TIME ||
        scriptIndex != kItemTimeSpawnFastpathScriptIndex)
    {
        return false;
    }

    ++g_ItemTimeSpawnInitFastpathStats.calls;
    AnmRawInstr *script68 = NULL;
    if (!ResolveItemTimeSpawnFastpathScript(owner, &script68))
        return false;

    InitializeItemTimeScript68Fastpath(&item->sprite, owner, script68);
    ++g_ItemTimeSpawnInitFastpathStats.hits;
    return true;
}
} // namespace

const ItemTimeSpawnInitFastpathStats &GetItemTimeSpawnInitFastpathStats()
{
    return g_ItemTimeSpawnInitFastpathStats;
}
#undef g_ItemTimeSpawnInitFastpathStats
#undef g_ItemTimeSpawnInitCache
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_ANM_IDLE_AUDIT) && \
    TH08_PSP_ITEM_TIME_ANM_IDLE_AUDIT
namespace
{
ItemTimeAnmIdleAuditStats g_ItemTimeAnmIdleAuditStats;

void ResetItemTimeAnmIdleAudit()
{
    memset(&g_ItemTimeAnmIdleAuditStats, 0,
           sizeof(g_ItemTimeAnmIdleAuditStats));
}

bool IsStockItemTimeScript68(const Item *item)
{
    const AnmLoaded *const anm = g_BulletManager.bulletAnm;
    if (item->itemType != ITEM_TIME || anm == NULL || anm->scripts == NULL ||
        item->sprite.anmFile != anm || item->sprite.scriptIndex != 68)
    {
        return false;
    }

    AnmRawInstr *const sprite = anm->scripts[68];
    if (sprite == NULL || item->sprite.beginningOfScript != sprite ||
        sprite->opcode != AnmOpcode_Sprite || sprite->instructionSize != 12 ||
        sprite->time != 0 || sprite->varMask != 0 ||
        sprite->intArgs[0] != 179)
    {
        return false;
    }

    AnmRawInstr *const color = reinterpret_cast<AnmRawInstr *>(
        reinterpret_cast<u8 *>(sprite) + sprite->instructionSize);
    if (color->opcode != AnmOpcode_ColorTime ||
        color->instructionSize != 28 || color->time != 30 ||
        color->varMask != 0 || color->intArgs[0] != 20 ||
        color->intArgs[1] != AnmInterpMode_Linear ||
        color->intArgs[2] != 128 || color->intArgs[3] != 128 ||
        color->intArgs[4] != 128)
    {
        return false;
    }

    AnmRawInstr *const alpha = reinterpret_cast<AnmRawInstr *>(
        reinterpret_cast<u8 *>(color) + color->instructionSize);
    if (alpha->opcode != AnmOpcode_AlphaTime ||
        alpha->instructionSize != 20 || alpha->time != 30 ||
        alpha->varMask != 0 || alpha->intArgs[0] != 20 ||
        alpha->intArgs[1] != AnmInterpMode_Linear ||
        alpha->intArgs[2] != 192)
    {
        return false;
    }

    AnmRawInstr *const stop = reinterpret_cast<AnmRawInstr *>(
        reinterpret_cast<u8 *>(alpha) + alpha->instructionSize);
    return stop->opcode == AnmOpcode_Static &&
           stop->instructionSize == 8 && stop->time == 50 &&
           stop->varMask == 0;
}

bool HasOnlyIdleItemTimeAnmDynamics(const AnmVm *vm)
{
    if (vm->flag19 != 0 || vm->pendingInterrupt != 0 ||
        vm->angleVel.x != 0.0f || vm->angleVel.y != 0.0f ||
        vm->angleVel.z != 0.0f || vm->scaleGrowth.x != 0.0f ||
        vm->scaleGrowth.y != 0.0f)
    {
        return false;
    }

    for (i32 i = 0; i < AnmInterp_Last; ++i)
    {
        if (vm->interpEndTimers[i].current > 0)
            return false;
    }
    return true;
}

// Mirror only ExecuteScript's no-instruction tail.  UV arithmetic is kept
// even for zero velocity because IEEE-754 signed-zero state is observable in
// a full-VM comparison.  ZunTimer::operator++ preserves previous/subFrame and
// Supervisor's fractional-rate behavior.
ZunBool ExecuteItemTimeAnmIdleShadow(AnmVm *vm, u32 *scriptsExecuted)
{
    vm->uvScrollPos.x += vm->uvScrollVel.x;
    if (vm->uvScrollPos.x >= 1.0f)
    {
        vm->uvScrollPos.x -= 1.0f;
    }
    else if (vm->uvScrollPos.x < 0.0f)
    {
        vm->uvScrollPos.x += 1.0f;
    }

    vm->uvScrollPos.y += vm->uvScrollVel.y;
    if (vm->uvScrollPos.y >= 1.0f)
    {
        vm->uvScrollPos.y -= 1.0f;
    }
    else if (vm->uvScrollPos.y < 0.0f)
    {
        vm->uvScrollPos.y += 1.0f;
    }

    vm->currentTimeInScript++;
    ++*scriptsExecuted;
    return FALSE;
}

ZunBool ExecuteItemAnmWithIdleAudit(Item *item)
{
    ++g_ItemTimeAnmIdleAuditStats.canonicalCalls;

    const bool script68 = IsStockItemTimeScript68(item);
    if (!script68)
    {
        return g_AnmManager->ExecuteScript(&item->sprite);
    }
    ++g_ItemTimeAnmIdleAuditStats.script68Candidates;

    AnmRawInstr *const idleInstruction =
        reinterpret_cast<AnmRawInstr *>(
            reinterpret_cast<u8 *>(item->sprite.beginningOfScript) + 12);
    const bool idleWindow =
        item->sprite.currentInstruction == idleInstruction &&
        item->sprite.currentTimeInScript.current >= 1 &&
        item->sprite.currentTimeInScript.current <= 29;
    if (!idleWindow)
    {
        ++g_ItemTimeAnmIdleAuditStats.rejectedWindowCalls;
        return g_AnmManager->ExecuteScript(&item->sprite);
    }

    const bool idleDynamics =
        HasOnlyIdleItemTimeAnmDynamics(&item->sprite);
    if (!idleDynamics)
    {
        ++g_ItemTimeAnmIdleAuditStats.rejectedDynamicStateCalls;
        return g_AnmManager->ExecuteScript(&item->sprite);
    }

    ++g_ItemTimeAnmIdleAuditStats.eligibleIdleCalls;
    AnmVm before;
    memcpy(&before, &item->sprite, sizeof(before));

    const u32 canonicalCounterBefore =
        g_AnmManager->scriptsExecutedThisFrame;
    const ZunBool canonicalReturn =
        g_AnmManager->ExecuteScript(&item->sprite);
    const u32 canonicalCounterAfter =
        g_AnmManager->scriptsExecutedThisFrame;

    AnmVm fastShadow;
    memcpy(&fastShadow, &before, sizeof(fastShadow));
    u32 fastCounter = canonicalCounterBefore;
    const ZunBool fastReturn =
        ExecuteItemTimeAnmIdleShadow(&fastShadow, &fastCounter);

    if (memcmp(&item->sprite, &fastShadow, sizeof(AnmVm)) == 0)
        ++g_ItemTimeAnmIdleAuditStats.fullVmMatches;
    else
        ++g_ItemTimeAnmIdleAuditStats.fullVmMismatches;

    if (canonicalReturn == fastReturn)
        ++g_ItemTimeAnmIdleAuditStats.returnMatches;
    else
        ++g_ItemTimeAnmIdleAuditStats.returnMismatches;

    if (canonicalCounterAfter == fastCounter)
        ++g_ItemTimeAnmIdleAuditStats.counterMatches;
    else
        ++g_ItemTimeAnmIdleAuditStats.counterMismatches;

    // The authoritative VM and global counter always come from the one and
    // only canonical ExecuteScript call above.  fastShadow is observational.
    return canonicalReturn;
}
} // namespace

const ItemTimeAnmIdleAuditStats &GetItemTimeAnmIdleAuditStats()
{
    return g_ItemTimeAnmIdleAuditStats;
}
#endif

#if defined(PSP) && TH08_PSP_ITEM_TIME_ANM_IDLE_PRODUCT_ENABLED
namespace
{
struct ItemTimeAnmIdleScriptCache
{
    const AnmLoaded *owner;
    AnmRawInstr *script68;
    ZunBool fingerprintValid;
};

ItemTimeAnmIdleScriptCache g_ItemTimeAnmIdleScriptCache;
ItemTimeAnmIdleFastpathStats g_ItemTimeAnmIdleFastpathStats;

void ResetItemTimeAnmIdleFastpath()
{
    memset(&g_ItemTimeAnmIdleScriptCache, 0,
           sizeof(g_ItemTimeAnmIdleScriptCache));
    memset(&g_ItemTimeAnmIdleFastpathStats, 0,
           sizeof(g_ItemTimeAnmIdleFastpathStats));
}

bool ValidateItemTimeAnmIdleScript68(AnmRawInstr *sprite)
{
    if (sprite == NULL || sprite->opcode != AnmOpcode_Sprite ||
        sprite->instructionSize != 12 || sprite->time != 0 ||
        sprite->varMask != 0 || sprite->intArgs[0] != 179)
    {
        return false;
    }

    AnmRawInstr *const color = reinterpret_cast<AnmRawInstr *>(
        reinterpret_cast<u8 *>(sprite) + sprite->instructionSize);
    if (color->opcode != AnmOpcode_ColorTime ||
        color->instructionSize != 28 || color->time != 30 ||
        color->varMask != 0 || color->intArgs[0] != 20 ||
        color->intArgs[1] != AnmInterpMode_Linear ||
        color->intArgs[2] != 128 || color->intArgs[3] != 128 ||
        color->intArgs[4] != 128)
    {
        return false;
    }

    AnmRawInstr *const alpha = reinterpret_cast<AnmRawInstr *>(
        reinterpret_cast<u8 *>(color) + color->instructionSize);
    if (alpha->opcode != AnmOpcode_AlphaTime ||
        alpha->instructionSize != 20 || alpha->time != 30 ||
        alpha->varMask != 0 || alpha->intArgs[0] != 20 ||
        alpha->intArgs[1] != AnmInterpMode_Linear ||
        alpha->intArgs[2] != 192)
    {
        return false;
    }

    AnmRawInstr *const stop = reinterpret_cast<AnmRawInstr *>(
        reinterpret_cast<u8 *>(alpha) + alpha->instructionSize);
    return stop->opcode == AnmOpcode_Static &&
           stop->instructionSize == 8 && stop->time == 50 &&
           stop->varMask == 0;
}

AnmRawInstr *GetValidatedItemTimeAnmIdleScript68()
{
    const AnmLoaded *const owner = g_BulletManager.bulletAnm;
    AnmRawInstr *script68 = NULL;
    if (owner != NULL && owner->scripts != NULL)
    {
        // This load is deliberately performed on every possible fast-path
        // call.  A cached validation never authorizes a replaced script.
        script68 = owner->scripts[68];
    }

    if (g_ItemTimeAnmIdleScriptCache.owner != owner ||
        g_ItemTimeAnmIdleScriptCache.script68 != script68)
    {
#if defined(TH08_REPLAY_SYNC_AUDIT) && TH08_REPLAY_SYNC_AUDIT
        ++g_ItemTimeAnmIdleFastpathStats.cachePointerChanges;
#endif
        g_ItemTimeAnmIdleScriptCache.owner = owner;
        g_ItemTimeAnmIdleScriptCache.script68 = script68;
        g_ItemTimeAnmIdleScriptCache.fingerprintValid = FALSE;

        if (owner != NULL && script68 != NULL)
        {
#if defined(TH08_REPLAY_SYNC_AUDIT) && TH08_REPLAY_SYNC_AUDIT
            ++g_ItemTimeAnmIdleFastpathStats.cacheRevalidations;
#endif
            g_ItemTimeAnmIdleScriptCache.fingerprintValid =
                ValidateItemTimeAnmIdleScript68(script68);
#if defined(TH08_REPLAY_SYNC_AUDIT) && TH08_REPLAY_SYNC_AUDIT
            if (!g_ItemTimeAnmIdleScriptCache.fingerprintValid)
                ++g_ItemTimeAnmIdleFastpathStats.cacheValidationFailures;
#endif
        }
    }

    return g_ItemTimeAnmIdleScriptCache.fingerprintValid ? script68 : NULL;
}

bool HasOnlyItemTimeAnmIdleFastpathDynamics(const AnmVm *vm)
{
    if (vm->flag19 != 0 || vm->pendingInterrupt != 0 ||
        vm->angleVel.x != 0.0f || vm->angleVel.y != 0.0f ||
        vm->angleVel.z != 0.0f || vm->scaleGrowth.x != 0.0f ||
        vm->scaleGrowth.y != 0.0f)
    {
        return false;
    }

    for (i32 i = 0; i < AnmInterp_Last; ++i)
    {
        if (vm->interpEndTimers[i].current > 0)
            return false;
    }
    return true;
}

// This is the byte-exact tail proven by the separate idle audit feature.
// Keep the arithmetic and ZunTimer operation in canonical source order.
ZunBool ExecuteItemTimeAnmIdleFastpathTail(AnmVm *vm)
{
    vm->uvScrollPos.x += vm->uvScrollVel.x;
    if (vm->uvScrollPos.x >= 1.0f)
    {
        vm->uvScrollPos.x -= 1.0f;
    }
    else if (vm->uvScrollPos.x < 0.0f)
    {
        vm->uvScrollPos.x += 1.0f;
    }

    vm->uvScrollPos.y += vm->uvScrollVel.y;
    if (vm->uvScrollPos.y >= 1.0f)
    {
        vm->uvScrollPos.y -= 1.0f;
    }
    else if (vm->uvScrollPos.y < 0.0f)
    {
        vm->uvScrollPos.y += 1.0f;
    }

    vm->currentTimeInScript++;
    g_AnmManager->scriptsExecutedThisFrame++;
    return FALSE;
}

ZunBool ExecuteItemTimeAnmCanonicalFallback(Item *item,
                                            u32 *specificFallbacks)
{
#if defined(TH08_REPLAY_SYNC_AUDIT) && TH08_REPLAY_SYNC_AUDIT
    ++g_ItemTimeAnmIdleFastpathStats.canonicalFallbacks;
    ++*specificFallbacks;
#else
    (void)specificFallbacks;
#endif
    return g_AnmManager->ExecuteScript(&item->sprite);
}

ZunBool ExecuteItemAnmWithIdleFastpath(Item *item)
{
#if defined(TH08_REPLAY_SYNC_AUDIT) && TH08_REPLAY_SYNC_AUDIT
    ++g_ItemTimeAnmIdleFastpathStats.calls;
#endif

    if (item->itemType != ITEM_TIME || item->sprite.scriptIndex != 68)
    {
        return ExecuteItemTimeAnmCanonicalFallback(
            item, &g_ItemTimeAnmIdleFastpathStats.identityFallbacks);
    }

    AnmRawInstr *const script68 = GetValidatedItemTimeAnmIdleScript68();
    const AnmLoaded *const owner = g_BulletManager.bulletAnm;
    if (script68 == NULL || item->sprite.anmFile != owner ||
        item->sprite.beginningOfScript != script68)
    {
        return ExecuteItemTimeAnmCanonicalFallback(
            item, &g_ItemTimeAnmIdleFastpathStats.identityFallbacks);
    }

    AnmRawInstr *const idleInstruction = reinterpret_cast<AnmRawInstr *>(
        reinterpret_cast<u8 *>(script68) + 12);
    if (item->sprite.currentInstruction != idleInstruction ||
        item->sprite.currentTimeInScript.current < 1 ||
        item->sprite.currentTimeInScript.current > 29)
    {
        return ExecuteItemTimeAnmCanonicalFallback(
            item, &g_ItemTimeAnmIdleFastpathStats.windowFallbacks);
    }

    if (!HasOnlyItemTimeAnmIdleFastpathDynamics(&item->sprite))
    {
        return ExecuteItemTimeAnmCanonicalFallback(
            item, &g_ItemTimeAnmIdleFastpathStats.dynamicFallbacks);
    }

#if defined(TH08_REPLAY_SYNC_AUDIT) && TH08_REPLAY_SYNC_AUDIT
    ++g_ItemTimeAnmIdleFastpathStats.hits;
#endif
    return ExecuteItemTimeAnmIdleFastpathTail(&item->sprite);
}
} // namespace

const ItemTimeAnmIdleFastpathStats &GetItemTimeAnmIdleFastpathStats()
{
    return g_ItemTimeAnmIdleFastpathStats;
}
#endif

#if TH08_PSP_ITEM_TIME_INLINE_DRAW_ENABLED
// DrawNoRotation/DrawInner keep this four-corner scratch between calls.  The
// modern PSP build gives DIFFABLE_STATIC_ARRAY external linkage; preserving
// its complete bytes is part of the frontend equivalence contract (notably W
// and the diffuse color after a culled draw).
extern VertexTex1DiffuseXyzrhw g_QuadVertices[4];

namespace
{
enum ItemTimeInlineDrawReject
{
    ITEM_TIME_INLINE_DRAW_ACCEPT = 0,
    ITEM_TIME_INLINE_DRAW_REJECT_IDENTITY,
    ITEM_TIME_INLINE_DRAW_REJECT_VISIBILITY,
    ITEM_TIME_INLINE_DRAW_REJECT_ROTATION,
    ITEM_TIME_INLINE_DRAW_REJECT_TEXTURE,
};

#if defined(TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT) && \
    TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT
ItemTimeInlineDrawAuditStats g_ItemTimeInlineDrawAuditStats;

void ResetItemTimeInlineDrawAudit()
{
    memset(&g_ItemTimeInlineDrawAuditStats, 0,
           sizeof(g_ItemTimeInlineDrawAuditStats));
}
#endif

inline u8 MixItemTimeInlineDrawColor(u8 color1, u8 color2)
{
    u32 color = (color1 * color2) / 128U;
    if (color >= 256U)
        color = 255U;
    return static_cast<u8>(color);
}

ItemTimeInlineDrawReject ValidateItemTimeInlineDraw(const AnmVm *vm)
{
    if (g_AnmManager == NULL || g_Supervisor.d3dDevice == NULL ||
        vm == NULL)
        return ITEM_TIME_INLINE_DRAW_REJECT_IDENTITY;
    if (!vm->visible || !vm->flag1 || vm->color1.a == 0)
        return ITEM_TIME_INLINE_DRAW_REJECT_VISIBILITY;
    // Draw2D ignores rotation.x/y.  Its z==0 branch is exactly
    // DrawNoRotation, including negative zero; every other z value retains
    // the canonical sine/cosine path.
    if (vm->rotation.z != 0.0f)
        return ITEM_TIME_INLINE_DRAW_REJECT_ROTATION;
    if (vm->loadedSprite == NULL)
        return ITEM_TIME_INLINE_DRAW_REJECT_TEXTURE;
    return ITEM_TIME_INLINE_DRAW_ACCEPT;
}

// Reproduce DrawNoRotation followed by DrawInner(flags=1) in source-store
// order.  output may be g_QuadVertices for the product or private M0 storage.
bool BuildItemTimeInlineDrawQuad(AnmVm *vm,
                                 VertexTex1DiffuseXyzrhw *output)
{
    if (output != g_QuadVertices)
        memcpy(output, g_QuadVertices, sizeof(g_QuadVertices));

    const f32 spriteHalfWidth =
        (vm->spriteSize.x * vm->scale.x) / 2.0f;
    const f32 spriteHalfHeight =
        (vm->spriteSize.y * vm->scale.y) / 2.0f;

    if ((vm->anchor & 1) == 0)
    {
        output[0].pos.x = output[2].pos.x =
            vm->pos.x - spriteHalfWidth;
        output[1].pos.x = output[3].pos.x =
            spriteHalfWidth + vm->pos.x;
    }
    else
    {
        output[0].pos.x = output[2].pos.x = vm->pos.x;
        output[1].pos.x = output[3].pos.x =
            spriteHalfWidth + vm->pos.x + spriteHalfWidth;
    }

    if ((vm->anchor & 2) == 0)
    {
        output[0].pos.y = output[1].pos.y =
            vm->pos.y - spriteHalfHeight;
        output[2].pos.y = output[3].pos.y =
            spriteHalfHeight + vm->pos.y;
    }
    else
    {
        output[0].pos.y = output[1].pos.y = vm->pos.y;
        output[2].pos.y = output[3].pos.y =
            spriteHalfHeight + vm->pos.y + spriteHalfHeight;
    }
    output[0].pos.z = output[1].pos.z =
        output[2].pos.z = output[3].pos.z = vm->pos.z;

    output[0].pos.x += g_AnmManager->screenShakeOffset.x;
    output[0].pos.y += g_AnmManager->screenShakeOffset.y;
    output[1].pos.x += g_AnmManager->screenShakeOffset.x;
    output[1].pos.y += g_AnmManager->screenShakeOffset.y;
    output[2].pos.x += g_AnmManager->screenShakeOffset.x;
    output[2].pos.y += g_AnmManager->screenShakeOffset.y;
    output[3].pos.x += g_AnmManager->screenShakeOffset.x;
    output[3].pos.y += g_AnmManager->screenShakeOffset.y;

    const f32 triangleX1 = nearbyintf(output[0].pos.x) - 0.5f;
    const f32 triangleX2 = nearbyintf(output[1].pos.x) - 0.5f;
    const f32 triangleY1 = nearbyintf(output[0].pos.y) - 0.5f;
    const f32 triangleY2 = nearbyintf(output[2].pos.y) - 0.5f;
    output[2].pos.y = output[3].pos.y = triangleY2;
    output[0].pos.y = output[1].pos.y = triangleY1;
    output[1].pos.x = output[3].pos.x = triangleX2;
    output[0].pos.x = output[2].pos.x = triangleX1;

    output[0].textureUV.x = output[2].textureUV.x =
        vm->loadedSprite->uvStart.x + vm->uvScrollPos.x;
    output[1].textureUV.x = output[3].textureUV.x =
        vm->loadedSprite->uvEnd.x + vm->uvScrollPos.x;
    output[0].textureUV.y = output[1].textureUV.y =
        vm->loadedSprite->uvStart.y + vm->uvScrollPos.y;
    output[2].textureUV.y = output[3].textureUV.y =
        vm->loadedSprite->uvEnd.y + vm->uvScrollPos.y;

    f32 maxX = ZUN_MAX(output[0].pos.x, output[1].pos.x);
    maxX = ZUN_MAX(output[2].pos.x, maxX);
    maxX = ZUN_MAX(output[3].pos.x, maxX);
    f32 maxY = ZUN_MAX(output[0].pos.y, output[1].pos.y);
    maxY = ZUN_MAX(output[2].pos.y, maxY);
    maxY = ZUN_MAX(output[3].pos.y, maxY);
    f32 minX = ZUN_MIN(output[0].pos.x, output[1].pos.x);
    minX = ZUN_MIN(output[2].pos.x, minX);
    minX = ZUN_MIN(output[3].pos.x, minX);
    f32 minY = ZUN_MIN(output[0].pos.y, output[1].pos.y);
    minY = ZUN_MIN(output[2].pos.y, minY);
    minY = ZUN_MIN(output[3].pos.y, minY);

    if (maxX < g_Supervisor.viewport.X ||
        maxY < g_Supervisor.viewport.Y ||
        minX > g_Supervisor.viewport.X + g_Supervisor.viewport.Width ||
        minY > g_Supervisor.viewport.Y + g_Supervisor.viewport.Height)
    {
        return false;
    }

    ZunColor color;
    color.d3dColor = vm->flag17 ? vm->color2.d3dColor
                                : vm->color1.d3dColor;
    if (g_AnmManager->useMixColor)
    {
        color.r = MixItemTimeInlineDrawColor(
            color.r, g_AnmManager->color.r);
        color.g = MixItemTimeInlineDrawColor(
            color.g, g_AnmManager->color.g);
        color.b = MixItemTimeInlineDrawColor(
            color.b, g_AnmManager->color.b);
        color.a = MixItemTimeInlineDrawColor(
            color.a, g_AnmManager->color.a);
    }
    output[0].diffuse = output[1].diffuse =
        output[2].diffuse = output[3].diffuse = color.d3dColor;
    return true;
}

void CommitItemTimeInlineDraw(AnmVm *vm, bool visible)
{
    if (!visible)
        return;
    if (g_AnmManager->currentTexture != vm->loadedSprite->texture)
    {
        g_AnmManager->currentTexture = vm->loadedSprite->texture;
        g_AnmManager->FlushVertexBuffer();
        g_Supervisor.d3dDevice->SetTexture(
            0, g_AnmManager->currentTexture);
    }
    if (g_AnmManager->currentVertexShader != 1)
    {
        g_AnmManager->FlushVertexBuffer();
        g_AnmManager->currentVertexShader = 1;
    }
    g_AnmManager->SetRenderStateForVm(vm);
    g_AnmManager->AddSpriteToDrawBuffer(g_QuadVertices);
}

#if TH08_PSP_ITEM_TIME_INLINE_DRAW_PRODUCT_ENABLED
bool TryDrawItemTimeInline(AnmVm *vm)
{
    if (ValidateItemTimeInlineDraw(vm) !=
        ITEM_TIME_INLINE_DRAW_ACCEPT)
    {
        return false;
    }
    const bool visible = BuildItemTimeInlineDrawQuad(vm, g_QuadVertices);
    CommitItemTimeInlineDraw(vm, visible);
    return true;
}
#endif

#if defined(TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT) && \
    TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT
ZunResult AuditItemTimeInlineDraw(AnmVm *vm)
{
    ++g_ItemTimeInlineDrawAuditStats.candidates;
    if (ValidateItemTimeInlineDraw(vm) !=
        ITEM_TIME_INLINE_DRAW_ACCEPT)
    {
        ++g_ItemTimeInlineDrawAuditStats.canonicalFallbacks;
        return g_AnmManager->Draw2D(vm);
    }

    ++g_ItemTimeInlineDrawAuditStats.eligible;
    VertexTex1DiffuseXyzrhw candidate[4];
    const bool candidateVisible =
        BuildItemTimeInlineDrawQuad(vm, candidate);
    VertexTex1DiffuseXyzrhw *const endBefore =
        g_AnmManager->vertexBufferEndPtr;
    const u32 spritesBefore = g_AnmManager->spritesToDraw;
    const ZunResult result = g_AnmManager->Draw2D(vm);

    if (memcmp(candidate, g_QuadVertices, sizeof(candidate)) == 0)
        ++g_ItemTimeInlineDrawAuditStats.quadMatches;
    else
        ++g_ItemTimeInlineDrawAuditStats.quadMismatches;

    if (!candidateVisible)
    {
        ++g_ItemTimeInlineDrawAuditStats.culled;
        if (g_AnmManager->vertexBufferEndPtr == endBefore &&
            g_AnmManager->spritesToDraw == spritesBefore)
        {
            ++g_ItemTimeInlineDrawAuditStats.streamMatches;
        }
        else
        {
            ++g_ItemTimeInlineDrawAuditStats.streamMismatches;
        }
        return result;
    }

    ++g_ItemTimeInlineDrawAuditStats.visible;
    VertexTex1DiffuseXyzrhw expected[6];
    expected[0] = candidate[0];
    expected[1] = candidate[1];
    expected[2] = candidate[2];
    expected[3] = candidate[1];
    expected[4] = candidate[2];
    expected[5] = candidate[3];
    const VertexTex1DiffuseXyzrhw *const end =
        g_AnmManager->vertexBufferEndPtr;
    if (end >= g_AnmManager->vertexBuffer + 6 &&
        memcmp(end - 6, expected, sizeof(expected)) == 0)
    {
        ++g_ItemTimeInlineDrawAuditStats.streamMatches;
    }
    else
    {
        ++g_ItemTimeInlineDrawAuditStats.streamMismatches;
    }
    return result;
}
#endif
} // namespace

#if defined(TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT) && \
    TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT
const ItemTimeInlineDrawAuditStats &GetItemTimeInlineDrawAuditStats()
{
    return g_ItemTimeInlineDrawAuditStats;
}
#endif
#endif

// FUNCTION: th08 0x441830
ZunBool ZunTimer::operator!=(int value)
{
    return this->current != value;
}

// FUNCTION: th08 0x440010
ItemManager::ItemManager()
{
}

// FUNCTION: th08 0x4337f0
void ItemManager::Initialize()
{
#if defined(TH08_PSP_STAGE_POOL_ARENA)
    Item *const itemPool = this->items;
    if (itemPool == NULL)
        return;
    memset(static_cast<void *>(this), 0, sizeof(*this));
    this->items = itemPool;
    memset(static_cast<void *>(this->items), 0,
           sizeof(Item) * th08::psp::kItemPoolStorageCount);
#else
    memset(this, 0, sizeof(ItemManager));
#endif
#if defined(PSP) && defined(TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT) && \
    TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT
    ResetItemAutocollectCacheAudit();
#endif
#if defined(PSP) && defined(TH08_PSP_ITEM_AUTOCOLLECT_CACHE) && \
    TH08_PSP_ITEM_AUTOCOLLECT_CACHE
    ResetItemAutocollectCache();
#endif
#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT) && \
    TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT
    ResetItemTimeSpawnInitAudit();
#endif
#if defined(PSP) && TH08_PSP_ITEM_TIME_SPAWN_INIT_PRODUCT_ENABLED
    ResetItemTimeSpawnInitFastpath();
#endif
#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_ANM_IDLE_AUDIT) && \
    TH08_PSP_ITEM_TIME_ANM_IDLE_AUDIT
    ResetItemTimeAnmIdleAudit();
#endif
#if defined(PSP) && TH08_PSP_ITEM_TIME_ANM_IDLE_PRODUCT_ENABLED
    ResetItemTimeAnmIdleFastpath();
#endif
#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT) && \
    TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT
    ResetItemTimeInlineDrawAudit();
#endif
#if TH08_PSP_ITEM_TIME_DRAW_PAIR_ENABLED
    if (g_AnmManager != NULL)
        g_AnmManager->ResetPspItemTimeDrawPairStats();
#endif
    this->itemListTail = &this->itemListHead;
}

// FUNCTION: th08 0x440050
Item::Item()
{
}

#pragma var_order(i, item)
Item *ItemManager::SpawnItem(Float3 *position, ItemType itemType, i32 state)
{
    i32 i;
    Item *item = &this->items[this->nextIndex];

    ReplaySyncAudit::RecordItemSpawnRequest(
        position, static_cast<i32>(itemType), state, this->nextIndex);
#if defined(PSP)
    th08::psp::RenderPerfNoteItemSpawnRequest();
#endif

    if (position->x < -64.0f || position->x > 448.0f)
    {
        ReplaySyncAudit::RecordItemSpawnResult(
            ReplaySyncAudit::ITEM_SPAWN_AUDIT_REJECT_X,
            static_cast<i32>(itemType), state, this->nextIndex);
        return &this->items[MAX_ITEMS];
    }

    if (g_GameManager.GetPower() >= 128 && (itemType == ITEM_POWER_SMALL || itemType == ITEM_POWER_BIG))
    {
        itemType = ITEM_POINT_SMALL;
    }
    if (itemType == ITEM_TIME)
    {
        state = ITEM_STATE_TIME_RISING;
    }
    else if (itemType == ITEM_TIME_APEX_AUTOCOLLECT_REQUEST)
    {
        state = ITEM_STATE_TIME_RISING_TO_APEX;
        itemType = ITEM_TIME;
    }

    for (i = 0; i < MAX_ITEMS; i++)
    {
        this->nextIndex++;

        if (item->isInUse)
        {
            if (this->nextIndex >= MAX_ITEMS)
            {
                this->nextIndex = 0;
                item = &this->items[0];
            }
            else
            {
                item++;
            }

            if (itemType == ITEM_TIME)
            {
                ReplaySyncAudit::RecordItemSpawnResult(
                    ReplaySyncAudit::ITEM_SPAWN_AUDIT_REJECT_TIME_FIRST_SLOT,
                    static_cast<i32>(itemType), state, this->nextIndex);
                return &this->items[MAX_ITEMS];
            }

            continue;
        }

        if (this->nextIndex >= MAX_ITEMS)
        {
            this->nextIndex = 0;
        }

        item->isInUse = true;
        item->currentPosition = *position;
        item->startPositionOrVelocity.x = 0.0f;
        item->startPositionOrVelocity.y = -2.2f;
        item->startPositionOrVelocity.z = 0.0f;
        item->itemType = itemType;
        item->state = state;
        item->timer = 0;

        if (state == ITEM_STATE_DEATH_DROP_SPREAD)
        {
            item->targetPosition.x = g_Rng.GetRandomF32InRange(288.0f) + 48.0f;
            item->targetPosition.y = g_Rng.GetRandomF32InRange(192.0f) - 64.0f;
            item->targetPosition.z = 0.0f;
            item->startPositionOrVelocity = item->currentPosition;
        }
        else if (state == ITEM_STATE_TIME_RISING)
        {
            item->startPositionOrVelocity.y = -2.0f - g_Rng.GetRandomF32InRange(0.2f);
            item->startPositionOrVelocity.x = g_Rng.GetRandomF32SignedInRange(0.6f);

            if (g_Player.playerState == PLAYER_STATE_DYING)
            {
                item->state = ITEM_STATE_DEFAULT;
                item->startPositionOrVelocity.x = 0.0f;
                item->startPositionOrVelocity.y = -0.9f;
                item->startPositionOrVelocity.z = 0.0f;
            }
        }
        // The initialization is duplicated, but this state has a distinct update transition.
        else if (state == ITEM_STATE_TIME_RISING_TO_APEX)
        {
            item->startPositionOrVelocity.y = -2.0f - g_Rng.GetRandomF32InRange(0.2f);
            item->startPositionOrVelocity.x = g_Rng.GetRandomF32SignedInRange(0.6f);

            if (g_Player.playerState == PLAYER_STATE_DYING)
            {
                item->state = ITEM_STATE_DEFAULT;
                item->startPositionOrVelocity.x = 0.0f;
                item->startPositionOrVelocity.y = -0.9f;
                item->startPositionOrVelocity.z = 0.0f;
            }
        }

#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT) && \
    TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT
        SetAndAuditItemTimeSpawnScript(item, g_BulletManager.bulletAnm,
                                       itemType + 61);
#elif defined(PSP) && TH08_PSP_ITEM_TIME_SPAWN_INIT_PRODUCT_ENABLED
        if (!TryInitializeItemTimeSpawnScript68Fastpath(
                item, g_BulletManager.bulletAnm, itemType + 61))
        {
            g_BulletManager.bulletAnm->SetAndExecuteScriptIdx(
                &item->sprite, itemType + 61);
        }
#else
        g_BulletManager.bulletAnm->SetAndExecuteScriptIdx(&item->sprite,
                                                          itemType + 61);
#endif

        item->sprite.color1.d3dColor = 0xFFFFFFFF;
        item->sprite.zWriteDisabled = true;
        item->isMaxValue = false;
        item->isOnscreen = true;
        this->itemListTail->next = item;
        item->prev = this->itemListTail;
        item->next = NULL;
        this->itemListTail = item;

        break;
    }

    ReplaySyncAudit::RecordItemSpawnResult(
        i < MAX_ITEMS ? ReplaySyncAudit::ITEM_SPAWN_AUDIT_ACCEPTED
                      : ReplaySyncAudit::ITEM_SPAWN_AUDIT_REJECT_POOL_FULL,
        static_cast<i32>(itemType), state, this->nextIndex);
    return i < MAX_ITEMS ? item : &this->items[MAX_ITEMS];
}

DIFFABLE_STATIC_ARRAY_ASSIGN(i32, 6, g_PointItemExtendThresholds) = {100, 250, 500, 800, 1100, 9999};
DIFFABLE_STATIC_ARRAY_ASSIGN(i32, 4, g_ExPointItemExtendThresholds) = {200, 666, 9999, 1};

void ItemManager::UpdatePointItemExtendThreshold()
{
    if (g_GameManager.difficulty < 4)
    {
        if (g_GameManager.globals->pointItemExtendsSoFar < 6)
        {
            g_GameManager.globals->nextPointItemExtendThreshold =
                g_PointItemExtendThresholds[g_GameManager.globals->pointItemExtendsSoFar];
        }
        else
        {
            g_GameManager.globals->nextPointItemExtendThreshold =
                (g_GameManager.globals->pointItemExtendsSoFar - 5) * 500 + g_PointItemExtendThresholds[5];
        }
    }
    else
    {
        if (g_GameManager.globals->pointItemExtendsSoFar < 3)
        {
            g_GameManager.globals->nextPointItemExtendThreshold =
                g_ExPointItemExtendThresholds[g_GameManager.globals->pointItemExtendsSoFar];
        }
        else
        {
            g_GameManager.globals->nextPointItemExtendThreshold = 99999;
        }
    }
}

#if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED
namespace
{
// Preserves the canonical short-circuit exactly: CalcItemBoxCollision runs
// only when the item is not ITEM_STATE_TIME_RISING, with the same arguments
// and the same truthiness test.  Only a sampled item pays two clock reads.
inline bool ItemUpdateSubprofileCollisionProbe(
    Item *item, Float3 *itemBox, bool tickActive, bool sampled,
    th08::psp::ItemUpdateTickCounters *counters)
{
    if (item->state == ITEM_STATE_TIME_RISING)
        return false;
    if (tickActive)
        ++counters->values[th08::psp::kItemUpdateCounterCollisionProbe];
    std::uint64_t startUs = 0U;
    if (sampled)
        startUs = th08::psp::ItemUpdateSubprofileReadClock();
    const u32 collided =
        g_Player.CalcItemBoxCollision(&item->currentPosition, itemBox);
    if (sampled)
        th08::psp::ItemUpdateSubprofileRecordSection(
            th08::psp::ItemUpdateSection::Collision, startUs);
    if (tickActive && collided != 0U)
        ++counters->values[th08::psp::kItemUpdateCounterPickup];
    return collided != 0U;
}
} // namespace
#endif

#if TH08_PSP_ITEM_ATAN2_FASTPATH_PRODUCT_ENABLED
namespace
{
// Same f32 deltas as Player::AngleToPoint (player minus item).  The fast path
// either returns the identical binary32 angle or declines, in which case the
// canonical call runs unchanged (it also owns the both-zero pi/2 case).
inline f32 ItemAutocollectAngleFastpath(Item *item)
{
    const f32 xDelta = g_Player.position.x - item->currentPosition.x;
    const f32 yDelta = g_Player.position.y - item->currentPosition.y;
    f32 angle = 0.0f;
    th08::psp::ItemAtan2FastpathReason reason =
        th08::psp::ItemAtan2FastpathReason::Count;
    if (th08::psp::ItemAtan2FastpathTry(yDelta, xDelta, &angle, &reason))
    {
        th08::psp::ItemAtan2ProductNote(reason);
        return angle;
    }
    th08::psp::ItemAtan2ProductNote(reason);
    return g_Player.AngleToPoint(&item->currentPosition);
}
} // namespace
#endif

// FUNCTION: th08 0x440500
#pragma var_order(speed, interp, pickupScore, angle, itemBox, soundIndex, item)
void ItemManager::OnUpdate()
{
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
    th08::psp::PerfAttributionScope perfScope(
        th08::psp::PerfAttributionPhase::ItemUpdate);
#endif
#if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED
    std::uint32_t itemUpdateRotation = 0U;
    const bool itemUpdateTickActive =
        th08::psp::ItemUpdateSubprofileBeginTick(itemUpdateRotation);
    th08::psp::ItemUpdateTickCounters itemUpdateCounters = {};
    bool itemUpdateSampled = false;
    bool itemUpdateScriptRuns = false;
    std::uint64_t itemUpdateWholeStartUs = 0U;
    std::uint64_t itemUpdateSectionStartUs = 0U;
#endif
#if TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED
    bool itemAtan2Sampled = false;
    std::uint64_t itemAtan2StartUs = 0U;
    std::uint64_t itemAtan2VelocityStartUs = 0U;
#endif
    f32 speed;
    f32 interp;
    i32 pickupScore;
    f32 angle;
    i32 soundIndex = 0;
    Item *item = this->itemListHead.next;
    Float3 itemBox(g_Player.primaryShtFile->itemCollectionBoxSize,
                   g_Player.primaryShtFile->itemCollectionBoxSize, 16.0f);

    this->itemCount = 0;
    speed = g_Player.focusMode ? g_Player.secondaryShtFile->itemMovementSpeed
                                    : g_Player.primaryShtFile->itemMovementSpeed;
    speed *= g_Supervisor.framerateMultiplier;

    while (item != NULL)
    {
        this->itemCount++;
#if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED
        itemUpdateSampled =
            itemUpdateTickActive &&
            th08::psp::ItemUpdateShouldSampleOrdinal(this->itemCount - 1U,
                                                      itemUpdateRotation);
        if (itemUpdateTickActive)
        {
            ++itemUpdateCounters.values[th08::psp::kItemUpdateCounterVisit];
            ++itemUpdateCounters.values[th08::psp::ItemUpdateStateCounter(
                static_cast<int>(item->state))];
        }
        if (itemUpdateSampled)
            itemUpdateWholeStartUs = th08::psp::ItemUpdateSubprofileReadClock();
#endif

        if (item->state == ITEM_STATE_DEATH_DROP_SPREAD)
        {
            if (item->timer < 60)
            {
                interp = (f32)item->timer / 60.0f;
                item->currentPosition =
                    item->targetPosition * interp + item->startPositionOrVelocity * (1.0f - interp);
                goto pickup;
            }
            if (item->timer == 60)
            {
                item->startPositionOrVelocity = Float3(0.0f, 0.0f, 0.0f);
                item->state = ITEM_STATE_DEFAULT;
            }
            goto moveItem;
        }
        else if (item->state == ITEM_STATE_TIME_RISING)
        {
            item->startPositionOrVelocity.y += 0.05f * g_Supervisor.framerateMultiplier;
            if (item->startPositionOrVelocity.y > 0.0f ||
                g_Player.shotTimer < 0)
            {
                item->state = ITEM_STATE_AUTOCOLLECT;
            }
            if (g_Player.playerState == PLAYER_STATE_DYING)
            {
                item->state = ITEM_STATE_DEFAULT;
                item->startPositionOrVelocity.x = 0.0f;
                item->startPositionOrVelocity.y = -0.7f;
                item->startPositionOrVelocity.z = 0.0f;
            }
            goto moveItem;
        }
        else if (item->state == ITEM_STATE_TIME_RISING_TO_APEX)
        {
            item->startPositionOrVelocity.y += 0.05f * g_Supervisor.framerateMultiplier;
            item->currentPosition += item->startPositionOrVelocity * speed;
            if (item->startPositionOrVelocity.y > 0.0f)
            {
                item->state = ITEM_STATE_AUTOCOLLECT;
            }
            else
            {
                goto executeOnly;
            }
            if (g_Player.playerState == PLAYER_STATE_DYING)
            {
                item->state = ITEM_STATE_DEFAULT;
                item->startPositionOrVelocity.x = 0.0f;
                item->startPositionOrVelocity.y = -0.7f;
                item->startPositionOrVelocity.z = 0.0f;
            }
            goto moveItem;
        }
        else
        {
            if (item->state == ITEM_STATE_AUTOCOLLECT ||
                (g_Player.position.y < g_Player.primaryShtFile->pointItemValueLine &&
                 (g_GameManager.GetPower() >= 0.0 ||
                  g_Player.focusMode != PLAYER_FOCUS_MODE_UNFOCUSED ||
                  g_GameManager.shotType == 1 || g_GameManager.shotType == 6)))
            {
                if (g_Player.playerState != PLAYER_STATE_DYING && g_Player.playerState != PLAYER_STATE_SPAWNING)
                {
#if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED
                    if (itemUpdateTickActive)
                        ++itemUpdateCounters
                              .values[th08::psp::kItemUpdateCounterAutocollect];
                    if (itemUpdateSampled)
                        itemUpdateSectionStartUs =
                            th08::psp::ItemUpdateSubprofileReadClock();
#endif
#if TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED
                    itemAtan2Sampled =
                        th08::psp::ItemAtan2AuditBeginCall(&itemAtan2StartUs);
#endif
#if TH08_PSP_ITEM_ATAN2_FASTPATH_PRODUCT_ENABLED
                    angle = ItemAutocollectAngleFastpath(item);
#else
                    angle = g_Player.AngleToPoint(&item->currentPosition);
#endif
#if TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED
                    th08::psp::ItemAtan2AuditAfterCanonical(
                        static_cast<u32>(item - this->items),
                        g_Player.position.x, g_Player.position.y,
                        item->currentPosition.x, item->currentPosition.y,
                        angle, itemAtan2Sampled, itemAtan2StartUs);
                    if (itemAtan2Sampled)
                        itemAtan2VelocityStartUs =
                            th08::psp::ItemAtan2AuditReadClock();
#endif
#if defined(PSP) && defined(TH08_PSP_ITEM_AUTOCOLLECT_CACHE) && \
    TH08_PSP_ITEM_AUTOCOLLECT_CACHE
                    SetItemAutocollectVelocity(
                        this, item, angle,
                        g_Player.primaryShtFile->itemAutoCollectSpeed);
#elif defined(PSP)
                    ItemAutocollectVelocityCompute(
                        item, angle, g_Player.primaryShtFile->itemAutoCollectSpeed);
#else
                    item->startPositionOrVelocity.FromAngleMagnitude(
                        angle, g_Player.primaryShtFile->itemAutoCollectSpeed);
#endif
#if TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED
                    if (itemAtan2Sampled)
                        th08::psp::ItemAtan2AuditEndVelocity(
                            itemAtan2VelocityStartUs);
#endif
#if defined(PSP) && defined(TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT) && \
    TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT
                    ObserveItemAutocollectCanonicalResult(
                        this, item, angle,
                        g_Player.primaryShtFile->itemAutoCollectSpeed);
#endif
#if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED
                    if (itemUpdateSampled)
                        th08::psp::ItemUpdateSubprofileRecordSection(
                            th08::psp::ItemUpdateSection::Autocollect,
                            itemUpdateSectionStartUs);
#endif
                    item->state = ITEM_STATE_AUTOCOLLECT;
                    item->currentPosition += item->startPositionOrVelocity * g_Supervisor.framerateMultiplier;
                    goto pickup;
                }
                item->startPositionOrVelocity.y = -0.7f;
                item->state = ITEM_STATE_DEFAULT;
            }
            else
            {
                item->startPositionOrVelocity.x = 0.0f;
                item->startPositionOrVelocity.z = 0.0f;
                if (item->startPositionOrVelocity.y < -2.2f)
                    item->startPositionOrVelocity.y = -2.2f;
            }
        }

moveItem:
        item->currentPosition += item->startPositionOrVelocity * speed;
        if (item->state == ITEM_STATE_DEFAULT && g_GameManager.arcadeRegionSize.y + 16.0f <= item->currentPosition.y)
        {
            g_GameManager.DecreaseSubrank(3);
            item->Delete();
#if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED
            if (itemUpdateTickActive)
                ++itemUpdateCounters.values[th08::psp::kItemUpdateCounterOffscreen];
            if (itemUpdateSampled)
                th08::psp::ItemUpdateSubprofileRecordSection(
                    th08::psp::ItemUpdateSection::Whole, itemUpdateWholeStartUs);
#endif
            item = item->next;
            continue;
        }

        if (item->startPositionOrVelocity.operator float *()[1] < 3.0f)
            item->startPositionOrVelocity.y += 0.03f * speed;
        else
            item->startPositionOrVelocity.y = 3.0f;

pickup:
#if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED
        if (ItemUpdateSubprofileCollisionProbe(item, &itemBox,
                                               itemUpdateTickActive,
                                               itemUpdateSampled,
                                               &itemUpdateCounters))
#else
        if (item->state != ITEM_STATE_TIME_RISING &&
            g_Player.CalcItemBoxCollision(&item->currentPosition, &itemBox))
#endif
        {
            g_ReplayManager->frameEventFlags |= 0x40;
            switch (item->itemType)
            {
            case ITEM_POWER_SMALL:
                item->CollectPowerSmall();
                break;
            case ITEM_POINT:
                item->CollectPoint();
                break;
            case ITEM_POINT_SMALL:
                item->CollectPointSmall();
                break;
            case ITEM_POWER_BIG:
                item->CollectPowerBig();
                break;
            case ITEM_BOMB:
                if (g_GameManager.GetBombsRemaining() < 8)
                {
                    g_GameManager.AddToBombCount(1);
                    g_Gui.flags.bombDisplayUpdateFrames = 2;
                }
                g_GameManager.IncreaseSubrank(5);
                break;
            case ITEM_EXTEND:
                g_GameManager.CollectExtend();
                break;
            case ITEM_POWER_FULL:
                if (g_GameManager.GetPower() < 128)
                {
                    g_BulletManager.ClearBulletsForTransition();
                    g_Gui.ShowPopupText(0, 1);
                    g_SoundPlayer.PlaySoundByIdx(SOUND_POWERUP, 0);
                    g_AsciiManager.CreatePlayerPointPopup(&item->currentPosition, -1, 0xffffc0a0);
                    this->ConvertAllPowerItemsToTimeOrbs(item);
                }
                g_GameManager.SetPower(128);
                g_GameManager.AddScore(1000);
                g_AsciiManager.CreatePlayerPointPopup(&item->currentPosition, 1000, 0xffffffff);
                g_Gui.flags.powerDisplayUpdateFrames = 2;
                break;
            case ITEM_POINT_STAR:
                if (g_Player.itemTimeOrbMode == 0)
                {
                    pickupScore = (g_GameManager.globals->graze / 40) * 10 + 300;
                    if (pickupScore <= 0)
                        pickupScore = 10;
                }
                else
                {
                    pickupScore = 100;
                }
                g_AsciiManager.CreateScorePopup(&item->currentPosition, pickupScore, 0xffffffff);
                g_GameManager.AddScore(pickupScore);
                break;
            case ITEM_TIME:
                item->CollectTimeOrb();
                break;
            default:
                break;
            }

            if (soundIndex <= SOUND_ITEM)
                soundIndex = item->isMaxValue ? SOUND_2C : SOUND_ITEM;
            item->Delete();
#if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED
            if (itemUpdateSampled)
                th08::psp::ItemUpdateSubprofileRecordSection(
                    th08::psp::ItemUpdateSection::Whole, itemUpdateWholeStartUs);
#endif
            item = item->next;
            continue;
        }

executeOnly:
        item->timer++;
#if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED
        itemUpdateScriptRuns = item->sprite.currentInstruction != NULL;
        if (itemUpdateTickActive && itemUpdateScriptRuns)
            ++itemUpdateCounters.values[th08::psp::kItemUpdateCounterScript];
        if (itemUpdateSampled && itemUpdateScriptRuns)
            itemUpdateSectionStartUs = th08::psp::ItemUpdateSubprofileReadClock();
#endif
        if (item->sprite.currentInstruction != NULL)
#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_ANM_IDLE_AUDIT) && \
    TH08_PSP_ITEM_TIME_ANM_IDLE_AUDIT
            ExecuteItemAnmWithIdleAudit(item);
#elif defined(PSP) && TH08_PSP_ITEM_TIME_ANM_IDLE_PRODUCT_ENABLED
            ExecuteItemAnmWithIdleFastpath(item);
#else
            g_AnmManager->ExecuteScript(&item->sprite);
#endif
#if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED
        if (itemUpdateSampled && itemUpdateScriptRuns)
            th08::psp::ItemUpdateSubprofileRecordSection(
                th08::psp::ItemUpdateSection::Script, itemUpdateSectionStartUs);
        if (itemUpdateSampled)
            th08::psp::ItemUpdateSubprofileRecordSection(
                th08::psp::ItemUpdateSection::Whole, itemUpdateWholeStartUs);
#endif
        item = item->next;
    }
#if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED
    if (itemUpdateTickActive)
        th08::psp::ItemUpdateSubprofileEndTick(this->itemCount,
                                               itemUpdateCounters);
#endif

    if (soundIndex != 0)
        g_SoundPlayer.PlaySoundByIdx((SoundIdx)soundIndex, 0);

    if (g_Player.timeOrbGaugeChangeSuppressionTimer != 0)
    {
        g_Player.timeOrbGaugeChangeSuppressionTimer--;
        if (g_Player.timeOrbGaugeChangeSuppressionTimer <= 0)
            g_Player.timeOrbGaugeChangeSuppressionTimer = 0;
    }
}

// FUNCTION: th08 0x440cf0
#pragma var_order(powerLevel, oldPowerLevel)
void Item::CollectPowerSmall()
{
    i32 powerLevel;
    i32 oldPowerLevel;

    if (g_GameManager.GetPower() >= 0x80)
    {
        goto increaseSubrank;
    }

    powerLevel = 0;
    while (g_GameManager.GetPower() >= g_PowerUpThresholds[powerLevel])
    {
        powerLevel++;
    }
    oldPowerLevel = powerLevel;

    g_GameManager.character = 0;
    g_GameManager.AddPower(1);

    if (g_GameManager.GetPower() >= 0x80)
    {
        g_GameManager.SetPower(0x80);
        if (!g_Spellcard.IsActive())
        {
            g_BulletManager.ClearBulletsForTransition();
        }
        g_Gui.ShowPopupText(0, 1);
        g_ItemManager.ConvertAllPowerItemsToTimeOrbs(this);
    }

    g_GameManager.AddScore(10);
    g_Gui.flags.powerDisplayUpdateFrames = 2;

    while (g_GameManager.GetPower() >= g_PowerUpThresholds[powerLevel])
    {
        powerLevel++;
    }

    if (powerLevel != oldPowerLevel)
    {
        g_AsciiManager.CreateScorePopup(&this->currentPosition, -1, 0xffffc0a0);
        g_SoundPlayer.PlaySoundByIdx(SOUND_POWERUP, 0);
    }
    else
    {
        g_AsciiManager.CreateScorePopup(&this->currentPosition, 10, 0xffffffff);
    }

increaseSubrank:
    g_GameManager.IncreaseSubrank(1);
}

// FUNCTION: th08 0x440e40
#pragma var_order(pointItemValueBase, currentPointItemValue)
void Item::CollectPoint()
{
    i32 pointItemValueBase = g_GameManager.globals->pointItemValue;
    i32 currentPointItemValue;

    currentPointItemValue = static_cast<ZunBool>(this->currentPosition.y < g_Player.primaryShtFile->pointItemValueLine)
                                ? pointItemValueBase
                                : pointItemValueBase / 2 -
                                      (i32)(this->currentPosition.y - g_Player.primaryShtFile->pointItemValueLine) *
                                          (g_GameManager.globals->pointItemValue / 1000);
    if (this->isMaxValue == 1)
    {
        currentPointItemValue = pointItemValueBase;
    }

    currentPointItemValue -= currentPointItemValue % 10;
    if (g_GameManager.GaugeIsExtremelyHuman())
    {
        currentPointItemValue += currentPointItemValue;
    }

    g_AsciiManager.CreateScorePopup(&this->currentPosition, currentPointItemValue,
                                    currentPointItemValue >= pointItemValueBase ? 0xffffff00 : 0xffffffff);
    if (currentPointItemValue >= pointItemValueBase)
    {
        this->isMaxValue = true;
    }

    g_GameManager.AddScore(currentPointItemValue);
    g_GameManager.globals->pointItemsCollectedInStage++;
    g_GameManager.globals->pointItemsCollected++;
    g_Gui.flags.pointDisplayUpdateFrames = 2;

    if (currentPointItemValue >= pointItemValueBase)
    {
        g_GameManager.IncreaseSubrank(10);
    }
    else
    {
        g_GameManager.IncreaseSubrank(3);
    }

    if ((i32)g_GameManager.globals->pointItemExtendsSoFar >= 0)
    {
        while ((ItemManager::UpdatePointItemExtendThreshold(),
                g_GameManager.globals->pointItemsCollected >= g_GameManager.globals->nextPointItemExtendThreshold))
        {
            g_GameManager.CollectExtend();
            g_GameManager.globals->pointItemExtendsSoFar++;
        }
    }

    g_MaxValuePointItemsCollected++;
    g_GameManager.UpdateAntiTamper();
}

// FUNCTION: th08 0x441020
#pragma var_order(pointItemValueBase, currentPointItemValue)
void Item::CollectPointSmall()
{
    i32 pointItemValueBase = g_GameManager.globals->pointItemValue;
    i32 currentPointItemValue;

    currentPointItemValue = static_cast<ZunBool>(this->currentPosition.y < g_Player.primaryShtFile->pointItemValueLine)
                                ? pointItemValueBase
                                : pointItemValueBase / 2 -
                                      (i32)(this->currentPosition.y - g_Player.primaryShtFile->pointItemValueLine) *
                                          (g_GameManager.globals->pointItemValue / 1000);
    if (this->isMaxValue == 1)
    {
        currentPointItemValue = pointItemValueBase;
    }

    pointItemValueBase /= 10;
    pointItemValueBase -= pointItemValueBase % 10;
    currentPointItemValue /= 10;
    currentPointItemValue -= currentPointItemValue % 10;
    if (g_GameManager.GaugeIsExtremelyHuman())
    {
        currentPointItemValue += currentPointItemValue;
    }

    g_AsciiManager.CreateScorePopup(&this->currentPosition, currentPointItemValue,
                                    currentPointItemValue >= pointItemValueBase ? 0xffffff00 : 0xffffffff);
    g_GameManager.AddScore(currentPointItemValue);
    if (currentPointItemValue >= pointItemValueBase)
    {
        this->isMaxValue = true;
    }
}

// FUNCTION: th08 0x441170
#pragma var_order(powerLevel, oldPowerLevel)
void Item::CollectPowerBig()
{
    i32 powerLevel;
    i32 oldPowerLevel;

    if (g_GameManager.GetPower() >= 0x80)
    {
        return;
    }

    powerLevel = 0;
    while (g_GameManager.GetPower() >= g_PowerUpThresholds[powerLevel])
    {
        powerLevel++;
    }
    oldPowerLevel = powerLevel;

    g_GameManager.AddPower(8);

    if (g_GameManager.GetPower() >= 0x80)
    {
        g_GameManager.SetPower(0x80);
        if (!g_Spellcard.IsActive())
        {
            g_BulletManager.ClearBulletsForTransition();
        }
        g_Gui.ShowPopupText(0, 1);
        g_ItemManager.ConvertAllPowerItemsToTimeOrbs(this);
    }

    g_Gui.flags.powerDisplayUpdateFrames = 2;
    g_GameManager.AddScore(10);

    while (g_GameManager.GetPower() >= g_PowerUpThresholds[powerLevel])
    {
        powerLevel++;
    }

    if (powerLevel != oldPowerLevel)
    {
        g_AsciiManager.CreateScorePopup(&this->currentPosition, -1, 0xffffc0a0);
        g_SoundPlayer.PlaySoundByIdx(SOUND_POWERUP, 0);
    }
    else
    {
        g_AsciiManager.CreateScorePopup(&this->currentPosition, 10, 0xffffffff);
    }
}

// FUNCTION: th08 0x4412b0
#pragma var_order(score)
void Item::CollectTimeOrb()
{
    i32 score;

    if (g_Player.itemTimeOrbMode == 0)
    {
        if (g_GameManager.globals->pointItemsCollectedInStage >= 2000)
        {
            score = 10000;
        }
        else
        {
            score = (g_GameManager.globals->pointItemsCollected / 2) * 10;
            if (score < 100)
                score = 100;
        }
    }
    else
    {
        score = 100;
    }

    if (this != NULL)
    {
        g_AsciiManager.CreateScorePopup(
            &this->currentPosition, score,
            g_GameManager.GetTimeOrbs() < g_GameManager.GetLastSpellTimeOrbThreshold() ? -536870913 : -536875136);
    }

    g_Gui.flags.timeDisplayUpdateFrames = 2;
    g_GameManager.AddScore(score);
    g_GameManager.AddTimeOrbs(1);
    g_Spellcard.AddBonusProgress(8000);

    if (g_Player.timeOrbGaugeChangeSuppressionTimer == 0)
    {
        score = 111;
        g_GameManager.AddToYoukaiGauge(
            g_Player.focusMode ? score : -score, 0);
    }
}

// FUNCTION: th08 0x4413e0
void ItemManager::AutoCollectAllItems()
{
    Item *item = this->itemListHead.next;
    while (item != NULL)
    {
        item->state = ITEM_STATE_AUTOCOLLECT;
        item->startPositionOrVelocity = Float3(0.0f, -0.5f, 0.0f);
        item = item->next;
    }
}

// FUNCTION: th08 0x441450
void ItemManager::ConvertAllPowerItemsToTimeOrbs(Item *item)
{
    Item *current = this->itemListHead.next;

    while (current != NULL)
    {
        if (current != item && (current->itemType == ITEM_POWER_SMALL || current->itemType == ITEM_POWER_BIG))
        {
            if (current->startPositionOrVelocity.y > -0.5f)
            {
                current->startPositionOrVelocity.x = 0.0f;
                current->startPositionOrVelocity.y = -0.5f;
                current->startPositionOrVelocity.z = 0.0f;
            }
            g_EffectManager.SpawnEffect(0, reinterpret_cast<D3DXVECTOR3 *>(&current->currentPosition), 1, -1);
            current->itemType = ITEM_POINT_SMALL;
            g_BulletManager.bulletAnm->SetAndExecuteScriptIdx(&current->sprite, ITEM_POINT_SMALL + 61);
        }
        current = current->next;
    }
}

// FUNCTION: th08 0x441530
void ItemManager::CancelAutoCollect()
{
    Item *item = this->itemListHead.next;
    while (item != NULL)
    {
        if (item->state == ITEM_STATE_AUTOCOLLECT)
        {
            item->state = ITEM_STATE_DEFAULT;
            item->startPositionOrVelocity.x = 0.0f;
            item->startPositionOrVelocity.y = -0.9f;
            item->startPositionOrVelocity.z = 0.0f;
        }
        item = item->next;
    }
}

// FUNCTION: th08 0x4415a0
#pragma var_order(alpha, item, this)
void ItemManager::OnDraw()
{
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
    th08::psp::PerfAttributionScope perfScope(
        th08::psp::PerfAttributionPhase::ItemDraw);
#endif
    i32 alpha;
    Item *item = this->itemListHead.next;

#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
    PspItemNaturalQuadNotePass();
#elif TH08_PSP_ITEM_TIME_DRAW_PAIR_ENABLED
    g_AnmManager->BeginPspItemTimeDrawPairPass();
#endif

    while (item != NULL)
    {
#if defined(PSP)
        th08::psp::RenderPerfNoteItemDrawn();
#endif
        item->sprite.pos.x = g_GameManager.arcadeRegionTopLeftPos.x + item->currentPosition.x;
        item->sprite.pos.y = g_GameManager.arcadeRegionTopLeftPos.y + item->currentPosition.y;
        item->sprite.pos.z = 0.15f;

        // Keep the target's Float3::operator float *() call shape: direct .y
        // access removes both calls and changes the VC7 function extent.
        if (((f32 *)item->currentPosition)[1] < -8.0f)
        {
            item->sprite.pos.y = 8.0f + g_GameManager.arcadeRegionTopLeftPos.y;
            if (item->isOnscreen)
            {
                g_BulletManager.bulletAnm->SetSprite(&item->sprite, item->itemType + 0xb6);
                item->isOnscreen = false;
                item->sprite.zWriteDisabled = true;
            }

            alpha = 255 - (i32)(((8.0f - ((f32 *)item->currentPosition)[1]) * 255.0f) / 128.0f);
            if (alpha < 0x40)
            {
                alpha = 0x40;
            }
            item->sprite.color1.d3dColor = (item->sprite.color1.d3dColor & 0xffffff) | (alpha << 24);
        }
        else
        {
            if (!item->isOnscreen)
            {
                g_BulletManager.bulletAnm->SetSprite(&item->sprite, item->itemType + 0xac);
                item->isOnscreen = true;
                item->sprite.color1.d3dColor = 0xffffffff;
                item->sprite.zWriteDisabled = true;
            }
        }

#if TH08_PSP_ITEM_TIME_INLINE_DRAW_ENABLED
        if (item->itemType == ITEM_TIME)
        {
#if defined(TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT) && \
    TH08_PSP_ITEM_TIME_INLINE_DRAW_AUDIT
            AuditItemTimeInlineDraw(&item->sprite);
#elif TH08_PSP_ITEM_TIME_INLINE_DRAW_PRODUCT_ENABLED
            if (!TryDrawItemTimeInline(&item->sprite))
            {
                g_AnmManager->Draw2D(&item->sprite);
            }
#endif
        }
        else
        {
            g_AnmManager->Draw2D(&item->sprite);
        }
#elif TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
        if (item->itemType == ITEM_TIME)
        {
            // This token identifies only the current Draw2D call.  DrawInner
            // marks the existing natural batch after its canonical 6V append;
            // culled draws never mark and no pass Begin/End or Flush is added.
            PspItemNaturalQuadSetCurrentTarget(true);
            g_AnmManager->Draw2D(&item->sprite);
            PspItemNaturalQuadSetCurrentTarget(false);
        }
        else
        {
            g_AnmManager->Draw2D(&item->sprite);
        }
#elif TH08_PSP_ITEM_TIME_DRAW_PAIR_ENABLED
        if (item->itemType == ITEM_TIME)
        {
            const i32 expectedSpriteIndex = item->isOnscreen ? 179 : 189;
            const PspItemTimeDrawPairRejectReason identityReason =
                g_AnmManager->PspValidateItemTimeDrawPairIdentity(
                    &item->sprite, g_BulletManager.bulletAnm,
                    expectedSpriteIndex);
#if defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) && \
    TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT
            g_AnmManager->DrawPspItemTimePairAudit(
                &item->sprite, identityReason);
#elif TH08_PSP_ITEM_TIME_DRAW_PAIR_PRODUCT_ENABLED
            if (!g_AnmManager->TryDrawPspItemTimeSpritePair(
                    &item->sprite, identityReason))
            {
                g_AnmManager->Draw2D(&item->sprite);
            }
#endif
        }
        else
        {
            // A non-ITEM_TIME sprite must never be reordered across a delayed
            // pair run, even when it happens to share texture/render state.
            g_AnmManager->PspItemTimeDrawPairBoundary();
            g_AnmManager->Draw2D(&item->sprite);
        }
#else
        g_AnmManager->Draw2D(&item->sprite);
#endif
        item = item->next;
    }

#if TH08_PSP_ITEM_TIME_DRAW_PAIR_ENABLED
    g_AnmManager->EndPspItemTimeDrawPairPass();
#endif
}

void Item::Delete()
{
    this->isInUse = false;
    this->prev->next = this->next;
    if (this->next != NULL)
    {
        this->next->prev = this->prev;
    }
    if (g_ItemManager.itemListTail == this)
    {
        g_ItemManager.itemListTail = this->prev;
    }
}

i32 ItemManager::GetTimeOrbCount()
{
    Item *next = this->itemListHead.next;
    i32 count = 0;

    while (next != NULL)
    {
        if (next->itemType == ITEM_TIME)
        {
            count++;
        }
        next = next->next;
    }

    return count;
}

} /* namespace th08 */
