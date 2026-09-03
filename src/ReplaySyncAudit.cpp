#include "th_pch.h"

#include "ReplaySyncAudit.hpp"

#if defined(TH08_REPLAY_SYNC_AUDIT)

#include "BulletManager.hpp"
#include "EclManager.hpp"
#include "EnemyManager.hpp"
#include "GameManager.hpp"
#include "Gui.hpp"
#include "ItemManager.hpp"
#include "Player.hpp"
#include "ReplayManager.hpp"
#include "Spellcard.hpp"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PSP)
#include "fileio.hpp"
#endif

namespace th08
{
namespace ReplaySyncAudit
{
namespace
{

// Wire format version 8.  Every integer in the header and records is emitted
// explicitly in little-endian order; no C++ object representation, pointer,
// padding byte, or whole-structure hash is ever observed.
const u32 kSchema = 8;
// TH07's correctness-profile rule is retained: one fixed 1 MiB RAM buffer,
// no per-frame Memory Stick I/O, and one checkpoint at a stage boundary.  A
// stage-local buffer holds 9,287 logical replay frames plus its begin/end
// records after reserving a fixed schema-8 diagnostic detail section.
// Overflow is an explicit trace failure, never a silent sample.
const size_t kBufferBytes = 1024U * 1024U;
const size_t kHeaderBytes = 8192U;
const size_t kRecordBytes = 112U;
const u32 kRecordCapacity = static_cast<u32>(
    (kBufferBytes - kHeaderBytes) / kRecordBytes);
const u32 kPeriodicFrameInterval = 1U;
const u32 kPeriodicOnlyFrameCapacity =
    kRecordCapacity * kPeriodicFrameInterval;
const u32 kSingleReplayLifecycleReserve =
    2U;
const u32 kSingleReplayFrameCapacity =
    (kRecordCapacity - kSingleReplayLifecycleReserve) *
    kPeriodicFrameInterval;
const u32 kKnownDemo3FrameCount = 7020U;
const u32 kKnownDemo2FrameCount = 6983U;
const u32 kLocalLaneCount = 6U;
const u32 kLocalLaneAlgorithm = 1U;
const size_t kLocalLaneRecordOffset = 72U;
// RSA05 reclaims RSA03's five cumulative digest pairs.  RSA04 comparisons
// already treated those pairs as diagnostic-only because presentation and
// process-local state poisoned them across platforms.  Ten fresh FNV lanes
// now split projectile/item state without growing the fixed 1 MiB trace.
const u32 kDiagnosticLaneCount = 10U;
const size_t kDiagnosticLaneRecordOffset = 32U;
const size_t kItemSpawnWitnessRecordOffset = 96U;
const size_t kItemSpawnWitnessBytes = 16U;
const size_t kIdentityTableOffset = 160U;
const size_t kIdentityEntryBytes = 32U;
const u32 kIdentityCapacity = 27U;
// RSA07+ keeps the per-frame record geometry unchanged.  The first frame with
// at least 64 SpawnItem calls is retained in a header-resident detail section,
// using the section itself as transient frame storage until that frame is
// found.  This observes raw arguments and already-selected results only.
const size_t kItemSpawnBurstDetailOffset = 1024U;
const size_t kItemSpawnBurstDetailHeaderBytes = 96U;
const size_t kItemSpawnBurstEntryOffset =
    kItemSpawnBurstDetailOffset + kItemSpawnBurstDetailHeaderBytes;
const size_t kItemSpawnBurstEntryBytes = 40U;
const u32 kItemSpawnBurstThreshold = 64U;
const u32 kItemSpawnBurstCapacity = 128U;
const u32 kNoReplayFrame = 0xffffffffU;
const u32 kFnvOffset = 2166136261U;
const u32 kFnvPrime = 16777619U;
const u32 kJenkinsOffset = 0x9e3779b9U;

typedef char AuditFloatMustBe32Bits[
    sizeof(f32) == sizeof(u32) ? 1 : -1];
typedef char AuditIdentityTableMustFitHeader[
    kIdentityTableOffset + kIdentityCapacity * kIdentityEntryBytes <=
            kItemSpawnBurstDetailOffset
        ? 1
        : -1];
typedef char AuditItemSpawnBurstMustFitHeader[
    kItemSpawnBurstEntryOffset +
                kItemSpawnBurstCapacity * kItemSpawnBurstEntryBytes <=
            kHeaderBytes
        ? 1
        : -1];
typedef char AuditKnownDemo3MustFit[
    kRecordCapacity >= kKnownDemo3FrameCount +
                           kSingleReplayLifecycleReserve
        ? 1
        : -1];
typedef char AuditKnownDemo2MustFit[
    kRecordCapacity >= kKnownDemo2FrameCount +
                           kSingleReplayLifecycleReserve
        ? 1
        : -1];
typedef char AuditLocalLanesMustFitRecord[
    kLocalLaneRecordOffset + kLocalLaneCount * sizeof(u32) ==
            kItemSpawnWitnessRecordOffset
        ? 1
        : -1];
typedef char AuditDiagnosticLanesMustFitPrefix[
    kDiagnosticLaneRecordOffset +
                kDiagnosticLaneCount * sizeof(u32) ==
            kLocalLaneRecordOffset
        ? 1
        : -1];
typedef char AuditItemSpawnWitnessMustFitRecord[
    kItemSpawnWitnessRecordOffset + kItemSpawnWitnessBytes == kRecordBytes
        ? 1
        : -1];

enum SnapshotType
{
    SNAPSHOT_STAGE_BEGIN = 1,
    SNAPSHOT_PERIODIC = 2,
    SNAPSHOT_STAGE_TERMINAL = 3
};

enum DigestDomain
{
    DIGEST_CORE = 1,
    DIGEST_PLAYER = 2,
    DIGEST_ENEMY = 3,
    DIGEST_PROJECTILE = 4,
    DIGEST_ITEM_EFFECT = 5
};

enum LocalDigestLane
{
    LOCAL_LANE_CORE_GAMEPLAY = 0,
    LOCAL_LANE_PLAYER_BODY = 1,
    LOCAL_LANE_PLAYER_OPTIONS = 2,
    LOCAL_LANE_PLAYER_OWNED = 3,
    LOCAL_LANE_ENEMY_ECL = 4,
    LOCAL_LANE_PROJECTILE_ITEMS = 5
};

enum DiagnosticDigestLane
{
    DIAGNOSTIC_LANE_PROJECTILE_MANAGER = 0,
    DIAGNOSTIC_LANE_BULLET_LIFECYCLE = 1,
    DIAGNOSTIC_LANE_BULLET_KINEMATICS = 2,
    DIAGNOSTIC_LANE_BULLET_TRANSFORMS = 3,
    DIAGNOSTIC_LANE_LASERS = 4,
    DIAGNOSTIC_LANE_ITEM_LIFECYCLE = 5,
    DIAGNOSTIC_LANE_ITEM_KINEMATICS = 6,
    DIAGNOSTIC_LANE_GRAZE_EFFECTS = 7,
    DIAGNOSTIC_LANE_BULLET_COUNTS = 8,
    DIAGNOSTIC_LANE_OBJECT_COUNTS = 9
};

enum AuditPointKind
{
    AUDIT_POINT_STAGE_BEGIN = 1,
    AUDIT_POINT_FRAME_END = 2,
    AUDIT_POINT_STAGE_TERMINAL = 3
};

enum ErrorFlags
{
    ERROR_FRAME_GAP = 1U << 0,
    ERROR_DUPLICATE_FRAME = 1U << 1,
    ERROR_RECORD_OVERFLOW = 1U << 2,
    ERROR_TERMINAL_PENDING = 1U << 3,
    ERROR_BEGIN_WHILE_PENDING = 1U << 4,
    ERROR_STAGE_BEGIN_WHILE_ACTIVE = 1U << 5,
    ERROR_SOURCE_UNAVAILABLE = 1U << 6,
    ERROR_SHUTDOWN_WHILE_ACTIVE = 1U << 7,
    ERROR_IDENTITY_TABLE_OVERFLOW = 1U << 8
};

enum SnapshotFlags
{
    SNAPSHOT_FLAG_PENDING_FRAME = 1U << 0
};

enum ItemSpawnBurstCaptureFlags
{
    ITEM_SPAWN_BURST_CAPTURED = 1U << 0,
    ITEM_SPAWN_BURST_ENTRY_OVERFLOW = 1U << 1,
    ITEM_SPAWN_BURST_REQUEST_OPEN_AT_FRAME_END = 1U << 2
};

struct RollingDigest
{
    u32 fnv1a;
    u32 jenkins;
    bool includeJenkins;
};

struct ReplayIdentity
{
    u32 magic;
    u16 version;
    u8 usesExtendedInputRecords;
    u8 hasUserDataSection;
    i32 fileSize;
    i32 checksum;
    i32 compressedSize;
    i32 decompressedSize;
    u8 minorVersion;
    u8 shotType;
    u8 difficulty;
    u8 isPractice;
    i16 spellcardNumber;
    u16 majorVersion;
};

struct PendingFrame
{
    bool valid;
    u32 replayFrame;
    u16 input;
    u8 inputRecordKind;
    u16 rngSeedBegin;
    u32 rngGenerationBegin;
};

struct ItemSpawnWitnessState
{
    u8 requestCount;
    u8 acceptedCount;
    u8 rejectXCount;
    u8 rejectTimeFirstSlotCount;
    u32 requestDigest;
    u32 firstRejectXBits;
    u32 lastResultMeta;
    u32 currentRequestXBits;
    bool requestOpen;
    bool counterOverflow;
};

struct ItemSpawnBurstCaptureState
{
    bool captured;
    u32 captureFlags;
    u32 frameFlags;
    u32 frameTotalRequestCount;
    u32 frameStoredRequestCount;
    i32 currentEntryIndex;
    u32 stage;
    u32 runOrdinal;
    u32 replayIdentityIndex;
    u32 replayFrame;
    u32 completedFrames;
    u32 input;
    u32 inputRecordKind;
    u32 rngSeedBegin;
    u32 rngSeedEnd;
    u32 rngGenerationBegin;
    u32 rngGenerationEnd;
    u32 capturedTotalRequestCount;
    u32 capturedStoredRequestCount;
};

u8 gBuffer[kBufferBytes];
char gOutputPath[512];
size_t gWriteOffset;
bool gInitialized;
bool gRuntimeEnabled;
bool gFlushed;
u32 gLastCheckpointRecordCount;
bool gStageActive;
bool gHavePreviousReplayFrame;
u8 gStage;
u8 gStageInputRecordKind;
u16 gRunOrdinal;
u16 gCurrentReplayIdentityIndex;
u32 gRunCount;
u32 gCompletedFrames;
u32 gPreviousReplayFrame;
u16 gLastInput;
u16 gLastRngSeedBegin;
u16 gLastRngSeedEnd;
u32 gLastRngGenerationBegin;
u32 gLastRngGenerationEnd;
// ReplayManager's physical-input repeat globals retain launcher history and
// therefore vary with asynchronous setup duration.  These audit-only fields
// reconstruct the identical recurrence solely from the replay input stream.
u16 gCanonicalPreviousInput;
u16 gCanonicalInputHeldFrames;
u16 gCanonicalInputEighthFrame;
// playtimeFrames includes launcher/setup ticks.  Hash its modulo-2^32 delta
// from StageBegin so the lane still detects skipped or extra gameplay ticks.
u32 gStagePlaytimeBaseline;
RollingDigest gCoreDigest;
RollingDigest gPlayerDigest;
RollingDigest gEnemyDigest;
RollingDigest gProjectileDigest;
RollingDigest gItemEffectDigest;
u32 gLocalLaneDigests[kLocalLaneCount];
u32 gDiagnosticLaneValues[kDiagnosticLaneCount];
PendingFrame gPending;
ItemSpawnWitnessState gItemSpawnWitness;
ItemSpawnBurstCaptureState gItemSpawnBurstCapture;
ReplayIdentity gReplayIdentities[kIdentityCapacity];
u32 gReplayIdentityCount;
u32 gReplayIdentityOverflowCount;

u32 gRecordCount;
u32 gErrorFlags;
u32 gEndWithoutPendingCount;
u32 gGapCount;
u32 gDuplicateCount;
u32 gOverflowCount;
u32 gTerminalPendingCount;
u32 gBeginWhilePendingCount;
u32 gStageBeginWhileActiveCount;
u32 gTerminalWithoutActiveCount;
u32 gStageBeginSnapshotCount;
u32 gPeriodicSnapshotCount;
u32 gTerminalSnapshotCount;
bool gStageCheckpointReady;
bool gStageCheckpointWritten;
char gArchivePath[512];

void Increment(u32 *value)
{
    if (*value != 0xffffffffU)
        ++*value;
}

void ResetCanonicalReplayInputState()
{
    gCanonicalPreviousInput = 0;
    gCanonicalInputHeldFrames = 0;
    gCanonicalInputEighthFrame = 0;
}

void AdvanceCanonicalReplayInputState(u16 input)
{
    gCanonicalInputEighthFrame = 0;
    if (gCanonicalPreviousInput == input)
    {
        if (gCanonicalInputHeldFrames >= 0x1eU)
        {
            if (gCanonicalInputHeldFrames % 8U == 0U)
                gCanonicalInputEighthFrame = 1;
            if (gCanonicalInputHeldFrames >= 0x26U)
                gCanonicalInputHeldFrames = 0x1eU;
        }
        ++gCanonicalInputHeldFrames;
    }
    else
    {
        gCanonicalInputHeldFrames = 0;
    }
    gCanonicalPreviousInput = input;
}

void PutU8(size_t offset, u8 value)
{
    gBuffer[offset] = value;
}

void PutU16(size_t offset, u16 value)
{
    gBuffer[offset + 0] = static_cast<u8>(value & 0xffU);
    gBuffer[offset + 1] = static_cast<u8>((value >> 8) & 0xffU);
}

void PutU32(size_t offset, u32 value)
{
    gBuffer[offset + 0] = static_cast<u8>(value & 0xffU);
    gBuffer[offset + 1] = static_cast<u8>((value >> 8) & 0xffU);
    gBuffer[offset + 2] = static_cast<u8>((value >> 16) & 0xffU);
    gBuffer[offset + 3] = static_cast<u8>((value >> 24) & 0xffU);
}

void ResetItemSpawnWitness()
{
    memset(&gItemSpawnWitness, 0, sizeof(gItemSpawnWitness));
    gItemSpawnWitness.firstRejectXBits = 0xffffffffU;
}

void IncrementItemSpawnCounter(u8 *counter)
{
    if (*counter == 0xffU)
        gItemSpawnWitness.counterOverflow = true;
    else
        ++*counter;
}

void RollItemSpawnWitnessU32(u32 value)
{
    for (u32 shift = 0; shift < 32U; shift += 8U)
    {
        gItemSpawnWitness.requestDigest ^=
            static_cast<u8>((value >> shift) & 0xffU);
        gItemSpawnWitness.requestDigest *= kFnvPrime;
    }
}

u32 PackItemSpawnCounts()
{
    return static_cast<u32>(gItemSpawnWitness.requestCount) |
           (static_cast<u32>(gItemSpawnWitness.acceptedCount) << 8) |
           (static_cast<u32>(gItemSpawnWitness.rejectXCount) << 16) |
           (static_cast<u32>(gItemSpawnWitness.rejectTimeFirstSlotCount)
            << 24);
}

void ResetItemSpawnBurstCapture()
{
    memset(&gItemSpawnBurstCapture, 0, sizeof(gItemSpawnBurstCapture));
    gItemSpawnBurstCapture.currentEntryIndex = -1;
    gItemSpawnBurstCapture.replayFrame = kNoReplayFrame;
}

void BeginItemSpawnBurstFrame()
{
    if (gItemSpawnBurstCapture.captured)
        return;
    gItemSpawnBurstCapture.frameFlags = 0;
    gItemSpawnBurstCapture.frameTotalRequestCount = 0;
    gItemSpawnBurstCapture.frameStoredRequestCount = 0;
    gItemSpawnBurstCapture.currentEntryIndex = -1;
}

size_t ItemSpawnBurstEntryBase(u32 index)
{
    return kItemSpawnBurstEntryOffset +
           static_cast<size_t>(index) * kItemSpawnBurstEntryBytes;
}

void RecordItemSpawnBurstRequest(const u32 positionBits[3], i32 itemType,
                                 i32 state, i32 nextIndex)
{
    if (gItemSpawnBurstCapture.captured)
        return;

    Increment(&gItemSpawnBurstCapture.frameTotalRequestCount);
    if (gItemSpawnBurstCapture.frameStoredRequestCount >=
        kItemSpawnBurstCapacity)
    {
        gItemSpawnBurstCapture.frameFlags |=
            ITEM_SPAWN_BURST_ENTRY_OVERFLOW;
        gItemSpawnBurstCapture.currentEntryIndex = -1;
        return;
    }

    const u32 index = gItemSpawnBurstCapture.frameStoredRequestCount;
    Increment(&gItemSpawnBurstCapture.frameStoredRequestCount);
    const size_t base = ItemSpawnBurstEntryBase(index);
    PutU32(base + 0, positionBits[0]);
    PutU32(base + 4, positionBits[1]);
    PutU32(base + 8, positionBits[2]);
    PutU32(base + 12, static_cast<u32>(itemType));
    PutU32(base + 16, static_cast<u32>(state));
    PutU32(base + 20, static_cast<u32>(nextIndex));
    PutU32(base + 24, 0U); // no result observed yet
    PutU32(base + 28, 0xffffffffU);
    PutU32(base + 32, 0xffffffffU);
    PutU32(base + 36, 0xffffffffU);
    gItemSpawnBurstCapture.currentEntryIndex = static_cast<i32>(index);
}

void RecordItemSpawnBurstResult(u32 outcome, i32 itemType, i32 state,
                                i32 nextIndex)
{
    if (gItemSpawnBurstCapture.captured ||
        gItemSpawnBurstCapture.currentEntryIndex < 0)
        return;

    const size_t base = ItemSpawnBurstEntryBase(
        static_cast<u32>(gItemSpawnBurstCapture.currentEntryIndex));
    PutU32(base + 24, outcome);
    PutU32(base + 28, static_cast<u32>(itemType));
    PutU32(base + 32, static_cast<u32>(state));
    PutU32(base + 36, static_cast<u32>(nextIndex));
    gItemSpawnBurstCapture.currentEntryIndex = -1;
}

void CaptureItemSpawnBurstIfEligible(u32 replayFrame, u32 completedFrames,
                                     u16 input, u8 inputRecordKind,
                                     u16 rngSeedBegin, u16 rngSeedEnd,
                                     u32 rngGenerationBegin,
                                     u32 rngGenerationEnd)
{
    if (gItemSpawnBurstCapture.captured ||
        gItemSpawnBurstCapture.frameTotalRequestCount <
            kItemSpawnBurstThreshold)
        return;

    gItemSpawnBurstCapture.captured = true;
    gItemSpawnBurstCapture.captureFlags =
        ITEM_SPAWN_BURST_CAPTURED | gItemSpawnBurstCapture.frameFlags;
    if (gItemSpawnWitness.requestOpen)
    {
        gItemSpawnBurstCapture.captureFlags |=
            ITEM_SPAWN_BURST_REQUEST_OPEN_AT_FRAME_END;
    }
    gItemSpawnBurstCapture.stage = gStage;
    gItemSpawnBurstCapture.runOrdinal = gRunOrdinal;
    gItemSpawnBurstCapture.replayIdentityIndex =
        gCurrentReplayIdentityIndex;
    gItemSpawnBurstCapture.replayFrame = replayFrame;
    gItemSpawnBurstCapture.completedFrames = completedFrames;
    gItemSpawnBurstCapture.input = input;
    gItemSpawnBurstCapture.inputRecordKind = inputRecordKind;
    gItemSpawnBurstCapture.rngSeedBegin = rngSeedBegin;
    gItemSpawnBurstCapture.rngSeedEnd = rngSeedEnd;
    gItemSpawnBurstCapture.rngGenerationBegin = rngGenerationBegin;
    gItemSpawnBurstCapture.rngGenerationEnd = rngGenerationEnd;
    gItemSpawnBurstCapture.capturedTotalRequestCount =
        gItemSpawnBurstCapture.frameTotalRequestCount;
    gItemSpawnBurstCapture.capturedStoredRequestCount =
        gItemSpawnBurstCapture.frameStoredRequestCount;
    gItemSpawnBurstCapture.currentEntryIndex = -1;
}

void RollU8(RollingDigest *digest, u8 value)
{
    digest->fnv1a ^= static_cast<u32>(value);
    digest->fnv1a *= kFnvPrime;

    // Jenkins one-at-a-time is deliberately paired with FNV-1a instead of a
    // second FNV seed.  The two 32-bit recurrences have different collision
    // structure, while avoiding emulated 64-bit multiplication on Allegrex.
    if (digest->includeJenkins)
    {
        digest->jenkins += static_cast<u32>(value);
        digest->jenkins += digest->jenkins << 10;
        digest->jenkins ^= digest->jenkins >> 6;
    }
}

void RollU16(RollingDigest *digest, u16 value)
{
    RollU8(digest, static_cast<u8>(value & 0xffU));
    RollU8(digest, static_cast<u8>((value >> 8) & 0xffU));
}

void RollU32(RollingDigest *digest, u32 value)
{
    RollU8(digest, static_cast<u8>(value & 0xffU));
    RollU8(digest, static_cast<u8>((value >> 8) & 0xffU));
    RollU8(digest, static_cast<u8>((value >> 16) & 0xffU));
    RollU8(digest, static_cast<u8>((value >> 24) & 0xffU));
}

void RollI32(RollingDigest *digest, i32 value)
{
    RollU32(digest, static_cast<u32>(value));
}

u32 FloatBits(f32 value)
{
    u32 bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void RollF32(RollingDigest *digest, f32 value)
{
    RollU32(digest, FloatBits(value));
}

[[maybe_unused]] u32 FinalizeJenkins(u32 digest)
{
    digest += digest << 3;
    digest ^= digest >> 11;
    digest += digest << 15;
    return digest;
}

void RollBytes(RollingDigest *digest, const u8 *bytes, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        RollU8(digest, bytes[i]);
}

void RollFloat2(RollingDigest *digest, const Float2 &value)
{
    RollF32(digest, value.x);
    RollF32(digest, value.y);
}

void RollFloat3(RollingDigest *digest, const Float3 &value)
{
    RollF32(digest, value.x);
    RollF32(digest, value.y);
    RollF32(digest, value.z);
}

void RollTimer(RollingDigest *digest, const ZunTimer &timer)
{
    RollI32(digest, timer.previous);
    RollF32(digest, timer.subFrame);
    RollI32(digest, timer.current);
}

u32 StableByteOffset(const void *base, const void *pointer)
{
    if (pointer == NULL)
        return 0xffffffffU;
    if (base == NULL)
        return 0xfffffffeU;
    const uintptr_t baseValue = reinterpret_cast<uintptr_t>(base);
    const uintptr_t pointerValue = reinterpret_cast<uintptr_t>(pointer);
    if (pointerValue < baseValue || pointerValue - baseValue > 0x0fffffffU)
        return 0xfffffffeU;
    return static_cast<u32>(pointerValue - baseValue);
}

i32 StableArrayIndex(const void *base, size_t stride, size_t count,
                     const void *pointer)
{
    if (pointer == NULL)
        return -1;
    if (base == NULL || stride == 0)
        return -2;
    const uintptr_t baseValue = reinterpret_cast<uintptr_t>(base);
    const uintptr_t pointerValue = reinterpret_cast<uintptr_t>(pointer);
    if (pointerValue < baseValue)
        return -2;
    const uintptr_t delta = pointerValue - baseValue;
    if (delta % stride != 0 || delta / stride >= count)
        return -2;
    return static_cast<i32>(delta / stride);
}

i32 EnemyIndex(const Enemy *enemy)
{
#if defined(TH08_PSP_STAGE_POOL_ARENA)
    if (g_EnemyManager.enemies == NULL)
        return enemy == NULL ? -1 : -2;
#endif
    return StableArrayIndex(g_EnemyManager.enemies, sizeof(Enemy), 481, enemy);
}

i32 BulletTypeIndex(const BulletTypeSprites *sprites)
{
    return StableArrayIndex(g_BulletManager.bulletTypeSprites,
                            sizeof(BulletTypeSprites), 0x20, sprites);
}

i32 BulletIndex(const Bullet *bullet)
{
#if defined(TH08_PSP_STAGE_POOL_ARENA)
    if (g_BulletManager.bullets == NULL)
        return bullet == NULL ? -1 : -2;
#endif
    return StableArrayIndex(g_BulletManager.bullets, sizeof(Bullet), 0x601,
                            bullet);
}

i32 LaserIndex(const Laser *laser)
{
#if defined(TH08_PSP_STAGE_POOL_ARENA)
    if (g_BulletManager.lasers == NULL)
        return laser == NULL ? -1 : -2;
#endif
    return StableArrayIndex(g_BulletManager.lasers, sizeof(Laser), 0x100,
                            laser);
}

i32 EffectIndex(const Effect *effect)
{
    return StableArrayIndex(g_EffectManager.effects, sizeof(Effect), 654,
                            effect);
}

i32 CollisionRegionIndex(const PlayerCollisionRegion *region,
                         const PlayerCollisionRegion *base)
{
    return StableArrayIndex(base, sizeof(PlayerCollisionRegion), 192, region);
}

i32 ItemLinkIndex(const Item *item)
{
    if (item == NULL)
        return -1;
    if (item == &g_ItemManager.itemListHead)
        return -3;
#if defined(TH08_PSP_STAGE_POOL_ARENA)
    if (g_ItemManager.items == NULL)
        return -2;
#endif
    return StableArrayIndex(g_ItemManager.items, sizeof(Item), MAX_ITEMS + 1,
                            item);
}

i32 EclCallbackIndex(EclExInstructionCallback callback)
{
    if (callback == NULL)
        return -1;
    for (i32 i = 0; i < 32; ++i)
    {
        if (g_EclExInsn[i] == callback)
            return i;
    }
    return -2;
}

void RollAnmVmLogical(RollingDigest *digest, const AnmVm &vm)
{
    // ANM interpreter/motion state can consume the global RNG and, for enemy
    // and player objects, feed collision/spawn logic.  Texture handles,
    // loadedSprite pointers, matrices, colors, blend/visibility and other
    // pure draw state are intentionally absent.
    RollFloat3(digest, vm.rotation);
    RollFloat3(digest, vm.angleVel);
    RollFloat2(digest, vm.scale);
    RollFloat2(digest, vm.scaleGrowth);
    RollFloat3(digest, vm.pos);
    RollFloat3(digest, vm.pos2);
    RollTimer(digest, vm.currentTimeInScript);
    RollTimer(digest, vm.waitTimer);
    for (i32 i = 0; i < AnmInterp_Last; ++i)
    {
        RollTimer(digest, vm.interpCurrentTimers[i]);
        RollTimer(digest, vm.interpEndTimers[i]);
        RollU8(digest, vm.interpModes[i]);
    }
    RollI32(digest, vm.intVar0);
    RollI32(digest, vm.intVar1);
    RollI32(digest, vm.intVar2);
    RollI32(digest, vm.intVar3);
    RollF32(digest, vm.floatVar0);
    RollF32(digest, vm.floatVar1);
    RollF32(digest, vm.floatVar2);
    RollF32(digest, vm.floatVar3);
    RollI32(digest, vm.counterVar0);
    RollI32(digest, vm.counterVar1);
    RollU32(digest, vm.flagsWord & ((1U << 14) | (1U << 17) | (1U << 19)));
    RollI32(digest, vm.type);
    RollI32(digest, vm.pendingInterrupt);
    RollI32(digest, vm.playerBulletHitAnimationType);
    RollI32(digest, vm.activeSpriteIndex);
    RollI32(digest, vm.anmFileIndex);
    RollI32(digest, vm.baseSpriteIndex);
    RollI32(digest, vm.scriptIndex);
    RollU32(digest, StableByteOffset(vm.beginningOfScript,
                                     vm.currentInstruction));
    RollTimer(digest, vm.interruptReturnTime);
    RollU32(digest, StableByteOffset(vm.beginningOfScript,
                                     vm.interruptReturnInstruction));
    RollFloat3(digest, vm.posInitial);
    RollFloat3(digest, vm.posFinal);
    RollFloat3(digest, vm.rotateInitial);
    RollFloat3(digest, vm.rotateFinal);
    RollFloat2(digest, vm.scaleInitial);
    RollFloat2(digest, vm.scaleFinal);
    RollI32(digest, vm.timeOfLastSpriteSet);
}

void RollBulletTransform(RollingDigest *digest,
                         const BulletTransformRecord &record)
{
    // The four payload words are serialized individually.  Their active union
    // interpretation is selected by kind; hashing the words is canonical and
    // retains every future transform operand without observing padding.
    RollF32(digest, record.payload.raw.float0);
    RollF32(digest, record.payload.raw.float1);
    RollI32(digest, record.payload.raw.int0);
    RollI32(digest, record.payload.raw.int1);
    RollU32(digest, record.kind);
    RollI32(digest, record.allowWhileActive);
}

void RollBulletSpawnDescriptor(RollingDigest *digest,
                               const BulletSpawnDescriptor &descriptor)
{
    RollI32(digest, descriptor.bulletType);
    RollI32(digest, descriptor.color);
    RollFloat3(digest, descriptor.position);
    RollF32(digest, descriptor.angle);
    RollF32(digest, descriptor.angleStep);
    RollF32(digest, descriptor.speed1);
    RollF32(digest, descriptor.speed2);
    for (i32 i = 0; i < 18; ++i)
        RollBulletTransform(digest, descriptor.transforms[i]);
    RollF32(digest, descriptor.laserStartOffset);
    RollF32(digest, descriptor.laserEndOffset);
    RollF32(digest, descriptor.laserStartLength);
    RollF32(digest, descriptor.laserWidth);
    RollI32(digest, descriptor.laserStartTime);
    RollI32(digest, descriptor.laserDuration);
    RollI32(digest, descriptor.laserDespawnDuration);
    RollI32(digest, descriptor.laserHitboxStartTime);
    RollI32(digest, descriptor.laserHitboxEndDelay);
    RollI32(digest, descriptor.count1);
    RollI32(digest, descriptor.count2);
    RollU16(digest, descriptor.aimMode);
    RollU16(digest, descriptor.unconsumedWord1FA);
    RollU32(digest, descriptor.transformFlags);
    RollI32(digest, descriptor.spawnSound);
    RollI32(digest, descriptor.transformSound);
    RollI32(digest, descriptor.transformStartIndex);
    RollI32(digest, BulletTypeIndex(descriptor.templateSprites));
}

RollingDigest BeginDigest(u8 domain)
{
    RollingDigest digest;
    digest.fnv1a = kFnvOffset;
    digest.jenkins = kJenkinsOffset;
    digest.includeJenkins = true;
    RollU32(&digest, kSchema);
    RollU8(&digest, domain);
    RollU8(&digest, gStage);
    return digest;
}

void ReadReplayIdentity(ReplayIdentity *identity)
{
    identity->magic = 0;
    identity->version = 0;
    identity->usesExtendedInputRecords = 0;
    identity->hasUserDataSection = 0;
    identity->fileSize = 0;
    identity->checksum = 0;
    identity->compressedSize = 0;
    identity->decompressedSize = 0;
    identity->minorVersion = 0;
    identity->shotType = 0;
    identity->difficulty = 0;
    identity->isPractice = 0;
    identity->spellcardNumber = 0;
    identity->majorVersion = 0;

    if (g_ReplayManager == NULL || g_ReplayManager->replayData == NULL)
    {
        gErrorFlags |= ERROR_SOURCE_UNAVAILABLE;
        return;
    }

    const ReplayData *replay = g_ReplayManager->replayData;
    const ReplayDataHeader *header = &replay->header;
    identity->magic = header->magic;
    identity->version = header->version;
    identity->usesExtendedInputRecords = header->usesExtendedInputRecords;
    identity->hasUserDataSection = header->hasUserDataSection;
    identity->fileSize = header->fileSize;
    identity->checksum = header->checksum;
    identity->compressedSize = header->compressedSize;
    identity->decompressedSize = header->decompressedSize;
    identity->minorVersion = replay->minorVersion;
    identity->shotType = replay->shotType;
    identity->difficulty = replay->difficulty;
    identity->isPractice = replay->isPractice;
    identity->spellcardNumber = replay->spellcardNumber;
    identity->majorVersion = replay->majorVersion;
}

bool ReplayIdentitiesEqual(const ReplayIdentity *left,
                           const ReplayIdentity *right)
{
    return left->magic == right->magic &&
           left->version == right->version &&
           left->usesExtendedInputRecords == right->usesExtendedInputRecords &&
           left->hasUserDataSection == right->hasUserDataSection &&
           left->fileSize == right->fileSize &&
           left->checksum == right->checksum &&
           left->compressedSize == right->compressedSize &&
           left->decompressedSize == right->decompressedSize &&
           left->minorVersion == right->minorVersion &&
           left->shotType == right->shotType &&
           left->difficulty == right->difficulty &&
           left->isPractice == right->isPractice &&
           left->spellcardNumber == right->spellcardNumber &&
           left->majorVersion == right->majorVersion;
}

void CopyReplayIdentity(ReplayIdentity *destination,
                        const ReplayIdentity *source)
{
    destination->magic = source->magic;
    destination->version = source->version;
    destination->usesExtendedInputRecords = source->usesExtendedInputRecords;
    destination->hasUserDataSection = source->hasUserDataSection;
    destination->fileSize = source->fileSize;
    destination->checksum = source->checksum;
    destination->compressedSize = source->compressedSize;
    destination->decompressedSize = source->decompressedSize;
    destination->minorVersion = source->minorVersion;
    destination->shotType = source->shotType;
    destination->difficulty = source->difficulty;
    destination->isPractice = source->isPractice;
    destination->spellcardNumber = source->spellcardNumber;
    destination->majorVersion = source->majorVersion;
}

u16 RegisterReplayIdentity(const ReplayIdentity *identity)
{
    for (u32 i = 0; i < gReplayIdentityCount; ++i)
    {
        if (ReplayIdentitiesEqual(&gReplayIdentities[i], identity))
            return static_cast<u16>(i);
    }

    if (gReplayIdentityCount >= kIdentityCapacity)
    {
        gErrorFlags |= ERROR_IDENTITY_TABLE_OVERFLOW;
        Increment(&gReplayIdentityOverflowCount);
        return 0xffffU;
    }

    const u32 index = gReplayIdentityCount;
    CopyReplayIdentity(&gReplayIdentities[index], identity);
    ++gReplayIdentityCount;
    return static_cast<u16>(index);
}

void RollReplayIdentity(const ReplayIdentity *identity)
{
    RollU32(&gCoreDigest, identity->magic);
    RollU16(&gCoreDigest, identity->version);
    RollU8(&gCoreDigest, identity->usesExtendedInputRecords);
    RollU8(&gCoreDigest, identity->hasUserDataSection);
    RollI32(&gCoreDigest, identity->fileSize);
    RollI32(&gCoreDigest, identity->checksum);
    RollI32(&gCoreDigest, identity->compressedSize);
    RollI32(&gCoreDigest, identity->decompressedSize);
    RollU8(&gCoreDigest, identity->minorVersion);
    RollU8(&gCoreDigest, identity->shotType);
    RollU8(&gCoreDigest, identity->difficulty);
    RollU8(&gCoreDigest, identity->isPractice);
    RollU16(&gCoreDigest, static_cast<u16>(identity->spellcardNumber));
    RollU16(&gCoreDigest, identity->majorVersion);
}

void RollSpellState(RollingDigest *digest)
{
    RollU32(digest, g_Spellcard.flags);
    RollI32(digest, EnemyIndex(g_Spellcard.activeEnemy));
    RollI32(digest, g_Spellcard.spellCardNumber);
    RollI32(digest, g_Spellcard.activeEnemyIndexSnapshot);
    RollI32(digest, g_Spellcard.pendingTimeOrbs);
    RollI32(digest, EffectIndex(g_Spellcard.spellEffect));
    RollI32(digest, EffectIndex(g_Spellcard.rewardEffect));
    RollI32(digest, g_Spellcard.bonusProgress);
    RollI32(digest, g_Spellcard.bonusCounter);
    RollI32(digest, g_Spellcard.bonusAward);
    RollTimer(digest, g_Spellcard.timeRemaining);
    RollTimer(digest, g_Spellcard.timeLimit);
    RollI32(digest, g_Spellcard.scoreLimit);
}

void RollGuiState(RollingDigest *digest)
{
    RollU32(digest, g_Gui.frameCounter);
    RollU32(digest, g_Gui.flags.lifeDisplayUpdateFrames);
    RollU32(digest, g_Gui.flags.bombDisplayUpdateFrames);
    RollU32(digest, g_Gui.flags.powerDisplayUpdateFrames);
    RollU32(digest, g_Gui.flags.grazeDisplayUpdateFrames);
    RollU32(digest, g_Gui.flags.pointDisplayUpdateFrames);
    RollU32(digest, g_Gui.flags.timeDisplayUpdateFrames);
    RollI32(digest, g_Gui.eclSetLives);
    RollI32(digest, g_Gui.spellcardSecondsRemaining);
    RollI32(digest, g_Gui.previousSpellcardSecondsRemaining);
    RollU8(digest, static_cast<u8>(g_Gui.bossPresent));
    RollF32(digest, g_Gui.bossLifeBarTargetSize);
    RollF32(digest, g_Gui.bossLifeBarDisplayedSize);

    if (g_Gui.impl == NULL)
    {
        RollU8(digest, 0);
        return;
    }

    RollU8(digest, 1);
    const GuiImpl &impl = *g_Gui.impl;
    RollI32(digest, impl.stageTransitionActiveVmCount);
    RollI32(digest, impl.stageClearScreenState);
    RollI32(digest, impl.stageClearBonusTotal);
    RollI32(digest, impl.stageClear.stageBonus);
    RollI32(digest, impl.stageClear.power);
    RollI32(digest, impl.stageClear.pointItemsCollected);
    RollI32(digest, impl.stageClear.graze);
    RollI32(digest, impl.stageClear.timeOrbs);
    RollI32(digest, impl.stageClear.clockIncrement);
    RollI32(digest, impl.stageClear.clockDisplayStart);
    RollI32(digest, impl.stageClear.clockDisplayTarget);
    RollI32(digest, impl.stageClear.clockDisplayCurrent);
    RollI32(digest, impl.stageClear.clockDisplayTimer);

    const GuiMsgVm &message = impl.message;
    RollU8(digest, message.msgFile != NULL ? 1 : 0);
    RollU32(digest, StableByteOffset(message.msgFile, message.currentInstr));
    RollI32(digest, message.currentMsgIdx);
    RollTimer(digest, message.timer);
    RollI32(digest, message.framesElapsedDuringPause);
    RollI32(digest, message.waitThreshold);
    RollU32(digest, message.ignoreWaitCounter);
    RollU8(digest, message.dialogueSkippable);
    RollU8(digest, message.textColorIndex);
    RollU8(digest, message.resetDialogueLines);
    RollU8(digest, message.dialogueLineIndex);
    RollU8(digest, message.currentPortraitIndex);
    RollU8(digest, message.textBoxVisible);
    RollU8(digest, message.selectedOption);
}

[[maybe_unused]] void RollCoreState(u8 auditPointKind, u32 replayFrame, u16 input,
                   u8 inputRecordKind,
                   u16 rngSeedBegin, u16 rngSeedEnd,
                   u32 rngGenerationBegin, u32 rngGenerationEnd)
{
    RollingDigest *digest = &gCoreDigest;
    // Replay/input and every RNG series are the first fields so an input/RNG
    // divergence is localized to CORE before scanning pool digests.
    RollU8(digest, auditPointKind);
    RollU8(digest, gStage);
    RollU32(digest, replayFrame);
    RollU8(digest, inputRecordKind);
    RollU16(digest, input);
    RollU16(digest, rngSeedBegin);
    RollU16(digest, rngSeedEnd);
    RollU16(digest, g_Rng.GetSavedSeed());
    RollU32(digest, rngGenerationBegin);
    RollU32(digest, rngGenerationEnd);
    RollU16(digest, g_CurFrameInput);
    RollU16(digest, g_LastFrameInput);
    RollU16(digest, g_NumOfFramesInputsWereHeld);
    RollU16(digest, g_IsEighthFrameOfHeldInput);
    RollU16(digest, g_GuiMessageInputCurrent);
    RollU16(digest, g_GuiMessageInputPrevious);

    if (g_ReplayManager == NULL)
    {
        gErrorFlags |= ERROR_SOURCE_UNAVAILABLE;
        for (i32 i = 0; i < 7; ++i)
            RollU32(digest, 0xffffffffU);
    }
    else
    {
        RollI32(digest, g_ReplayManager->frameCounter);
        RollI32(digest, g_ReplayManager->inputDelay);
        RollI32(digest, g_ReplayManager->isDemo);
        RollU16(digest, g_ReplayManager->stageResetWord);
        RollU16(digest, g_ReplayManager->frameRngSeed);
        RollU16(digest, g_ReplayManager->frameEventFlags);
        RollU8(digest, g_ReplayManager->replayData != NULL ? 1 : 0);
    }

    RollI32(digest, g_Supervisor.calcCount);
    RollF32(digest, g_Supervisor.framerateMultiplier);
    RollF32(digest, g_Supervisor.lagNumerator);
    RollF32(digest, g_Supervisor.lagDenominator);
    RollU8(digest, static_cast<u8>(g_Supervisor.flags.speedhackDetected));
    RollU8(digest, static_cast<u8>(g_Supervisor.flags.forceExtraTimerStep));

    RollI32(digest, g_GameManager.currentStage);
    RollI32(digest, g_GameManager.stageAtStart);
    RollU32(digest, g_GameManager.gameplayFrameCounter);
    RollU32(digest, g_GameManager.frameSkipCounter);
    RollU32(digest, g_GameManager.runActiveFrames);
    RollU32(digest, g_GameManager.stageActiveFrames);
    RollU32(digest, g_GameManager.runExtremeYoukaiFrames);
    RollU32(digest, g_GameManager.runExtremeHumanFrames);
    RollU32(digest, g_GameManager.stageExtremeYoukaiFrames);
    RollU32(digest, g_GameManager.stageExtremeHumanFrames);
    RollI32(digest, g_GameManager.stagePlayTimeAll);
    RollI32(digest, g_GameManager.playtimeFrames);
    RollI32(digest, g_GameManager.demoFrameCount);
    RollI32(digest, g_GameManager.scriptedUpdateFreeze);
    RollI32(digest, g_GameManager.skipCurrentFrame);
    RollU8(digest, static_cast<u8>(g_GameManager.flags.isPracticeMode));
    RollU8(digest, static_cast<u8>(g_GameManager.flags.isDemoMode));
    RollU8(digest, static_cast<u8>(g_GameManager.flags.replayInputEnabled));
    RollU8(digest, static_cast<u8>(g_GameManager.flags.isReplay));
    RollU8(digest, static_cast<u8>(g_GameManager.flags.gameCleared));
    RollU8(digest, static_cast<u8>(g_GameManager.flags.stageTransitionState));
    RollU8(digest, static_cast<u8>(g_GameManager.flags.playerDeathDissolveMode));
    RollU8(digest, static_cast<u8>(g_GameManager.flags.stageClearSequenceActive));
    RollU8(digest, static_cast<u8>(g_GameManager.flags.deathbombFreezeActive));
    RollU8(digest, static_cast<u8>(g_GameManager.flags.finalStageRoute));
    RollU8(digest, static_cast<u8>(g_GameManager.flags.suppressPlayerShots));
    RollU8(digest, static_cast<u8>(g_GameManager.flags.isSpellPractice));
    RollU8(digest, g_GameManager.replayMode);
    RollU16(digest, g_GameManager.currentStageClearFlag);
    RollU16(digest, g_GameManager.stageRngSeed);
    RollI32(digest, g_GameManager.rank);
    RollI32(digest, g_GameManager.maxRank);
    RollI32(digest, g_GameManager.minRank);
    RollI32(digest, g_GameManager.subRank);
    RollI32(digest, g_GameManager.difficulty);
    RollI32(digest, g_GameManager.difficultyMask);
    RollU8(digest, g_GameManager.shotType);
    RollU8(digest, g_GameManager.character);
    RollU16(digest, static_cast<u16>(g_GameManager.currentSpellCardNumber));
    RollI32(digest, g_GameManager.humanityRateNumerator);
    RollI32(digest, g_GameManager.humanityRateDenominator);

    if (g_GameManager.globals == NULL)
    {
        gErrorFlags |= ERROR_SOURCE_UNAVAILABLE;
        for (i32 i = 0; i < 64; ++i)
            RollU32(digest, 0xffffffffU);
    }
    else
    {
        const ZunGlobals &globals = *g_GameManager.globals;
        RollU32(digest, globals.displayScore);
        RollI32(digest, globals.grazeInStage);
        RollU32(digest, globals.score);
        RollI32(digest, globals.graze);
        RollI32(digest, globals.scoreDisplayStep);
        RollU32(digest, globals.displayedHighScore);
        RollU8(digest, globals.continuesUsedInHighScore);
        RollI32(digest, globals.spellcardsCaptured);
        RollU16(digest, static_cast<u16>(globals.youkaiGaugeCopy));
        RollU16(digest, static_cast<u16>(globals.youkaiGauge));
        RollI32(digest, globals.pointItemValue);
        RollU8(digest, globals.clockTime);
        RollU8(digest, globals.numRetries);
        RollI32(digest, globals.pointItemsCollectedInStage);
        RollI32(digest, globals.pointItemsCollected);
        RollU32(digest, globals.pointItemExtendsSoFar);
        RollI32(digest, globals.nextPointItemExtendThreshold);
        RollI32(digest, globals.currentTimeOrbs);
        RollI32(digest, globals.lastSpellTimeOrbThreshold);
        RollI32(digest, globals.totalTimeOrbs);
        for (i32 i = 0; i < 7; ++i)
            RollI32(digest, globals.rng1[i]);
        RollF32(digest, globals.deaths);
        RollF32(digest, globals.deathInStage);
        for (i32 i = 0; i < 2; ++i)
            RollF32(digest, globals.rng2[i]);
        RollF32(digest, globals.livesRemaining);
        for (i32 i = 0; i < 2; ++i)
            RollF32(digest, globals.rng3[i]);
        RollF32(digest, globals.bombsRemaining);
        RollF32(digest, globals.bombsUsed);
        RollF32(digest, globals.bombsUsedInStage);
        for (i32 i = 0; i < 3; ++i)
            RollF32(digest, globals.rng4[i]);
        RollF32(digest, globals.playerPower);
        for (i32 i = 0; i < 2; ++i)
            RollF32(digest, globals.rng5[i]);
        RollI32(digest, globals.rng6);
        for (i32 i = 0; i < 8; ++i)
            RollI32(digest, globals.rng7[i]);
        RollU32(digest, globals.antiTamperValue);
        RollI32(digest, globals.antiTamperChecksum);
        for (i32 i = 0; i < 5; ++i)
            RollI32(digest, globals.rng8[i]);
    }

    RollU16(digest, g_EnemyManager.enemyDropCounter);
    RollU16(digest, g_EnemyManager.enemyDropScheduleIndex);
    RollI32(digest, g_EnemyManager.activeEnemyCount);
    RollI32(digest, g_EnemyManager.opcode163Value);
    RollI32(digest, g_EnemyManager.lastSpawnFailed);
    RollI32(digest, g_EnemyManager.suppressTimelineSpawns);
    RollTimer(digest, g_EnemyManager.timer);
    for (i32 i = 0; i < 4; ++i)
        RollI32(digest, g_EnemyManager.timelineEventSlots[i]);
    for (i32 i = 0; i < 16; ++i)
    {
        RollTimer(digest, g_EnemyManager.timelines[i].timer);
        RollU32(digest, StableByteOffset(g_EclManager.eclFile,
                                         g_EnemyManager.timelines[i].instruction));
    }
    for (i32 i = 0; i < 8; ++i)
    {
        RollF32(digest, g_EclManager.timelineState.vectors[i].x);
        RollF32(digest, g_EclManager.timelineState.vectors[i].y);
        RollF32(digest, g_EclManager.timelineState.vectors[i].z);
    }

    RollI32(digest, g_BulletManager.activeBulletCount);
    RollI32(digest, g_BulletManager.spawnSuppressionFrames);
    RollTimer(digest, g_BulletManager.timer);
    RollI32(digest, g_BulletManager.frameCounter);
    RollI32(digest, g_BulletManager.cancelItemType);
    RollI32(digest, g_ItemManager.nextIndex);
    RollU32(digest, g_ItemManager.itemCount);
    RollI32(digest, ItemLinkIndex(g_ItemManager.itemListHead.next));
    RollI32(digest, ItemLinkIndex(g_ItemManager.itemListHead.prev));
    RollI32(digest, ItemLinkIndex(g_ItemManager.itemListTail));
    RollI32(digest, g_EffectManager.nextEffectIndex);
    RollI32(digest, g_EffectManager.activeCount);
    RollI32(digest, g_EffectManager.tamperCheckCounter);

    RollSpellState(digest);
    RollGuiState(digest);
}

void RollCollisionRegion(RollingDigest *digest,
                         const PlayerCollisionRegion &region)
{
    RollU8(digest, region.active);
    if (region.active == 0)
        return;
    RollFloat2(digest, region.center);
    RollF32(digest, region.radius);
    RollF32(digest, region.radiusGrowth);
    RollFloat2(digest, region.size);
    RollFloat2(digest, region.sizeGrowth);
    RollF32(digest, region.angle);
    RollI32(digest, region.lifetime);
    RollI32(digest, region.collisionValue);
    RollI32(digest, region.damage);
    RollI32(digest, region.hitAccumulator);
    RollI32(digest, region.hitCap);
    RollI32(digest, region.collisionInterval);
    RollU8(digest, region.mode);
}

void RollShtHeader(RollingDigest *digest, const PlayerRawShtFile *sht)
{
    RollU8(digest, sht != NULL ? 1 : 0);
    if (sht == NULL)
        return;
    RollU16(digest, sht->shotPowerLevelCount);
    RollF32(digest, sht->initialBombCount);
    RollI32(digest, sht->deathbombWindowFrames);
    RollF32(digest, sht->hurtboxSize);
    RollF32(digest, sht->grazeBoxSize);
    RollF32(digest, sht->itemAutoCollectSpeed);
    RollF32(digest, sht->itemCollectionBoxSize);
    RollF32(digest, sht->pointItemValueLine);
    RollF32(digest, sht->normalAxisSpeed);
    RollF32(digest, sht->focusedAxisSpeed);
    RollF32(digest, sht->normalDiagonalSpeed);
    RollF32(digest, sht->focusedDiagonalSpeed);
    RollF32(digest, sht->itemMovementSpeed);
}

[[maybe_unused]] void RollPlayerState(u8 auditPointKind)
{
    RollingDigest *digest = &gPlayerDigest;
    RollU8(digest, auditPointKind);
    RollU8(digest, static_cast<u8>(g_Player.playerState));
    RollU8(digest, g_Player.playerType);
    RollU8(digest, g_Player.focusMode);
    RollU8(digest, g_Player.deathbombPending);
    RollU8(digest, g_Player.isYoukai);
    RollU8(digest, g_Player.forceDeathbombAtWindowEnd);
    RollI32(digest, g_Player.focusTransitionFrames);
    RollAnmVmLogical(digest, g_Player.mainVm);
    RollFloat3(digest, g_Player.position);
    RollFloat3(digest, g_Player.position2);
    for (i32 i = 0; i < 16; ++i)
        RollFloat3(digest, g_Player.positionHistory[i]);
    RollFloat3(digest, g_Player.hurtboxBoundsMin);
    RollFloat3(digest, g_Player.hurtboxBoundsMax);
    RollFloat3(digest, g_Player.grazeBoundsMin);
    RollFloat3(digest, g_Player.grazeBoundsMax);
    RollFloat3(digest, g_Player.itemCollectionBoundsMin);
    RollFloat3(digest, g_Player.itemCollectionBoundsMax);
    RollFloat3(digest, g_Player.hurtboxHalfSize);
    RollFloat3(digest, g_Player.grazeHalfSize);
    RollFloat3(digest, g_Player.itemCollectionHalfSize);
    RollFloat3(digest, g_Player.velocity);
    RollF32(digest, g_Player.horizontalSpeedMultiplier);
    RollF32(digest, g_Player.verticalSpeedMultiplier);
    RollShtHeader(digest, g_Player.primaryShtFile);
    RollShtHeader(digest, g_Player.secondaryShtFile);

    for (i32 i = 0; i < 4; ++i)
    {
        const PlayerOptionState &option = g_Player.optionStates[i];
        RollI32(digest, option.lifecycleState);
        RollI32(digest, PlayerOptionCallbackStableId(option.updateCallback));
        RollI32(digest, PlayerOptionCallbackStableId(option.renderCallback));
        if (option.lifecycleState == PLAYER_OPTION_INACTIVE)
            continue;
        RollAnmVmLogical(digest, option.vm);
        RollFloat3(digest, option.position);
        RollFloat3(digest, option.target);
        RollFloat3(digest, option.velocity);
        RollI32(digest, option.behaviorState);
        RollI32(digest, option.optionIndex);
        RollF32(digest, option.orbitAngle);
        RollF32(digest, option.facingAngle);
        RollTimer(digest, option.timer);
    }

    const PlayerBombState &bomb = g_Player.bombState;
    RollI32(digest, bomb.isInUse);
    if (bomb.isInUse != 0)
    {
        RollI32(digest, bomb.callbackVariant);
        RollI32(digest, bomb.duration);
        RollI32(digest, bomb.bombsConsumed);
        RollI32(digest, bomb.secondaryWorkCursor);
        RollTimer(digest, bomb.timer);
        RollFloat3(digest, bomb.tailPosition);
        for (i32 i = 0; i < 128; ++i)
        {
            const PlayerBombWorkItem &work = bomb.workItems[i];
            RollI32(digest, work.state);
            if (work.state == PLAYER_BOMB_WORK_ITEM_INACTIVE)
                continue;
            RollI32(digest, work.stateTimer);
            RollF32(digest, work.motionStep);
            RollF32(digest, work.speed);
            RollF32(digest, work.angle);
            RollFloat3(digest, work.position);
            for (i32 point = 0; point < 32; ++point)
                RollFloat3(digest, work.pathPoints[point]);
            RollFloat3(digest, work.motion);
            RollFloat3(digest, work.auxiliaryMotion);
            for (i32 vm = 0; vm < 8; ++vm)
                RollAnmVmLogical(digest, work.vms[vm]);
            RollI32(digest, EffectIndex(work.effect));
            RollTimer(digest, work.timer);
            RollI32(digest, CollisionRegionIndex(work.damageRegion,
                                                 g_Player.damageRegions));
            RollI32(digest, CollisionRegionIndex(work.cancelRegion,
                                                 g_Player.cancelRegions));
        }
    }

    for (i32 i = 0; i < 192; ++i)
        RollCollisionRegion(digest, g_Player.damageRegions[i]);
    for (i32 i = 0; i < 192; ++i)
        RollCollisionRegion(digest, g_Player.cancelRegions[i]);

    for (i32 i = 0; i < 128; ++i)
    {
        const PlayerShot &shot = g_Player.shots[i];
        RollI32(digest, shot.state);
        if (shot.state == PLAYER_SHOT_INACTIVE)
            continue;
        RollAnmVmLogical(digest, shot.vm);
        RollFloat3(digest, shot.position);
        for (i32 history = 0; history < 32; ++history)
            RollFloat3(digest, shot.positionHistory[history]);
        RollFloat3(digest, shot.hitboxSize);
        RollF32(digest, shot.velocity.x);
        RollF32(digest, shot.velocity.y);
        RollF32(digest, shot.velocity.z);
        RollF32(digest, shot.auxiliaryValue);
        RollF32(digest, shot.speed);
        RollF32(digest, shot.angle);
        RollTimer(digest, shot.timer);
        RollI32(digest, shot.damage);
        RollI32(digest, shot.shotType);
        RollI32(digest, shot.timelineIndex);
        RollI32(digest, shot.sourceOptionIndex);
        RollI32(digest, shot.trailSegmentCount);
        RollU8(digest, shot.focusMode);
        RollI32(digest, shot.animationIndex);
        RollI32(digest, shot.tintInExtremeYoukai);
    }

    for (i32 i = 0; i < 3; ++i)
    {
        RollTimer(digest, g_Player.timelines[i].timer);
        RollU32(digest, StableByteOffset(g_EclManager.eclFile,
                                         g_Player.timelines[i].instruction));
    }
    RollI32(digest, g_Player.deathbombWindowFrames);
    RollI32(digest, g_Player.bombInputLockFrames);
    RollI32(digest, g_Player.playerStateSlotCooldown);
    RollI32(digest, g_Player.itemTimeOrbMode);
    RollI32(digest, g_Player.bulletCancelItemType);
    RollU8(digest, g_Player.shotHitEffectCounter);
    RollI32(digest, static_cast<i32>(g_Player.movementDirection));
    RollF32(digest, g_Player.currentHorizontalSpeed);
    RollF32(digest, g_Player.currentVerticalSpeed);
    RollFloat3(digest, g_Player.tailPosition0);
    RollFloat3(digest, g_Player.tailPosition1);
    RollI32(digest, EnemyIndex(g_Player.optionHomingTarget));
    RollI32(digest, g_Player.enemyTrackedPositionValid);
    RollTimer(digest, g_Player.shotTimer);
    RollTimer(digest, g_Player.gaugeShiftDelayTimer);
    RollTimer(digest, g_Player.timeOrbGaugeChangeSuppressionTimer);
    RollTimer(digest, g_Player.shootingGaugeChangeRampTimer);
    RollTimer(digest, g_Player.timer);
    RollTimer(digest, g_Player.timerE2B00);
    RollF32(digest, g_Player.baseShotAngle);
    RollI32(digest, EffectIndex(g_Player.focusEffect));
    RollI32(digest, EffectIndex(g_Player.stateEffect));
    RollI32(digest, EffectIndex(g_Player.extremeGaugeEffect));
    RollI32(digest, EffectIndex(g_Player.deathbombEffect));
    RollI32(digest, g_Player.damageAccumulatorThreshold);

    if (g_GameManager.globals == NULL)
    {
        gErrorFlags |= ERROR_SOURCE_UNAVAILABLE;
        RollU32(digest, 0xffffffffU);
        RollU32(digest, 0xffffffffU);
        RollU32(digest, 0xffffffffU);
    }
    else
    {
        RollF32(digest, g_GameManager.globals->livesRemaining);
        RollF32(digest, g_GameManager.globals->bombsRemaining);
        RollF32(digest, g_GameManager.globals->playerPower);
    }
}

void RollEclContext(RollingDigest *digest, const EnemyEclContext &context)
{
    RollU32(digest, StableByteOffset(g_EclManager.eclFile,
                                     context.currentInstr));
    RollTimer(digest, context.time);
    RollI32(digest, EclCallbackIndex(context.perFrameCallback));
    RollU32(digest, StableByteOffset(g_EclManager.eclFile,
                                     context.perFrameInstruction));
    for (i32 i = 0; i < 8; ++i)
        RollI32(digest, context.intVariables[i]);
    for (i32 i = 0; i < 8; ++i)
        RollF32(digest, context.floatVariables[i]);
    for (i32 i = 0; i < 4; ++i)
        RollI32(digest, context.extraIntVariables[i]);
    for (i32 i = 0; i < 2; ++i)
        RollF32(digest, context.extraFloatVariables[i]);
    for (i32 i = 0; i < 4; ++i)
        RollI32(digest, context.callParameterInts[i]);
    for (i32 i = 0; i < 4; ++i)
        RollF32(digest, context.callParameterFloats[i]);
    RollTimer(digest, context.secondaryTime);
    for (i32 i = 0; i < 8; ++i)
    {
        const EnemyEclInterpolationSlot &slot = context.interpolationSlots[i];
        RollU8(digest, slot.callback != NULL ? 1 : 0);
        RollTimer(digest, slot.timer);
        RollI32(digest, slot.duration);
        RollI32(digest, slot.callbackIndex);
        RollI32(digest, slot.easing);
        for (i32 parameter = 0; parameter < 4; ++parameter)
            RollF32(digest, slot.parameters[parameter]);
        RollF32(digest, slot.affectedVariable);
    }
    RollI32(digest, context.childContextSlot);
    RollI32(digest, context.subId);
}

[[maybe_unused]] void RollEnemyState(u8 auditPointKind)
{
    RollingDigest *digest = &gEnemyDigest;
    RollU8(digest, auditPointKind);
    RollI32(digest, g_EnemyManager.activeEnemyCount);
    for (i32 boss = 0; boss < 8; ++boss)
        RollI32(digest, EnemyIndex(g_EnemyManager.bosses[boss]));

#if defined(TH08_PSP_STAGE_POOL_ARENA)
    if (g_EnemyManager.enemies == NULL)
    {
        gErrorFlags |= ERROR_SOURCE_UNAVAILABLE;
        RollU32(digest, 0xffffffffU);
        return;
    }
#endif

    for (i32 index = 0; index < 481; ++index)
    {
        const Enemy &enemy = g_EnemyManager.enemies[index];
        const bool active = (enemy.flags1 & ENEMY_FLAG_ACTIVE) != 0;
        RollU8(digest, active ? 1 : 0);
        if (!active)
            continue;

        RollI32(digest, EnemyIndex(enemy.previousInAttachmentChain));
        RollI32(digest, EnemyIndex(enemy.nextInAttachmentChain));
        RollI32(digest, EnemyIndex(enemy.parentEnemy));
        RollAnmVmLogical(digest, enemy.vm);
        RollAnmVmLogical(digest, enemy.secondaryVms[0]);
        RollAnmVmLogical(digest, enemy.secondaryVms[1]);
        RollEclContext(digest, enemy.mainEclContextStorage);
        RollI32(digest, enemy.mainEclCallStackDepth);
        const i32 mainDepth = enemy.mainEclCallStackDepth < 0
                                  ? 0
                                  : (enemy.mainEclCallStackDepth > 16
                                         ? 16
                                         : enemy.mainEclCallStackDepth);
        for (i32 stack = 0; stack < mainDepth; ++stack)
            RollEclContext(digest, enemy.mainEclCallStackStorage[stack]);
        for (i32 i = 0; i < 8; ++i)
            RollI32(digest, enemy.eclIntVariables[i]);
        for (i32 i = 0; i < 8; ++i)
            RollF32(digest, enemy.eclFloatVariables[i]);
        RollI32(digest, enemy.deathCallbackSubId);
        for (i32 i = 0; i < 32; ++i)
            RollI32(digest, enemy.eclSubroutineIds[i]);
        RollI32(digest, enemy.pendingEclSubroutineIndex);
        RollFloat3(digest, enemy.position);
        RollFloat3(digest, enemy.positionOffset);
        RollFloat3(digest, enemy.velocity);
        RollFloat3(digest, enemy.previousPosition);
        RollFloat3(digest, enemy.lastFrameDisplacement);
        RollFloat3(digest, enemy.hitboxDimensions);
        RollFloat3(digest, enemy.secondaryHitboxDimensions);
        RollFloat3(digest, enemy.worldPosition);
        RollF32(digest, enemy.movementAngle);
        RollF32(digest, enemy.angularVelocity);
        RollF32(digest, enemy.orbitAngle);
        RollF32(digest, enemy.orbitAngularVelocity);
        RollF32(digest, enemy.speed);
        RollF32(digest, enemy.acceleration);
        RollF32(digest, enemy.orbitRadius);
        RollF32(digest, enemy.radialVelocity);
        RollFloat3(digest, enemy.shootOffset);
        RollFloat3(digest, enemy.movementInterpolationDelta);
        RollFloat3(digest, enemy.movementInterpolationOrigin);
        RollTimer(digest, enemy.movementTimer);
        RollI32(digest, enemy.movementDuration);
        RollF32(digest, enemy.bulletRankInfluence.speedLow);
        RollF32(digest, enemy.bulletRankInfluence.speedHigh);
        RollI32(digest, enemy.bulletRankInfluence.count1Low);
        RollI32(digest, enemy.bulletRankInfluence.count1High);
        RollI32(digest, enemy.bulletRankInfluence.count2Low);
        RollI32(digest, enemy.bulletRankInfluence.count2High);
        RollI32(digest, enemy.life);
        RollI32(digest, enemy.maxLife);
        RollI32(digest, enemy.phaseStartingLife);
        RollI32(digest, enemy.score);
        RollI32(digest, enemy.enemyIndex);
        RollI32(digest, enemy.playerShotHitAccumulator);
        RollTimer(digest, enemy.bossTimer);
        RollBulletSpawnDescriptor(digest, enemy.bulletSpawnDescriptor);
        RollBytes(digest, enemy.pendingShotInstruction,
                  sizeof(enemy.pendingShotInstruction));
        RollI32(digest, enemy.shootIntervalFrames);
        RollTimer(digest, enemy.shootIntervalTimer);
        RollBulletSpawnDescriptor(digest, enemy.laserSpawnDescriptor);
        for (i32 laser = 0; laser < 32; ++laser)
            RollI32(digest, LaserIndex(enemy.laserSlots[laser]));
        RollI32(digest, enemy.selectedLaserSlot);
        RollI32(digest, enemy.itemDropType);
        RollI32(digest, enemy.pointItemDropCount);
        RollI32(digest, enemy.powerOrPointItemDropCount);
        RollI32(digest, enemy.bossSlot);
        RollU8(digest, enemy.damageFlashTimer);
        RollTimer(digest, enemy.timer3318);
        RollU32(digest, enemy.flags1);
        RollU32(digest, enemy.flags2);
        RollU8(digest, enemy.anmDirection);
        RollU8(digest, enemy.drawGroup);
        RollU8(digest, enemy.eclDifficultyMaskOverride);
        RollI32(digest, enemy.anmScripts.idleInitial);
        RollI32(digest, enemy.anmScripts.idleFromLeft);
        RollI32(digest, enemy.anmScripts.idleFromRight);
        RollI32(digest, enemy.anmScripts.moveLeft);
        RollI32(digest, enemy.anmScripts.moveRight);
        RollI32(digest, enemy.anmScripts.special);
        RollFloat2(digest, enemy.movementBounds.lower);
        RollFloat2(digest, enemy.movementBounds.upper);
        RollF32(digest, enemy.minimumPlayerDistanceSquared);
        RollI32(digest, enemy.lastDamage);
        for (i32 i = 0; i < 4; ++i)
        {
            RollI32(digest, enemy.lifeCallbackThresholds[i]);
            RollI32(digest, enemy.lifeCallbackSubIds[i]);
        }
        RollI32(digest, enemy.timerCallbackThresholdFrames);
        RollI32(digest, enemy.timerCallbackSubId);
        RollI32(digest, enemy.linkedChildCount);
        for (i32 child = 0; child < 4; ++child)
        {
            const EnemyChildEclBlock *block = enemy.childEclBlocks[child];
            RollU8(digest, block != NULL ? 1 : 0);
            if (block == NULL)
                continue;
            RollI32(digest, block->subId);
            RollI32(digest, block->callStackDepth);
            RollEclContext(digest, block->eclContext);
            const i32 childDepth = block->callStackDepth < 0
                                       ? 0
                                       : (block->callStackDepth > 16
                                              ? 16
                                              : block->callStackDepth);
            for (i32 stack = 0; stack < childDepth; ++stack)
                RollEclContext(digest, block->callStack[stack]);
        }
        RollU8(digest, enemy.trailFlags);
        RollI32(digest, enemy.trailHistoryLength);
        RollI32(digest, enemy.trailCollisionLength);
        RollI32(digest, enemy.trailSampleStride);
        const i32 trailCount = enemy.trailHistoryLength < 0
                                   ? 0
                                   : (enemy.trailHistoryLength > 96
                                          ? 96
                                          : enemy.trailHistoryLength);
        for (i32 trail = 0; trail < trailCount; ++trail)
        {
            RollFloat3(digest, enemy.trailSamples[trail].position);
            RollFloat3(digest, enemy.trailSamples[trail].velocity);
            RollF32(digest, enemy.trailSamples[trail].angle);
        }
        RollTimer(digest, enemy.damageReductionTimer);
        RollI32(digest, enemy.attachedEffectCount);
        const i32 effectCount = enemy.attachedEffectCount < 0
                                    ? 0
                                    : (enemy.attachedEffectCount > 24
                                           ? 24
                                           : enemy.attachedEffectCount);
        for (i32 effect = 0; effect < effectCount; ++effect)
            RollI32(digest, EffectIndex(enemy.attachedEffects[effect]));
        RollF32(digest, enemy.attachedEffectDistance);
        RollI32(digest, EffectIndex(enemy.alignmentEffect));
        RollI32(digest, enemy.phaseEndTimeRemainingSeconds);
    }
}

[[maybe_unused]] void RollProjectileState(u8 auditPointKind)
{
    RollingDigest *digest = &gProjectileDigest;
    RollU8(digest, auditPointKind);
    RollI32(digest, g_BulletManager.activeBulletCount);
    RollI32(digest, BulletIndex(g_BulletManager.bulletCursor));
    for (i32 type = 0; type < 0x20; ++type)
    {
        const BulletTypeSprites &sprites = g_BulletManager.bulletTypeSprites[type];
        RollFloat3(digest, sprites.collisionSize);
        RollU8(digest, sprites.spriteHeightPx);
    }

#if defined(TH08_PSP_STAGE_POOL_ARENA)
    if (g_BulletManager.bullets == NULL || g_BulletManager.lasers == NULL)
    {
        gErrorFlags |= ERROR_SOURCE_UNAVAILABLE;
        RollU32(digest, 0xffffffffU);
        return;
    }
#endif

    for (i32 index = 0; index < 0x601; ++index)
    {
        const Bullet &bullet = g_BulletManager.bullets[index];
        RollU16(digest, bullet.state);
        if (bullet.state == BULLET_STATE_UNUSED ||
            bullet.state == BULLET_STATE_SENTINEL)
            continue;
        RollFloat3(digest, bullet.sprites.collisionSize);
        RollU8(digest, bullet.sprites.spriteHeightPx);
        RollFloat3(digest, bullet.position);
        RollFloat3(digest, bullet.velocity);
        RollF32(digest, bullet.speed);
        RollF32(digest, bullet.angle);
        RollTimer(digest, bullet.stateTimer);
        RollTimer(digest, bullet.activeTimer);
        RollI32(digest, bullet.offscreenCullDelayFrames);
        RollU32(digest, bullet.activeTransformFlags);
        RollU32(digest, bullet.transformFlags);
        RollI32(digest, bullet.color);
        RollU16(digest, bullet.offscreenFrames);
        RollU8(digest, bullet.isGrazed);
        RollU8(digest, bullet.cancelledDuringSpawn);
        RollI32(digest, bullet.zoneTransitionCooldownFrames);
        RollI32(digest, bullet.transformSound);
        RollI32(digest, bullet.transformIndex);
        for (i32 transform = 0; transform < 18; ++transform)
            RollBulletTransform(digest, bullet.transforms[transform]);
        for (i32 state = 0; state < 7; ++state)
        {
            const BulletExState &ex = bullet.exStates[state];
            RollTimer(digest, ex.timer);
            RollF32(digest, ex.float0);
            RollF32(digest, ex.float1);
            RollFloat3(digest, ex.vector);
            RollI32(digest, ex.int0);
            RollI32(digest, ex.int1);
            RollI32(digest, ex.int2);
        }
        RollI32(digest, bullet.collisionDisabled);
    }

    for (i32 index = 0; index < 0x100; ++index)
    {
        const Laser &laser = g_BulletManager.lasers[index];
        RollI32(digest, laser.inUse);
        if (laser.inUse == 0)
            continue;
        RollFloat3(digest, laser.position);
        RollF32(digest, laser.angle);
        RollF32(digest, laser.startOffset);
        RollF32(digest, laser.endOffset);
        RollF32(digest, laser.startLength);
        RollF32(digest, laser.width);
        RollF32(digest, laser.currentWidth);
        RollF32(digest, laser.speed);
        RollI32(digest, laser.startTime);
        RollI32(digest, laser.hitboxStartTime);
        RollI32(digest, laser.duration);
        RollI32(digest, laser.despawnDuration);
        RollI32(digest, laser.hitboxEndDelay);
        RollTimer(digest, laser.timer);
        RollU16(digest, laser.flags);
        RollI32(digest, laser.color);
        RollU8(digest, laser.state);
        RollU8(digest, laser.hideCapDuringStartup);
    }
}

[[maybe_unused]] void RollItemEffectState(u8 auditPointKind)
{
    RollingDigest *digest = &gItemEffectDigest;
    RollU8(digest, auditPointKind);
    RollU32(digest, g_ItemManager.itemCount);
#if defined(TH08_PSP_STAGE_POOL_ARENA)
    if (g_ItemManager.items == NULL)
    {
        gErrorFlags |= ERROR_SOURCE_UNAVAILABLE;
        RollU32(digest, 0xffffffffU);
    }
    else
#endif
    {
        for (i32 index = 0; index < MAX_ITEMS + 1; ++index)
        {
            const Item &item = g_ItemManager.items[index];
            RollU8(digest, static_cast<u8>(item.isInUse));
            if (item.isInUse == 0)
                continue;
            RollAnmVmLogical(digest, item.sprite);
            RollFloat3(digest, item.currentPosition);
            RollFloat3(digest, item.startPositionOrVelocity);
            RollFloat3(digest, item.targetPosition);
            RollTimer(digest, item.timer);
            RollI32(digest, item.itemType);
            RollI32(digest, item.isOnscreen);
            RollI32(digest, item.state);
            RollI32(digest, item.isMaxValue);
            RollI32(digest, ItemLinkIndex(item.next));
            RollI32(digest, ItemLinkIndex(item.prev));
        }
    }

    RollI32(digest, g_EffectManager.activeCount);
    for (i32 index = 0; index < 654; ++index)
    {
        const Effect &effect = g_EffectManager.effects[index];
        RollU8(digest, static_cast<u8>(effect.active));
        if (effect.active == 0)
            continue;
        RollAnmVmLogical(digest, effect.vm);
        RollFloat3(digest, effect.position);
        RollFloat3(digest, effect.vector1);
        RollFloat3(digest, effect.vector2);
        RollFloat3(digest, effect.vector3);
        RollFloat3(digest, effect.vector4);
        RollFloat3(digest, effect.vector5);
        RollFloat3(digest, effect.vector6);
        RollFloat3(digest, effect.vector7);
        RollFloat3(digest, effect.orientationAxis);
        RollF32(digest, effect.orientationW);
        RollF32(digest, effect.radius);
        RollF32(digest, effect.angle);
        RollF32(digest, effect.shapeThickness);
        RollI32(digest, effect.vertexSegmentCount);
        RollI32(digest, effect.slotIndex);
        RollF32(digest, effect.secondaryRadius);
        RollF32(digest, effect.secondaryAngle);
        RollF32(digest, effect.radialWaveCount);
        RollTimer(digest, effect.timer);
        RollU8(digest, effect.updateCallback != NULL ? 1 : 0);
        RollI32(digest, effect.effectId);
        RollI32(digest, effect.releaseRequested);
        RollI32(digest, effect.releaseTimer);
        RollI32(digest, effect.updateDuringFreeze);
    }
}

// RSA04's six local lanes are deliberately rebuilt from the FNV offset at
// every snapshot.  Shared canonical serializers skip their Jenkins recurrence
// for these lanes, so only the emitted fnv1a recurrence costs work.  Keeping
// these serializers field-based also makes the fresh lanes comparable between
// PC and PSP object layouts.
RollingDigest BeginLocalDigest(u8 lane, u8 auditPointKind)
{
    RollingDigest digest;
    digest.fnv1a = kFnvOffset;
    digest.jenkins = kJenkinsOffset;
    digest.includeJenkins = false;
    RollU32(&digest, kSchema);
    RollU8(&digest, static_cast<u8>(0x80U + lane));
    RollU8(&digest, gStage);
    RollU8(&digest, auditPointKind);
    return digest;
}

bool AntiTamperValueIsInRetailRange(i32 value)
{
    return value >= 6543 && value <= 106543;
}

bool AntiTamperValueIsInRetailRange(f32 value)
{
    // Preserve the retail comparison semantics: NaN trips neither bound.
    return !(value < 6543.0f || value > 106543.0f);
}

bool AntiTamperRangesAreValid(const ZunGlobals &globals)
{
    if (!AntiTamperValueIsInRetailRange(globals.rng6))
        return false;
    for (i32 i = 0; i < 7; ++i)
        if (!AntiTamperValueIsInRetailRange(globals.rng1[i]))
            return false;
    for (i32 i = 0; i < 2; ++i)
        if (!AntiTamperValueIsInRetailRange(globals.rng2[i]) ||
            !AntiTamperValueIsInRetailRange(globals.rng3[i]) ||
            !AntiTamperValueIsInRetailRange(globals.rng5[i]))
            return false;
    for (i32 i = 0; i < 3; ++i)
        if (!AntiTamperValueIsInRetailRange(globals.rng4[i]))
            return false;
    for (i32 i = 0; i < 8; ++i)
        if (!AntiTamperValueIsInRetailRange(globals.rng7[i]))
            return false;
    for (i32 i = 0; i < 5; ++i)
        if (!AntiTamperValueIsInRetailRange(globals.rng8[i]))
            return false;
    return true;
}

void RollLocalCoreGameplay(u8 auditPointKind, u32 replayFrame, u16 input,
                           u8 inputRecordKind, u16 rngSeedBegin,
                           u16 rngSeedEnd)
{
    RollingDigest digest = BeginLocalDigest(LOCAL_LANE_CORE_GAMEPLAY,
                                            auditPointKind);
    RollU32(&digest, replayFrame);
    RollU8(&digest, inputRecordKind);
    RollU16(&digest, input);
    RollU16(&digest, rngSeedBegin);
    RollU16(&digest, rngSeedEnd);
    RollU16(&digest, g_Rng.GetSavedSeed());

    // Input-repeat state is gameplay, but the process globals retain physical
    // launcher input before replay setup.  Hash the audit-local recurrence
    // driven only by the replay stream; gameplay globals are never rewritten.
    // Supervisor calc/lag and RNG generation counters remain intentionally
    // absent because they are presentation-scheduler state.
    RollU16(&digest, gCanonicalInputHeldFrames);
    RollU16(&digest, gCanonicalInputEighthFrame);
    RollU16(&digest, g_GuiMessageInputCurrent);
    RollU16(&digest, g_GuiMessageInputPrevious);
    if (g_ReplayManager == NULL)
    {
        RollU32(&digest, 0xffffffffU);
    }
    else
    {
        RollI32(&digest, g_ReplayManager->frameCounter);
        RollI32(&digest, g_ReplayManager->inputDelay);
        RollI32(&digest, g_ReplayManager->isDemo);
        RollU16(&digest, g_ReplayManager->stageResetWord);
        RollU16(&digest, g_ReplayManager->frameRngSeed);
        RollU16(&digest, g_ReplayManager->frameEventFlags);
    }

    // framerateMultiplier participates directly in movement/timers.  The
    // presentation scheduler's calcCount and lag numerator/denominator do not.
    RollF32(&digest, g_Supervisor.framerateMultiplier);
    RollU8(&digest,
           static_cast<u8>(g_Supervisor.flags.forceExtraTimerStep));

    RollI32(&digest, g_GameManager.currentStage);
    RollI32(&digest, g_GameManager.stageAtStart);
    RollU32(&digest, g_GameManager.gameplayFrameCounter);
    RollU32(&digest, g_GameManager.runActiveFrames);
    RollU32(&digest, g_GameManager.stageActiveFrames);
    RollU32(&digest, g_GameManager.runExtremeYoukaiFrames);
    RollU32(&digest, g_GameManager.runExtremeHumanFrames);
    RollU32(&digest, g_GameManager.stageExtremeYoukaiFrames);
    RollU32(&digest, g_GameManager.stageExtremeHumanFrames);
    RollI32(&digest, g_GameManager.stagePlayTimeAll);
    RollU32(&digest, static_cast<u32>(g_GameManager.playtimeFrames) -
                         gStagePlaytimeBaseline);
    RollI32(&digest, g_GameManager.demoFrameCount);
    RollI32(&digest, g_GameManager.scriptedUpdateFreeze);
    RollI32(&digest, g_GameManager.skipCurrentFrame);
    RollU8(&digest, static_cast<u8>(g_GameManager.flags.isPracticeMode));
    RollU8(&digest, static_cast<u8>(g_GameManager.flags.isDemoMode));
    RollU8(&digest, static_cast<u8>(g_GameManager.flags.replayInputEnabled));
    RollU8(&digest, static_cast<u8>(g_GameManager.flags.isReplay));
    RollU8(&digest, static_cast<u8>(g_GameManager.flags.gameCleared));
    RollU8(&digest,
           static_cast<u8>(g_GameManager.flags.stageTransitionState));
    RollU8(&digest,
           static_cast<u8>(g_GameManager.flags.playerDeathDissolveMode));
    RollU8(&digest,
           static_cast<u8>(g_GameManager.flags.stageClearSequenceActive));
    RollU8(&digest,
           static_cast<u8>(g_GameManager.flags.deathbombFreezeActive));
    RollU8(&digest, static_cast<u8>(g_GameManager.flags.finalStageRoute));
    RollU8(&digest,
           static_cast<u8>(g_GameManager.flags.suppressPlayerShots));
    RollU8(&digest, static_cast<u8>(g_GameManager.flags.isSpellPractice));
    RollU8(&digest, g_GameManager.replayMode);
    RollU16(&digest, g_GameManager.currentStageClearFlag);
    RollU16(&digest, g_GameManager.stageRngSeed);
    RollI32(&digest, g_GameManager.rank);
    RollI32(&digest, g_GameManager.maxRank);
    RollI32(&digest, g_GameManager.minRank);
    RollI32(&digest, g_GameManager.subRank);
    RollI32(&digest, g_GameManager.difficulty);
    RollI32(&digest, g_GameManager.difficultyMask);
    RollU8(&digest, g_GameManager.shotType);
    RollU8(&digest, g_GameManager.character);
    RollU16(&digest,
            static_cast<u16>(g_GameManager.currentSpellCardNumber));
    RollI32(&digest, g_GameManager.humanityRateNumerator);
    RollI32(&digest, g_GameManager.humanityRateDenominator);

    if (g_GameManager.globals == NULL)
    {
        RollU32(&digest, 0xffffffffU);
    }
    else
    {
        const ZunGlobals &globals = *g_GameManager.globals;
        // Display interpolation/high-score presentation is excluded.  These
        // are the score, resource, gauge and clock values consumed by play.
        RollI32(&digest, globals.grazeInStage);
        RollU32(&digest, globals.score);
        RollI32(&digest, globals.graze);
        RollU8(&digest, globals.continuesUsedInHighScore);
        RollI32(&digest, globals.spellcardsCaptured);
        RollU16(&digest, static_cast<u16>(globals.youkaiGaugeCopy));
        RollU16(&digest, static_cast<u16>(globals.youkaiGauge));
        RollI32(&digest, globals.pointItemValue);
        RollU8(&digest, globals.clockTime);
        RollU8(&digest, globals.numRetries);
        RollI32(&digest, globals.pointItemsCollectedInStage);
        RollI32(&digest, globals.pointItemsCollected);
        RollU32(&digest, globals.pointItemExtendsSoFar);
        RollI32(&digest, globals.nextPointItemExtendThreshold);
        RollI32(&digest, globals.currentTimeOrbs);
        RollI32(&digest, globals.lastSpellTimeOrbThreshold);
        RollI32(&digest, globals.totalTimeOrbs);
        RollF32(&digest, globals.deaths);
        RollF32(&digest, globals.deathInStage);
        RollF32(&digest, globals.livesRemaining);
        RollF32(&digest, globals.bombsRemaining);
        RollF32(&digest, globals.bombsUsed);
        RollF32(&digest, globals.bombsUsedInStage);
        RollF32(&digest, globals.playerPower);
        // The decoy payload is randomized before the replay seed is restored,
        // so exact values are process history rather than replay semantics.
        // Observe only the retail range and integrity branch predicates.
        // IsTampered is read-only; CalcAntiTamperChecksum is deliberately not
        // called because that routine mutates antiTamperValue while reading.
        RollU8(&digest, AntiTamperRangesAreValid(globals) ? 1U : 0U);
        RollU8(&digest, g_GameManager.IsTampered() ? 1U : 0U);
    }

    RollU32(&digest, g_Spellcard.flags);
    RollI32(&digest, EnemyIndex(g_Spellcard.activeEnemy));
    RollI32(&digest, g_Spellcard.spellCardNumber);
    RollI32(&digest, g_Spellcard.activeEnemyIndexSnapshot);
    RollI32(&digest, g_Spellcard.pendingTimeOrbs);
    RollI32(&digest, g_Spellcard.bonusProgress);
    RollI32(&digest, g_Spellcard.bonusCounter);
    RollI32(&digest, g_Spellcard.bonusAward);
    RollTimer(&digest, g_Spellcard.timeRemaining);
    RollTimer(&digest, g_Spellcard.timeLimit);
    RollI32(&digest, g_Spellcard.scoreLimit);

    RollI32(&digest, g_Gui.eclSetLives);
    RollI32(&digest, g_Gui.spellcardSecondsRemaining);
    RollI32(&digest, g_Gui.previousSpellcardSecondsRemaining);
    if (g_Gui.impl != NULL)
    {
        const GuiImpl &impl = *g_Gui.impl;
        RollI32(&digest, impl.stageClearScreenState);
        RollI32(&digest, impl.stageClearBonusTotal);
        RollI32(&digest, impl.stageClear.stageBonus);
        RollI32(&digest, impl.stageClear.power);
        RollI32(&digest, impl.stageClear.pointItemsCollected);
        RollI32(&digest, impl.stageClear.graze);
        RollI32(&digest, impl.stageClear.timeOrbs);
        RollI32(&digest, impl.stageClear.clockIncrement);
        const GuiMsgVm &message = impl.message;
        RollU32(&digest,
                StableByteOffset(message.msgFile, message.currentInstr));
        RollI32(&digest, message.currentMsgIdx);
        RollTimer(&digest, message.timer);
        RollI32(&digest, message.framesElapsedDuringPause);
        RollI32(&digest, message.waitThreshold);
        RollU32(&digest, message.ignoreWaitCounter);
        RollU8(&digest, message.dialogueSkippable);
        RollU8(&digest, message.selectedOption);
    }
    else
    {
        RollU32(&digest, 0xffffffffU);
    }

    gLocalLaneDigests[LOCAL_LANE_CORE_GAMEPLAY] = digest.fnv1a;
}

void RollLocalPlayerBody(u8 auditPointKind)
{
    RollingDigest digest = BeginLocalDigest(LOCAL_LANE_PLAYER_BODY,
                                            auditPointKind);
    RollU8(&digest, static_cast<u8>(g_Player.playerState));
    RollU8(&digest, g_Player.playerType);
    RollU8(&digest, g_Player.focusMode);
    RollU8(&digest, g_Player.deathbombPending);
    RollU8(&digest, g_Player.isYoukai);
    RollU8(&digest, g_Player.forceDeathbombAtWindowEnd);
    RollI32(&digest, g_Player.focusTransitionFrames);
    // mainVm and all Effect references are presentation state and excluded.
    RollFloat3(&digest, g_Player.position);
    RollFloat3(&digest, g_Player.position2);
    RollFloat3(&digest, g_Player.hurtboxBoundsMin);
    RollFloat3(&digest, g_Player.hurtboxBoundsMax);
    RollFloat3(&digest, g_Player.grazeBoundsMin);
    RollFloat3(&digest, g_Player.grazeBoundsMax);
    RollFloat3(&digest, g_Player.itemCollectionBoundsMin);
    RollFloat3(&digest, g_Player.itemCollectionBoundsMax);
    RollFloat3(&digest, g_Player.hurtboxHalfSize);
    RollFloat3(&digest, g_Player.grazeHalfSize);
    RollFloat3(&digest, g_Player.itemCollectionHalfSize);
    RollFloat3(&digest, g_Player.velocity);
    RollF32(&digest, g_Player.horizontalSpeedMultiplier);
    RollF32(&digest, g_Player.verticalSpeedMultiplier);
    RollF32(&digest, g_Player.currentHorizontalSpeed);
    RollF32(&digest, g_Player.currentVerticalSpeed);
    RollI32(&digest, static_cast<i32>(g_Player.movementDirection));
    RollShtHeader(&digest, g_Player.primaryShtFile);
    RollShtHeader(&digest, g_Player.secondaryShtFile);
    RollI32(&digest, g_Player.deathbombWindowFrames);
    RollI32(&digest, g_Player.bombInputLockFrames);
    RollI32(&digest, g_Player.playerStateSlotCooldown);
    RollI32(&digest, g_Player.itemTimeOrbMode);
    RollI32(&digest, g_Player.bulletCancelItemType);
    RollFloat3(&digest, g_Player.tailPosition0);
    RollFloat3(&digest, g_Player.tailPosition1);
    RollTimer(&digest, g_Player.shotTimer);
    RollTimer(&digest, g_Player.gaugeShiftDelayTimer);
    RollTimer(&digest, g_Player.timeOrbGaugeChangeSuppressionTimer);
    RollTimer(&digest, g_Player.shootingGaugeChangeRampTimer);
    RollTimer(&digest, g_Player.timer);
    RollTimer(&digest, g_Player.timerE2B00);
    RollI32(&digest, g_Player.damageAccumulatorThreshold);
    gLocalLaneDigests[LOCAL_LANE_PLAYER_BODY] = digest.fnv1a;
}

void RollLocalPlayerOptions(u8 auditPointKind)
{
    RollingDigest digest = BeginLocalDigest(LOCAL_LANE_PLAYER_OPTIONS,
                                            auditPointKind);
    RollU8(&digest, g_GameManager.shotType);
    for (i32 history = 0; history < 16; ++history)
        RollFloat3(&digest, g_Player.positionHistory[history]);
    for (i32 i = 0; i < 4; ++i)
    {
        const PlayerOptionState &option = g_Player.optionStates[i];
        RollI32(&digest, option.lifecycleState);
        RollI32(&digest,
                PlayerOptionCallbackStableId(option.updateCallback));
        RollI32(&digest,
                PlayerOptionCallbackStableId(option.renderCallback));
        if (option.lifecycleState == PLAYER_OPTION_INACTIVE)
            continue;
        // Option ANM is excluded; familiar motion and stable callback identity
        // are the gameplay state needed for team-2/team-3 focus diagnosis.
        RollFloat3(&digest, option.position);
        RollFloat3(&digest, option.target);
        RollFloat3(&digest, option.velocity);
        RollI32(&digest, option.behaviorState);
        RollI32(&digest, option.optionIndex);
        RollF32(&digest, option.orbitAngle);
        RollF32(&digest, option.facingAngle);
        RollTimer(&digest, option.timer);
    }
    RollI32(&digest, EnemyIndex(g_Player.optionHomingTarget));
    RollI32(&digest, g_Player.enemyTrackedPositionValid);
    gLocalLaneDigests[LOCAL_LANE_PLAYER_OPTIONS] = digest.fnv1a;
}

void RollLocalPlayerOwned(u8 auditPointKind)
{
    RollingDigest digest = BeginLocalDigest(LOCAL_LANE_PLAYER_OWNED,
                                            auditPointKind);
    const PlayerBombState &bomb = g_Player.bombState;
    RollI32(&digest, bomb.isInUse);
    if (bomb.isInUse != 0)
    {
        RollI32(&digest, bomb.callbackVariant);
        RollI32(&digest, bomb.duration);
        RollI32(&digest, bomb.bombsConsumed);
        RollI32(&digest, bomb.secondaryWorkCursor);
        RollTimer(&digest, bomb.timer);
        RollFloat3(&digest, bomb.tailPosition);
        for (i32 i = 0; i < 128; ++i)
        {
            const PlayerBombWorkItem &work = bomb.workItems[i];
            RollI32(&digest, work.state);
            if (work.state == PLAYER_BOMB_WORK_ITEM_INACTIVE)
                continue;
            RollI32(&digest, work.stateTimer);
            RollF32(&digest, work.motionStep);
            RollF32(&digest, work.speed);
            RollF32(&digest, work.angle);
            RollFloat3(&digest, work.position);
            for (i32 point = 0; point < 32; ++point)
                RollFloat3(&digest, work.pathPoints[point]);
            RollFloat3(&digest, work.motion);
            RollFloat3(&digest, work.auxiliaryMotion);
            // Bomb VMs/effects are presentation-only.  Pool references are
            // canonical indexes because their regions affect collision.
            RollI32(&digest, CollisionRegionIndex(work.damageRegion,
                                                  g_Player.damageRegions));
            RollI32(&digest, CollisionRegionIndex(work.cancelRegion,
                                                  g_Player.cancelRegions));
            RollTimer(&digest, work.timer);
        }
    }

    for (i32 i = 0; i < 192; ++i)
        RollCollisionRegion(&digest, g_Player.damageRegions[i]);
    for (i32 i = 0; i < 192; ++i)
        RollCollisionRegion(&digest, g_Player.cancelRegions[i]);

    for (i32 i = 0; i < 128; ++i)
    {
        const PlayerShot &shot = g_Player.shots[i];
        RollI32(&digest, shot.state);
        if (shot.state == PLAYER_SHOT_INACTIVE)
            continue;
        // Shot ANM/callback pointers/descriptor pointers are excluded.
        RollFloat3(&digest, shot.position);
        for (i32 history = 0; history < 32; ++history)
            RollFloat3(&digest, shot.positionHistory[history]);
        RollFloat3(&digest, shot.hitboxSize);
        RollF32(&digest, shot.velocity.x);
        RollF32(&digest, shot.velocity.y);
        RollF32(&digest, shot.velocity.z);
        RollF32(&digest, shot.auxiliaryValue);
        RollF32(&digest, shot.speed);
        RollF32(&digest, shot.angle);
        RollTimer(&digest, shot.timer);
        RollI32(&digest, shot.damage);
        RollI32(&digest, shot.shotType);
        RollI32(&digest, shot.timelineIndex);
        RollI32(&digest, shot.sourceOptionIndex);
        RollI32(&digest, shot.trailSegmentCount);
        RollU8(&digest, shot.focusMode);
    }
    RollF32(&digest, g_Player.baseShotAngle);
    gLocalLaneDigests[LOCAL_LANE_PLAYER_OWNED] = digest.fnv1a;
}

void RollLocalBulletTransform(RollingDigest *digest,
                              const BulletTransformRecord &record)
{
    RollF32(digest, record.payload.raw.float0);
    RollF32(digest, record.payload.raw.float1);
    RollI32(digest, record.payload.raw.int0);
    RollI32(digest, record.payload.raw.int1);
    RollU32(digest, record.kind);
    RollI32(digest, record.allowWhileActive);
}

void RollLocalBulletSpawnDescriptor(
    RollingDigest *digest, const BulletSpawnDescriptor &descriptor)
{
    RollI32(digest, descriptor.bulletType);
    // Color, sound and sprite pointers are presentation state.
    RollFloat3(digest, descriptor.position);
    RollF32(digest, descriptor.angle);
    RollF32(digest, descriptor.angleStep);
    RollF32(digest, descriptor.speed1);
    RollF32(digest, descriptor.speed2);
    for (i32 i = 0; i < 18; ++i)
        RollLocalBulletTransform(digest, descriptor.transforms[i]);
    RollF32(digest, descriptor.laserStartOffset);
    RollF32(digest, descriptor.laserEndOffset);
    RollF32(digest, descriptor.laserStartLength);
    RollF32(digest, descriptor.laserWidth);
    RollI32(digest, descriptor.laserStartTime);
    RollI32(digest, descriptor.laserDuration);
    RollI32(digest, descriptor.laserDespawnDuration);
    RollI32(digest, descriptor.laserHitboxStartTime);
    RollI32(digest, descriptor.laserHitboxEndDelay);
    RollI32(digest, descriptor.count1);
    RollI32(digest, descriptor.count2);
    RollU16(digest, descriptor.aimMode);
    RollU16(digest, descriptor.unconsumedWord1FA);
    RollU32(digest, descriptor.transformFlags);
    RollI32(digest, descriptor.transformStartIndex);
}

void RollLocalEnemyEcl(u8 auditPointKind)
{
    RollingDigest digest = BeginLocalDigest(LOCAL_LANE_ENEMY_ECL,
                                            auditPointKind);
    RollI32(&digest, g_EnemyManager.activeEnemyCount);
    RollU16(&digest, g_EnemyManager.enemyDropCounter);
    RollU16(&digest, g_EnemyManager.enemyDropScheduleIndex);
    RollI32(&digest, g_EnemyManager.opcode163Value);
    RollI32(&digest, g_EnemyManager.lastSpawnFailed);
    RollI32(&digest, g_EnemyManager.suppressTimelineSpawns);
    RollTimer(&digest, g_EnemyManager.timer);
    for (i32 boss = 0; boss < 8; ++boss)
        RollI32(&digest, EnemyIndex(g_EnemyManager.bosses[boss]));
    for (i32 i = 0; i < 4; ++i)
        RollI32(&digest, g_EnemyManager.timelineEventSlots[i]);
    for (i32 i = 0; i < 16; ++i)
    {
        RollTimer(&digest, g_EnemyManager.timelines[i].timer);
        RollU32(&digest, StableByteOffset(
                             g_EclManager.eclFile,
                             g_EnemyManager.timelines[i].instruction));
    }
    for (i32 i = 0; i < 8; ++i)
    {
        RollF32(&digest, g_EclManager.timelineState.vectors[i].x);
        RollF32(&digest, g_EclManager.timelineState.vectors[i].y);
        RollF32(&digest, g_EclManager.timelineState.vectors[i].z);
    }

#if defined(TH08_PSP_STAGE_POOL_ARENA)
    if (g_EnemyManager.enemies == NULL)
    {
        RollU32(&digest, 0xffffffffU);
        gLocalLaneDigests[LOCAL_LANE_ENEMY_ECL] = digest.fnv1a;
        return;
    }
#endif

    const u32 presentationFlags1 =
        (1U << 4) | (1U << 5) | (1U << 10) | (1U << 25);
    const u32 presentationFlags2 =
        (1U << 1) | (1U << 2) | (3U << 4) | (1U << 8);
    for (i32 index = 0; index < 481; ++index)
    {
        const Enemy &enemy = g_EnemyManager.enemies[index];
        const bool active = (enemy.flags1 & ENEMY_FLAG_ACTIVE) != 0;
        RollU8(&digest, active ? 1 : 0);
        if (!active)
            continue;

        RollI32(&digest, EnemyIndex(enemy.previousInAttachmentChain));
        RollI32(&digest, EnemyIndex(enemy.nextInAttachmentChain));
        RollI32(&digest, EnemyIndex(enemy.parentEnemy));
        // All primary/secondary ANM VMs are excluded from this lane.
        RollEclContext(&digest, enemy.mainEclContextStorage);
        RollI32(&digest, enemy.mainEclCallStackDepth);
        RollI32(&digest, enemy.activeEclCallStackDepth);
        const i32 mainDepth = enemy.mainEclCallStackDepth < 0
                                  ? 0
                                  : (enemy.mainEclCallStackDepth > 16
                                         ? 16
                                         : enemy.mainEclCallStackDepth);
        for (i32 stack = 0; stack < mainDepth; ++stack)
            RollEclContext(&digest,
                           enemy.mainEclCallStackStorage[stack]);
        for (i32 i = 0; i < 8; ++i)
            RollI32(&digest, enemy.eclIntVariables[i]);
        for (i32 i = 0; i < 8; ++i)
            RollF32(&digest, enemy.eclFloatVariables[i]);
        RollI32(&digest, enemy.deathCallbackSubId);
        for (i32 i = 0; i < 32; ++i)
            RollI32(&digest, enemy.eclSubroutineIds[i]);
        RollI32(&digest, enemy.pendingEclSubroutineIndex);
        RollFloat3(&digest, enemy.position);
        RollFloat3(&digest, enemy.positionOffset);
        RollFloat3(&digest, enemy.velocity);
        RollFloat3(&digest, enemy.previousPosition);
        RollFloat3(&digest, enemy.lastFrameDisplacement);
        RollFloat3(&digest, enemy.hitboxDimensions);
        RollFloat3(&digest, enemy.secondaryHitboxDimensions);
        RollFloat3(&digest, enemy.worldPosition);
        RollF32(&digest, enemy.movementAngle);
        RollF32(&digest, enemy.angularVelocity);
        RollF32(&digest, enemy.orbitAngle);
        RollF32(&digest, enemy.orbitAngularVelocity);
        RollF32(&digest, enemy.speed);
        RollF32(&digest, enemy.acceleration);
        RollF32(&digest, enemy.orbitRadius);
        RollF32(&digest, enemy.radialVelocity);
        RollFloat3(&digest, enemy.shootOffset);
        RollFloat3(&digest, enemy.movementInterpolationDelta);
        RollFloat3(&digest, enemy.movementInterpolationOrigin);
        RollTimer(&digest, enemy.movementTimer);
        RollI32(&digest, enemy.movementDuration);
        RollF32(&digest, enemy.bulletRankInfluence.speedLow);
        RollF32(&digest, enemy.bulletRankInfluence.speedHigh);
        RollI32(&digest, enemy.bulletRankInfluence.count1Low);
        RollI32(&digest, enemy.bulletRankInfluence.count1High);
        RollI32(&digest, enemy.bulletRankInfluence.count2Low);
        RollI32(&digest, enemy.bulletRankInfluence.count2High);
        RollI32(&digest, enemy.life);
        RollI32(&digest, enemy.maxLife);
        RollI32(&digest, enemy.phaseStartingLife);
        RollI32(&digest, enemy.score);
        RollI32(&digest, enemy.enemyIndex);
        RollI32(&digest, enemy.playerShotHitAccumulator);
        RollTimer(&digest, enemy.bossTimer);
        RollLocalBulletSpawnDescriptor(&digest,
                                       enemy.bulletSpawnDescriptor);
        RollBytes(&digest, enemy.pendingShotInstruction,
                  sizeof(enemy.pendingShotInstruction));
        RollI32(&digest, enemy.shootIntervalFrames);
        RollTimer(&digest, enemy.shootIntervalTimer);
        RollLocalBulletSpawnDescriptor(&digest,
                                       enemy.laserSpawnDescriptor);
        for (i32 laser = 0; laser < 32; ++laser)
            RollI32(&digest, LaserIndex(enemy.laserSlots[laser]));
        RollI32(&digest, enemy.selectedLaserSlot);
        RollI32(&digest, enemy.itemDropType);
        RollI32(&digest, enemy.pointItemDropCount);
        RollI32(&digest, enemy.powerOrPointItemDropCount);
        RollI32(&digest, enemy.bossSlot);
        RollTimer(&digest, enemy.timer3318);
        RollU32(&digest, enemy.flags1 & ~presentationFlags1);
        RollU32(&digest, enemy.flags2 & ~presentationFlags2);
        RollU8(&digest, enemy.eclDifficultyMaskOverride);
        RollFloat2(&digest, enemy.movementBounds.lower);
        RollFloat2(&digest, enemy.movementBounds.upper);
        RollF32(&digest, enemy.minimumPlayerDistanceSquared);
        RollI32(&digest, enemy.lastDamage);
        for (i32 i = 0; i < 4; ++i)
        {
            RollI32(&digest, enemy.lifeCallbackThresholds[i]);
            RollI32(&digest, enemy.lifeCallbackSubIds[i]);
        }
        RollI32(&digest, enemy.timerCallbackThresholdFrames);
        RollI32(&digest, enemy.timerCallbackSubId);
        RollI32(&digest, enemy.linkedChildCount);
        for (i32 child = 0; child < 4; ++child)
        {
            const EnemyChildEclBlock *block = enemy.childEclBlocks[child];
            RollU8(&digest, block != NULL ? 1 : 0);
            if (block == NULL)
                continue;
            RollI32(&digest, block->subId);
            RollI32(&digest, block->callStackDepth);
            RollEclContext(&digest, block->eclContext);
            const i32 childDepth = block->callStackDepth < 0
                                       ? 0
                                       : (block->callStackDepth > 16
                                              ? 16
                                              : block->callStackDepth);
            for (i32 stack = 0; stack < childDepth; ++stack)
                RollEclContext(&digest, block->callStack[stack]);
        }
        RollU8(&digest, enemy.trailFlags);
        RollI32(&digest, enemy.trailHistoryLength);
        RollI32(&digest, enemy.trailCollisionLength);
        RollI32(&digest, enemy.trailSampleStride);
        const i32 trailCount = enemy.trailHistoryLength < 0
                                   ? 0
                                   : (enemy.trailHistoryLength > 96
                                          ? 96
                                          : enemy.trailHistoryLength);
        for (i32 trail = 0; trail < trailCount; ++trail)
        {
            RollFloat3(&digest, enemy.trailSamples[trail].position);
            RollFloat3(&digest, enemy.trailSamples[trail].velocity);
            RollF32(&digest, enemy.trailSamples[trail].angle);
        }
        RollTimer(&digest, enemy.damageReductionTimer);
        RollI32(&digest, enemy.phaseEndTimeRemainingSeconds);
    }
    gLocalLaneDigests[LOCAL_LANE_ENEMY_ECL] = digest.fnv1a;
}

void RollLocalProjectileItems(u8 auditPointKind)
{
    RollingDigest digest = BeginLocalDigest(LOCAL_LANE_PROJECTILE_ITEMS,
                                            auditPointKind);
    RollingDigest manager = BeginLocalDigest(
        static_cast<u8>(kLocalLaneCount +
                        DIAGNOSTIC_LANE_PROJECTILE_MANAGER),
        auditPointKind);
    RollingDigest bulletLifecycle = BeginLocalDigest(
        static_cast<u8>(kLocalLaneCount +
                        DIAGNOSTIC_LANE_BULLET_LIFECYCLE),
        auditPointKind);
    RollingDigest bulletKinematics = BeginLocalDigest(
        static_cast<u8>(kLocalLaneCount +
                        DIAGNOSTIC_LANE_BULLET_KINEMATICS),
        auditPointKind);
    RollingDigest bulletTransforms = BeginLocalDigest(
        static_cast<u8>(kLocalLaneCount +
                        DIAGNOSTIC_LANE_BULLET_TRANSFORMS),
        auditPointKind);
    RollingDigest lasers = BeginLocalDigest(
        static_cast<u8>(kLocalLaneCount + DIAGNOSTIC_LANE_LASERS),
        auditPointKind);
    RollingDigest itemLifecycle = BeginLocalDigest(
        static_cast<u8>(kLocalLaneCount +
                        DIAGNOSTIC_LANE_ITEM_LIFECYCLE),
        auditPointKind);
    RollingDigest itemKinematics = BeginLocalDigest(
        static_cast<u8>(kLocalLaneCount +
                        DIAGNOSTIC_LANE_ITEM_KINEMATICS),
        auditPointKind);
    RollingDigest grazeEffects = BeginLocalDigest(
        static_cast<u8>(kLocalLaneCount +
                        DIAGNOSTIC_LANE_GRAZE_EFFECTS),
        auditPointKind);
    u32 scannedActiveBullets = 0;
    u32 scannedGrazedBullets = 0;
    u32 scannedActiveLasers = 0;
    u32 scannedActiveItems = 0;
    u32 scannedGrazeEffects = 0;

    RollI32(&digest, g_BulletManager.activeBulletCount);
    RollI32(&digest, BulletIndex(g_BulletManager.bulletCursor));
    RollI32(&digest, g_BulletManager.spawnSuppressionFrames);
    RollTimer(&digest, g_BulletManager.timer);
    RollI32(&digest, g_BulletManager.frameCounter);
    RollI32(&digest, g_BulletManager.cancelItemType);
    RollI32(&manager, g_BulletManager.activeBulletCount);
    RollI32(&manager, BulletIndex(g_BulletManager.bulletCursor));
    RollI32(&manager, g_BulletManager.spawnSuppressionFrames);
    RollTimer(&manager, g_BulletManager.timer);
    RollI32(&manager, g_BulletManager.frameCounter);
    RollI32(&manager, g_BulletManager.cancelItemType);
    for (i32 type = 0; type < 0x20; ++type)
    {
        RollFloat3(&digest,
                   g_BulletManager.bulletTypeSprites[type].collisionSize);
        RollFloat3(&manager,
                   g_BulletManager.bulletTypeSprites[type].collisionSize);
    }

#if defined(TH08_PSP_STAGE_POOL_ARENA)
    if (g_BulletManager.bullets == NULL ||
        g_BulletManager.lasers == NULL)
    {
        RollU32(&digest, 0xffffffffU);
        RollU32(&bulletLifecycle, 0xffffffffU);
        RollU32(&bulletKinematics, 0xffffffffU);
        RollU32(&bulletTransforms, 0xffffffffU);
        RollU32(&lasers, 0xffffffffU);
    }
    else
#endif
    {
        for (i32 index = 0; index < 0x601; ++index)
        {
            const Bullet &bullet = g_BulletManager.bullets[index];
            RollU16(&digest, bullet.state);
            RollU16(&bulletLifecycle, bullet.state);
            RollU16(&bulletKinematics, bullet.state);
            RollU16(&bulletTransforms, bullet.state);
            if (bullet.state == BULLET_STATE_UNUSED ||
                bullet.state == BULLET_STATE_SENTINEL)
                continue;
            ++scannedActiveBullets;
            if (bullet.isGrazed != 0)
                ++scannedGrazedBullets;
            RollFloat3(&digest, bullet.sprites.collisionSize);
            RollFloat3(&digest, bullet.position);
            RollFloat3(&digest, bullet.velocity);
            RollF32(&digest, bullet.speed);
            RollF32(&digest, bullet.angle);
            RollFloat3(&bulletKinematics, bullet.sprites.collisionSize);
            RollFloat3(&bulletKinematics, bullet.position);
            RollFloat3(&bulletKinematics, bullet.velocity);
            RollF32(&bulletKinematics, bullet.speed);
            RollF32(&bulletKinematics, bullet.angle);
            RollTimer(&digest, bullet.stateTimer);
            RollTimer(&digest, bullet.activeTimer);
            RollI32(&digest, bullet.offscreenCullDelayFrames);
            RollU32(&digest, bullet.activeTransformFlags);
            RollU32(&digest, bullet.transformFlags);
            RollU16(&digest, bullet.offscreenFrames);
            RollU8(&digest, bullet.isGrazed);
            RollU8(&digest, bullet.cancelledDuringSpawn);
            RollI32(&digest, bullet.zoneTransitionCooldownFrames);
            RollI32(&digest, bullet.transformIndex);
            RollTimer(&bulletLifecycle, bullet.stateTimer);
            RollTimer(&bulletLifecycle, bullet.activeTimer);
            RollI32(&bulletLifecycle, bullet.offscreenCullDelayFrames);
            RollU16(&bulletLifecycle, bullet.offscreenFrames);
            RollU8(&bulletLifecycle, bullet.isGrazed);
            RollU8(&bulletLifecycle, bullet.cancelledDuringSpawn);
            RollI32(&bulletLifecycle,
                    bullet.zoneTransitionCooldownFrames);
            RollU32(&bulletTransforms, bullet.activeTransformFlags);
            RollU32(&bulletTransforms, bullet.transformFlags);
            RollI32(&bulletTransforms, bullet.transformIndex);
            for (i32 transform = 0; transform < 18; ++transform)
            {
                RollLocalBulletTransform(&digest,
                                         bullet.transforms[transform]);
                RollLocalBulletTransform(&bulletTransforms,
                                         bullet.transforms[transform]);
            }
            for (i32 state = 0; state < 7; ++state)
            {
                const BulletExState &ex = bullet.exStates[state];
                RollTimer(&digest, ex.timer);
                RollF32(&digest, ex.float0);
                RollF32(&digest, ex.float1);
                RollFloat3(&digest, ex.vector);
                RollI32(&digest, ex.int0);
                RollI32(&digest, ex.int1);
                RollI32(&digest, ex.int2);
                RollTimer(&bulletTransforms, ex.timer);
                RollF32(&bulletTransforms, ex.float0);
                RollF32(&bulletTransforms, ex.float1);
                RollFloat3(&bulletTransforms, ex.vector);
                RollI32(&bulletTransforms, ex.int0);
                RollI32(&bulletTransforms, ex.int1);
                RollI32(&bulletTransforms, ex.int2);
            }
            RollI32(&digest, bullet.collisionDisabled);
            RollI32(&bulletLifecycle, bullet.collisionDisabled);
        }
        for (i32 index = 0; index < 0x100; ++index)
        {
            const Laser &laser = g_BulletManager.lasers[index];
            RollI32(&digest, laser.inUse);
            RollI32(&lasers, laser.inUse);
            if (laser.inUse == 0)
                continue;
            ++scannedActiveLasers;
            RollFloat3(&digest, laser.position);
            RollF32(&digest, laser.angle);
            RollF32(&digest, laser.startOffset);
            RollF32(&digest, laser.endOffset);
            RollF32(&digest, laser.startLength);
            RollF32(&digest, laser.width);
            RollF32(&digest, laser.currentWidth);
            RollF32(&digest, laser.speed);
            RollI32(&digest, laser.startTime);
            RollI32(&digest, laser.hitboxStartTime);
            RollI32(&digest, laser.duration);
            RollI32(&digest, laser.despawnDuration);
            RollI32(&digest, laser.hitboxEndDelay);
            RollTimer(&digest, laser.timer);
            RollU16(&digest, laser.flags);
            RollU8(&digest, laser.state);
            RollFloat3(&lasers, laser.position);
            RollF32(&lasers, laser.angle);
            RollF32(&lasers, laser.startOffset);
            RollF32(&lasers, laser.endOffset);
            RollF32(&lasers, laser.startLength);
            RollF32(&lasers, laser.width);
            RollF32(&lasers, laser.currentWidth);
            RollF32(&lasers, laser.speed);
            RollI32(&lasers, laser.startTime);
            RollI32(&lasers, laser.hitboxStartTime);
            RollI32(&lasers, laser.duration);
            RollI32(&lasers, laser.despawnDuration);
            RollI32(&lasers, laser.hitboxEndDelay);
            RollTimer(&lasers, laser.timer);
            RollU16(&lasers, laser.flags);
            RollU8(&lasers, laser.state);
        }
    }

    RollI32(&digest, g_ItemManager.nextIndex);
    RollU32(&digest, g_ItemManager.itemCount);
    RollI32(&digest, ItemLinkIndex(g_ItemManager.itemListHead.next));
    RollI32(&digest, ItemLinkIndex(g_ItemManager.itemListHead.prev));
    RollI32(&digest, ItemLinkIndex(g_ItemManager.itemListTail));
    RollI32(&itemLifecycle, g_ItemManager.nextIndex);
    RollU32(&itemLifecycle, g_ItemManager.itemCount);
    RollI32(&itemLifecycle,
            ItemLinkIndex(g_ItemManager.itemListHead.next));
    RollI32(&itemLifecycle,
            ItemLinkIndex(g_ItemManager.itemListHead.prev));
    RollI32(&itemLifecycle, ItemLinkIndex(g_ItemManager.itemListTail));
#if defined(TH08_PSP_STAGE_POOL_ARENA)
    if (g_ItemManager.items == NULL)
    {
        RollU32(&digest, 0xffffffffU);
        RollU32(&itemLifecycle, 0xffffffffU);
        RollU32(&itemKinematics, 0xffffffffU);
    }
    else
#endif
    {
        for (i32 index = 0; index < MAX_ITEMS + 1; ++index)
        {
            const Item &item = g_ItemManager.items[index];
            RollU8(&digest, static_cast<u8>(item.isInUse));
            RollU8(&itemLifecycle, static_cast<u8>(item.isInUse));
            RollU8(&itemKinematics, static_cast<u8>(item.isInUse));
            if (item.isInUse == 0)
                continue;
            ++scannedActiveItems;
            // Item ANM and the entire Effect pool are intentionally excluded.
            RollFloat3(&digest, item.currentPosition);
            RollFloat3(&digest, item.startPositionOrVelocity);
            RollFloat3(&digest, item.targetPosition);
            RollFloat3(&itemKinematics, item.currentPosition);
            RollFloat3(&itemKinematics, item.startPositionOrVelocity);
            RollFloat3(&itemKinematics, item.targetPosition);
            RollTimer(&digest, item.timer);
            RollI32(&digest, item.itemType);
            RollI32(&digest, item.isOnscreen);
            RollI32(&digest, item.state);
            RollI32(&digest, item.isMaxValue);
            RollI32(&digest, ItemLinkIndex(item.next));
            RollI32(&digest, ItemLinkIndex(item.prev));
            RollTimer(&itemLifecycle, item.timer);
            RollI32(&itemLifecycle, item.itemType);
            RollI32(&itemLifecycle, item.isOnscreen);
            RollI32(&itemLifecycle, item.state);
            RollI32(&itemLifecycle, item.isMaxValue);
            RollI32(&itemLifecycle, ItemLinkIndex(item.next));
            RollI32(&itemLifecycle, ItemLinkIndex(item.prev));
        }
    }

    // Effect 8 is the graze particle path.  It consumes the global RNG during
    // initialization, so a one-sided graze can be distinguished from bullet
    // kinematics before it propagates into player death/replay failure.
    for (i32 index = 0; index < 654; ++index)
    {
        const Effect &effect = g_EffectManager.effects[index];
        const bool isGrazeEffect = effect.active != 0 && effect.effectId == 8;
        RollU8(&grazeEffects, isGrazeEffect ? 1U : 0U);
        if (!isGrazeEffect)
            continue;
        ++scannedGrazeEffects;
        RollFloat3(&grazeEffects, effect.position);
        RollFloat3(&grazeEffects, effect.vector1);
        RollFloat3(&grazeEffects, effect.vector2);
        RollFloat3(&grazeEffects, effect.vector3);
        RollTimer(&grazeEffects, effect.timer);
        RollI32(&grazeEffects, effect.slotIndex);
        RollI32(&grazeEffects, effect.releaseRequested);
        RollI32(&grazeEffects, effect.releaseTimer);
    }

    gLocalLaneDigests[LOCAL_LANE_PROJECTILE_ITEMS] = digest.fnv1a;
    gDiagnosticLaneValues[DIAGNOSTIC_LANE_PROJECTILE_MANAGER] =
        manager.fnv1a;
    gDiagnosticLaneValues[DIAGNOSTIC_LANE_BULLET_LIFECYCLE] =
        bulletLifecycle.fnv1a;
    gDiagnosticLaneValues[DIAGNOSTIC_LANE_BULLET_KINEMATICS] =
        bulletKinematics.fnv1a;
    gDiagnosticLaneValues[DIAGNOSTIC_LANE_BULLET_TRANSFORMS] =
        bulletTransforms.fnv1a;
    gDiagnosticLaneValues[DIAGNOSTIC_LANE_LASERS] = lasers.fnv1a;
    gDiagnosticLaneValues[DIAGNOSTIC_LANE_ITEM_LIFECYCLE] =
        itemLifecycle.fnv1a;
    gDiagnosticLaneValues[DIAGNOSTIC_LANE_ITEM_KINEMATICS] =
        itemKinematics.fnv1a;
    gDiagnosticLaneValues[DIAGNOSTIC_LANE_GRAZE_EFFECTS] =
        grazeEffects.fnv1a;
    gDiagnosticLaneValues[DIAGNOSTIC_LANE_BULLET_COUNTS] =
        (scannedActiveBullets & 0xffffU) |
        ((scannedGrazedBullets & 0xffffU) << 16);
    gDiagnosticLaneValues[DIAGNOSTIC_LANE_OBJECT_COUNTS] =
        (scannedActiveLasers & 0x1ffU) |
        ((scannedActiveItems & 0xfffU) << 9) |
        ((scannedGrazeEffects & 0x3ffU) << 21);
}

void RollAllLocalLanes(u8 auditPointKind, u32 replayFrame, u16 input,
                       u8 inputRecordKind, u16 rngSeedBegin,
                       u16 rngSeedEnd)
{
    RollLocalCoreGameplay(auditPointKind, replayFrame, input,
                          inputRecordKind, rngSeedBegin, rngSeedEnd);
    RollLocalPlayerBody(auditPointKind);
    RollLocalPlayerOptions(auditPointKind);
    RollLocalPlayerOwned(auditPointKind);
    RollLocalEnemyEcl(auditPointKind);
    RollLocalProjectileItems(auditPointKind);
}

void RollAllState(u8 auditPointKind, u32 replayFrame, u16 input,
                  u8 inputRecordKind,
                  u16 rngSeedBegin, u16 rngSeedEnd,
                  u32 rngGenerationBegin, u32 rngGenerationEnd)
{
    // RSA05's fresh canonical lanes supersede the cumulative RSA03 domains.
    // Avoid hashing every large pool twice per frame; offsets 32..71 now hold
    // the targeted projectile/item diagnostics populated by the local pass.
    (void)rngGenerationBegin;
    (void)rngGenerationEnd;
    RollAllLocalLanes(auditPointKind, replayFrame, input, inputRecordKind,
                      rngSeedBegin, rngSeedEnd);
}

void AppendSnapshot(u8 snapshotType, u32 replayFrame, u16 input,
                    u16 rngSeedBegin, u16 rngSeedEnd,
                    u32 rngGenerationBegin, u32 rngGenerationEnd)
{
    if (gWriteOffset + kRecordBytes > kBufferBytes)
    {
        gErrorFlags |= ERROR_RECORD_OVERFLOW;
        Increment(&gOverflowCount);
        return;
    }

    const size_t base = gWriteOffset;
    PutU8(base + 0, snapshotType);
    PutU8(base + 1, gStage);
    PutU8(base + 2, gStageInputRecordKind);
    PutU8(base + 3, gPending.valid
                        ? static_cast<u8>(SNAPSHOT_FLAG_PENDING_FRAME)
                        : static_cast<u8>(0));
    PutU16(base + 4, gRunOrdinal);
    PutU16(base + 6, static_cast<u16>(gErrorFlags & 0xffffU));
    PutU16(base + 8, input);
    PutU16(base + 10, gCurrentReplayIdentityIndex);
    PutU32(base + 12, replayFrame);
    PutU32(base + 16, gCompletedFrames);
    PutU16(base + 20, rngSeedBegin);
    PutU16(base + 22, rngSeedEnd);
    PutU32(base + 24, rngGenerationBegin);
    PutU32(base + 28, rngGenerationEnd);
    for (u32 lane = 0; lane < kDiagnosticLaneCount; ++lane)
    {
        PutU32(base + kDiagnosticLaneRecordOffset +
                   static_cast<size_t>(lane) * sizeof(u32),
               gDiagnosticLaneValues[lane]);
    }
    for (u32 lane = 0; lane < kLocalLaneCount; ++lane)
    {
        PutU32(base + kLocalLaneRecordOffset +
                   static_cast<size_t>(lane) * sizeof(u32),
               gLocalLaneDigests[lane]);
    }
    PutU32(base + kItemSpawnWitnessRecordOffset + 0,
           PackItemSpawnCounts());
    PutU32(base + kItemSpawnWitnessRecordOffset + 4,
           gItemSpawnWitness.requestDigest);
    PutU32(base + kItemSpawnWitnessRecordOffset + 8,
           gItemSpawnWitness.firstRejectXBits);
    PutU32(base + kItemSpawnWitnessRecordOffset + 12,
           gItemSpawnWitness.lastResultMeta |
               (gItemSpawnWitness.counterOverflow ? 0x80000000U : 0U));

    gWriteOffset += kRecordBytes;
    Increment(&gRecordCount);
    if (snapshotType == SNAPSHOT_STAGE_BEGIN)
        Increment(&gStageBeginSnapshotCount);
    else if (snapshotType == SNAPSHOT_PERIODIC)
        Increment(&gPeriodicSnapshotCount);
    else if (snapshotType == SNAPSHOT_STAGE_TERMINAL)
        Increment(&gTerminalSnapshotCount);
}

void CopyPath(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0)
        return;
    size_t i = 0;
    if (source != NULL)
    {
        while (i + 1 < capacity && source[i] != '\0')
        {
            destination[i] = source[i];
            ++i;
        }
    }
    destination[i] = '\0';
}

#if !defined(PSP)
bool EnvironmentEnablesAudit(const char *value)
{
    if (value == NULL || value[0] == '\0')
        return false;
    return value[0] == '1' || value[0] == 'y' || value[0] == 'Y' ||
           value[0] == 't' || value[0] == 'T';
}
#endif

void ResetStageTraceStorage()
{
    gWriteOffset = kHeaderBytes;
    memset(gBuffer, 0, kHeaderBytes);
    gLastCheckpointRecordCount = 0;
    gStageActive = false;
    gHavePreviousReplayFrame = false;
    gCompletedFrames = 0;
    gPreviousReplayFrame = 0;
    gLastInput = 0;
    gLastRngSeedBegin = 0;
    gLastRngSeedEnd = 0;
    gLastRngGenerationBegin = 0;
    gLastRngGenerationEnd = 0;
    ResetCanonicalReplayInputState();
    gStagePlaytimeBaseline = 0;
    for (u32 lane = 0; lane < kLocalLaneCount; ++lane)
        gLocalLaneDigests[lane] = 0;
    for (u32 lane = 0; lane < kDiagnosticLaneCount; ++lane)
        gDiagnosticLaneValues[lane] = 0;
    ResetItemSpawnWitness();
    ResetItemSpawnBurstCapture();
    gPending.valid = false;
    gReplayIdentityCount = 0;
    gReplayIdentityOverflowCount = 0;
    gRecordCount = 0;
    gErrorFlags = 0;
    gEndWithoutPendingCount = 0;
    gGapCount = 0;
    gDuplicateCount = 0;
    gOverflowCount = 0;
    gTerminalPendingCount = 0;
    gBeginWhilePendingCount = 0;
    gStageBeginWhileActiveCount = 0;
    gTerminalWithoutActiveCount = 0;
    gStageBeginSnapshotCount = 0;
    gPeriodicSnapshotCount = 0;
    gTerminalSnapshotCount = 0;
    gStageCheckpointReady = false;
    gStageCheckpointWritten = false;
    gArchivePath[0] = '\0';
}

void BuildArchivePath(const ReplayIdentity &identity)
{
#if defined(PSP)
    const int length = snprintf(
        gArchivePath, sizeof(gArchivePath),
        "ms0:/PSP/GAME/TH08PSP/TH08_RSA08_C%08X_S%02u_R%04u.bin",
        static_cast<unsigned int>(identity.checksum),
        static_cast<unsigned int>(gStage),
        static_cast<unsigned int>(gRunOrdinal));
#else
    const int length = snprintf(
        gArchivePath, sizeof(gArchivePath), "%s.C%08X.S%02u.R%04u.bin",
        gOutputPath, static_cast<unsigned int>(identity.checksum),
        static_cast<unsigned int>(gStage),
        static_cast<unsigned int>(gRunOrdinal));
#endif
    if (length < 0 || static_cast<size_t>(length) >= sizeof(gArchivePath))
        gArchivePath[0] = '\0';
}

void PopulateItemSpawnBurstDetailHeader()
{
    static const char magic[8] = {'T', 'H', '8', 'I', '7', 'D', '0', '1'};
    const size_t base = kItemSpawnBurstDetailOffset;
    if (!gItemSpawnBurstCapture.captured)
    {
        // Unqualified frames use the entry section as scratch.  Do not publish
        // those stale, sub-threshold requests in an uncaptured sidecar.
        memset(gBuffer + kItemSpawnBurstEntryOffset, 0,
               kItemSpawnBurstCapacity * kItemSpawnBurstEntryBytes);
    }
    for (size_t i = 0; i < sizeof(magic); ++i)
        PutU8(base + i, static_cast<u8>(magic[i]));
    PutU32(base + 8, 1U); // detail format version
    PutU32(base + 12, static_cast<u32>(kItemSpawnBurstDetailHeaderBytes));
    PutU32(base + 16, static_cast<u32>(kItemSpawnBurstEntryBytes));
    PutU32(base + 20, kItemSpawnBurstCapacity);
    PutU32(base + 24, kItemSpawnBurstThreshold);
    PutU32(base + 28, gItemSpawnBurstCapture.captureFlags);
    PutU32(base + 32, gItemSpawnBurstCapture.stage);
    PutU32(base + 36, gItemSpawnBurstCapture.runOrdinal);
    PutU32(base + 40, gItemSpawnBurstCapture.replayIdentityIndex);
    PutU32(base + 44, gItemSpawnBurstCapture.replayFrame);
    PutU32(base + 48, gItemSpawnBurstCapture.completedFrames);
    PutU32(base + 52,
           gItemSpawnBurstCapture.capturedTotalRequestCount);
    PutU32(base + 56,
           gItemSpawnBurstCapture.capturedStoredRequestCount);
    PutU32(base + 60, gItemSpawnBurstCapture.input);
    PutU32(base + 64, gItemSpawnBurstCapture.inputRecordKind);
    PutU32(base + 68, gItemSpawnBurstCapture.rngSeedBegin);
    PutU32(base + 72, gItemSpawnBurstCapture.rngSeedEnd);
    PutU32(base + 76, gItemSpawnBurstCapture.rngGenerationBegin);
    PutU32(base + 80, gItemSpawnBurstCapture.rngGenerationEnd);
    PutU32(base + 84, static_cast<u32>(kItemSpawnBurstEntryOffset));
    PutU32(base + 88, static_cast<u32>(
                            kItemSpawnBurstCapacity *
                            kItemSpawnBurstEntryBytes));
    PutU32(base + 92, 0U);
}

void PopulateHeader()
{
    // 0x00 magic[8]        "TH8RSA08"
    // 0x08 schema          8
    // 0x0c header bytes    8192 (identity table + SpawnItem burst detail)
    // 0x10 record bytes    112 (RSA05 lanes + 16-byte SpawnItem witness)
    // 0x14 endian marker   0x01020304 (serialized LE)
    // 0x18..0x5f counters/status, 0x60 platform, 0x64 identity
    // table metadata, 0x78 digest algorithm, and 0x7c..0x8c capacity.
    // Identity entries begin at 0xa0 and contain only stable decoded replay
    // scalars.  RSA03+ extends the header identity with shot/difficulty so a
    // healthy team-1 demo cannot be aligned accidentally with a failing
    // team-2/team-3 demo that happens to share process-local indexes.
    static const char magic[8] = {'T', 'H', '8', 'R', 'S', 'A', '0', '8'};
    for (size_t i = 0; i < sizeof(magic); ++i)
        PutU8(i, static_cast<u8>(magic[i]));
    PutU32(8, kSchema);
    PutU32(12, static_cast<u32>(kHeaderBytes));
    PutU32(16, static_cast<u32>(kRecordBytes));
    PutU32(20, 0x01020304U);
    PutU32(24, gRecordCount);
    PutU32(28, gRunCount);
    PutU32(32, static_cast<u32>(gWriteOffset));
    PutU32(36, static_cast<u32>(kBufferBytes));
    PutU32(40, gErrorFlags);
    PutU32(44, gEndWithoutPendingCount);
    PutU32(48, gGapCount);
    PutU32(52, gDuplicateCount);
    PutU32(56, gOverflowCount);
    PutU32(60, gTerminalPendingCount);
    PutU32(64, gBeginWhilePendingCount);
    PutU32(68, gStageBeginWhileActiveCount);
    PutU32(72, gTerminalWithoutActiveCount);
    PutU32(76, gStageBeginSnapshotCount);
    PutU32(80, gPeriodicSnapshotCount);
    PutU32(84, gTerminalSnapshotCount);
    PutU32(88, gStageActive ? 1U : 0U);
    PutU32(92, gPending.valid ? 1U : 0U);
#if defined(PSP)
    PutU32(96, 1U);
#else
    PutU32(96, 2U);
#endif
    PutU32(100, gReplayIdentityCount);
    PutU32(104, kIdentityCapacity);
    PutU32(108, static_cast<u32>(kIdentityEntryBytes));
    PutU32(112, static_cast<u32>(kIdentityTableOffset));
    PutU32(116, gReplayIdentityOverflowCount);
    PutU32(120, 1U); // digest algorithm: FNV-1a 32 + Jenkins OAT 32
    PutU32(124, kRecordCapacity);
    PutU32(128, kPeriodicFrameInterval);
    PutU32(132, kPeriodicOnlyFrameCapacity);
    PutU32(136, kSingleReplayFrameCapacity);
    PutU32(140, kSingleReplayLifecycleReserve);
    PutU32(144, kLocalLaneCount);
    PutU32(148, kLocalLaneAlgorithm);
    PutU32(152, static_cast<u32>(kLocalLaneRecordOffset));
    PutU32(156, kKnownDemo3FrameCount);

    PopulateItemSpawnBurstDetailHeader();

    for (u32 i = 0; i < gReplayIdentityCount; ++i)
    {
        const size_t base = kIdentityTableOffset +
                            static_cast<size_t>(i) * kIdentityEntryBytes;
        const ReplayIdentity *identity = &gReplayIdentities[i];
        PutU32(base + 0, identity->magic);
        PutU16(base + 4, identity->version);
        PutU8(base + 6, identity->usesExtendedInputRecords);
        PutU8(base + 7, identity->hasUserDataSection);
        PutU32(base + 8, static_cast<u32>(identity->fileSize));
        PutU32(base + 12, static_cast<u32>(identity->checksum));
        PutU32(base + 16, static_cast<u32>(identity->compressedSize));
        PutU32(base + 20, static_cast<u32>(identity->decompressedSize));
        PutU8(base + 24, identity->minorVersion);
        PutU8(base + 25, identity->shotType);
        PutU8(base + 26, identity->difficulty);
        PutU8(base + 27, identity->isPractice);
        PutU16(base + 28, static_cast<u16>(identity->spellcardNumber));
        PutU16(base + 30, identity->majorVersion);
    }
}

bool WriteSidecarPath(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return false;
#if defined(PSP)
    // Keep the terminal checkpoint outside newlib's allocator and stdio locks.
    // The backend completes short writes before close establishes the
    // checkpoint on the Memory Stick.
    return th08::psp::WriteFileExact(path, gBuffer, gWriteOffset);
#else
    FILE *file = fopen(path, "wb");
    if (file == NULL)
        return false;
    const size_t written = fwrite(gBuffer, 1, gWriteOffset, file);
    const int closeResult = fclose(file);
    return written == gWriteOffset && closeResult == 0;
#endif
}

bool WriteSidecars()
{
    PopulateHeader();
    const bool latestWritten = WriteSidecarPath(gOutputPath);
    const bool archiveWritten = gArchivePath[0] == '\0' ||
                                strcmp(gArchivePath, gOutputPath) == 0 ||
                                WriteSidecarPath(gArchivePath);
    return latestWritten && archiveWritten;
}

} // namespace

void RecordItemSpawnRequest(const void *positionBytes, i32 itemType,
                            i32 state, i32 nextIndex)
{
    if (!gRuntimeEnabled || !gStageActive || !gPending.valid ||
        positionBytes == NULL)
        return;

    u32 positionBits[3];
    memcpy(positionBits, positionBytes, sizeof(positionBits));
    if (gItemSpawnWitness.requestCount == 0U)
        gItemSpawnWitness.requestDigest = kFnvOffset;
    IncrementItemSpawnCounter(&gItemSpawnWitness.requestCount);
    RollItemSpawnWitnessU32(positionBits[0]);
    RollItemSpawnWitnessU32(positionBits[1]);
    RollItemSpawnWitnessU32(positionBits[2]);
    RollItemSpawnWitnessU32(static_cast<u32>(itemType));
    RollItemSpawnWitnessU32(static_cast<u32>(state));
    RollItemSpawnWitnessU32(static_cast<u32>(nextIndex));
    RecordItemSpawnBurstRequest(positionBits, itemType, state, nextIndex);
    gItemSpawnWitness.currentRequestXBits = positionBits[0];
    gItemSpawnWitness.requestOpen = true;
}

void RecordItemSpawnResult(u32 outcome, i32 itemType, i32 state,
                           i32 nextIndex)
{
    if (!gRuntimeEnabled || !gStageActive || !gPending.valid ||
        !gItemSpawnWitness.requestOpen)
        return;

    switch (outcome)
    {
    case ITEM_SPAWN_AUDIT_ACCEPTED:
        IncrementItemSpawnCounter(&gItemSpawnWitness.acceptedCount);
        break;
    case ITEM_SPAWN_AUDIT_REJECT_X:
        if (gItemSpawnWitness.rejectXCount == 0U)
        {
            gItemSpawnWitness.firstRejectXBits =
                gItemSpawnWitness.currentRequestXBits;
        }
        IncrementItemSpawnCounter(&gItemSpawnWitness.rejectXCount);
        break;
    case ITEM_SPAWN_AUDIT_REJECT_TIME_FIRST_SLOT:
        IncrementItemSpawnCounter(
            &gItemSpawnWitness.rejectTimeFirstSlotCount);
        break;
    default:
        break;
    }

    // Bits 0..11 nextIndex, 12..19 effective type, 20..27 effective state,
    // 28..30 exit outcome, 31 saturated-counter flag (added at write time).
    gItemSpawnWitness.lastResultMeta =
        (static_cast<u32>(nextIndex) & 0x00000fffU) |
        ((static_cast<u32>(itemType) & 0xffU) << 12) |
        ((static_cast<u32>(state) & 0xffU) << 20) |
        ((outcome & 0x7U) << 28);
    RecordItemSpawnBurstResult(outcome, itemType, state, nextIndex);
    gItemSpawnWitness.requestOpen = false;
}

void InitializeRuntime()
{
    if (gInitialized)
        return;

    gInitialized = true;
    ResetStageTraceStorage();

#if defined(PSP)
    gRuntimeEnabled = true;
    CopyPath(gOutputPath, sizeof(gOutputPath),
             "ms0:/PSP/GAME/TH08PSP/TH08_REPLAY_SYNC_AUDIT.bin");
#else
    gRuntimeEnabled = EnvironmentEnablesAudit(getenv("TH08_REPLAY_SYNC_AUDIT"));
    const char *configuredPath = getenv("TH08_REPLAY_SYNC_AUDIT_PATH");
    CopyPath(gOutputPath, sizeof(gOutputPath),
             configuredPath != NULL && configuredPath[0] != '\0'
                 ? configuredPath
                 : "TH08_REPLAY_SYNC_AUDIT.bin");
#endif
}

void StageBegin()
{
    if (!gRuntimeEnabled || !g_GameManager.flags.isReplay)
        return;

    if (gStageActive)
    {
        gErrorFlags |= ERROR_STAGE_BEGIN_WHILE_ACTIVE;
        Increment(&gStageBeginWhileActiveCount);
    }
    else if (gRecordCount != 0)
    {
        // Normal PSP teardown checkpoints before the next stage starts.  The
        // fallback keeps a complete prior-stage trace if that hook was
        // skipped, while still avoiding any per-frame I/O.
        if (gStageCheckpointReady && !gStageCheckpointWritten)
            gStageCheckpointWritten = WriteSidecars();
        ResetStageTraceStorage();
    }

    gStageActive = true;
    gPending.valid = false;
    ResetItemSpawnWitness();
    BeginItemSpawnBurstFrame();
    gHavePreviousReplayFrame = false;
    gCompletedFrames = 0;
    ResetCanonicalReplayInputState();
    gStagePlaytimeBaseline =
        static_cast<u32>(g_GameManager.playtimeFrames);
    gStage = static_cast<u8>(g_GameManager.stageAtStart);
    if (g_ReplayManager != NULL && g_ReplayManager->replayData != NULL &&
        g_ReplayManager->replayData->header.usesExtendedInputRecords != 0)
        gStageInputRecordKind = INPUT_RECORD_EXTENDED;
    else
        gStageInputRecordKind = INPUT_RECORD_NORMAL;
    if (g_ReplayManager == NULL || g_ReplayManager->replayData == NULL)
        gErrorFlags |= ERROR_SOURCE_UNAVAILABLE;

    ReplayIdentity replayIdentity;
    ReadReplayIdentity(&replayIdentity);
    gCurrentReplayIdentityIndex = RegisterReplayIdentity(&replayIdentity);

    Increment(&gRunCount);
    gRunOrdinal = static_cast<u16>(gRunCount & 0xffffU);
    BuildArchivePath(replayIdentity);
    gCoreDigest = BeginDigest(DIGEST_CORE);
    gPlayerDigest = BeginDigest(DIGEST_PLAYER);
    gEnemyDigest = BeginDigest(DIGEST_ENEMY);
    gProjectileDigest = BeginDigest(DIGEST_PROJECTILE);
    gItemEffectDigest = BeginDigest(DIGEST_ITEM_EFFECT);
    RollReplayIdentity(&replayIdentity);

    const u16 seed = g_Rng.GetSeed();
    const u32 generation = g_Rng.GetGenerationCount();
    RollAllState(AUDIT_POINT_STAGE_BEGIN, kNoReplayFrame, 0,
                 INPUT_RECORD_NONE,
                 seed, seed, generation, generation);
    gLastInput = 0;
    gLastRngSeedBegin = seed;
    gLastRngSeedEnd = seed;
    gLastRngGenerationBegin = generation;
    gLastRngGenerationEnd = generation;
    AppendSnapshot(SNAPSHOT_STAGE_BEGIN, kNoReplayFrame, 0,
                   seed, seed, generation, generation);
}

void BeginFrame(u32 replayFrame, u16 input, u8 inputRecordKind)
{
    if (!gRuntimeEnabled || !gStageActive)
        return;

    ResetItemSpawnWitness();
    BeginItemSpawnBurstFrame();

    if (gPending.valid)
    {
        gErrorFlags |= ERROR_BEGIN_WHILE_PENDING;
        Increment(&gBeginWhilePendingCount);
        // Preserve forward observability after declaring the trace invalid.
        // The abandoned frame is never folded into a digest as if complete.
        gPending.valid = false;
    }

    if (gHavePreviousReplayFrame)
    {
        if (replayFrame == gPreviousReplayFrame)
        {
            gErrorFlags |= ERROR_DUPLICATE_FRAME;
            Increment(&gDuplicateCount);
        }
        else if (replayFrame != gPreviousReplayFrame + 1U)
        {
            gErrorFlags |= ERROR_FRAME_GAP;
            Increment(&gGapCount);
        }
    }

    gPreviousReplayFrame = replayFrame;
    gHavePreviousReplayFrame = true;
    AdvanceCanonicalReplayInputState(input);
    gPending.valid = true;
    gPending.replayFrame = replayFrame;
    gPending.input = input;
    gPending.inputRecordKind = inputRecordKind;
    gPending.rngSeedBegin = g_Rng.GetSeed();
    gPending.rngGenerationBegin = g_Rng.GetGenerationCount();
}

void EndFrame()
{
    if (!gRuntimeEnabled || !gStageActive)
        return;
    if (!gPending.valid)
    {
        // Priority 18 can tick while replay input is disabled.  Such a tick
        // has no logical replay frame and is deliberately not hashed.
        Increment(&gEndWithoutPendingCount);
        return;
    }

    const u16 seedEnd = g_Rng.GetSeed();
    const u32 generationEnd = g_Rng.GetGenerationCount();
    RollAllState(AUDIT_POINT_FRAME_END, gPending.replayFrame, gPending.input,
                 gPending.inputRecordKind, gPending.rngSeedBegin, seedEnd,
                 gPending.rngGenerationBegin, generationEnd);

    gLastInput = gPending.input;
    gLastRngSeedBegin = gPending.rngSeedBegin;
    gLastRngSeedEnd = seedEnd;
    gLastRngGenerationBegin = gPending.rngGenerationBegin;
    gLastRngGenerationEnd = generationEnd;
    const u32 completedReplayFrame = gPending.replayFrame;
    const u32 completedFrameCount =
        gCompletedFrames == 0xffffffffU ? gCompletedFrames
                                       : gCompletedFrames + 1U;
    CaptureItemSpawnBurstIfEligible(
        completedReplayFrame, completedFrameCount, gPending.input,
        gPending.inputRecordKind, gPending.rngSeedBegin, seedEnd,
        gPending.rngGenerationBegin, generationEnd);
    gPending.valid = false;
    Increment(&gCompletedFrames);

    if (gCompletedFrames % kPeriodicFrameInterval == 0U)
    {
        AppendSnapshot(SNAPSHOT_PERIODIC, completedReplayFrame, gLastInput,
                       gLastRngSeedBegin, gLastRngSeedEnd,
                       gLastRngGenerationBegin, gLastRngGenerationEnd);
    }
}

void StageTerminal()
{
    if (!gRuntimeEnabled)
        return;
    if (!gStageActive)
    {
        // Recording and ordinary play also use GameManager's lifetime hook;
        // they are outside a replay-playback audit run and are true no-ops.
        if (!g_GameManager.flags.isReplay)
            return;
        Increment(&gTerminalWithoutActiveCount);
        return;
    }

    u32 replayFrame = gHavePreviousReplayFrame ? gPreviousReplayFrame : kNoReplayFrame;
    u16 input = gLastInput;
    u16 seedBegin = gLastRngSeedBegin;
    u32 generationBegin = gLastRngGenerationBegin;
    if (gPending.valid)
    {
        gErrorFlags |= ERROR_TERMINAL_PENDING;
        Increment(&gTerminalPendingCount);
        replayFrame = gPending.replayFrame;
        input = gPending.input;
        seedBegin = gPending.rngSeedBegin;
        generationBegin = gPending.rngGenerationBegin;
    }
    else
    {
        // Lifecycle records do not inherit the final completed frame's
        // SpawnItem witness.  A genuinely pending terminal frame retains its
        // witness alongside SNAPSHOT_FLAG_PENDING_FRAME.
        ResetItemSpawnWitness();
    }

    const u16 seedEnd = g_Rng.GetSeed();
    const u32 generationEnd = g_Rng.GetGenerationCount();
    // Fold the actual terminal state once under a distinct point marker.  A
    // pending replay frame remains pending: it is neither supplied as a frame
    // input here nor added to gCompletedFrames.
    RollAllState(AUDIT_POINT_STAGE_TERMINAL, kNoReplayFrame, 0,
                 INPUT_RECORD_NONE, seedEnd, seedEnd,
                 generationEnd, generationEnd);
    AppendSnapshot(SNAPSHOT_STAGE_TERMINAL, replayFrame, input,
                   seedBegin, seedEnd, generationBegin, generationEnd);
    gPending.valid = false;
    gStageActive = false;
    gStageCheckpointReady = true;
    gStageCheckpointWritten = false;
}

void CheckpointAfterStage()
{
    if (!gRuntimeEnabled || gFlushed || gStageActive ||
        gTerminalSnapshotCount == 0 ||
        !gStageCheckpointReady || gStageCheckpointWritten)
        return;

    // All gameplay owners and the stage arena binding have already been cut.
    // The checkpoint only serializes fixed audit storage, so PSP HOME/PPSSPP
    // stop cannot discard an otherwise complete replay trace.  The fixed
    // latest path preserves existing automation; the checksum/stage/run path
    // retains every stage before the 1 MiB RAM buffer is reused.
    if (WriteSidecars())
    {
        gLastCheckpointRecordCount = gRecordCount;
        gStageCheckpointWritten = true;
    }
}

void FlushAtShutdown()
{
    if (!gRuntimeEnabled || gFlushed)
        return;
    gFlushed = true;
    if (gStageActive)
    {
        // Chain teardown should always have delivered StageTerminal while the
        // owners were valid.  Do not inspect torn-down gameplay objects here;
        // publish the lifecycle failure in the sidecar instead.
        gErrorFlags |= ERROR_SHUTDOWN_WHILE_ACTIVE;
        if (gPending.valid)
        {
            gErrorFlags |= ERROR_TERMINAL_PENDING;
            Increment(&gTerminalPendingCount);
        }
    }
    // WinMain calls this after g_Chain.Release().  A PSP stage-terminal
    // checkpoint may already contain the same trace; rewrite it so shutdown
    // status (including any lifecycle error above) is authoritative.
    if (!gStageCheckpointWritten || gStageActive)
        gStageCheckpointWritten = WriteSidecars();
}

} // namespace ReplaySyncAudit
} // namespace th08

#endif
