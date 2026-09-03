#include "th_pch.h"

#include "BulletManager.hpp"
#include "GameManager.hpp"
#include "EclManager.hpp"
#include "ItemManager.hpp"
#include "Player.hpp"
#include "ReplayManager.hpp"
#include "SoundPlayer.hpp"
#include "Supervisor.hpp"

#if defined(PSP)
#include "perf_attribution.hpp"
#endif

#if defined(TH08_PSP_STAGE_POOL_ARENA)
#include "stage_pool_arena.hpp"
#endif
#if defined(PSP) && defined(TH08_PSP_STAGE_POOL_ARENA) && \
    defined(TH08_PSP_BULLET_FASTPATH) && TH08_PSP_BULLET_FASTPATH
#include "render_math.hpp"
#include <cstdio>
#define TH08_PSP_BULLET_RUNTIME_FASTPATH 1
#endif
#if defined(PSP) && defined(TH08_PSP_BULLET_CANCEL_SPATIAL) && \
    TH08_PSP_BULLET_CANCEL_SPATIAL
#if !defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
#error TH08_PSP_BULLET_CANCEL_SPATIAL requires the Bullet runtime sidecar
#endif
#define TH08_PSP_BULLET_CANCEL_SPATIAL_ENABLED 1
#endif
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH) && \
    defined(TH08_PSP_BULLET_LIVE_ENUM) && TH08_PSP_BULLET_LIVE_ENUM
#include "PspBulletLiveEnumerator.hpp"
#define TH08_PSP_BULLET_LIVE_ENUM_ENABLED 1
#endif
#if defined(PSP) && defined(TH08_PSP_BULLET_TRANSFORM_AUDIT) && \
    TH08_PSP_BULLET_TRANSFORM_AUDIT
#define TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED 1
#endif
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH) && \
    defined(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH) && \
    TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH
#define TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH_ENABLED 1
#endif
#if defined(PSP) && defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT) && \
    TH08_PSP_BULLET_COLLISION_GATE_AUDIT
#if !defined(TH08_PSP_PLAYER_SCAN_SIDECAR) || \
    !TH08_PSP_PLAYER_SCAN_SIDECAR
#error TH08_PSP_BULLET_COLLISION_GATE_AUDIT requires TH08_PSP_PLAYER_SCAN_SIDECAR
#endif
#include "PspBulletCollisionGate.hpp"
#define TH08_PSP_BULLET_COLLISION_GATE_AUDIT_ENABLED 1
#endif
#if defined(PSP) && defined(TH08_PSP_BULLET_COLLISION_GATE) && \
    TH08_PSP_BULLET_COLLISION_GATE
#if !defined(TH08_PSP_PLAYER_SCAN_SIDECAR) || \
    !TH08_PSP_PLAYER_SCAN_SIDECAR
#error TH08_PSP_BULLET_COLLISION_GATE requires TH08_PSP_PLAYER_SCAN_SIDECAR
#endif
#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT_ENABLED)
#error TH08_PSP_BULLET_COLLISION_GATE and its audit are mutually exclusive
#endif
#include "PspBulletCollisionGate.hpp"
#include "bullet_gate_stats.hpp"
#define TH08_PSP_BULLET_COLLISION_GATE_PRODUCT_ENABLED 1
#endif

namespace th08
{

DIFFABLE_STATIC(BulletManager, g_BulletManager);
DIFFABLE_STATIC(ChainElem, g_BulletManagerCalcChain);
DIFFABLE_STATIC(ChainElem, g_BulletManagerDrawChain);

#if defined(PSP)
namespace
{
// Keep this reservation present with the audit disabled.  A future product
// A/B can therefore toggle code without moving later globals or changing BSS
// geometry, while all fields remain diagnostic-only.
PspBulletCollisionGateAuditTelemetrySnapshot
    g_PspBulletCollisionGateAuditTelemetry{};
}
#endif

#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
namespace
{
constexpr u32 kPspBulletActiveWordCount =
    (static_cast<u32>(th08::psp::kBulletPoolLogicalCount) + 31U) / 32U;
#if defined(TH08_PSP_BULLET_LIVE_ENUM_ENABLED)
static_assert(kPspBulletActiveWordCount ==
                  th08::psp::PspBulletLiveEnumerator::kWordCount,
              "Bullet runtime sidecar/enumerator word counts must agree");
#endif

// Presentation values deliberately live beside, not inside, Bullet.  Nothing
// here participates in simulation, collision, RNG, replay serialization, or
// the reconstructed Bullet ABI.
struct PspBulletRenderRotationCache
{
    f32 sourceAngle;
    f32 renderAngle;
    f32 sine;
    f32 cosine;
    u32 valid;
};

struct PspBulletRuntimeCache
{
    u32 activeBits[kPspBulletActiveWordCount];
    PspBulletRenderRotationCache
        renderRotation[th08::psp::kBulletPoolLogicalCount];
    u32 transformTerminalBits[kPspBulletActiveWordCount];
};

static_assert(sizeof(PspBulletRenderRotationCache) == 5U * sizeof(u32),
              "Bullet render cache must remain five words");
static_assert(sizeof(PspBulletRuntimeCache) ==
                  th08::psp::kBulletRuntimeCacheBytes,
              "Stage arena Bullet runtime cache layout drifted");
static_assert(sizeof(PspBulletRuntimeCache::transformTerminalBits) ==
                  th08::psp::kBulletTransformTerminalBytes,
              "Bullet transform terminal sidecar must remain exactly 192 bytes");

PspBulletRuntimeCache *g_PspBulletRuntimeCache = NULL;
bool g_PspBulletRuntimeActiveBitsValid = false;
// Reserve the diagnostic state in both OFF/ON variants so the isolated live
// enumerator A/B cannot move later PSP globals or alter heap geometry.
PspBulletLiveEnumTelemetrySnapshot g_PspBulletLiveEnumTelemetry{};
struct PspBulletCancelSpatialCounters
{
    u32 calls;
    u32 indexedQueries;
    u32 rejectedQueries;
    u32 circles;
    u32 rects;
    u32 rebuilds;
    u32 fullCandidates;
    u32 indexedCandidates;
    u32 fallbackCandidates;
    u32 exactTests;
    u32 falsePositives;
    u32 fallbacks;
    u32 occupancyOwnerFallbacks;
    u32 unsupportedRegionFallbacks;
    u32 nonfiniteFallbacks;
    u32 duplicatePairs;
    u32 duplicateReplays;
    u32 duplicateExactTestsSaved;
    u32 duplicateFallbacks;
};

// Reserved in both OFF/ON builds. This keeps later PSP globals and BSS/heap
// geometry identical during the isolated experiment; OFF emits no hot code.
PspBulletCancelSpatialCounters g_PspBulletCancelSpatialCounters{};
// Reserve diagnostics in both OFF/ON builds, just like the sidecar storage,
// so an isolated feature A/B does not move later PSP globals.  Hot-path
// counters are 32-bit and reset every telemetry window; `firedCalls` is
// derived from the three mutually-exclusive outcomes.  This avoids two
// emulated 64-bit increments per live Bullet on Allegrex.
struct PspBulletTransformTerminalCounters
{
    u32 hits;
    u32 misses;
    u32 fallbacks;
    u32 invariantWitnesses;
    u32 repairs;
    u32 terminalIndexMarks;
    u32 terminalNoneMarks;
    u32 markFallbacks;
    u32 spawnProgramClears;
    u32 wholeSlotClears;
};
PspBulletTransformTerminalCounters g_PspBulletTransformTerminalTelemetry{};

#if defined(TH08_PSP_BULLET_LIVE_ENUM_ENABLED) && \
    TH08_PSP_RUNTIME_TELEMETRY
void PspBulletLiveEnumCommitFrame(u32 slotProbes, u32 wordProbes,
                                  u32 visited, bool fallback)
{
    ++g_PspBulletLiveEnumTelemetry.frames;
    g_PspBulletLiveEnumTelemetry.slotProbes += slotProbes;
    g_PspBulletLiveEnumTelemetry.wordProbes += wordProbes;
    g_PspBulletLiveEnumTelemetry.visited += visited;
    if (fallback)
        ++g_PspBulletLiveEnumTelemetry.fallbackFrames;
}
#endif

bool PspBulletSlotIndex(const Bullet *bullet, u32 *outIndex)
{
    if (bullet == NULL || outIndex == NULL || g_BulletManager.bullets == NULL)
        return false;

    const std::uintptr_t address =
        reinterpret_cast<std::uintptr_t>(bullet);
    const std::uintptr_t base =
        reinterpret_cast<std::uintptr_t>(g_BulletManager.bullets);
    if (address < base)
        return false;
    const std::uintptr_t offset = address - base;
    const std::uintptr_t logicalBytes =
        sizeof(Bullet) * th08::psp::kBulletPoolLogicalCount;
    if (offset >= logicalBytes || offset % sizeof(Bullet) != 0U)
        return false;

    *outIndex = static_cast<u32>(offset / sizeof(Bullet));
    return true;
}

inline bool PspBulletRuntimeBit(u32 index)
{
    // A missing or invalid sidecar must fall back to the original state test
    // rather than suppressing an otherwise live Bullet.
    if (g_PspBulletRuntimeCache == NULL ||
        !g_PspBulletRuntimeActiveBitsValid)
        return true;
    return (g_PspBulletRuntimeCache->activeBits[index >> 5U] &
            (1U << (index & 31U))) != 0U;
}

#if defined(TH08_REPLAY_SYNC_AUDIT)
inline bool PspBulletRuntimeRawBit(u32 index)
{
    return g_PspBulletRuntimeCache != NULL &&
           (g_PspBulletRuntimeCache->activeBits[index >> 5U] &
            (1U << (index & 31U))) != 0U;
}
#endif

inline void PspBulletRuntimeSetBit(u32 index, bool active)
{
    if (g_PspBulletRuntimeCache == NULL)
        return;
    u32 &word = g_PspBulletRuntimeCache->activeBits[index >> 5U];
    const u32 mask = 1U << (index & 31U);
    if (active)
        word |= mask;
    else
        word &= ~mask;
}

inline void PspBulletRuntimeInvalidateRotation(u32 index)
{
    if (g_PspBulletRuntimeCache != NULL)
        g_PspBulletRuntimeCache->renderRotation[index].valid = 0U;
}

void PspBulletRuntimeActivate(Bullet *bullet)
{
    u32 index;
    if (!PspBulletSlotIndex(bullet, &index))
        return;
    PspBulletRuntimeInvalidateRotation(index);
    PspBulletRuntimeSetBit(index, true);
}

void PspBulletRuntimeDeactivate(Bullet *bullet)
{
    u32 index;
    if (!PspBulletSlotIndex(bullet, &index))
        return;
    PspBulletRuntimeSetBit(index, false);
    PspBulletRuntimeInvalidateRotation(index);
}

#if defined(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH_ENABLED)
inline bool PspBulletTransformTerminalRawBit(u32 index)
{
    return g_PspBulletRuntimeCache != NULL &&
           (g_PspBulletRuntimeCache->transformTerminalBits[index >> 5U] &
            (1U << (index & 31U))) != 0U;
}

inline void PspBulletTransformTerminalSetBit(u32 index, bool terminal)
{
    u32 &word =
        g_PspBulletRuntimeCache->transformTerminalBits[index >> 5U];
    const u32 mask = 1U << (index & 31U);
    if (terminal)
        word |= mask;
    else
        word &= ~mask;
}

bool PspBulletTransformTerminalSetFor(Bullet *bullet, bool terminal)
{
    u32 index;
    if (g_PspBulletRuntimeCache == NULL ||
        !PspBulletSlotIndex(bullet, &index))
    {
        return false;
    }
    PspBulletTransformTerminalSetBit(index, terminal);
    return true;
}

inline void PspBulletTransformTerminalClearForSpawn(Bullet *bullet)
{
    if (PspBulletTransformTerminalSetFor(bullet, false))
        ++g_PspBulletTransformTerminalTelemetry.spawnProgramClears;
    else
        ++g_PspBulletTransformTerminalTelemetry.markFallbacks;
}

inline void PspBulletTransformTerminalClearForWholeSlotReset(Bullet *bullet)
{
    if (PspBulletTransformTerminalSetFor(bullet, false))
        ++g_PspBulletTransformTerminalTelemetry.wholeSlotClears;
    else
        ++g_PspBulletTransformTerminalTelemetry.markFallbacks;
}

inline void PspBulletTransformTerminalMarkIndex(Bullet *bullet)
{
    if (PspBulletTransformTerminalSetFor(bullet, true))
        ++g_PspBulletTransformTerminalTelemetry.terminalIndexMarks;
    else
        ++g_PspBulletTransformTerminalTelemetry.markFallbacks;
}

inline void PspBulletTransformTerminalMarkNone(Bullet *bullet)
{
    if (PspBulletTransformTerminalSetFor(bullet, true))
        ++g_PspBulletTransformTerminalTelemetry.terminalNoneMarks;
    else
        ++g_PspBulletTransformTerminalTelemetry.markFallbacks;
}

inline bool PspBulletTransformTerminalSkip(u32 index)
{
    if (g_PspBulletRuntimeCache == NULL)
    {
        ++g_PspBulletTransformTerminalTelemetry.fallbacks;
        return false;
    }
    if (!PspBulletTransformTerminalRawBit(index))
    {
        ++g_PspBulletTransformTerminalTelemetry.misses;
        return false;
    }
    ++g_PspBulletTransformTerminalTelemetry.hits;
    return true;
}
#endif

PspBulletRenderRotationCache *PspBulletRenderCacheFor(Bullet *bullet)
{
    u32 index;
    if (g_PspBulletRuntimeCache == NULL ||
        !PspBulletSlotIndex(bullet, &index))
    {
        return NULL;
    }
    return &g_PspBulletRuntimeCache->renderRotation[index];
}

#if defined(TH08_REPLAY_SYNC_AUDIT)
void PspBulletRuntimeAuditAndRepair(BulletManager *bulletManager)
{
    if (g_PspBulletRuntimeCache == NULL || bulletManager == NULL ||
        bulletManager->bullets == NULL)
    {
        return;
    }

    const bool rebuilt = !g_PspBulletRuntimeActiveBitsValid;
    if (rebuilt)
    {
        memset(g_PspBulletRuntimeCache->activeBits, 0,
               sizeof(g_PspBulletRuntimeCache->activeBits));
    }

    u32 mismatches = 0U;
    for (u32 index = 0U;
         index < static_cast<u32>(th08::psp::kBulletPoolLogicalCount);
         ++index)
    {
        const bool stateActive =
            bulletManager->bullets[index].state != BULLET_STATE_UNUSED;
        const bool bitActive = PspBulletRuntimeRawBit(index);
        if (stateActive != bitActive)
        {
            ++mismatches;
            PspBulletRuntimeSetBit(index, stateActive);
            PspBulletRuntimeInvalidateRotation(index);
        }

#if defined(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH_ENABLED)
        if (PspBulletTransformTerminalRawBit(index))
        {
            ++g_PspBulletTransformTerminalTelemetry.invariantWitnesses;
            const Bullet &bullet = bulletManager->bullets[index];
            const bool terminal =
                bullet.transformIndex >= 18 ||
                (bullet.transformIndex >= 0 &&
                 bullet.transforms[bullet.transformIndex].kind ==
                     BULLET_TRANSFORM_NONE);
            if (!terminal)
            {
                PspBulletTransformTerminalSetBit(index, false);
                ++g_PspBulletTransformTerminalTelemetry.repairs;
            }
        }
#endif
    }

    // Publish validity only after the complete authoritative pool pass.  A
    // partially rebuilt bitmap is never allowed to suppress a Bullet.
    g_PspBulletRuntimeActiveBitsValid = true;

    if (rebuilt || mismatches != 0U)
    {
        // Repair occurs before the optimized traversal.  The current frame
        // therefore retains the original 0,1535..1 simulation order even if
        // a future direct state write forgets to maintain the sidecar.
        fprintf(stderr,
                "TH08PSP BULLET_FASTPATH audit=REPAIRED frame=%ld "
                "rebuilt=%d mismatches=%lu\n",
                static_cast<long>(bulletManager->frameCounter),
                rebuilt ? 1 : 0,
                static_cast<unsigned long>(mismatches));
    }
}
#endif
} // namespace
#endif

#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED)
namespace
{
PspBulletTransformAuditTelemetrySnapshot
    g_PspBulletTransformAuditTelemetry{};

inline void PspBulletTransformAuditNoteFiredUpdateCall()
{
    ++g_PspBulletTransformAuditTelemetry.firedUpdateCalls;
}

inline void PspBulletTransformAuditNoteSpawnProgramWrite()
{
    ++g_PspBulletTransformAuditTelemetry.spawnProgramWrites;
}

inline void PspBulletTransformAuditNoteWholeSlotReset()
{
    ++g_PspBulletTransformAuditTelemetry.wholeSlotResets;
}

inline void PspBulletTransformAuditNoteRecordDispatch(
    u32 kind)
{
    ++g_PspBulletTransformAuditTelemetry.recordsDispatched;
    switch (kind)
    {
    case BULLET_TRANSFORM_DECELERATE:
    case BULLET_TRANSFORM_ACCELERATE_VECTOR:
    case BULLET_TRANSFORM_ACCELERATE_POLAR:
    case BULLET_TRANSFORM_CHANGE_DIRECTION_RELATIVE:
    case BULLET_TRANSFORM_CHANGE_DIRECTION_AIMED:
    case BULLET_TRANSFORM_CHANGE_DIRECTION_ABSOLUTE:
    case BULLET_TRANSFORM_BOUNCE_ALL_EDGES:
    case BULLET_TRANSFORM_BOUNCE_EXCEPT_BOTTOM:
    case BULLET_TRANSFORM_WAIT:
    case BULLET_TRANSFORM_WRAP_X:
    case BULLET_TRANSFORM_WRAP_Y:
        ++g_PspBulletTransformAuditTelemetry.activeTransformStarts;
        break;
    case BULLET_TRANSFORM_SPAWN_CHILD_PATTERN:
        ++g_PspBulletTransformAuditTelemetry.childSpawnRecords;
        break;
    default:
        break;
    }
}
} // namespace
#endif

#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT_ENABLED)
namespace
{
void PspBulletCollisionGateAuditNoteFrame(
    const PspPlayerBulletCollisionAuditSnapshot &snapshot)
{
    ++g_PspBulletCollisionGateAuditTelemetry.frames;
    if (snapshot.sidecarOwnerValid == 0U)
        ++g_PspBulletCollisionGateAuditTelemetry.sidecarOwnerInvalidFrames;
    if (snapshot.sidecarClaimsEmpty != 0U)
        ++g_PspBulletCollisionGateAuditTelemetry.sidecarClaimsEmptyFrames;
    if (snapshot.authoritativeEmpty != 0U)
        ++g_PspBulletCollisionGateAuditTelemetry.authoritativeEmptyFrames;
    if (snapshot.knownEmpty != 0U)
        ++g_PspBulletCollisionGateAuditTelemetry.knownEmptyFrames;
    if (snapshot.sidecarClaimsEmpty != 0U &&
        snapshot.authoritativeEmpty == 0U)
    {
        ++g_PspBulletCollisionGateAuditTelemetry
              .cancelFalseEmptyWitnesses;
    }
    if (snapshot.sidecarOwnerValid != 0U &&
        snapshot.sidecarClaimsEmpty == 0U &&
        snapshot.authoritativeEmpty != 0U)
    {
        ++g_PspBulletCollisionGateAuditTelemetry
              .cancelStalePositiveFrames;
    }
}

th08::psp::PspBulletCollisionGateDecision
PspBulletCollisionGateAuditBegin(
    const PspPlayerBulletCollisionAuditSnapshot &snapshot,
    bool snapshotBoundsValid, const Bullet *bullet, bool grazePath)
{
    ++g_PspBulletCollisionGateAuditTelemetry.collisionEligibleBullets;
    if (grazePath)
        ++g_PspBulletCollisionGateAuditTelemetry.grazePathBullets;
    else
        ++g_PspBulletCollisionGateAuditTelemetry.lethalPathBullets;

    if (!PspPlayerBulletCollisionAuditBoundsMatch(&g_Player, &snapshot))
    {
        ++g_PspBulletCollisionGateAuditTelemetry
              .snapshotMutationWitnesses;
        ++g_PspBulletCollisionGateAuditTelemetry.invalidSnapshotFallbacks;
        return th08::psp::
            PSP_BULLET_COLLISION_GATE_FALLBACK_SNAPSHOT_INVALID;
    }

    const bool grazeSuppressed =
        g_Player.playerState == PLAYER_STATE_DYING ||
        g_Player.playerState == PLAYER_STATE_SPAWNING;
    const th08::psp::PspBulletCollisionGateDecision decision =
        th08::psp::PspBulletCollisionDefinitelyClear(
            bullet->position.x, bullet->position.y,
            bullet->sprites.collisionSize.x,
            bullet->sprites.collisionSize.y, grazePath,
            grazeSuppressed, snapshot.knownEmpty != 0U,
            snapshotBoundsValid, snapshot.hurtboxBoundsMin.x,
            snapshot.hurtboxBoundsMin.y, snapshot.hurtboxBoundsMax.x,
            snapshot.hurtboxBoundsMax.y, snapshot.grazeBoundsMin.x,
            snapshot.grazeBoundsMin.y, snapshot.grazeBoundsMax.x,
            snapshot.grazeBoundsMax.y);

    switch (decision)
    {
    case th08::psp::PSP_BULLET_COLLISION_GATE_CLEAR_GRAZE_SUPPRESSED:
        ++g_PspBulletCollisionGateAuditTelemetry.clearGrazeSuppressed;
        break;
    case th08::psp::PSP_BULLET_COLLISION_GATE_CLEAR_GRAZE_SEPARATE:
        ++g_PspBulletCollisionGateAuditTelemetry.clearGrazeSeparate;
        break;
    case th08::psp::PSP_BULLET_COLLISION_GATE_CLEAR_LETHAL_SEPARATE:
        ++g_PspBulletCollisionGateAuditTelemetry.clearLethalSeparate;
        break;
    case th08::psp::PSP_BULLET_COLLISION_GATE_FALLBACK_CANCEL_UNKNOWN:
        ++g_PspBulletCollisionGateAuditTelemetry.cancelUnknownFallbacks;
        break;
    case th08::psp::PSP_BULLET_COLLISION_GATE_FALLBACK_SNAPSHOT_INVALID:
        ++g_PspBulletCollisionGateAuditTelemetry.invalidSnapshotFallbacks;
        break;
    case th08::psp::PSP_BULLET_COLLISION_GATE_FALLBACK_BULLET_INVALID:
        ++g_PspBulletCollisionGateAuditTelemetry.invalidBulletFallbacks;
        break;
    case th08::psp::PSP_BULLET_COLLISION_GATE_FALLBACK_TOUCH_OR_OVERLAP:
        ++g_PspBulletCollisionGateAuditTelemetry.touchOrOverlapFallbacks;
        break;
    }
    return decision;
}

void PspBulletCollisionGateAuditObserveCanonical(
    th08::psp::PspBulletCollisionGateDecision decision,
    i32 canonicalResult)
{
    switch (canonicalResult)
    {
    case 0:
        ++g_PspBulletCollisionGateAuditTelemetry.canonicalZero;
        break;
    case 1:
        ++g_PspBulletCollisionGateAuditTelemetry.canonicalOne;
        break;
    case 2:
        ++g_PspBulletCollisionGateAuditTelemetry.canonicalTwo;
        break;
    default:
        // No reconstructed Player collision routine has another result.  Treat
        // one as a failed implication if the proposed gate claimed clear.
        break;
    }

    if (!th08::psp::PspBulletCollisionGateIsClear(decision))
        return;
    if (canonicalResult != 0)
        ++g_PspBulletCollisionGateAuditTelemetry.falseClearWitnesses;
    // Both canonical first-call negative paths assign 6 before checking the
    // cancel set.  A future shortcut must reproduce that otherwise-observable
    // value even though it performs no collision, graze, RNG, or replay write.
    if (canonicalResult == 0 && g_Player.bulletCancelItemType != 6)
        ++g_PspBulletCollisionGateAuditTelemetry.itemTypeWitnesses;
}
} // namespace
#endif

#if defined(PSP)
#if defined(TH08_PSP_BULLET_CANCEL_SPATIAL_ENABLED)
ZunBool PspBulletCancelSpatialValidatePosition(const Float3 *position)
{
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
    if (position == NULL || g_BulletManager.bullets == NULL ||
        g_PspBulletRuntimeCache == NULL ||
        !g_PspBulletRuntimeActiveBitsValid)
    {
        return FALSE;
    }

    const std::uintptr_t address =
        reinterpret_cast<std::uintptr_t>(position);
    const std::uintptr_t firstPosition =
        reinterpret_cast<std::uintptr_t>(&g_BulletManager.bullets[0].position);
    if (address < firstPosition)
        return FALSE;
    const std::uintptr_t offset = address - firstPosition;
    if (offset % sizeof(Bullet) != 0U)
        return FALSE;
    const u32 index = static_cast<u32>(offset / sizeof(Bullet));
    if (index >= th08::psp::kBulletPoolLogicalCount)
        return FALSE;
    const Bullet &bullet = g_BulletManager.bullets[index];
    const u32 word = g_PspBulletRuntimeCache->activeBits[index >> 5U];
    return bullet.state != BULLET_STATE_UNUSED &&
           (word & (1U << (index & 31U))) != 0U;
#else
    return FALSE;
#endif
}

void PspBulletCancelSpatialNoteRebuild(u32 circles, u32 rects,
                                       ZunBool nonfiniteFallback)
{
    ++g_PspBulletCancelSpatialCounters.rebuilds;
    g_PspBulletCancelSpatialCounters.circles += circles;
    g_PspBulletCancelSpatialCounters.rects += rects;
    if (nonfiniteFallback)
        ++g_PspBulletCancelSpatialCounters.nonfiniteFallbacks;
}

void PspBulletCancelSpatialNoteQuery(u32 fullCandidates,
                                     u32 selectedCandidates,
                                     u32 exactTests,
                                     ZunBool indexed,
                                     ZunBool rejected,
                                     ZunBool falsePositive,
                                     ZunBool fallback,
                                     ZunBool ownerFallback,
                                     ZunBool nonfiniteFallback)
{
    PspBulletCancelSpatialCounters &telemetry =
        g_PspBulletCancelSpatialCounters;
    ++telemetry.calls;
    telemetry.fullCandidates += fullCandidates;
    telemetry.exactTests += exactTests;
    if (fallback)
    {
        ++telemetry.fallbacks;
        telemetry.fallbackCandidates += selectedCandidates;
    }
    else if (rejected)
    {
        ++telemetry.rejectedQueries;
    }
    else if (indexed)
    {
        ++telemetry.indexedQueries;
        telemetry.indexedCandidates += selectedCandidates;
    }
    if (falsePositive)
        ++telemetry.falsePositives;
    if (ownerFallback)
        ++telemetry.occupancyOwnerFallbacks;
    if (nonfiniteFallback)
        ++telemetry.nonfiniteFallbacks;
}

void PspBulletCancelSpatialNoteDuplicate(ZunBool replayed,
                                         ZunBool fallback)
{
    PspBulletCancelSpatialCounters &telemetry =
        g_PspBulletCancelSpatialCounters;
    ++telemetry.duplicatePairs;
    if (replayed)
    {
        ++telemetry.duplicateReplays;
        ++telemetry.duplicateExactTestsSaved;
    }
    if (fallback)
        ++telemetry.duplicateFallbacks;
}
#endif

PspBulletCancelSpatialTelemetrySnapshot
PspPeekBulletCancelSpatialTelemetry()
{
    PspBulletCancelSpatialTelemetrySnapshot snapshot{};
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
    const PspBulletCancelSpatialCounters &source =
        g_PspBulletCancelSpatialCounters;
    snapshot.calls = source.calls;
    snapshot.indexedQueries = source.indexedQueries;
    snapshot.rejectedQueries = source.rejectedQueries;
    snapshot.circles = source.circles;
    snapshot.rects = source.rects;
    snapshot.rebuilds = source.rebuilds;
    snapshot.fullCandidates = source.fullCandidates;
    snapshot.indexedCandidates = source.indexedCandidates;
    snapshot.fallbackCandidates = source.fallbackCandidates;
    snapshot.exactTests = source.exactTests;
    snapshot.falsePositives = source.falsePositives;
    snapshot.fallbacks = source.fallbacks;
    snapshot.occupancyOwnerFallbacks = source.occupancyOwnerFallbacks;
    snapshot.unsupportedRegionFallbacks = source.unsupportedRegionFallbacks;
    snapshot.nonfiniteFallbacks = source.nonfiniteFallbacks;
    snapshot.duplicatePairs = source.duplicatePairs;
    snapshot.duplicateReplays = source.duplicateReplays;
    snapshot.duplicateExactTestsSaved = source.duplicateExactTestsSaved;
    snapshot.duplicateFallbacks = source.duplicateFallbacks;
#endif
    return snapshot;
}

void PspResetBulletCancelSpatialTelemetry()
{
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
    g_PspBulletCancelSpatialCounters = PspBulletCancelSpatialCounters{};
#endif
}

PspBulletCancelSpatialTelemetrySnapshot
PspTakeBulletCancelSpatialTelemetry()
{
    const PspBulletCancelSpatialTelemetrySnapshot snapshot =
        PspPeekBulletCancelSpatialTelemetry();
    PspResetBulletCancelSpatialTelemetry();
    return snapshot;
}

PspBulletLiveEnumTelemetrySnapshot PspPeekBulletLiveEnumTelemetry()
{
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH) && \
    TH08_PSP_RUNTIME_TELEMETRY
    return g_PspBulletLiveEnumTelemetry;
#else
    return PspBulletLiveEnumTelemetrySnapshot{};
#endif
}

void PspResetBulletLiveEnumTelemetry()
{
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH) && \
    TH08_PSP_RUNTIME_TELEMETRY
    g_PspBulletLiveEnumTelemetry = PspBulletLiveEnumTelemetrySnapshot{};
#endif
}

PspBulletLiveEnumTelemetrySnapshot PspTakeBulletLiveEnumTelemetry()
{
    const PspBulletLiveEnumTelemetrySnapshot snapshot =
        PspPeekBulletLiveEnumTelemetry();
    PspResetBulletLiveEnumTelemetry();
    return snapshot;
}

PspBulletTransformAuditTelemetrySnapshot
PspPeekBulletTransformAuditTelemetry()
{
#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED)
    return g_PspBulletTransformAuditTelemetry;
#else
    return PspBulletTransformAuditTelemetrySnapshot{};
#endif
}

void PspResetBulletTransformAuditTelemetry()
{
#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED)
    g_PspBulletTransformAuditTelemetry =
        PspBulletTransformAuditTelemetrySnapshot{};
#endif
}

PspBulletTransformAuditTelemetrySnapshot
PspTakeBulletTransformAuditTelemetry()
{
    const PspBulletTransformAuditTelemetrySnapshot snapshot =
        PspPeekBulletTransformAuditTelemetry();
    PspResetBulletTransformAuditTelemetry();
    return snapshot;
}

PspBulletTransformTerminalTelemetrySnapshot
PspPeekBulletTransformTerminalTelemetry()
{
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
    PspBulletTransformTerminalTelemetrySnapshot snapshot{};
    snapshot.hits = g_PspBulletTransformTerminalTelemetry.hits;
    snapshot.misses = g_PspBulletTransformTerminalTelemetry.misses;
    snapshot.fallbacks = g_PspBulletTransformTerminalTelemetry.fallbacks;
    snapshot.firedCalls = snapshot.hits + snapshot.misses + snapshot.fallbacks;
    snapshot.invariantWitnesses =
        g_PspBulletTransformTerminalTelemetry.invariantWitnesses;
    snapshot.repairs = g_PspBulletTransformTerminalTelemetry.repairs;
    snapshot.terminalIndexMarks =
        g_PspBulletTransformTerminalTelemetry.terminalIndexMarks;
    snapshot.terminalNoneMarks =
        g_PspBulletTransformTerminalTelemetry.terminalNoneMarks;
    snapshot.markFallbacks =
        g_PspBulletTransformTerminalTelemetry.markFallbacks;
    snapshot.spawnProgramClears =
        g_PspBulletTransformTerminalTelemetry.spawnProgramClears;
    snapshot.wholeSlotClears =
        g_PspBulletTransformTerminalTelemetry.wholeSlotClears;
    return snapshot;
#else
    return PspBulletTransformTerminalTelemetrySnapshot{};
#endif
}

void PspResetBulletTransformTerminalTelemetry()
{
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
    g_PspBulletTransformTerminalTelemetry =
        PspBulletTransformTerminalCounters{};
#endif
}

PspBulletTransformTerminalTelemetrySnapshot
PspTakeBulletTransformTerminalTelemetry()
{
    const PspBulletTransformTerminalTelemetrySnapshot snapshot =
        PspPeekBulletTransformTerminalTelemetry();
    PspResetBulletTransformTerminalTelemetry();
    return snapshot;
}

PspBulletCollisionGateAuditTelemetrySnapshot
PspPeekBulletCollisionGateAuditTelemetry()
{
    return g_PspBulletCollisionGateAuditTelemetry;
}

void PspResetBulletCollisionGateAuditTelemetry()
{
    g_PspBulletCollisionGateAuditTelemetry =
        PspBulletCollisionGateAuditTelemetrySnapshot{};
}

PspBulletCollisionGateAuditTelemetrySnapshot
PspTakeBulletCollisionGateAuditTelemetry()
{
    const PspBulletCollisionGateAuditTelemetrySnapshot snapshot =
        PspPeekBulletCollisionGateAuditTelemetry();
    PspResetBulletCollisionGateAuditTelemetry();
    return snapshot;
}
#endif

void __fastcall CopyBulletAnmVmCore(AnmVm *dst, const AnmVm *src);
void __fastcall SelectBulletSprite(AnmVm *dst, AnmVm *base, AnmVm *sizeSource, i32 offset);


void __fastcall fsincos(f32 *sine, f32 *cosine, f32 angle)
{
#ifdef TH08_MODERN_PORT
    *sine = X87CompatibleSin(angle);
    *cosine = X87CompatibleCos(angle);
#endif
}

// FUNCTION: th08 0x42a410
BulletSpawnDescriptor::BulletSpawnDescriptor()
{
    memset(this, 0, sizeof(*this));
    this->transformSound = -1;
}

DIFFABLE_STATIC_ARRAY_ASSIGN(i32, 16, g_BulletSpriteOffsetSmall) = {
    0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 0,
};
DIFFABLE_STATIC_ARRAY_ASSIGN(i32, 8, g_BulletSpriteOffsetMedium) = {
    0, 1, 1, 2, 2, 3, 4, 0,
};

// FUNCTION: th08 0x42f360
#pragma var_order(i, bullet, this)
void BulletManager::Initialize()
{
    Bullet *bullet;
    i32 i;

#if defined(PSP)
#if defined(TH08_PSP_BULLET_CANCEL_SPATIAL_ENABLED)
    PspResetBulletCancelSpatialTelemetry();
#endif
    PspResetBulletTransformAuditTelemetry();
    PspResetBulletTransformTerminalTelemetry();
    PspResetBulletCollisionGateAuditTelemetry();
#endif

#if defined(TH08_PSP_STAGE_POOL_ARENA)
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
    g_PspBulletRuntimeCache = NULL;
    g_PspBulletRuntimeActiveBitsValid = false;
#if TH08_PSP_RUNTIME_TELEMETRY
    g_PspBulletLiveEnumTelemetry = PspBulletLiveEnumTelemetrySnapshot{};
#endif
#endif
    Bullet *const bulletPool = this->bullets;
    Laser *const laserPool = this->lasers;
    if (bulletPool == NULL || laserPool == NULL)
        return;
    memset(static_cast<void *>(this), 0, sizeof(*this));
    this->bullets = bulletPool;
    this->lasers = laserPool;
    memset(static_cast<void *>(this->bullets), 0,
           sizeof(Bullet) * th08::psp::kBulletPoolStorageCount);
    memset(static_cast<void *>(this->lasers), 0,
           sizeof(Laser) * th08::psp::kLaserPoolStorageCount);
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
    g_PspBulletRuntimeCache = static_cast<PspBulletRuntimeCache *>(
        th08::psp::StagePoolArenaBulletRuntimeCacheStorage());
    if (g_PspBulletRuntimeCache != NULL)
        memset(g_PspBulletRuntimeCache, 0, sizeof(*g_PspBulletRuntimeCache));
#endif
#else
    memset(this, 0, sizeof(BulletManager));
#endif
    this->bulletCursor = &this->bullets[0];
    this->bullets[0x600].state = BULLET_STATE_SENTINEL;
    this->cancelItemType = 6;

#if defined(TH08_PSP_STAGE_POOL_ARENA)
    bullet = &this->bullets[0];
#else
    // Preserve the original absolute g_BulletManager relocation on PC builds.
    bullet = &g_BulletManager.bullets[0];
#endif
    for (i = 0; i < 0x600; i++, bullet++)
    {
        bullet->sprites.bulletVm.scriptIndex = -1;
        bullet->sprites.despawnVm.scriptIndex = -1;
#if defined(TH08_PSP_COMPACT_BULLET_VM)
        bullet->sprites.selectedSpawnVm.scriptIndex = -1;
#else
        bullet->sprites.spawnFastVm.scriptIndex = -1;
        bullet->sprites.spawnNormalVm.scriptIndex = -1;
        bullet->sprites.spawnSlowVm.scriptIndex = -1;
#endif
    }
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
    // Initialize published every logical state to UNUSED and every occupancy
    // bit to zero.  Mark the bitmap usable only after both passes complete.
    g_PspBulletRuntimeActiveBitsValid = g_PspBulletRuntimeCache != NULL;
#endif
}

// FUNCTION: th08 0x42f420
BulletManager::BulletManager()
{
#if !defined(TH08_PSP_STAGE_POOL_ARENA)
    this->Initialize();
#endif
}

// FUNCTION: th08 0x42f4a0
BulletTypeSprites::BulletTypeSprites()
{
}

// FUNCTION: th08 0x42f500
Bullet::Bullet()
{
}

// FUNCTION: th08 0x42f580
Laser::Laser()
{
}

// FUNCTION: th08 0x42f5c0
BulletExState::BulletExState()
{
}

// FUNCTION: th08 0x42f5f0
#pragma var_order(speed, i, bullet, angle, transformFlags, this)
i32 BulletManager::SpawnSingleBullet(BulletSpawnDescriptor *descriptor, i32 index1, i32 index2, f32 angleToPlayer)
{
    f32 speed;
    i32 i;
    Bullet *bullet;
    f32 angle;
    u32 transformFlags;

    i = 0;
    bullet = this->bulletCursor;
    for (i = 0; i < 0x600; i++)
    {
        if (bullet->state == BULLET_STATE_UNUSED)
            break;
        bullet++;
        if (bullet->state == BULLET_STATE_SENTINEL)
            bullet = &this->bullets[0];
    }
    if (i >= 0x600)
        return 1;

    angle = 0.0f;
    if (descriptor->count2 > 1)
        speed = descriptor->speed1 -
                (descriptor->speed1 - descriptor->speed2) * (f32)index2 / (f32)descriptor->count2;
    else
        speed = descriptor->speed1;

    switch (descriptor->aimMode)
    {
    case BULLET_AIM_FAN_AIMED:
    case BULLET_AIM_FAN:
        if ((descriptor->count1 & 1) != 0)
            angle += (f32)((index1 + 1) / 2) * descriptor->angleStep;
        else
            angle += (f32)(index1 / 2) * descriptor->angleStep + descriptor->angleStep * 0.5f;
        if ((index1 & 1) != 0)
            angle *= -1.0f;
        if (descriptor->aimMode == BULLET_AIM_FAN_AIMED)
            angle += angleToPlayer;
        angle += descriptor->angle;
        break;
    case BULLET_AIM_CIRCLE_AIMED:
        angle += angleToPlayer;
    case BULLET_AIM_CIRCLE:
        angle += (f32)index1 * (ZUN_PI * 2.0f) / (f32)descriptor->count1;
        angle += (f32)index2 * descriptor->angleStep + descriptor->angle;
        break;
    case BULLET_AIM_OFFSET_CIRCLE_AIMED:
        angle += angleToPlayer;
    case BULLET_AIM_OFFSET_CIRCLE:
        angle += ZUN_PI / (f32)descriptor->count1;
        angle += (f32)index1 * (ZUN_PI * 2.0f) / (f32)descriptor->count1;
        angle += descriptor->angle;
        break;
    case BULLET_AIM_RANDOM_ANGLE:
        angle = g_Rng.GetRandomF32InRange(descriptor->angle - descriptor->angleStep) + descriptor->angleStep;
        break;
    case BULLET_AIM_RANDOM_SPEED:
        speed = g_Rng.GetRandomF32InRange(descriptor->speed1 - descriptor->speed2) + descriptor->speed2;
        angle += (f32)index1 * (ZUN_PI * 2.0f) / (f32)descriptor->count1;
        angle += (f32)index2 * descriptor->angleStep + descriptor->angle;
        break;
    case BULLET_AIM_RANDOM:
        angle = g_Rng.GetRandomF32InRange(descriptor->angle - descriptor->angleStep) + descriptor->angleStep;
        speed = g_Rng.GetRandomF32InRange(descriptor->speed1 - descriptor->speed2) + descriptor->speed2;
        break;
    default:
        break;
    }

    bullet->state = BULLET_STATE_FIRED;
    bullet->unconsumedSpawnMarkerDBC = 1;
    bullet->isGrazed = 0;
    bullet->stateTimer = 0;
    bullet->collisionDisabled = 0;
    bullet->activeTimer = 0;
    bullet->speed = speed;
    bullet->angle = AddNormalizeAngle(angle, 0.0f);
    bullet->position = descriptor->position;
    bullet->position.operator float *()[2] = 0.1f;
    bullet->velocity.FromAngleMagnitude(
        angle, speed * g_Supervisor.framerateMultiplier);

    bullet->activeTransformFlags = descriptor->transformFlags;
    bullet->color = descriptor->color;
    bullet->zoneTransitionCooldownFrames = 0;
    bullet->cancelledDuringSpawn = 0;

    CopyBulletAnmVmCore(&bullet->sprites.bulletVm, &descriptor->templateSprites->bulletVm);
    CopyBulletAnmVmCore(&bullet->sprites.despawnVm, &descriptor->templateSprites->despawnVm);
    bullet->sprites.collisionSize = descriptor->templateSprites->collisionSize;
    bullet->sprites.unconsumedTemplateByteD40 =
        descriptor->templateSprites->unconsumedTemplateByteD40;
    bullet->sprites.spriteHeightPx = descriptor->templateSprites->spriteHeightPx;
    bullet->sprites.drawBucketIndex = descriptor->templateSprites->drawBucketIndex;
    bullet->transformSound = descriptor->transformSound;
    bullet->offscreenCullDelayFrames = 0;

    if (bullet->sprites.bulletVm.activeSpriteIndex !=
        descriptor->templateSprites->bulletVm.activeSpriteIndex + descriptor->color)
    {
        this->bulletAnm->SetSprite(&bullet->sprites.bulletVm,
            descriptor->templateSprites->bulletVm.activeSpriteIndex + descriptor->color);
    }

    if (bullet->sprites.despawnVm.activeSpriteIndex !=
        descriptor->templateSprites->despawnVm.activeSpriteIndex + descriptor->color)
    {
        if (bullet->sprites.bulletVm.loadedSprite->heightPx <= 16.0f)
        {
            this->bulletAnm->SetSprite(&bullet->sprites.despawnVm,
                descriptor->templateSprites->despawnVm.activeSpriteIndex +
                    g_BulletSpriteOffsetSmall[descriptor->color]);
        }
        else if (bullet->sprites.bulletVm.loadedSprite->heightPx <= 32.0f)
        {
            this->bulletAnm->SetSprite(&bullet->sprites.despawnVm,
                descriptor->templateSprites->despawnVm.activeSpriteIndex +
                    g_BulletSpriteOffsetMedium[descriptor->color]);
        }
        else
        {
            this->bulletAnm->SetSprite(&bullet->sprites.despawnVm,
                descriptor->templateSprites->despawnVm.activeSpriteIndex + descriptor->color);
        }
    }

    transformFlags = descriptor->transformFlags;
    if ((descriptor->transformFlags & BULLET_TRANSFORM_SPAWN_FAST) != 0)
    {
#if defined(TH08_PSP_COMPACT_BULLET_VM)
        CopyBulletAnmVmCore(&bullet->sprites.selectedSpawnVm, &descriptor->templateSprites->spawnFastVm);
        SelectBulletSprite(&bullet->sprites.selectedSpawnVm,
                           &descriptor->templateSprites->spawnFastVm,
                           &bullet->sprites.bulletVm, descriptor->color);
#else
        CopyBulletAnmVmCore(&bullet->sprites.spawnFastVm, &descriptor->templateSprites->spawnFastVm);
        SelectBulletSprite(&bullet->sprites.spawnFastVm,
                           &descriptor->templateSprites->spawnFastVm,
                           &bullet->sprites.bulletVm, descriptor->color);
#endif
        bullet->state = BULLET_STATE_SPAWNING_FAST;
        bullet->position -= bullet->velocity * 4.0f;
    }
    else if ((descriptor->transformFlags & BULLET_TRANSFORM_SPAWN_NORMAL) != 0)
    {
#if defined(TH08_PSP_COMPACT_BULLET_VM)
        CopyBulletAnmVmCore(&bullet->sprites.selectedSpawnVm, &descriptor->templateSprites->spawnNormalVm);
        SelectBulletSprite(&bullet->sprites.selectedSpawnVm,
                           &descriptor->templateSprites->spawnNormalVm,
                           &bullet->sprites.bulletVm, descriptor->color);
#else
        CopyBulletAnmVmCore(&bullet->sprites.spawnNormalVm, &descriptor->templateSprites->spawnNormalVm);
        SelectBulletSprite(&bullet->sprites.spawnNormalVm,
                           &descriptor->templateSprites->spawnNormalVm,
                           &bullet->sprites.bulletVm, descriptor->color);
#endif
        bullet->state = BULLET_STATE_SPAWNING_NORMAL;
        bullet->position -= bullet->velocity * 4.0f;
    }
    else if ((descriptor->transformFlags & BULLET_TRANSFORM_SPAWN_SLOW) != 0)
    {
#if defined(TH08_PSP_COMPACT_BULLET_VM)
        CopyBulletAnmVmCore(&bullet->sprites.selectedSpawnVm, &descriptor->templateSprites->spawnSlowVm);
        SelectBulletSprite(&bullet->sprites.selectedSpawnVm,
                           &descriptor->templateSprites->spawnSlowVm,
                           &bullet->sprites.bulletVm, descriptor->color);
#else
        CopyBulletAnmVmCore(&bullet->sprites.spawnSlowVm, &descriptor->templateSprites->spawnSlowVm);
        SelectBulletSprite(&bullet->sprites.spawnSlowVm,
                           &descriptor->templateSprites->spawnSlowVm,
                           &bullet->sprites.bulletVm, descriptor->color);
#endif
        bullet->state = BULLET_STATE_SPAWNING_SLOW;
        bullet->position -= bullet->velocity * 4.0f;
    }

#if defined(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH_ENABLED)
    // A recycled slot may retain the previous immutable program's terminal
    // bit.  Clear it before publishing any byte of the new program, while
    // retaining the mandatory canonical initial Advance call below.
    PspBulletTransformTerminalClearForSpawn(bullet);
#endif
    memcpy(bullet->transforms, descriptor->transforms, sizeof(descriptor->transforms));
    bullet->transformFlags = descriptor->transformFlags;
    bullet->activeTransformFlags = 0;
    bullet->transformIndex = descriptor->transformStartIndex;
#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED)
    PspBulletTransformAuditNoteSpawnProgramWrite();
#endif
    bullet->AdvanceTransformProgram();

    if (this->spawnSuppressionFrames != 0 &&
        (bullet->transformFlags & BULLET_TRANSFORM_CANCEL_IMMUNE) == 0)
        bullet->state = BULLET_STATE_DESPAWNING;

#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
    PspBulletRuntimeActivate(bullet);
#endif
    bullet++;
    if (bullet->state == BULLET_STATE_SENTINEL)
        this->bulletCursor = &this->bullets[0];
    else
        this->bulletCursor = bullet;
    return 0;
}

// FUNCTION: th08 0x42fe70
void __fastcall CopyBulletAnmVmCore(AnmVm *dst, const AnmVm *src)
{
    *dst = *src;
}

// FUNCTION: th08 0x42fea0
void __fastcall SelectBulletSprite(AnmVm *dst, AnmVm *base, AnmVm *sizeSource, i32 offset)
{
    if (dst->activeSpriteIndex != base->activeSpriteIndex + offset)
    {
        if (sizeSource->loadedSprite->heightPx <= 16.0f)
        {
            g_BulletManager.bulletAnm->SetSprite(
                dst, base->activeSpriteIndex + g_BulletSpriteOffsetSmall[offset]);
        }
        else if (sizeSource->loadedSprite->heightPx <= 32.0f)
        {
            g_BulletManager.bulletAnm->SetSprite(
                dst, base->activeSpriteIndex + g_BulletSpriteOffsetMedium[offset]);
        }
        else
        {
            g_BulletManager.bulletAnm->SetSprite(
                dst, base->activeSpriteIndex + offset);
        }
    }
}

#if defined(TH08_PSP_COMPACT_BULLET_VM)
static void AssignRuntimeBulletSprites(Bullet *bullet, const BulletTypeSprites *source)
{
    // Keep the original full template table as the immutable source.  ECL can
    // retheme a bullet before its spawn animation completes, so preserve the
    // spawn VM selected by the current state.  A fired/despawning bullet never
    // returns to a spawn state; the fast template is a deterministic dormant
    // value in that case.
    const AnmVm *spawnSource = &source->spawnFastVm;
    if (bullet->state == BULLET_STATE_SPAWNING_NORMAL)
        spawnSource = &source->spawnNormalVm;
    else if (bullet->state == BULLET_STATE_SPAWNING_SLOW)
        spawnSource = &source->spawnSlowVm;

    CopyBulletAnmVmCore(&bullet->sprites.bulletVm, &source->bulletVm);
    CopyBulletAnmVmCore(&bullet->sprites.selectedSpawnVm, spawnSource);
    CopyBulletAnmVmCore(&bullet->sprites.despawnVm, &source->despawnVm);
    bullet->sprites.collisionSize = source->collisionSize;
    bullet->sprites.unconsumedTemplateByteD40 = source->unconsumedTemplateByteD40;
    bullet->sprites.spriteHeightPx = source->spriteHeightPx;
    bullet->sprites.drawBucketIndex = source->drawBucketIndex;
    bullet->sprites.trailingAlignmentD43 = source->trailingAlignmentD43;
}
#endif

struct BulletSpriteScriptRow
{
    i32 scripts[5];
};

static BulletSpriteScriptRow g_BulletSpriteScripts[21] = {
    {{0, 18, 19, 20, 15}},   {{1, 21, 22, 23, 16}},   {{2, 21, 22, 23, 16}},
    {{3, 21, 22, 23, 16}},   {{4, 21, 22, 23, 16}},   {{5, 21, 22, 23, 16}},
    {{6, 21, 22, 23, 16}},   {{7, 24, 24, 24, 17}},   {{8, 24, 24, 24, 17}},
    {{9, 24, 24, 24, 17}},   {{25, 27, 27, 27, 26}}, {{106, 21, 22, 23, 16}},
    {{107, 21, 22, 23, 16}}, {{108, 21, 22, 23, 16}}, {{109, 24, 24, 24, 17}},
    {{110, 24, 24, 24, 17}}, {{111, 21, 22, 23, 16}}, {{112, 21, 22, 23, 16}},
    {{113, 24, 24, 24, 17}}, {{114, 24, 24, 24, 17}}, {{115, 24, 24, 24, 17}},
};

// FUNCTION: th08 0x42ffc0
void Bullet::AdvanceTransformProgram()
{
    BulletTransformRecord *record;

#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED)
    ++g_PspBulletTransformAuditTelemetry.advanceCalls;
#endif

nextRecord:
    if (this->transformIndex >= 18)
    {
#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED)
        ++g_PspBulletTransformAuditTelemetry.terminalIndexReturns;
#endif
#if defined(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH_ENABLED)
        PspBulletTransformTerminalMarkIndex(this);
#endif
        return;
    }

    record = &this->transforms[this->transformIndex];
    if (record->kind == BULLET_TRANSFORM_NONE)
    {
#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED)
        ++g_PspBulletTransformAuditTelemetry.terminalNoneReturns;
#endif
#if defined(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH_ENABLED)
        PspBulletTransformTerminalMarkNone(this);
#endif
        return;
    }
    if (record->allowWhileActive == 0 && this->activeTransformFlags != 0)
    {
#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED)
        ++g_PspBulletTransformAuditTelemetry.activeBlockedReturns;
#endif
        return;
    }
    if ((this->transformFlags & record->kind) == 0)
    {
#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED)
        ++g_PspBulletTransformAuditTelemetry.disabledRecordsSkipped;
#endif
        ++this->transformIndex;
        goto nextRecord;
    }

#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED)
    PspBulletTransformAuditNoteRecordDispatch(record->kind);
#endif
    switch (record->kind)
    {
    case BULLET_TRANSFORM_DECELERATE:
        this->activeTransformFlags |= BULLET_TRANSFORM_DECELERATE;
        this->exStates[BULLET_TRANSFORM_STATE_DECELERATION].timer = 0;
        *reinterpret_cast<i32 *>(
            &this->exStates[BULLET_TRANSFORM_STATE_DECELERATION].vector.z) = 0;
        break;

    case BULLET_TRANSFORM_ACCELERATE_VECTOR:
        this->activeTransformFlags |= BULLET_TRANSFORM_ACCELERATE_VECTOR;
        this->exStates[BULLET_TRANSFORM_STATE_VECTOR_ACCELERATION].accelerationMagnitude =
            record->payload.vectorAcceleration.magnitude;
        this->exStates[BULLET_TRANSFORM_STATE_VECTOR_ACCELERATION].accelerationAngle =
            record->payload.vectorAcceleration.angle > -990.0f
                ? record->payload.vectorAcceleration.angle
                : this->angle;
        this->exStates[BULLET_TRANSFORM_STATE_VECTOR_ACCELERATION].timer = 0;
        this->exStates[BULLET_TRANSFORM_STATE_VECTOR_ACCELERATION].durationFrames =
            record->payload.vectorAcceleration.durationFrames;
        this->exStates[BULLET_TRANSFORM_STATE_VECTOR_ACCELERATION].vector.FromAngleMagnitude(
            this->exStates[BULLET_TRANSFORM_STATE_VECTOR_ACCELERATION].accelerationAngle,
            g_Supervisor.framerateMultiplier *
                this->exStates[BULLET_TRANSFORM_STATE_VECTOR_ACCELERATION].accelerationMagnitude);
        if (this->transformIndex != 0 && this->transformSound >= 0)
            g_SoundPlayer.PlaySoundByIdx(
                static_cast<SoundIdx>(this->transformSound), 0);
        break;

    case BULLET_TRANSFORM_ACCELERATE_POLAR:
        this->activeTransformFlags |= BULLET_TRANSFORM_ACCELERATE_POLAR;
        this->exStates[BULLET_TRANSFORM_STATE_POLAR_ACCELERATION].speedDelta =
            record->payload.polarAcceleration.speedDelta;
        this->exStates[BULLET_TRANSFORM_STATE_POLAR_ACCELERATION].angleDelta =
            record->payload.polarAcceleration.angleDelta;
        this->exStates[BULLET_TRANSFORM_STATE_POLAR_ACCELERATION].timer = 0;
        this->exStates[BULLET_TRANSFORM_STATE_POLAR_ACCELERATION].durationFrames =
            record->payload.polarAcceleration.durationFrames;
        if (this->transformIndex != 0 && this->transformSound >= 0)
            g_SoundPlayer.PlaySoundByIdx(
                static_cast<SoundIdx>(this->transformSound), 0);
        break;

    case BULLET_TRANSFORM_CHANGE_DIRECTION_RELATIVE:
    case BULLET_TRANSFORM_CHANGE_DIRECTION_AIMED:
    case BULLET_TRANSFORM_CHANGE_DIRECTION_ABSOLUTE:
        this->activeTransformFlags |= record->kind;
        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeAngle =
            record->payload.directionChange.angle;
        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeSpeed =
            record->payload.directionChange.speed > -999.0f
                ? record->payload.directionChange.speed
                : this->speed;
        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].timer = 0;
        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeIntervalFrames =
            record->payload.directionChange.intervalFrames;
        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeRepeatCount =
            record->payload.directionChange.repeatCount;
        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangesCompleted = 0;
        break;

    case BULLET_TRANSFORM_BOUNCE_ALL_EDGES:
    case BULLET_TRANSFORM_BOUNCE_EXCEPT_BOTTOM:
        this->activeTransformFlags |= record->kind;
        if (record->payload.boundaryBounce.speed >= 0.0f)
            this->exStates[BULLET_TRANSFORM_STATE_BOUNDARY_BOUNCE].bounceSpeed =
                record->payload.boundaryBounce.speed;
        else
            this->exStates[BULLET_TRANSFORM_STATE_BOUNDARY_BOUNCE].bounceSpeed =
                this->speed;
        this->exStates[BULLET_TRANSFORM_STATE_BOUNDARY_BOUNCE].bounceLimit =
            record->payload.boundaryBounce.bounceLimit;
        this->exStates[BULLET_TRANSFORM_STATE_BOUNDARY_BOUNCE].bouncesCompleted = 0;
        break;

    case BULLET_TRANSFORM_WRAP_X:
        this->activeTransformFlags |= record->kind;
        this->exStates[BULLET_TRANSFORM_STATE_WRAP].timer = record->payload.timed.frames;
        break;

    case BULLET_TRANSFORM_WRAP_Y:
        this->activeTransformFlags |= record->kind;
        this->exStates[BULLET_TRANSFORM_STATE_WRAP].timer = record->payload.timed.frames;
        break;

    case BULLET_TRANSFORM_WAIT:
        this->activeTransformFlags |= record->kind;
        this->exStates[BULLET_TRANSFORM_STATE_WAIT].timer = record->payload.timed.frames;
        break;

    case BULLET_TRANSFORM_SET_CULL_DELAY:
        this->offscreenCullDelayFrames = record->payload.cullDelay.frames;
        ++this->transformIndex;
        goto nextRecord;

    case BULLET_TRANSFORM_SET_SPRITE:
#if defined(TH08_PSP_COMPACT_BULLET_VM)
        AssignRuntimeBulletSprites(
            this, &g_BulletManager.bulletTypeSprites[record->payload.sprite.bulletType]);
#else
        this->sprites = g_BulletManager.bulletTypeSprites[record->payload.sprite.bulletType];
#endif
        g_BulletManager.bulletAnm->SetSprite(
            &this->sprites.bulletVm,
            this->sprites.bulletVm.activeSpriteIndex + record->payload.sprite.color);
        ++this->transformIndex;
        goto nextRecord;

    case BULLET_TRANSFORM_DESPAWN:
        this->state = BULLET_STATE_DESPAWNING;
        break;

    case BULLET_TRANSFORM_PLAY_SOUND:
        g_SoundPlayer.PlaySoundPositionedByIdx(
            static_cast<SoundIdx>(record->payload.sound.soundIndex), this->position.x);
        ++this->transformIndex;
        goto nextRecord;

    case BULLET_TRANSFORM_SPAWN_CHILD_PATTERN:
        {
            BulletSpawnDescriptor pattern;
            i32 fadeParent;
            pattern.position = this->position;
            fadeParent = record->payload.childPrimary.packedPattern & 0x80000000;
            pattern.aimMode =
                (static_cast<u32>(record->payload.childPrimary.packedPattern) & 0x7F000000) >> 24;
            pattern.bulletType =
                (static_cast<u32>(record->payload.childPrimary.packedPattern) & 0x00FF0000) >> 16;
            pattern.color =
                (static_cast<u32>(record->payload.childPrimary.packedPattern) & 0x0000FF00) >> 8;
            pattern.transformStartIndex = record->payload.childPrimary.packedPattern & 0xFF;
            pattern.count1 = static_cast<i16>(record->payload.childPrimary.count1);
            pattern.speed1 = record->payload.childPrimary.speed1;
            pattern.speed2 = record->payload.childPrimary.speed2;

            ++record;
            ++this->transformIndex;
            pattern.count2 = static_cast<i16>(record->payload.childSecondary.count2);
            pattern.transformFlags = record->payload.childSecondary.transformFlags;
            pattern.angle = record->payload.childSecondary.angle;
            pattern.angleStep = record->payload.childSecondary.angleStep;
            memcpy(pattern.transforms, this->transforms, sizeof(pattern.transforms));
            g_BulletManager.SpawnBulletPattern(&pattern);
            ++this->transformIndex;
            if (fadeParent != 0)
                this->state = BULLET_STATE_DESPAWNING;
            else
                goto nextRecord;
        }
        break;

    default:
        break;
    }

    ++this->transformIndex;
}

// FUNCTION: th08 0x430830
#pragma var_order(position, playerCollisionResult, bulletIndex, sine, bullet, laser, cosine, radius, this)
void BulletManager::RemoveAllBullets(i32 mode)
{
    Bullet *bullet = &g_BulletManager.bullets[0];
    i32 bulletIndex;
    i32 playerCollisionResult;
    Laser *laser;
    f32 position[3];
    f32 sine;
    f32 cosine;
    f32 radius;

    for (bulletIndex = 0; bulletIndex < 0x600; bulletIndex++, bullet++)
    {
        if (bullet->state == BULLET_STATE_UNUSED || bullet->state == BULLET_STATE_DESPAWNING)
        {
            continue;
        }

#if defined(TH08_PSP_BULLET_CANCEL_SPATIAL_ENABLED)
        PspArmNextBulletCancelDuplicateCollision();
        playerCollisionResult =
            g_Player.CheckBulletCancelCollision(
                &bullet->position, &bullet->sprites.collisionSize);
        if (PspReplayLastBulletCancelCollision(
                &g_Player, &bullet->position,
                &bullet->sprites.collisionSize,
                playerCollisionResult))
        {
            PspBulletCancelSpatialNoteDuplicate(TRUE, FALSE);
        }
        else
        {
            PspBulletCancelSpatialNoteDuplicate(FALSE, TRUE);
            playerCollisionResult =
                g_Player.CheckBulletCancelCollision(
                    &bullet->position, &bullet->sprites.collisionSize);
        }
        if (playerCollisionResult == 2)
#else
        playerCollisionResult = g_Player.CheckBulletCancelCollision(
            &bullet->position, &bullet->sprites.collisionSize);
        if (g_Player.CheckBulletCancelCollision(
                &bullet->position, &bullet->sprites.collisionSize) == 2)
#endif
        {
            g_ItemManager.SpawnItem(&bullet->position, static_cast<ItemType>(g_Player.bulletCancelItemType),
                                    ITEM_STATE_AUTOCOLLECT);
            memset(bullet, 0, sizeof(Bullet));
#if defined(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH_ENABLED)
            PspBulletTransformTerminalClearForWholeSlotReset(bullet);
#endif
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
            PspBulletRuntimeDeactivate(bullet);
#endif
#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED)
            PspBulletTransformAuditNoteWholeSlotReset();
#endif
        }
        else if (mode != 4)
        {
            g_ItemManager.SpawnItem(&bullet->position,
                                    static_cast<ItemType>(this->cancelItemType), mode);
            memset(bullet, 0, sizeof(Bullet));
#if defined(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH_ENABLED)
            PspBulletTransformTerminalClearForWholeSlotReset(bullet);
#endif
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
            PspBulletRuntimeDeactivate(bullet);
#endif
#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED)
            PspBulletTransformAuditNoteWholeSlotReset();
#endif
        }
        else
        {
            bullet->state = BULLET_STATE_DESPAWNING;
        }
    }

    laser = &this->lasers[0];
    reinterpret_cast<Float3 *>(position)->operator float *();
    for (bulletIndex = 0; bulletIndex < 0x100; bulletIndex++, laser++)
    {
        if (laser->inUse == 0)
        {
            continue;
        }
        if ((laser->flags & 4) != 0 && mode != 4)
        {
            continue;
        }

        if (laser->state < LASER_STATE_DESPAWNING)
        {
            laser->state = LASER_STATE_DESPAWNING;
            laser->timer = 0;
            *reinterpret_cast<i32 *>(&laser->width) =
                *reinterpret_cast<i32 *>(&laser->currentWidth);

            if (mode != 4)
            {
                radius = laser->startOffset;
                fsincos(&sine, &cosine, laser->angle);
                while (laser->endOffset > radius)
                {
                    position[0] = cosine * radius + laser->position.x;
                    position[1] = sine * radius + laser->position.y;
                    position[2] = 0.0f;
                    g_ItemManager.SpawnItem(reinterpret_cast<Float3 *>(position),
                                            static_cast<ItemType>(this->cancelItemType), mode);
                    radius = radius + 1.0f;
                }
            }
        }

        laser->hitboxEndDelay = 0;
    }

    this->spawnSuppressionFrames = 10;
}


// FUNCTION: th08 0x430aa0
#pragma var_order(score, totalScore, bulletCount, bulletIndex, sine, bullet, position, laser, cosine, radius, this)
i32 BulletManager::DespawnBullets(i32 maxScore, i32 awardLaserItems)
{
    f32 radius;
    f32 cosine;
    Laser *laser;
    f32 position[3];
    Bullet *bullet;
    f32 sine;
    i32 bulletIndex;
    i32 bulletCount;
    i32 totalScore;
    i32 score;

    totalScore = 0;
    score = 2000;
    bulletCount = 0;
    bullet = &g_BulletManager.bullets[0];
    for (bulletIndex = 0; bulletIndex < 0x600; bulletIndex++, bullet++)
    {
        if (bullet->state == BULLET_STATE_UNUSED)
        {
            continue;
        }

        if (g_Player.CheckBulletCancelCollision(&bullet->position,
                                 &bullet->sprites.collisionSize) == 2)
        {
            g_ItemManager.SpawnItem(&bullet->position,
                                    static_cast<ItemType>(g_Player.bulletCancelItemType),
                                    ITEM_STATE_AUTOCOLLECT);
        }
        else
        {
            g_ItemManager.SpawnItem(
                &bullet->position,
                static_cast<ItemType>(this->cancelItemType), ITEM_STATE_AUTOCOLLECT);
        }

        g_AsciiManager.CreateScorePopup(&bullet->position, score,
                                        score >= maxScore ? -256 : -1);
        totalScore += score;
        bulletCount++;
        score += 20;
        if (score > maxScore)
        {
            score = maxScore;
        }
        bullet->state = BULLET_STATE_DESPAWNING;
    }

    laser = &this->lasers[0];
    reinterpret_cast<Float3 *>(position)->operator float *();
    for (bulletIndex = 0; bulletIndex < 0x100; bulletIndex++, laser++)
    {
        if (laser->inUse == 0)
        {
            continue;
        }

        if (laser->state < LASER_STATE_DESPAWNING)
        {
            laser->state = LASER_STATE_DESPAWNING;
            laser->timer = 0;
            *reinterpret_cast<i32 *>(&laser->width) =
                *reinterpret_cast<i32 *>(&laser->currentWidth);

            if (awardLaserItems)
            {
                g_ItemManager.SpawnItem(
                    &laser->position,
                    static_cast<ItemType>(this->cancelItemType), ITEM_STATE_AUTOCOLLECT);
                radius = laser->startOffset;
                fsincos(&sine, &cosine, laser->angle);
                while (laser->endOffset > radius)
                {
                    position[0] = cosine * radius + laser->position.x;
                    position[1] = sine * radius + laser->position.y;
                    position[2] = 0.0f;
                    g_ItemManager.SpawnItem(
                        reinterpret_cast<Float3 *>(position),
                        static_cast<ItemType>(this->cancelItemType), ITEM_STATE_AUTOCOLLECT);
                    radius += 32.0f;
                }
            }
        }

        laser->hitboxEndDelay = 0;
    }

    this->spawnSuppressionFrames = 10;
    return totalScore;
}

// FUNCTION: th08 0x430d30
#pragma var_order(delta, i, bullet, this)
void BulletManager::RemoveBulletsInRadius(const Float3 *position, f32 radius)
{
    i32 i;
    Bullet *bullet;

    bullet = &g_BulletManager.bullets[0];
    Float3 delta;
    radius *= radius;
    for (i = 0; i < 0x600; i++, bullet++)
    {
        if (bullet->state == BULLET_STATE_UNUSED ||
            bullet->state == BULLET_STATE_DESPAWNING)
            continue;
        delta = bullet->position - *position;
        if (D3DXVec3LengthSq(reinterpret_cast<D3DXVECTOR3 *>(&delta)) > radius)
            continue;
        g_ItemManager.SpawnItem(&bullet->position, ITEM_POINT_STAR, ITEM_STATE_AUTOCOLLECT);
        memset(bullet, 0, sizeof(Bullet));
#if defined(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH_ENABLED)
        PspBulletTransformTerminalClearForWholeSlotReset(bullet);
#endif
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
        PspBulletRuntimeDeactivate(bullet);
#endif
#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED)
        PspBulletTransformAuditNoteWholeSlotReset();
#endif
    }
}

// FUNCTION: th08 0x430e10
#pragma var_order(i, angleToPlayer, j, this)
i32 BulletManager::SpawnBulletPattern(BulletSpawnDescriptor *descriptor)
{
    i32 i;
    f32 angleToPlayer;
    i32 j;

    g_ReplayManager->frameEventFlags |= 0x800;
    if (g_BulletManager.activeBulletCount >= 0x600)
        return 0;

    descriptor->templateSprites = &this->bulletTypeSprites[descriptor->bulletType];
    angleToPlayer = g_Player.AngleToPoint(&descriptor->position);
    for (j = 0; j < descriptor->count2; j++)
    {
        for (i = 0; i < descriptor->count1; i++)
        {
            if (this->SpawnSingleBullet(descriptor, i, j, angleToPlayer) != 0)
                goto doneSpawning;
        }
    }

doneSpawning:
    if ((descriptor->transformFlags & BULLET_TRANSFORM_PLAY_SPAWN_SOUND) != 0)
        g_SoundPlayer.PlaySoundPositionedByIdx(static_cast<SoundIdx>(descriptor->spawnSound), descriptor->position.x);
    return 0;
}

// FUNCTION: th08 0x430f20
#pragma var_order(i, laser, this)
Laser *BulletManager::SpawnLaserPattern(BulletSpawnDescriptor *descriptor)
{
    Laser *laser;
    i32 i;

    laser = &this->lasers[0];
    if (this->spawnSuppressionFrames != 0 &&
        (descriptor->transformFlags & BULLET_TRANSFORM_SPAWN_NORMAL) == 0)
        return laser;

    for (i = 0; i < 0x100; i++, laser++)
    {
        if (laser->inUse)
            continue;

        this->bulletAnm->SetAndExecuteScriptIdx(&laser->bodyVm, descriptor->bulletType + 10);
        this->bulletAnm->SetSprite(&laser->bodyVm, laser->bodyVm.activeSpriteIndex + descriptor->color);
        this->bulletAnm->InitializeAndSetSprite(
            &laser->startCapVm, g_BulletSpriteOffsetSmall[descriptor->color] + 0x92);
        laser->startCapVm.blendMode = 1;
        laser->position = descriptor->position;
        laser->color = descriptor->color;
        laser->inUse = 1;
        laser->angle = descriptor->angle;
        if (descriptor->aimMode == BULLET_AIM_FAN_AIMED)
            laser->angle = g_Player.AngleToPoint(&descriptor->position) + laser->angle;
        laser->flags = static_cast<u16>(descriptor->transformFlags);
        laser->timer = 0;
        laser->startOffset = descriptor->laserStartOffset;
        laser->endOffset = descriptor->laserEndOffset;
        laser->startLength = descriptor->laserStartLength;
        laser->width = descriptor->laserWidth;
        laser->speed = descriptor->speed1;
        laser->startTime = descriptor->laserStartTime;
        laser->duration = descriptor->laserDuration;
        laser->despawnDuration = descriptor->laserDespawnDuration;
        laser->hitboxStartTime = descriptor->laserHitboxStartTime;
        laser->hitboxEndDelay = descriptor->laserHitboxEndDelay;
        laser->hideCapDuringStartup = 0;
        if (laser->startTime == 0)
            laser->state = LASER_STATE_ACTIVE;
        else
            laser->state = LASER_STATE_STARTING;
        break;
    }
    return laser;
}

// FUNCTION: th08 0x4311a0
#pragma var_order(bulletManager, bulletAnmPath)
ZunResult BulletManager::RegisterChain(char *bulletAnmPath)
{
    BulletManager *bulletManager = &g_BulletManager;

#if defined(TH08_PSP_STAGE_POOL_ARENA)
    if (!th08::psp::StagePoolArenaIsBound())
        return ZUN_ERROR;
#endif
    bulletManager->Initialize();
    bulletManager->bulletAnmPath = bulletAnmPath;

    g_BulletManagerCalcChain.SetCallback((ChainCallback)BulletManager::OnUpdate);
    g_BulletManagerCalcChain.addedCallback = (ChainLifetimeCallback)BulletManager::AddedCallback;
    g_BulletManagerCalcChain.deletedCallback = (ChainLifetimeCallback)BulletManager::DeletedCallback;
    g_BulletManagerCalcChain.arg = bulletManager;
    if (g_Chain.AddToCalcChain(&g_BulletManagerCalcChain, CHAIN_PRIO_CALC_BULLETMANAGER))
    {
        return ZUN_ERROR;
    }

    g_BulletManagerDrawChain.SetCallback((ChainCallback)BulletManager::OnDraw);
    g_BulletManagerDrawChain.arg = bulletManager;
    g_Chain.AddToDrawChain(&g_BulletManagerDrawChain, CHAIN_PRIO_DRAW_BULLETMANAGER);

    return ZUN_SUCCESS;
}

// FUNCTION: th08 0x431240
#pragma var_order(collisionResult, i, currentWidth, bucketIndex, laserSize, bullet, alpha, laser, laserCenter, rampWindow, bulletManager)
ChainCallbackResult BulletManager::OnUpdate(BulletManager *bulletManager)
{
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
    th08::psp::PerfAttributionScope perfScope(
        th08::psp::PerfAttributionPhase::BulletUpdateInclusive);
#endif
    i32 collisionResult;
    i32 i;
    f32 currentWidth;
    i32 bucketIndex;
    f32 laserSize[3];
    Bullet *bullet;
    i32 alpha;
    Laser *laser;
    f32 laserCenter[3];
    i32 rampWindow;
#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT_ENABLED) || \
    defined(TH08_PSP_BULLET_COLLISION_GATE_PRODUCT_ENABLED)
    PspPlayerBulletCollisionAuditSnapshot collisionAuditSnapshot;
    bool collisionAuditSnapshotBoundsValid;
#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT_ENABLED)
    th08::psp::PspBulletCollisionGateDecision collisionAuditDecision;
    bool collisionAuditGrazePath;
#endif
#endif

    bucketIndex = 0;
    bullet = &bulletManager->bullets[0];
    if (((*reinterpret_cast<u32 *>(&g_GameManager.flags) >> 10) & 1) != 0)
        return CHAIN_CALLBACK_RESULT_CONTINUE;

    g_ItemManager.OnUpdate();
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH) && \
    defined(TH08_REPLAY_SYNC_AUDIT)
    // Item update can synchronously request a transition clear, so validate
    // after it and immediately before the first optimized Bullet decision.
    PspBulletRuntimeAuditAndRepair(bulletManager);
#endif
#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT_ENABLED) || \
    defined(TH08_PSP_BULLET_COLLISION_GATE_PRODUCT_ENABLED)
    collisionAuditSnapshot =
        PspCapturePlayerBulletCollisionAuditSnapshot(&g_Player);
    collisionAuditSnapshotBoundsValid =
        th08::psp::PspBulletCollisionSnapshotBoundsValid(
            collisionAuditSnapshot.hurtboxBoundsMin.x,
            collisionAuditSnapshot.hurtboxBoundsMin.y,
            collisionAuditSnapshot.hurtboxBoundsMax.x,
            collisionAuditSnapshot.hurtboxBoundsMax.y,
            collisionAuditSnapshot.grazeBoundsMin.x,
            collisionAuditSnapshot.grazeBoundsMin.y,
            collisionAuditSnapshot.grazeBoundsMax.x,
            collisionAuditSnapshot.grazeBoundsMax.y);
#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT_ENABLED)
    PspBulletCollisionGateAuditNoteFrame(collisionAuditSnapshot);
#endif
#endif
    bulletManager->activeBulletCount = 0;
    bulletManager->ClearDrawBuckets();

#if defined(TH08_PSP_BULLET_LIVE_ENUM_ENABLED)
    // Construct only after the replay audit has repaired false negatives.  A
    // missing or unpublished sidecar selects the untouched 1536-slot loop.
    th08::psp::PspBulletLiveEnumerator liveEnumerator(
        g_PspBulletRuntimeCache != NULL
            ? g_PspBulletRuntimeCache->activeBits
            : NULL,
        g_PspBulletRuntimeActiveBitsValid);
    const bool useLiveEnumerator = liveEnumerator.IsUsable();
#if TH08_PSP_RUNTIME_TELEMETRY
    u32 liveEnumSlotProbes = 0U;
    u32 liveEnumWordProbes = 0U;
    u32 liveEnumVisited = 0U;
#endif
#endif

    for (i = 0; i < 0x600; i++)
    {
#if defined(TH08_PSP_BULLET_LIVE_ENUM_ENABLED)
        if (useLiveEnumerator)
        {
            u32 liveSlot = 0U;
#if TH08_PSP_RUNTIME_TELEMETRY
            if (!liveEnumerator.Next(&liveSlot, &liveEnumSlotProbes,
                                     &liveEnumWordProbes))
#else
            if (!liveEnumerator.Next(&liveSlot, NULL, NULL))
#endif
            {
                break;
            }
            bucketIndex = static_cast<i32>(liveSlot);
            bullet = &bulletManager->bullets[liveSlot];
        }
#endif
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
        // bucketIndex is the exact current slot in the original traversal:
        // 0, 1535, 1534, ... 1.  Test the compact sidecar before touching the
        // multi-kilobyte Bullet payload, without changing simulation order.
#if defined(TH08_PSP_BULLET_LIVE_ENUM_ENABLED)
        if (!useLiveEnumerator &&
            !PspBulletRuntimeBit(static_cast<u32>(bucketIndex)))
#else
        if (!PspBulletRuntimeBit(static_cast<u32>(bucketIndex)))
#endif
            goto nextBullet;
#endif
        if (bullet->state == BULLET_STATE_UNUSED)
        {
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
            // False-positive bits remain harmless in non-audit builds too.
            PspBulletRuntimeDeactivate(bullet);
#endif
            goto nextBullet;
        }

        ++bulletManager->activeBulletCount;
#if defined(TH08_PSP_BULLET_LIVE_ENUM_ENABLED) && \
    TH08_PSP_RUNTIME_TELEMETRY
        ++liveEnumVisited;
#endif
            switch (bullet->state)
            {
activateBullet:
            bullet->state = BULLET_STATE_FIRED;
            bullet->stateTimer = 0;
            case BULLET_STATE_FIRED:
updateBullet:
#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED)
            PspBulletTransformAuditNoteFiredUpdateCall();
#endif
#if defined(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH_ENABLED)
            if (!PspBulletTransformTerminalSkip(
                    static_cast<u32>(bucketIndex)))
#endif
            bullet->AdvanceTransformProgram();
            if (bullet->activeTransformFlags != 0)
            {
                if ((bullet->activeTransformFlags & BULLET_TRANSFORM_DECELERATE) != 0)
                    bullet->UpdateDeceleration();
                if ((bullet->activeTransformFlags & BULLET_TRANSFORM_ACCELERATE_VECTOR) != 0)
                    bullet->UpdateVectorAcceleration();
                if ((bullet->activeTransformFlags & BULLET_TRANSFORM_ACCELERATE_POLAR) != 0)
                    bullet->UpdatePolarAcceleration();
                if ((bullet->activeTransformFlags & BULLET_TRANSFORM_CHANGE_DIRECTION_RELATIVE) != 0)
                    bullet->UpdateRelativeDirectionChange();
                if ((bullet->activeTransformFlags & BULLET_TRANSFORM_CHANGE_DIRECTION_ABSOLUTE) != 0)
                    bullet->UpdateAbsoluteDirectionChange();
                if ((bullet->activeTransformFlags & BULLET_TRANSFORM_CHANGE_DIRECTION_AIMED) != 0)
                    bullet->UpdateAimedDirectionChange();
                if ((bullet->activeTransformFlags &
                     (BULLET_TRANSFORM_BOUNCE_ALL_EDGES |
                      BULLET_TRANSFORM_BOUNCE_EXCEPT_BOTTOM)) != 0)
                    bullet->UpdateBoundaryBounce();
                if ((bullet->activeTransformFlags & BULLET_TRANSFORM_WRAP_X) != 0)
                    bullet->UpdateHorizontalWrap();
                if ((bullet->activeTransformFlags & BULLET_TRANSFORM_WRAP_Y) != 0)
                    bullet->UpdateVerticalWrap();
                if ((bullet->activeTransformFlags & BULLET_TRANSFORM_WAIT) != 0)
                {
                    if (bullet->exStates[BULLET_TRANSFORM_STATE_WAIT].timer <= 0)
                        bullet->activeTransformFlags ^= BULLET_TRANSFORM_WAIT;
                    else
                        bullet->exStates[BULLET_TRANSFORM_STATE_WAIT].timer--;
                }
            }

            if (bullet->offscreenCullDelayFrames != 0)
                --bullet->offscreenCullDelayFrames;
            if (!g_GameManager.scriptedUpdateFreeze)
                bullet->position += bullet->velocity;

            if (bullet->offscreenCullDelayFrames == 0)
            {
                if (!g_GameManager.IsWithinPlayfield(
                        bullet->position.operator float *()[0],
                        bullet->position.operator float *()[1],
                        bullet->sprites.bulletVm.loadedSprite->widthPx,
                        bullet->sprites.bulletVm.loadedSprite->heightPx))
                {
                    if ((bullet->activeTransformFlags &
                         (BULLET_TRANSFORM_CHANGE_DIRECTION_RELATIVE |
                          BULLET_TRANSFORM_CHANGE_DIRECTION_AIMED |
                          BULLET_TRANSFORM_CHANGE_DIRECTION_ABSOLUTE |
                          BULLET_TRANSFORM_BOUNCE_ALL_EDGES |
                          BULLET_TRANSFORM_BOUNCE_EXCEPT_BOTTOM)) != 0)
                    {
                        ++bullet->offscreenFrames;
                        if (bullet->offscreenFrames >= 0x80)
                        {
                            bullet->Deactivate();
                            goto nextBullet;
                        }
                    }
                    else
                    {
                        if (bullet->offscreenFrames == 0)
                        {
                            bullet->Deactivate();
                            goto nextBullet;
                        }
                        --bullet->offscreenFrames;
                    }
                }
                else
                    bullet->offscreenFrames = 0;
            }

            if (bullet->collisionDisabled == 0)
            {
#if defined(TH08_PSP_BULLET_COLLISION_GATE_PRODUCT_ENABLED)
                // Negative gate: when the frame snapshot proves the cancel set
                // empty and this bullet strictly separated from the box the
                // canonical first call would test, that call would return 0
                // with the sole side effect bulletCancelItemType = 6.
                // Reproduce it and continue exactly where a 0 continues.
                if (PspPlayerBulletCollisionAuditBoundsMatch(&g_Player,
                                                             &collisionAuditSnapshot))
                {
                    const bool gateGrazePath =
                        bullet->isGrazed == 0 &&
                        (i32)bullet->activeTimer >= 16;
                    const bool gateGrazeSuppressed =
                        g_Player.playerState == PLAYER_STATE_DYING ||
                        g_Player.playerState == PLAYER_STATE_SPAWNING;
                    const th08::psp::PspBulletCollisionGateDecision gateDecision =
                        th08::psp::PspBulletCollisionDefinitelyClear(
                            bullet->position.x, bullet->position.y,
                            bullet->sprites.collisionSize.x,
                            bullet->sprites.collisionSize.y, gateGrazePath,
                            gateGrazeSuppressed,
                            collisionAuditSnapshot.knownEmpty != 0U,
                            collisionAuditSnapshotBoundsValid,
                            collisionAuditSnapshot.hurtboxBoundsMin.x,
                            collisionAuditSnapshot.hurtboxBoundsMin.y,
                            collisionAuditSnapshot.hurtboxBoundsMax.x,
                            collisionAuditSnapshot.hurtboxBoundsMax.y,
                            collisionAuditSnapshot.grazeBoundsMin.x,
                            collisionAuditSnapshot.grazeBoundsMin.y,
                            collisionAuditSnapshot.grazeBoundsMax.x,
                            collisionAuditSnapshot.grazeBoundsMax.y);
                    th08::psp::BulletGateNote(
                        static_cast<th08::psp::BulletGateOutcome>(
                            gateDecision ==
                                    th08::psp::PSP_BULLET_COLLISION_GATE_CLEAR_GRAZE_SUPPRESSED
                                ? 0
                            : gateDecision ==
                                    th08::psp::PSP_BULLET_COLLISION_GATE_CLEAR_GRAZE_SEPARATE
                                ? 1
                            : gateDecision ==
                                    th08::psp::PSP_BULLET_COLLISION_GATE_CLEAR_LETHAL_SEPARATE
                                ? 2
                            : gateDecision ==
                                    th08::psp::PSP_BULLET_COLLISION_GATE_FALLBACK_CANCEL_UNKNOWN
                                ? 3
                            : gateDecision ==
                                    th08::psp::PSP_BULLET_COLLISION_GATE_FALLBACK_SNAPSHOT_INVALID
                                ? 4
                            : gateDecision ==
                                    th08::psp::PSP_BULLET_COLLISION_GATE_FALLBACK_BULLET_INVALID
                                ? 5
                                : 6));
                    if (th08::psp::PspBulletCollisionGateIsClear(gateDecision))
                    {
                        g_Player.bulletCancelItemType = 6;
                        goto executeBulletScript;
                    }
                }
                else
                {
                    th08::psp::BulletGateNote(
                        th08::psp::BulletGateOutcome::FallbackBoundsMutated);
                }
#endif
#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT_ENABLED)
                collisionAuditGrazePath =
                    bullet->isGrazed == 0 &&
                    (i32)bullet->activeTimer >= 16;
                collisionAuditDecision = PspBulletCollisionGateAuditBegin(
                    collisionAuditSnapshot,
                    collisionAuditSnapshotBoundsValid, bullet,
                    collisionAuditGrazePath);
#endif
                if (bullet->isGrazed == 0 &&
                    (i32)bullet->activeTimer >= 16)
                {
                    collisionResult = g_Player.CheckGrazeCollision(&bullet->position,
                                                            &bullet->sprites.collisionSize);
#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT_ENABLED)
                    PspBulletCollisionGateAuditObserveCanonical(
                        collisionAuditDecision, collisionResult);
#endif
                    if (collisionResult == 1)
                    {
                        bullet->isGrazed = 1;
                        goto lethalCollision;
                    }
                    if (collisionResult == 2 &&
                        (bullet->transformFlags & BULLET_TRANSFORM_CANCEL_IMMUNE) == 0)
                    {
                        bullet->state = BULLET_STATE_DESPAWNING;
                        if (g_Player.bulletCancelItemType == 9)
                        {
                            g_ItemManager.SpawnItem(&bullet->position, ITEM_TIME, ITEM_STATE_AUTOCOLLECT);
                            g_ItemManager.SpawnItem(&bullet->position, ITEM_TIME, ITEM_STATE_AUTOCOLLECT);
                        }
                        else if (g_Player.bulletCancelItemType >= 0)
                            g_ItemManager.SpawnItem(&bullet->position,
                                                    static_cast<ItemType>(g_Player.bulletCancelItemType),
                                                    ITEM_STATE_AUTOCOLLECT);
                    }
                    goto executeBulletScript;
                }

lethalCollision:
                collisionResult = g_Player.CheckBulletCollision(&bullet->position,
                                                        &bullet->sprites.collisionSize);
#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT_ENABLED)
                // A newly grazed bullet reaches this second canonical call as
                // part of the original behavior.  The proposed first-call
                // gate was already observed above; never duplicate or count
                // this lethal follow-up as another audit decision.
                if (!collisionAuditGrazePath)
                {
                    PspBulletCollisionGateAuditObserveCanonical(
                        collisionAuditDecision, collisionResult);
                }
#endif
                if (collisionResult != 0 &&
                    (collisionResult != 2 ||
                     (bullet->transformFlags & BULLET_TRANSFORM_CANCEL_IMMUNE) == 0))
                {
                    bullet->state = BULLET_STATE_DESPAWNING;
                    if (collisionResult == 2)
                    {
                        if (g_Player.bulletCancelItemType == 9)
                        {
                            g_ItemManager.SpawnItem(&bullet->position, ITEM_TIME, ITEM_STATE_AUTOCOLLECT);
                            g_ItemManager.SpawnItem(&bullet->position, ITEM_TIME, ITEM_STATE_AUTOCOLLECT);
                        }
                        else if (g_Player.bulletCancelItemType >= 0)
                            g_ItemManager.SpawnItem(&bullet->position,
                                                    static_cast<ItemType>(g_Player.bulletCancelItemType),
                                                    ITEM_STATE_AUTOCOLLECT);
                    }
                }
            }
executeBulletScript:
            if (bullet->sprites.bulletVm.currentInstruction != NULL)
                g_AnmManager->ExecuteScript(&bullet->sprites.bulletVm);
                break;
            case BULLET_STATE_SPAWNING_FAST:
                bullet->activeTimer--;
                bullet->position += bullet->velocity / 2.0f;
                if ((bullet->transformFlags & BULLET_TRANSFORM_CANCEL_IMMUNE) == 0 &&
                    g_Player.CheckBulletCancelCollision(&bullet->position,
                                         &bullet->sprites.collisionSize) == 2)
                    bullet->cancelledDuringSpawn = 1;
#if defined(TH08_PSP_COMPACT_BULLET_VM)
                if (g_AnmManager->ExecuteScript(&bullet->sprites.selectedSpawnVm) == 0)
#else
                if (g_AnmManager->ExecuteScript(&bullet->sprites.spawnFastVm) == 0)
#endif
                    break;
                if (bullet->cancelledDuringSpawn != 0)
                {
                    bullet->state = BULLET_STATE_DESPAWNING;
                    if (g_Player.bulletCancelItemType == 9)
                    {
                        g_ItemManager.SpawnItem(&bullet->position, ITEM_TIME, ITEM_STATE_AUTOCOLLECT);
                        g_ItemManager.SpawnItem(&bullet->position, ITEM_TIME, ITEM_STATE_AUTOCOLLECT);
                    }
                    else if (g_Player.bulletCancelItemType >= 0)
                        g_ItemManager.SpawnItem(&bullet->position,
                                                static_cast<ItemType>(g_Player.bulletCancelItemType),
                                                ITEM_STATE_AUTOCOLLECT);
                }
                goto activateBullet;
            case BULLET_STATE_SPAWNING_NORMAL:
                bullet->activeTimer--;
                bullet->position += bullet->velocity / 2.5f;
                if ((bullet->transformFlags & BULLET_TRANSFORM_CANCEL_IMMUNE) == 0 &&
                    g_Player.CheckBulletCancelCollision(&bullet->position,
                                         &bullet->sprites.collisionSize) == 2)
                    bullet->cancelledDuringSpawn = 1;
#if defined(TH08_PSP_COMPACT_BULLET_VM)
                if (g_AnmManager->ExecuteScript(&bullet->sprites.selectedSpawnVm) == 0)
#else
                if (g_AnmManager->ExecuteScript(&bullet->sprites.spawnNormalVm) == 0)
#endif
                    break;
                if (bullet->cancelledDuringSpawn != 0)
                {
                    bullet->state = BULLET_STATE_DESPAWNING;
                    if (g_Player.bulletCancelItemType == 9)
                    {
                        g_ItemManager.SpawnItem(&bullet->position, ITEM_TIME, ITEM_STATE_AUTOCOLLECT);
                        g_ItemManager.SpawnItem(&bullet->position, ITEM_TIME, ITEM_STATE_AUTOCOLLECT);
                    }
                    else if (g_Player.bulletCancelItemType >= 0)
                        g_ItemManager.SpawnItem(&bullet->position,
                                                static_cast<ItemType>(g_Player.bulletCancelItemType),
                                                ITEM_STATE_AUTOCOLLECT);
                }
                goto activateBullet;
            case BULLET_STATE_SPAWNING_SLOW:
                bullet->activeTimer--;
                bullet->position += bullet->velocity / 3.0f;
                if ((bullet->transformFlags & BULLET_TRANSFORM_CANCEL_IMMUNE) == 0 &&
                    g_Player.CheckBulletCancelCollision(&bullet->position,
                                         &bullet->sprites.collisionSize) == 2)
                    bullet->cancelledDuringSpawn = 1;
#if defined(TH08_PSP_COMPACT_BULLET_VM)
                if (g_AnmManager->ExecuteScript(&bullet->sprites.selectedSpawnVm) == 0)
#else
                if (g_AnmManager->ExecuteScript(&bullet->sprites.spawnSlowVm) == 0)
#endif
                    break;
                if (bullet->cancelledDuringSpawn != 0)
                {
                    bullet->state = BULLET_STATE_DESPAWNING;
                    if (g_Player.bulletCancelItemType == 9)
                    {
                        g_ItemManager.SpawnItem(&bullet->position, ITEM_TIME, ITEM_STATE_AUTOCOLLECT);
                        g_ItemManager.SpawnItem(&bullet->position, ITEM_TIME, ITEM_STATE_AUTOCOLLECT);
                    }
                    else if (g_Player.bulletCancelItemType >= 0)
                        g_ItemManager.SpawnItem(&bullet->position,
                                                static_cast<ItemType>(g_Player.bulletCancelItemType),
                                                ITEM_STATE_AUTOCOLLECT);
                }
                goto activateBullet;
            case BULLET_STATE_DESPAWNING:
                bullet->position += bullet->velocity / 2.0f;
                if (g_AnmManager->ExecuteScript(&bullet->sprites.despawnVm) != 0)
                {
                    bullet->Deactivate();
                    goto nextBullet;
                }
                break;
            default:
                break;
            }

updateTimers:
            bullet->stateTimer++;
            bullet->activeTimer++;
            bullet->nextInDrawBucket =
                bulletManager->drawBuckets[bullet->sprites.drawBucketIndex];
            bulletManager->drawBuckets[bullet->sprites.drawBucketIndex] = bullet;
nextBullet:
#if defined(TH08_PSP_BULLET_LIVE_ENUM_ENABLED)
        if (useLiveEnumerator)
            continue;
#endif
        --bucketIndex;
        if (bucketIndex < 0)
        {
            bucketIndex = 0x5FF;
            bullet += 0x600;
        }
        bullet--;
    }

#if defined(TH08_PSP_BULLET_LIVE_ENUM_ENABLED) && \
    TH08_PSP_RUNTIME_TELEMETRY
    if (!useLiveEnumerator)
        liveEnumSlotProbes += 0x600U;
    PspBulletLiveEnumCommitFrame(liveEnumSlotProbes, liveEnumWordProbes,
                                 liveEnumVisited, !useLiveEnumerator);
#endif

    laser = &bulletManager->lasers[0];
    reinterpret_cast<Float3 *>(laserCenter)->operator float *();
    reinterpret_cast<Float3 *>(laserSize)->operator float *();
    for (i = 0; i < 0x100; i++, laser++)
    {
            if (laser->inUse == 0)
                continue;

            laser->endOffset += g_Supervisor.framerateMultiplier * laser->speed;
            if (laser->endOffset - laser->startOffset > laser->startLength)
                laser->startOffset = laser->endOffset - laser->startLength;
            if (laser->startOffset < 0.0f)
                laser->startOffset = 0.0f;

            laserSize[1] = laser->width / 2.0f;
            if (laser->startOffset <= 0.0f)
                laserSize[0] = laser->endOffset - laser->startOffset;
            else
                laserSize[0] = (laser->endOffset - laser->startOffset) * 0.7f;
            laserCenter[0] = (laser->endOffset - laser->startOffset) / 2.0f +
                            laser->startOffset + laser->position.x;
            laserCenter[1] = laser->position.y;
            laser->bodyVm.scale.x = laser->width / laser->bodyVm.loadedSprite->widthPx;
            currentWidth = laser->endOffset - laser->startOffset;
            laser->bodyVm.scale.y = currentWidth / laser->bodyVm.loadedSprite->heightPx;
            laser->bodyVm.SetZRotation(
                AddNormalizeAngle(ZUN_PI / 2.0f + laser->angle, 0.0f));

            switch (laser->state)
            {
            case LASER_STATE_STARTING:
                if ((laser->flags & 1) != 0)
                {
                    alpha = (i32)((f32)laser->timer * 255.0f / laser->startTime);
                    if (alpha > 255)
                        alpha = 255;
                    laser->bodyVm.color1.d3dColor = alpha << 24;
                }
                else
                {
                    rampWindow = laser->startTime > 30
                                     ? 30
                                     : laser->startTime;
                    if (laser->startTime - rampWindow < (i32)laser->timer)
                        currentWidth = (f32)laser->timer * laser->width / laser->startTime;
                    else
                        currentWidth = 1.2f;
                    laser->currentWidth = currentWidth;
                    laser->bodyVm.scale.x = currentWidth / 16.0f;
                    laserSize[0] = currentWidth / 2.0f;
                }
                if (laser->timer >= laser->hitboxStartTime)
                    g_Player.CalcLaserHitbox(reinterpret_cast<Float3 *>(laserCenter), reinterpret_cast<Float3 *>(laserSize),
                                             &laser->position, laser->angle, 0);
                if (laser->timer < laser->startTime)
                    break;
                laser->timer = 0;
                ++laser->state;
                laser->currentWidth = laser->width;
            case LASER_STATE_ACTIVE:
                g_Player.CalcLaserHitbox(reinterpret_cast<Float3 *>(laserCenter), reinterpret_cast<Float3 *>(laserSize),
                                         &laser->position, laser->angle,
                                         ((i32)laser->timer) % 20 == 0);
                if (laser->timer < laser->duration)
                    break;
                laser->timer = 0;
                ++laser->state;
                if (laser->despawnDuration == 0)
                {
                    laser->inUse = 0;
                    continue;
                }
            case LASER_STATE_DESPAWNING:
                if ((laser->flags & 1) != 0)
                {
                    alpha = (i32)((f32)laser->timer * 255.0f / laser->startTime);
                    if (alpha > 255)
                        alpha = 255;
                    laser->bodyVm.color1.d3dColor = alpha << 24;
                }
                else if (laser->despawnDuration > 0)
                {
                    currentWidth = laser->width -
                                   (f32)laser->timer * laser->width / laser->despawnDuration;
                    laser->bodyVm.scale.x = currentWidth / 16.0f;
                    laserSize[0] = currentWidth / 2.0f;
                }
                if (laser->timer < laser->hitboxEndDelay)
                    g_Player.CalcLaserHitbox(reinterpret_cast<Float3 *>(laserCenter), reinterpret_cast<Float3 *>(laserSize),
                                             &laser->position, laser->angle, 0);
                if (laser->timer < laser->despawnDuration)
                    break;
                laser->inUse = 0;
                continue;
            }

            if (laser->startOffset >= 640.0f)
                laser->inUse = 0;
            laser->timer++;
            g_AnmManager->ExecuteScript(&laser->bodyVm);
        }

    if (bulletManager->spawnSuppressionFrames != 0)
        --bulletManager->spawnSuppressionFrames;
    bulletManager->timer++;
    ++bulletManager->frameCounter;
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

// FUNCTION: th08 0x432170
void Bullet::Deactivate()
{
    this->state = BULLET_STATE_UNUSED;
    this->stateTimer = 0;
    this->activeTimer = 0;
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
    PspBulletRuntimeDeactivate(this);
#endif
}

// FUNCTION: th08 0x4321b0
void BulletManager::ClearDrawBuckets()
{
    this->drawBuckets[5] = NULL;
    this->drawBuckets[4] = NULL;
    this->drawBuckets[3] = NULL;
    this->drawBuckets[2] = NULL;
    this->drawBuckets[1] = NULL;
    this->drawBuckets[0] = NULL;
}

// FUNCTION: th08 0x432210
#pragma var_order(magnitude, this)
void Bullet::UpdateDeceleration()
{
    f32 magnitude;

    if (this->exStates[BULLET_TRANSFORM_STATE_DECELERATION].timer <= 16)
    {
        magnitude =
            5.0f -
            ((f32)this->exStates[BULLET_TRANSFORM_STATE_DECELERATION].timer * 5.0f) /
                16.0f;
        this->velocity.FromAngleMagnitude(this->angle,
                                 (magnitude + this->speed) *
                                     g_Supervisor.framerateMultiplier);
    }
    else
    {
        this->activeTransformFlags ^= BULLET_TRANSFORM_DECELERATE;
    }

    this->exStates[BULLET_TRANSFORM_STATE_DECELERATION].timer++;
}


// FUNCTION: th08 0x4322b0
#pragma var_order(delta, this)
void Bullet::UpdateVectorAcceleration()
{
    if (this->exStates[BULLET_TRANSFORM_STATE_VECTOR_ACCELERATION].timer >=
        this->exStates[BULLET_TRANSFORM_STATE_VECTOR_ACCELERATION].durationFrames)
    {
        this->activeTransformFlags &= ~BULLET_TRANSFORM_ACCELERATE_VECTOR;
    }
    else
    {
        this->velocity +=
            this->exStates[BULLET_TRANSFORM_STATE_VECTOR_ACCELERATION].vector *
            g_Supervisor.framerateMultiplier;

        if (fabsf(this->velocity.x) > 0.0001f ||
            fabsf(this->velocity.y) > 0.0001f)
        {
            this->angle = VectorAngle(this->velocity.y, this->velocity.x);
        }
    }

    this->exStates[BULLET_TRANSFORM_STATE_VECTOR_ACCELERATION].timer++;
}

// FUNCTION: th08 0x432390
void Bullet::UpdatePolarAcceleration()
{
    if (this->exStates[BULLET_TRANSFORM_STATE_POLAR_ACCELERATION].timer >=
        this->exStates[BULLET_TRANSFORM_STATE_POLAR_ACCELERATION].durationFrames)
    {
        this->activeTransformFlags &= ~BULLET_TRANSFORM_ACCELERATE_POLAR;
    }
    else
    {
        this->angle =
            AddNormalizeAngle(this->angle,
                              g_Supervisor.framerateMultiplier *
                                  this->exStates[BULLET_TRANSFORM_STATE_POLAR_ACCELERATION].angleDelta);
        this->speed +=
            g_Supervisor.framerateMultiplier *
            this->exStates[BULLET_TRANSFORM_STATE_POLAR_ACCELERATION].speedDelta;
        this->velocity.FromAngleMagnitude(this->angle,
                                 g_Supervisor.framerateMultiplier *
                                     this->speed);
    }

    this->exStates[BULLET_TRANSFORM_STATE_POLAR_ACCELERATION].timer++;
}

// FUNCTION: th08 0x432460
#pragma var_order(magnitude, this)
void Bullet::UpdateRelativeDirectionChange()
{
    f32 magnitude;

    if (this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].timer >=
        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeIntervalFrames)
    {
        if (this->transformSound >= 0)
        {
            g_SoundPlayer.PlaySoundByIdx(static_cast<SoundIdx>(this->transformSound), 0);
        }
        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangesCompleted += 1;
        if (this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangesCompleted >=
            this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeRepeatCount)
        {
            this->activeTransformFlags &= ~BULLET_TRANSFORM_CHANGE_DIRECTION_RELATIVE;
        }
        this->angle +=
            this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeAngle;
        *reinterpret_cast<i32 *>(&this->speed) =
            *reinterpret_cast<i32 *>(
                &this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeSpeed);
        magnitude = this->speed;
        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].timer = 0;
    }
    else
    {
        magnitude = this->speed -
                    ((f32)this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].timer *
                     this->speed) /
                        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeIntervalFrames;
    }

    this->velocity.FromAngleMagnitude(this->angle,
                             magnitude * g_Supervisor.framerateMultiplier);
    this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].timer++;
}

// FUNCTION: th08 0x4325a0
#pragma var_order(magnitude, this)
void Bullet::UpdateAbsoluteDirectionChange()
{
    f32 magnitude;

    if (this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].timer >=
        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeIntervalFrames)
    {
        if (this->transformSound >= 0)
        {
            g_SoundPlayer.PlaySoundByIdx(static_cast<SoundIdx>(this->transformSound), 0);
        }
        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangesCompleted += 1;
        if (this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangesCompleted >=
            this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeRepeatCount)
        {
            this->activeTransformFlags &= ~BULLET_TRANSFORM_CHANGE_DIRECTION_ABSOLUTE;
        }
        *reinterpret_cast<i32 *>(&this->angle) =
            *reinterpret_cast<i32 *>(
                &this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeAngle);
        *reinterpret_cast<i32 *>(&this->speed) =
            *reinterpret_cast<i32 *>(
                &this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeSpeed);
        magnitude = this->speed;
        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].timer = 0;
    }
    else
    {
        magnitude = this->speed -
                    ((f32)this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].timer *
                     this->speed) /
                        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeIntervalFrames;
    }

    this->velocity.FromAngleMagnitude(this->angle,
                             magnitude * g_Supervisor.framerateMultiplier);
    this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].timer++;
}

// FUNCTION: th08 0x4326e0
#pragma var_order(magnitude, this)
void Bullet::UpdateAimedDirectionChange()
{
    f32 magnitude;

    if (this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].timer >=
        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeIntervalFrames)
    {
        if (this->transformSound >= 0)
        {
            g_SoundPlayer.PlaySoundByIdx(static_cast<SoundIdx>(this->transformSound), 0);
        }
        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangesCompleted += 1;
        if (this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangesCompleted >=
            this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeRepeatCount)
        {
            this->activeTransformFlags &= ~BULLET_TRANSFORM_CHANGE_DIRECTION_AIMED;
        }
        this->angle =
            AddNormalizeAngle(g_Player.AngleToPoint(&this->position),
                              this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeAngle);
        *reinterpret_cast<i32 *>(&this->speed) =
            *reinterpret_cast<i32 *>(
                &this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeSpeed);
        magnitude = this->speed;
        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].timer = 0;
    }
    else
    {
        magnitude = this->speed -
                    ((f32)this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].timer *
                     this->speed) /
                        this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].directionChangeIntervalFrames;
    }

    this->velocity.FromAngleMagnitude(this->angle,
                             magnitude * g_Supervisor.framerateMultiplier);
    this->exStates[BULLET_TRANSFORM_STATE_DIRECTION_CHANGE].timer++;
}


// FUNCTION: th08 0x432830
#pragma var_order(magnitude, this)
void Bullet::UpdateBoundaryBounce()
{
    f32 magnitude;

    if (!g_GameManager.IsWithinPlayfield(this->position.operator float *()[0],
                                         this->position.operator float *()[1],
                                         this->sprites.bulletVm.loadedSprite->widthPx,
                                         this->sprites.bulletVm.loadedSprite->heightPx))
    {
        if (this->transformSound >= 0)
        {
            g_SoundPlayer.PlaySoundByIdx(static_cast<SoundIdx>(this->transformSound), 0);
        }

        if (this->position.x < 0.0f || this->position.x >= 384.0f)
        {
            this->angle = -this->angle - ZUN_PI;
            this->angle = AddNormalizeAngle(this->angle, 0.0f);
        }

        if (this->position.y < 0.0f ||
            (this->position.y >= 448.0f &&
             (this->activeTransformFlags & BULLET_TRANSFORM_BOUNCE_ALL_EDGES) != 0))
        {
            this->angle = -this->angle;
        }

        *reinterpret_cast<i32 *>(&this->speed) =
            *reinterpret_cast<i32 *>(
                &this->exStates[BULLET_TRANSFORM_STATE_BOUNDARY_BOUNCE].bounceSpeed);
        magnitude = this->speed;
        this->velocity.FromAngleMagnitude(this->angle,
                                 magnitude * g_Supervisor.framerateMultiplier);
        this->exStates[BULLET_TRANSFORM_STATE_BOUNDARY_BOUNCE].bouncesCompleted += 1;
        if (this->exStates[BULLET_TRANSFORM_STATE_BOUNDARY_BOUNCE].bouncesCompleted >=
            this->exStates[BULLET_TRANSFORM_STATE_BOUNDARY_BOUNCE].bounceLimit)
        {
            this->activeTransformFlags &=
                ~(BULLET_TRANSFORM_BOUNCE_ALL_EDGES |
                  BULLET_TRANSFORM_BOUNCE_EXCEPT_BOTTOM);
        }
    }
}

// FUNCTION: th08 0x4329f0
void Bullet::UpdateHorizontalWrap()
{
    if (this->position.x < 0.0)
    {
        this->position.x += 384.0f;
    }
    else if (this->position.x > 384.0)
    {
        this->position.x -= 384.0f;
    }

    if (this->exStates[BULLET_TRANSFORM_STATE_WRAP].timer <= 0)
    {
        this->activeTransformFlags ^= BULLET_TRANSFORM_WRAP_X;
    }
    else
    {
        this->exStates[BULLET_TRANSFORM_STATE_WRAP].timer--;
    }
}

// FUNCTION: th08 0x432aa0
void Bullet::UpdateVerticalWrap()
{
    if (this->position.y < 0.0)
    {
        this->position.y += 448.0f;
    }
    else if (this->position.y > 448.0)
    {
        this->position.y -= 448.0f;
    }

    if (this->exStates[BULLET_TRANSFORM_STATE_WRAP].timer <= 0)
    {
        this->activeTransformFlags ^= BULLET_TRANSFORM_WRAP_Y;
    }
    else
    {
        this->exStates[BULLET_TRANSFORM_STATE_WRAP].timer--;
    }
}

// FUNCTION: th08 0x432b50
#pragma var_order(i, sine, laser, halfLength, cosine, node, bulletManager)
ChainCallbackResult BulletManager::OnDraw(BulletManager *bulletManager)
{
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
    th08::psp::PerfAttributionScope perfScope(
        th08::psp::PerfAttributionPhase::BulletDrawInclusive);
#endif
    i32 i;
    f32 sine;
    Laser *laser;
    f32 halfLength;
    f32 cosine;
    Bullet *node;

    if (g_GameManager.flags.deathbombFreezeActive)
        g_AnmManager->SetMixColor(0xfff01010);

    laser = bulletManager->lasers;
#if defined(PSP) && TH08_PSP_ITEM_MIXED_QUADS_ENABLED
    g_AnmManager->BeginPspItemMixedQuadBatch();
#elif defined(PSP) && defined(TH08_PSP_ITEM_DIRECT_GE) && \
    TH08_PSP_ITEM_DIRECT_GE
    g_AnmManager->BeginPspItemUnifiedQuadBatch();
#endif
    g_ItemManager.OnDraw();
#if defined(PSP) && TH08_PSP_ITEM_MIXED_QUADS_ENABLED
    g_AnmManager->EndPspItemMixedQuadBatch();
#elif defined(PSP) && defined(TH08_PSP_ITEM_DIRECT_GE) && \
    TH08_PSP_ITEM_DIRECT_GE
    g_AnmManager->EndPspItemUnifiedQuadBatch();
#endif

#if defined(TH08_PSP_STAGE_POOL_ARENA)
    for (i = 0; i < static_cast<i32>(th08::psp::kLaserPoolStorageCount); i++, laser++)
#else
    for (i = 0; i < ARRAY_SIZE_SIGNED(bulletManager->lasers); i++, laser++)
#endif
    {
        if (laser->inUse == 0)
            continue;

        fsincos(&sine, &cosine, laser->angle);
        halfLength = (laser->endOffset - laser->startOffset) / 2.0f + laser->startOffset;

        laser->bodyVm.pos.operator float *()[0] =
            laser->position.operator float *()[0] + cosine * halfLength;
        laser->bodyVm.pos.operator float *()[1] =
            laser->position.operator float *()[1] + sine * halfLength;
        laser->bodyVm.pos.operator float *()[2] = 0.06f;
        laser->color = (laser->color & 0xff000000) | 0xffffff;
        laser->bodyVm.pos.x += g_GameManager.arcadeRegionTopLeftPos.x;
        laser->bodyVm.pos.y += g_GameManager.arcadeRegionTopLeftPos.y;
        g_AnmManager->Draw2D(&laser->bodyVm);

        if (laser->startOffset < 16.0f || laser->speed == 0.0f)
        {
            if (!laser->hideCapDuringStartup || laser->state != LASER_STATE_STARTING)
            {
                laser->startCapVm.pos.operator float *()[0] =
                    laser->position.operator float *()[0] +
                    cosine * laser->startOffset;
                laser->startCapVm.pos.operator float *()[1] =
                    laser->position.operator float *()[1] +
                    sine * laser->startOffset;
                laser->startCapVm.pos.operator float *()[2] = 0.05f;
                laser->startCapVm.color1.d3dColor = laser->bodyVm.color1.d3dColor;
                *reinterpret_cast<u32 *>(&laser->startCapVm.flags) |= 0x40;
                laser->startCapVm.color1.d3dColor =
                    (laser->startCapVm.color1.d3dColor & 0xffffff) | 0xff000000;
                laser->startCapVm.scale.x =
                    laser->width / 10.0f * ((16.0f - laser->startOffset) / 16.0f);
                laser->startCapVm.scale.y = laser->startCapVm.scale.x;
                if (laser->startCapVm.scale.y <= 0.0f)
                {
                    laser->startCapVm.scale.x = laser->width / 10.0f;
                    laser->startCapVm.scale.y = laser->startCapVm.scale.x;
                }
                laser->startCapVm.pos.x += g_GameManager.arcadeRegionTopLeftPos.x;
                laser->startCapVm.pos.y += g_GameManager.arcadeRegionTopLeftPos.y;
                g_AnmManager->Draw2D(&laser->startCapVm);
            }
        }
    }

#if defined(PSP) && defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
    TH08_PSP_BULLET_UNIFIED_QUADS
    g_AnmManager->BeginPspBulletUnifiedQuadBatch();
#endif
    for (i = 0; i < 6; i++)
    {
        node = bulletManager->drawBuckets[i];
        while (node != NULL)
        {
            node->DrawSingleBullet();
            node = node->nextInDrawBucket;
        }
    }
#if defined(PSP) && defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
    TH08_PSP_BULLET_UNIFIED_QUADS
    g_AnmManager->EndPspBulletUnifiedQuadBatch();
#endif

    g_EffectManager.DrawBulletLayerEffects();
    if (g_GameManager.flags.deathbombFreezeActive)
        g_AnmManager->SetMixColorDefault();

    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

// FUNCTION: th08 0x432f20
ZunResult Bullet::DrawSingleBullet()
{
    AnmVm *vm;

    switch (this->state)
    {
#if defined(TH08_PSP_COMPACT_BULLET_VM)
    case BULLET_STATE_SPAWNING_FAST:
    case BULLET_STATE_SPAWNING_NORMAL:
    case BULLET_STATE_SPAWNING_SLOW:
        vm = &this->sprites.selectedSpawnVm;
        break;
#else
    case BULLET_STATE_SPAWNING_FAST:
        vm = &this->sprites.spawnFastVm;
        break;
    case BULLET_STATE_SPAWNING_NORMAL:
        vm = &this->sprites.spawnNormalVm;
        break;
    case BULLET_STATE_SPAWNING_SLOW:
        vm = &this->sprites.spawnSlowVm;
        break;
#endif
    case BULLET_STATE_DESPAWNING:
        vm = &this->sprites.despawnVm;
        break;
    default:
        vm = &this->sprites.bulletVm;
        break;
    }

    vm->pos.operator float *()[0] =
        g_GameManager.arcadeRegionTopLeftPos.x + this->position.operator float *()[0];
    vm->pos.operator float *()[1] =
        g_GameManager.arcadeRegionTopLeftPos.y + this->position.operator float *()[1];
    vm->pos.operator float *()[2] = 0.05f;
    vm->color1.d3dColor = (vm->color1.d3dColor & 0xff000000) | 0xffffff;

    if (vm->type != 0)
    {
#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
        PspBulletRenderRotationCache *cache =
            PspBulletRenderCacheFor(this);
        f32 renderAngle;
        f32 sine;
        f32 cosine;
        if (cache != NULL && cache->valid != 0U &&
            cache->sourceAngle == this->angle)
        {
            renderAngle = cache->renderAngle;
            sine = cache->sine;
            cosine = cache->cosine;
        }
        else
        {
            renderAngle = AddNormalizeAngle(
                ZUN_PI / 2.0f + this->angle, 0.0f);
            th08::psp::RenderSinCos(renderAngle, &sine, &cosine);
            if (cache != NULL)
            {
                cache->sourceAngle = this->angle;
                cache->renderAngle = renderAngle;
                cache->sine = sine;
                cache->cosine = cosine;
                cache->valid = 1U;
            }
        }
        // Preserve the original VM mutation on every draw.  Only the
        // presentation trig is memoized; VM/script/gameplay state is not.
        vm->SetZRotation(renderAngle);
#if defined(TH08_PSP_BULLET_ONEPASS_4V_AUDIT) && \
    TH08_PSP_BULLET_ONEPASS_4V_AUDIT
        if (this->state == BULLET_STATE_FIRED)
        {
            return g_AnmManager->DrawPspBulletOnePass4VAudit(
                vm, sine, cosine);
        }
#elif defined(TH08_PSP_BULLET_ONEPASS_4V_FASTPATH) && \
    TH08_PSP_BULLET_ONEPASS_4V_FASTPATH
        if (this->state == BULLET_STATE_FIRED)
        {
            ZunResult onePassResult;
            if (g_AnmManager->TryDrawPspBulletOnePass4V(
                    vm, sine, cosine, &onePassResult))
            {
                return onePassResult;
            }
        }
#endif
        return g_AnmManager->Draw2DWithPrecomputedRotation(vm, sine, cosine);
#else
        vm->SetZRotation(AddNormalizeAngle(
            ZUN_PI / 2.0f + this->angle, 0.0f));
#endif
    }

    return g_AnmManager->Draw2D(vm);
}

// FUNCTION: th08 0x433070
#pragma var_order(i)
ZunResult BulletManager::AddedCallback(BulletManager *bulletManager)
{
    u32 i;

    if (IsResourceReloadEnabled())
    {
        g_EffectManager.effectAnm = g_AnmManager->PreloadAnm(6, "etama.anm");
        bulletManager->bulletAnm = g_EffectManager.effectAnm;
        if (bulletManager->bulletAnm == NULL)
            return ZUN_ERROR;
    }
    else
    {
        bulletManager->bulletAnm = g_AnmManager->GetAnm(6);
    }

    for (i = 0; i < 21; i++)
    {
        bulletManager->bulletAnm->SetAndExecuteScriptIdx(&bulletManager->bulletTypeSprites[i].bulletVm, g_BulletSpriteScripts[i].scripts[0]);
        bulletManager->bulletAnm->SetAndExecuteScriptIdx(&bulletManager->bulletTypeSprites[i].spawnFastVm, g_BulletSpriteScripts[i].scripts[1]);
        bulletManager->bulletAnm->SetAndExecuteScriptIdx(&bulletManager->bulletTypeSprites[i].spawnNormalVm, g_BulletSpriteScripts[i].scripts[2]);
        bulletManager->bulletAnm->SetAndExecuteScriptIdx(&bulletManager->bulletTypeSprites[i].spawnSlowVm, g_BulletSpriteScripts[i].scripts[3]);
        bulletManager->bulletAnm->SetAndExecuteScriptIdx(&bulletManager->bulletTypeSprites[i].despawnVm, g_BulletSpriteScripts[i].scripts[4]);

        bulletManager->bulletTypeSprites[i].bulletVm.zWriteDisabled = TRUE;
        bulletManager->bulletTypeSprites[i].spawnFastVm.zWriteDisabled = TRUE;
        bulletManager->bulletTypeSprites[i].spawnNormalVm.zWriteDisabled = TRUE;
        bulletManager->bulletTypeSprites[i].spawnSlowVm.zWriteDisabled = TRUE;
        bulletManager->bulletTypeSprites[i].despawnVm.zWriteDisabled = TRUE;

        bulletManager->bulletTypeSprites[i].bulletVm.baseSpriteIndex =
            bulletManager->bulletTypeSprites[i].bulletVm.activeSpriteIndex;
        bulletManager->bulletTypeSprites[i].spriteHeightPx =
            (u8)bulletManager->bulletTypeSprites[i].bulletVm.loadedSprite->heightPx;

        if (bulletManager->bulletTypeSprites[i].bulletVm.loadedSprite->heightPx <= 8.0f)
        {
            bulletManager->bulletTypeSprites[i].collisionSize.x = 4.0f;
            bulletManager->bulletTypeSprites[i].collisionSize.y = 4.0f;
            bulletManager->bulletTypeSprites[i].drawBucketIndex = 5;
        }
        else if (bulletManager->bulletTypeSprites[i].bulletVm.loadedSprite->heightPx <= 16.0f)
        {
            switch (g_BulletSpriteScripts[i].scripts[0])
            {
            case 2:
            case 111:
            case 112:
                bulletManager->bulletTypeSprites[i].collisionSize.x = 4.0f;
                bulletManager->bulletTypeSprites[i].collisionSize.y = 4.0f;
                bulletManager->bulletTypeSprites[i].drawBucketIndex = 4;
                break;
            case 4:
            case 6:
                bulletManager->bulletTypeSprites[i].collisionSize.x = 4.0f;
                bulletManager->bulletTypeSprites[i].collisionSize.y = 4.0f;
                bulletManager->bulletTypeSprites[i].drawBucketIndex = 4;
                break;
            case 5:
                bulletManager->bulletTypeSprites[i].collisionSize.x = 4.0f;
                bulletManager->bulletTypeSprites[i].collisionSize.y = 4.0f;
                bulletManager->bulletTypeSprites[i].drawBucketIndex = 3;
            case 106:
                bulletManager->bulletTypeSprites[i].collisionSize.x = 4.0f;
                bulletManager->bulletTypeSprites[i].collisionSize.y = 4.0f;
                bulletManager->bulletTypeSprites[i].drawBucketIndex = 4;
                break;
            case 107:
            case 108:
                bulletManager->bulletTypeSprites[i].collisionSize.x = 4.0f;
                bulletManager->bulletTypeSprites[i].collisionSize.y = 4.0f;
                bulletManager->bulletTypeSprites[i].drawBucketIndex = 4;
                break;
            default:
                bulletManager->bulletTypeSprites[i].collisionSize.x = 6.0f;
                bulletManager->bulletTypeSprites[i].collisionSize.y = 6.0f;
                bulletManager->bulletTypeSprites[i].drawBucketIndex = 3;
                break;
            }
        }
        else if (bulletManager->bulletTypeSprites[i].bulletVm.loadedSprite->heightPx <= 32.0f)
        {
            switch (g_BulletSpriteScripts[i].scripts[0])
            {
            case 8:
            case 113:
            case 114:
            case 115:
                bulletManager->bulletTypeSprites[i].collisionSize.x = 5.0f;
                bulletManager->bulletTypeSprites[i].collisionSize.y = 5.0f;
                bulletManager->bulletTypeSprites[i].drawBucketIndex = 2;
                break;
            case 9:
            case 109:
            case 110:
                bulletManager->bulletTypeSprites[i].collisionSize.x = 8.0f;
                bulletManager->bulletTypeSprites[i].collisionSize.y = 8.0f;
                bulletManager->bulletTypeSprites[i].drawBucketIndex = 1;
                break;
            default:
                bulletManager->bulletTypeSprites[i].collisionSize.x = 10.0f;
                bulletManager->bulletTypeSprites[i].collisionSize.y = 10.0f;
                bulletManager->bulletTypeSprites[i].drawBucketIndex = 1;
                break;
            }
        }
        else
        {
            bulletManager->bulletTypeSprites[i].drawBucketIndex = 0;
            bulletManager->bulletTypeSprites[i].collisionSize.x = 24.0f;
            bulletManager->bulletTypeSprites[i].collisionSize.y = 24.0f;
        }
    }

    g_ItemManager.Initialize();
    return ZUN_SUCCESS;
}

// FUNCTION: th08 0x433820
ZunResult BulletManager::DeletedCallback(BulletManager *bulletManager)
{
    if (IsBulletManagerAnmReleaseRequired())
    {
        g_AnmManager->ReleaseAnm(6);
    }

#if defined(TH08_PSP_BULLET_RUNTIME_FASTPATH)
    g_PspBulletRuntimeActiveBitsValid = false;
    g_PspBulletRuntimeCache = NULL;
#endif

    return ZUN_SUCCESS;
}

// FUNCTION: th08 0x433850
void BulletManager::CutChain()
{
    g_Chain.Cut(&g_BulletManagerCalcChain);
    g_Chain.Cut(&g_BulletManagerDrawChain);
}

// FUNCTION: th08 0x4338b0
i32 IsResourceReloadEnabled()
{
    return g_Supervisor.isInitialStageLoad;
}

// FUNCTION: th08 0x4338c0
i32 IsBulletManagerAnmReleaseRequired()
{
    return g_Supervisor.releaseResourcesOnRestart;
}

} /* namespace th08 */
