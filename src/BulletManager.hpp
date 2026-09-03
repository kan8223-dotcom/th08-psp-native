#pragma once

#include "AnmManager.hpp"
#include "Global.hpp"

namespace th08
{

struct BulletTypeSprites;

struct BulletTransformRawPayload
{
    f32 float0;
    f32 float1;
    i32 int0;
    i32 int1;
};

struct BulletVectorAccelerationPayload
{
    f32 magnitude;
    f32 angle;
    i32 durationFrames;
    i32 reserved0C;
};

struct BulletPolarAccelerationPayload
{
    f32 speedDelta;
    f32 angleDelta;
    i32 durationFrames;
    i32 reserved0C;
};

struct BulletDirectionChangePayload
{
    f32 angle;
    f32 speed;
    i32 intervalFrames;
    i32 repeatCount;
};

struct BulletBoundaryBouncePayload
{
    f32 speed;
    f32 reserved04;
    i32 bounceLimit;
    i32 reserved0C;
};

struct BulletTimedTransformPayload
{
    f32 reserved00;
    f32 reserved04;
    i32 frames;
    i32 reserved0C;
};

struct BulletCullDelayPayload
{
    f32 reserved00;
    f32 reserved04;
    i32 frames;
    i32 reserved0C;
};

struct BulletSpriteTransformPayload
{
    f32 reserved00;
    f32 reserved04;
    i32 bulletType;
    i32 color;
};

struct BulletSoundTransformPayload
{
    f32 reserved00;
    f32 reserved04;
    i32 soundIndex;
    i32 reserved0C;
};

struct BulletChildPatternPrimaryPayload
{
    f32 speed1;
    f32 speed2;
    i32 packedPattern;
    i32 count1;
};

struct BulletChildPatternSecondaryPayload
{
    f32 angle;
    f32 angleStep;
    i32 count2;
    i32 transformFlags;
};

union BulletTransformPayload
{
    BulletTransformRawPayload raw;
    BulletVectorAccelerationPayload vectorAcceleration;
    BulletPolarAccelerationPayload polarAcceleration;
    BulletDirectionChangePayload directionChange;
    BulletBoundaryBouncePayload boundaryBounce;
    BulletTimedTransformPayload timed;
    BulletCullDelayPayload cullDelay;
    BulletSpriteTransformPayload sprite;
    BulletSoundTransformPayload sound;
    BulletChildPatternPrimaryPayload childPrimary;
    BulletChildPatternSecondaryPayload childSecondary;
};
C_ASSERT(sizeof(BulletTransformPayload) == 0x10);

struct BulletTransformRecord
{
    BulletTransformPayload payload;
    u32 kind;
    i32 allowWhileActive;
};
C_ASSERT(sizeof(BulletTransformRecord) == 0x18);
C_ASSERT(offsetof(BulletTransformRecord, payload) == 0x0);
C_ASSERT(offsetof(BulletTransformRawPayload, float1) == 0x4);
C_ASSERT(offsetof(BulletTransformRawPayload, int0) == 0x8);
C_ASSERT(offsetof(BulletTransformRawPayload, int1) == 0xc);
C_ASSERT(offsetof(BulletTransformRecord, kind) == 0x10);
C_ASSERT(offsetof(BulletTransformRecord, allowWhileActive) == 0x14);

enum BulletTransformKind
{
    BULLET_TRANSFORM_NONE = 0,
    BULLET_TRANSFORM_DECELERATE = 0x1,
    BULLET_TRANSFORM_SPAWN_FAST = 0x2,
    BULLET_TRANSFORM_SPAWN_NORMAL = 0x4,
    BULLET_TRANSFORM_SPAWN_SLOW = 0x8,
    BULLET_TRANSFORM_ACCELERATE_VECTOR = 0x10,
    BULLET_TRANSFORM_ACCELERATE_POLAR = 0x20,
    BULLET_TRANSFORM_CHANGE_DIRECTION_RELATIVE = 0x40,
    BULLET_TRANSFORM_CHANGE_DIRECTION_AIMED = 0x80,
    BULLET_TRANSFORM_CHANGE_DIRECTION_ABSOLUTE = 0x100,
    BULLET_TRANSFORM_PLAY_SPAWN_SOUND = 0x200,
    BULLET_TRANSFORM_BOUNCE_ALL_EDGES = 0x400,
    BULLET_TRANSFORM_BOUNCE_EXCEPT_BOTTOM = 0x800,
    BULLET_TRANSFORM_CANCEL_IMMUNE = 0x1000,
    BULLET_TRANSFORM_SET_CULL_DELAY = 0x2000,
    BULLET_TRANSFORM_SET_SPRITE = 0x4000,
    BULLET_TRANSFORM_ONLY_WHEN_PLAYER_YOUKAI = 0x8000,
    BULLET_TRANSFORM_ONLY_WHEN_PLAYER_HUMAN = 0x10000,
    BULLET_TRANSFORM_WAIT = 0x20000,
    BULLET_TRANSFORM_DESPAWN = 0x40000,
    BULLET_TRANSFORM_PLAY_SOUND = 0x80000,
    BULLET_TRANSFORM_ECL_EX_TRIGGER_MARKER = 0x100000,
    BULLET_TRANSFORM_WRAP_X = 0x400000,
    BULLET_TRANSFORM_WRAP_Y = 0x800000,
    BULLET_TRANSFORM_SPAWN_CHILD_PATTERN = 0x1000000,
};

enum BulletAimMode
{
    BULLET_AIM_FAN_AIMED = 0,
    BULLET_AIM_FAN = 1,
    BULLET_AIM_CIRCLE_AIMED = 2,
    BULLET_AIM_CIRCLE = 3,
    BULLET_AIM_OFFSET_CIRCLE_AIMED = 4,
    BULLET_AIM_OFFSET_CIRCLE = 5,
    BULLET_AIM_RANDOM_ANGLE = 6,
    BULLET_AIM_RANDOM_SPEED = 7,
    BULLET_AIM_RANDOM = 8,
};

struct BulletSpawnDescriptor
{
    i16 bulletType;
    i16 color;
    Float3 position;
    f32 angle;
    f32 angleStep;
    f32 speed1;
    f32 speed2;
    BulletTransformRecord transforms[18];
    f32 laserStartOffset;       // +0x1D0
    f32 laserEndOffset;         // +0x1D4
    f32 laserStartLength;       // +0x1D8
    f32 laserWidth;             // +0x1DC
    i32 laserStartTime;         // +0x1E0
    i32 laserDuration;          // +0x1E4
    i32 laserDespawnDuration;   // +0x1E8
    i32 laserHitboxStartTime;   // +0x1EC
    i32 laserHitboxEndDelay;    // +0x1F0
    i16 count1;
    i16 count2;
    u16 aimMode;
    u16 unconsumedWord1FA;
    u32 transformFlags;
    i32 spawnSound;
    i32 transformSound;
    i32 transformStartIndex;
    BulletTypeSprites *templateSprites;

    BulletSpawnDescriptor();
};
C_ASSERT(sizeof(BulletSpawnDescriptor) == 0x210);
C_ASSERT(offsetof(BulletSpawnDescriptor, position) == 0x4);
C_ASSERT(offsetof(BulletSpawnDescriptor, angle) == 0x10);
C_ASSERT(offsetof(BulletSpawnDescriptor, angleStep) == 0x14);
C_ASSERT(offsetof(BulletSpawnDescriptor, speed1) == 0x18);
C_ASSERT(offsetof(BulletSpawnDescriptor, speed2) == 0x1c);
C_ASSERT(offsetof(BulletSpawnDescriptor, transforms) == 0x20);
C_ASSERT(offsetof(BulletSpawnDescriptor, laserStartOffset) == 0x1d0);
C_ASSERT(offsetof(BulletSpawnDescriptor, count1) == 0x1f4);
C_ASSERT(offsetof(BulletSpawnDescriptor, aimMode) == 0x1f8);
C_ASSERT(offsetof(BulletSpawnDescriptor, unconsumedWord1FA) == 0x1fa);
C_ASSERT(offsetof(BulletSpawnDescriptor, transformFlags) == 0x1fc);
C_ASSERT(offsetof(BulletSpawnDescriptor, spawnSound) == 0x200);
C_ASSERT(offsetof(BulletSpawnDescriptor, transformSound) == 0x204);
C_ASSERT(offsetof(BulletSpawnDescriptor, transformStartIndex) == 0x208);
C_ASSERT(offsetof(BulletSpawnDescriptor, templateSprites) == 0x20c);

i32 IsBulletManagerAnmReleaseRequired();

struct BulletTypeSprites
{
    BulletTypeSprites();

    AnmVm bulletVm;
    AnmVm spawnFastVm;
    AnmVm spawnNormalVm;
    AnmVm spawnSlowVm;
    AnmVm despawnVm;
    Float3 collisionSize;
    u8 unconsumedTemplateByteD40;
    u8 spriteHeightPx;
    u8 drawBucketIndex;
    u8 trailingAlignmentD43;
};
C_ASSERT(sizeof(BulletTypeSprites) == 0xd44);
C_ASSERT(offsetof(BulletTypeSprites, spawnFastVm) == 0x2a4);
C_ASSERT(offsetof(BulletTypeSprites, spawnNormalVm) == 0x548);
C_ASSERT(offsetof(BulletTypeSprites, spawnSlowVm) == 0x7ec);
C_ASSERT(offsetof(BulletTypeSprites, despawnVm) == 0xa90);
C_ASSERT(offsetof(BulletTypeSprites, collisionSize) == 0xd34);
C_ASSERT(offsetof(BulletTypeSprites, unconsumedTemplateByteD40) == 0xd40);
C_ASSERT(offsetof(BulletTypeSprites, spriteHeightPx) == 0xd41);
C_ASSERT(offsetof(BulletTypeSprites, drawBucketIndex) == 0xd42);
C_ASSERT(offsetof(BulletTypeSprites, trailingAlignmentD43) == 0xd43);

#if defined(TH08_PSP_COMPACT_BULLET_VM)
#if !defined(PSP) || !defined(TH08_PSP_STAGE_POOL_ARENA)
#error TH08_PSP_COMPACT_BULLET_VM requires the PSP pointer-backed stage pool
#endif

// A bullet can be in only one of the three spawn states at a time.  Keep the
// complete five-VM type templates above, but copy only the selected spawn VM
// into each PSP runtime slot.  The primary VM must remain separate: TH08 ECL
// instructions can mutate it while the spawn VM is visible.  The despawn VM
// also remains separate so every existing one-way state transition keeps its
// original preinitialized animation without transition-time reconstruction.
struct PspCompactBulletSprites
{
    AnmVm bulletVm;
    AnmVm selectedSpawnVm;
    AnmVm despawnVm;
    Float3 collisionSize;
    u8 unconsumedTemplateByteD40;
    u8 spriteHeightPx;
    u8 drawBucketIndex;
    u8 trailingAlignmentD43;
};
C_ASSERT(sizeof(PspCompactBulletSprites) == 0x7fc);
C_ASSERT(offsetof(PspCompactBulletSprites, selectedSpawnVm) == 0x2a4);
C_ASSERT(offsetof(PspCompactBulletSprites, despawnVm) == 0x548);
C_ASSERT(offsetof(PspCompactBulletSprites, collisionSize) == 0x7ec);
C_ASSERT(offsetof(PspCompactBulletSprites, unconsumedTemplateByteD40) == 0x7f8);
C_ASSERT(offsetof(PspCompactBulletSprites, spriteHeightPx) == 0x7f9);
C_ASSERT(offsetof(PspCompactBulletSprites, drawBucketIndex) == 0x7fa);
C_ASSERT(offsetof(PspCompactBulletSprites, trailingAlignmentD43) == 0x7fb);
#endif

struct BulletExState
{
    BulletExState();

    ZunTimer timer;
    union
    {
        f32 float0;
        f32 accelerationMagnitude;
        f32 speedDelta;
        f32 directionChangeSpeed;
        f32 bounceSpeed;
    };
    union
    {
        f32 float1;
        f32 accelerationAngle;
        f32 angleDelta;
        f32 directionChangeAngle;
    };
    Float3 vector;
    union
    {
        i32 int0;
        i32 durationFrames;
        i32 directionChangeIntervalFrames;
        i32 bouncesCompleted;
    };
    union
    {
        i32 int1;
        i32 directionChangeRepeatCount;
        i32 bounceLimit;
    };
    union
    {
        i32 int2;
        i32 directionChangesCompleted;
    };
};
C_ASSERT(sizeof(BulletExState) == 0x2c);
C_ASSERT(offsetof(BulletExState, float0) == 0xc);
C_ASSERT(offsetof(BulletExState, float1) == 0x10);
C_ASSERT(offsetof(BulletExState, vector) == 0x14);
C_ASSERT(offsetof(BulletExState, int0) == 0x20);
C_ASSERT(offsetof(BulletExState, int1) == 0x24);
C_ASSERT(offsetof(BulletExState, int2) == 0x28);

enum BulletTransformStateSlot
{
    BULLET_TRANSFORM_STATE_DECELERATION = 0,
    BULLET_TRANSFORM_STATE_VECTOR_ACCELERATION = 1,
    BULLET_TRANSFORM_STATE_POLAR_ACCELERATION = 2,
    BULLET_TRANSFORM_STATE_DIRECTION_CHANGE = 3,
    BULLET_TRANSFORM_STATE_BOUNDARY_BOUNCE = 4,
    BULLET_TRANSFORM_STATE_WAIT = 5,
    BULLET_TRANSFORM_STATE_WRAP = 6,
};

enum LaserState
{
    LASER_STATE_STARTING = 0,
    LASER_STATE_ACTIVE = 1,
    LASER_STATE_DESPAWNING = 2,
};

struct Laser
{
    Laser();

    AnmVm bodyVm;
    AnmVm startCapVm;
    Float3 position;
    f32 angle;                 // +0x554
    f32 startOffset;           // +0x558
    f32 endOffset;             // +0x55C
    f32 startLength;           // +0x560
    f32 width;                 // +0x564
    f32 currentWidth;          // +0x568
    f32 speed;                 // +0x56C
    i32 startTime;             // +0x570
    i32 hitboxStartTime;       // +0x574
    i32 duration;              // +0x578
    i32 despawnDuration;       // +0x57C
    i32 hitboxEndDelay;        // +0x580
    i32 inUse;                 // +0x584
    ZunTimer timer;            // +0x588
    u16 flags;                 // +0x594
    i16 color;                 // +0x596
    u8 state;                  // +0x598
    u8 hideCapDuringStartup;   // +0x599
    u8 trailingAlignment59A[2];
};
C_ASSERT(sizeof(Laser) == 0x59c);
C_ASSERT(offsetof(Laser, startCapVm) == 0x2a4);
C_ASSERT(offsetof(Laser, position) == 0x548);
C_ASSERT(offsetof(Laser, angle) == 0x554);
C_ASSERT(offsetof(Laser, startOffset) == 0x558);
C_ASSERT(offsetof(Laser, endOffset) == 0x55c);
C_ASSERT(offsetof(Laser, startLength) == 0x560);
C_ASSERT(offsetof(Laser, width) == 0x564);
C_ASSERT(offsetof(Laser, currentWidth) == 0x568);
C_ASSERT(offsetof(Laser, speed) == 0x56c);
C_ASSERT(offsetof(Laser, startTime) == 0x570);
C_ASSERT(offsetof(Laser, hitboxStartTime) == 0x574);
C_ASSERT(offsetof(Laser, duration) == 0x578);
C_ASSERT(offsetof(Laser, despawnDuration) == 0x57c);
C_ASSERT(offsetof(Laser, hitboxEndDelay) == 0x580);
C_ASSERT(offsetof(Laser, inUse) == 0x584);
C_ASSERT(offsetof(Laser, timer) == 0x588);
C_ASSERT(offsetof(Laser, flags) == 0x594);
C_ASSERT(offsetof(Laser, color) == 0x596);
C_ASSERT(offsetof(Laser, state) == 0x598);
C_ASSERT(offsetof(Laser, hideCapDuringStartup) == 0x599);
C_ASSERT(offsetof(Laser, trailingAlignment59A) == 0x59a);

enum BulletState
{
    BULLET_STATE_UNUSED = 0,
    BULLET_STATE_FIRED = 1,
    BULLET_STATE_SPAWNING_FAST = 2,
    BULLET_STATE_SPAWNING_NORMAL = 3,
    BULLET_STATE_SPAWNING_SLOW = 4,
    BULLET_STATE_DESPAWNING = 5,
    BULLET_STATE_SENTINEL = 6,
};

#if defined(PSP)
// Interval diagnostics for the all-frame Player cancel-region broad phase.
// The spatial cache and duplicate-call replay remain external to Bullet/Player
// and never enter replay or gameplay serialization.
struct PspBulletCancelSpatialTelemetrySnapshot
{
    unsigned long long calls;
    unsigned long long indexedQueries;
    unsigned long long rejectedQueries;
    unsigned long long circles;
    unsigned long long rects;
    unsigned long long rebuilds;
    unsigned long long fullCandidates;
    unsigned long long indexedCandidates;
    unsigned long long fallbackCandidates;
    unsigned long long exactTests;
    unsigned long long falsePositives;
    unsigned long long fallbacks;
    unsigned long long occupancyOwnerFallbacks;
    unsigned long long unsupportedRegionFallbacks;
    unsigned long long nonfiniteFallbacks;
    unsigned long long duplicatePairs;
    unsigned long long duplicateReplays;
    unsigned long long duplicateExactTestsSaved;
    unsigned long long duplicateFallbacks;
};

PspBulletCancelSpatialTelemetrySnapshot
PspPeekBulletCancelSpatialTelemetry();
PspBulletCancelSpatialTelemetrySnapshot
PspTakeBulletCancelSpatialTelemetry();
void PspResetBulletCancelSpatialTelemetry();

#if defined(TH08_PSP_BULLET_CANCEL_SPATIAL) && \
    TH08_PSP_BULLET_CANCEL_SPATIAL
// Fail-closed Bullet runtime-owner proof. Non-Bullet callers (for example
// Enemy laser graze checks), invalid occupancy, and stale slots retain the
// canonical full cancel-region scan.
ZunBool PspBulletCancelSpatialValidatePosition(const Float3 *position);
void PspBulletCancelSpatialNoteRebuild(u32 circles, u32 rects,
                                       ZunBool nonfiniteFallback);
void PspBulletCancelSpatialNoteQuery(u32 fullCandidates,
                                     u32 selectedCandidates,
                                     u32 exactTests,
                                     ZunBool indexed,
                                     ZunBool rejected,
                                     ZunBool falsePositive,
                                     ZunBool fallback,
                                     ZunBool ownerFallback,
                                     ZunBool nonfiniteFallback);
void PspBulletCancelSpatialNoteDuplicate(ZunBool replayed,
                                         ZunBool fallback);
#endif

// Interval counters are diagnostic only and live outside every reconstructed
// gameplay object.  `slotProbes` is the number of individual occupancy/state
// candidates inspected, `wordProbes` is the number of live-bitset word loads,
// and `visited` is the number of authoritative non-unused Bullet bodies run.
struct PspBulletLiveEnumTelemetrySnapshot
{
    unsigned long long frames;
    unsigned long long slotProbes;
    unsigned long long wordProbes;
    unsigned long long visited;
    unsigned long long fallbackFrames;
};

PspBulletLiveEnumTelemetrySnapshot PspPeekBulletLiveEnumTelemetry();
PspBulletLiveEnumTelemetrySnapshot PspTakeBulletLiveEnumTelemetry();
void PspResetBulletLiveEnumTelemetry();

// Counter-only audit of the canonical transform interpreter.  These values
// are diagnostic state outside Bullet and never participate in simulation,
// collision, RNG, or replay serialization.
struct PspBulletTransformAuditTelemetrySnapshot
{
    unsigned long long advanceCalls;
    unsigned long long firedUpdateCalls;
    unsigned long long terminalIndexReturns;
    unsigned long long terminalNoneReturns;
    unsigned long long activeBlockedReturns;
    unsigned long long disabledRecordsSkipped;
    unsigned long long recordsDispatched;
    unsigned long long activeTransformStarts;
    unsigned long long childSpawnRecords;
    unsigned long long spawnProgramWrites;
    unsigned long long wholeSlotResets;
};

PspBulletTransformAuditTelemetrySnapshot
PspPeekBulletTransformAuditTelemetry();
PspBulletTransformAuditTelemetrySnapshot
PspTakeBulletTransformAuditTelemetry();
void PspResetBulletTransformAuditTelemetry();

// Product-gated transform-terminal sidecar counters.  The 192-byte bitset
// itself resides in the retained stage arena; these interval counters are
// diagnostic-only and never participate in simulation or replay state.
struct PspBulletTransformTerminalTelemetrySnapshot
{
    unsigned long long firedCalls;
    unsigned long long hits;
    unsigned long long misses;
    unsigned long long fallbacks;
    unsigned long long invariantWitnesses;
    unsigned long long repairs;
    unsigned long long terminalIndexMarks;
    unsigned long long terminalNoneMarks;
    unsigned long long markFallbacks;
    unsigned long long spawnProgramClears;
    unsigned long long wholeSlotClears;
};

PspBulletTransformTerminalTelemetrySnapshot
PspPeekBulletTransformTerminalTelemetry();
PspBulletTransformTerminalTelemetrySnapshot
PspTakeBulletTransformTerminalTelemetry();
void PspResetBulletTransformTerminalTelemetry();

// Reserved in every PSP build so a later audit-OFF/product A/B retains the
// same global layout.  The audit observes the existing canonical collision
// call exactly once and never permits these counters to affect simulation.
struct PspBulletCollisionGateAuditTelemetrySnapshot
{
    unsigned long long frames;
    unsigned long long sidecarOwnerInvalidFrames;
    unsigned long long sidecarClaimsEmptyFrames;
    unsigned long long authoritativeEmptyFrames;
    unsigned long long knownEmptyFrames;
    unsigned long long cancelFalseEmptyWitnesses;
    unsigned long long cancelStalePositiveFrames;
    unsigned long long collisionEligibleBullets;
    unsigned long long grazePathBullets;
    unsigned long long lethalPathBullets;
    unsigned long long clearGrazeSuppressed;
    unsigned long long clearGrazeSeparate;
    unsigned long long clearLethalSeparate;
    unsigned long long cancelUnknownFallbacks;
    unsigned long long invalidSnapshotFallbacks;
    unsigned long long invalidBulletFallbacks;
    unsigned long long touchOrOverlapFallbacks;
    unsigned long long snapshotMutationWitnesses;
    unsigned long long canonicalZero;
    unsigned long long canonicalOne;
    unsigned long long canonicalTwo;
    unsigned long long falseClearWitnesses;
    unsigned long long itemTypeWitnesses;
};

PspBulletCollisionGateAuditTelemetrySnapshot
PspPeekBulletCollisionGateAuditTelemetry();
PspBulletCollisionGateAuditTelemetrySnapshot
PspTakeBulletCollisionGateAuditTelemetry();
void PspResetBulletCollisionGateAuditTelemetry();
#endif

struct Bullet
{
    Bullet();
    void Deactivate();
    void AdvanceTransformProgram();
    void UpdatePolarAcceleration();
    void UpdateAbsoluteDirectionChange();
    void UpdateAimedDirectionChange();
    void UpdateBoundaryBounce();
    void UpdateRelativeDirectionChange();
    void UpdateDeceleration();
    void UpdateVectorAcceleration();
    void UpdateVerticalWrap();
    void UpdateHorizontalWrap();
    ZunResult DrawSingleBullet();

#if defined(TH08_PSP_COMPACT_BULLET_VM)
    PspCompactBulletSprites sprites;
#else
    BulletTypeSprites sprites;
#endif
    Float3 position;
    Float3 velocity;
    Float3 unconsumedVectorD5C;
    f32 speed;
    u32 unconsumedDwordsD6C[2];
    f32 angle;
    u32 unconsumedDwordsD78[2];
    ZunTimer stateTimer;
    ZunTimer activeTimer;
    u32 unconsumedDwordsD98[4];
    i32 offscreenCullDelayFrames;
    u32 activeTransformFlags;
    u32 transformFlags;
    i16 color;
    u16 unconsumedWordDB6;
    u16 state;
    u16 offscreenFrames;
    u8 unconsumedSpawnMarkerDBC;
    u8 isGrazed;
    u8 cancelledDuringSpawn;
    u8 pointerAlignmentDBF;
    Bullet *nextInDrawBucket;
    i32 zoneTransitionCooldownFrames;
    i32 transformSound;
    i32 transformIndex;
    BulletTransformRecord transforms[18];
    BulletExState exStates[7];
    i8 collisionDisabled;
    u8 trailingAlignment10B5[3];
};
#if defined(TH08_PSP_COMPACT_BULLET_VM)
// Removing two 0x2a4-byte VMs shifts the complete gameplay tail by 0x548.
C_ASSERT(sizeof(Bullet) == 0xb70);
C_ASSERT(offsetof(Bullet, position) == 0x7fc);
C_ASSERT(offsetof(Bullet, velocity) == 0x808);
C_ASSERT(offsetof(Bullet, unconsumedVectorD5C) == 0x814);
C_ASSERT(offsetof(Bullet, speed) == 0x820);
C_ASSERT(offsetof(Bullet, unconsumedDwordsD6C) == 0x824);
C_ASSERT(offsetof(Bullet, angle) == 0x82c);
C_ASSERT(offsetof(Bullet, unconsumedDwordsD78) == 0x830);
C_ASSERT(offsetof(Bullet, stateTimer) == 0x838);
C_ASSERT(offsetof(Bullet, activeTimer) == 0x844);
C_ASSERT(offsetof(Bullet, unconsumedDwordsD98) == 0x850);
C_ASSERT(offsetof(Bullet, offscreenCullDelayFrames) == 0x860);
C_ASSERT(offsetof(Bullet, activeTransformFlags) == 0x864);
C_ASSERT(offsetof(Bullet, transformFlags) == 0x868);
C_ASSERT(offsetof(Bullet, color) == 0x86c);
C_ASSERT(offsetof(Bullet, unconsumedWordDB6) == 0x86e);
C_ASSERT(offsetof(Bullet, state) == 0x870);
C_ASSERT(offsetof(Bullet, offscreenFrames) == 0x872);
C_ASSERT(offsetof(Bullet, unconsumedSpawnMarkerDBC) == 0x874);
C_ASSERT(offsetof(Bullet, isGrazed) == 0x875);
C_ASSERT(offsetof(Bullet, cancelledDuringSpawn) == 0x876);
C_ASSERT(offsetof(Bullet, pointerAlignmentDBF) == 0x877);
C_ASSERT(offsetof(Bullet, nextInDrawBucket) == 0x878);
C_ASSERT(offsetof(Bullet, zoneTransitionCooldownFrames) == 0x87c);
C_ASSERT(offsetof(Bullet, transformSound) == 0x880);
C_ASSERT(offsetof(Bullet, transformIndex) == 0x884);
C_ASSERT(offsetof(Bullet, transforms) == 0x888);
C_ASSERT(offsetof(Bullet, exStates) == 0xa38);
C_ASSERT(offsetof(Bullet, collisionDisabled) == 0xb6c);
C_ASSERT(offsetof(Bullet, trailingAlignment10B5) == 0xb6d);
#else
C_ASSERT(sizeof(Bullet) == 0x10b8);
C_ASSERT(offsetof(Bullet, position) == 0xd44);
C_ASSERT(offsetof(Bullet, velocity) == 0xd50);
C_ASSERT(offsetof(Bullet, unconsumedVectorD5C) == 0xd5c);
C_ASSERT(offsetof(Bullet, speed) == 0xd68);
C_ASSERT(offsetof(Bullet, unconsumedDwordsD6C) == 0xd6c);
C_ASSERT(offsetof(Bullet, angle) == 0xd74);
C_ASSERT(offsetof(Bullet, unconsumedDwordsD78) == 0xd78);
C_ASSERT(offsetof(Bullet, stateTimer) == 0xd80);
C_ASSERT(offsetof(Bullet, activeTimer) == 0xd8c);
C_ASSERT(offsetof(Bullet, unconsumedDwordsD98) == 0xd98);
C_ASSERT(offsetof(Bullet, offscreenCullDelayFrames) == 0xda8);
C_ASSERT(offsetof(Bullet, activeTransformFlags) == 0xdac);
C_ASSERT(offsetof(Bullet, transformFlags) == 0xdb0);
C_ASSERT(offsetof(Bullet, color) == 0xdb4);
C_ASSERT(offsetof(Bullet, unconsumedWordDB6) == 0xdb6);
C_ASSERT(offsetof(Bullet, state) == 0xdb8);
C_ASSERT(offsetof(Bullet, offscreenFrames) == 0xdba);
C_ASSERT(offsetof(Bullet, unconsumedSpawnMarkerDBC) == 0xdbc);
C_ASSERT(offsetof(Bullet, isGrazed) == 0xdbd);
C_ASSERT(offsetof(Bullet, cancelledDuringSpawn) == 0xdbe);
C_ASSERT(offsetof(Bullet, pointerAlignmentDBF) == 0xdbf);
C_ASSERT(offsetof(Bullet, nextInDrawBucket) == 0xdc0);
C_ASSERT(offsetof(Bullet, zoneTransitionCooldownFrames) == 0xdc4);
C_ASSERT(offsetof(Bullet, transformSound) == 0xdc8);
C_ASSERT(offsetof(Bullet, transformIndex) == 0xdcc);
C_ASSERT(offsetof(Bullet, transforms) == 0xdd0);
C_ASSERT(offsetof(Bullet, exStates) == 0xf80);
C_ASSERT(offsetof(Bullet, collisionDisabled) == 0x10b4);
C_ASSERT(offsetof(Bullet, trailingAlignment10B5) == 0x10b5);
#endif

struct BulletManager
{
    BulletTypeSprites bulletTypeSprites[0x20];
#if defined(TH08_PSP_STAGE_POOL_ARENA)
    Bullet *bullets;
    Laser *lasers;
#else
    Bullet bullets[0x601];
    Laser lasers[0x100];
#endif
    i32 activeBulletCount;
    i32 spawnSuppressionFrames;
    ZunTimer timer;
    i32 frameCounter;
    char *bulletAnmPath;
    Bullet *drawBuckets[6];
    Bullet *bulletCursor;
    i32 cancelItemType;
    AnmLoaded *bulletAnm;

    BulletManager();

    void Initialize();
    void ClearBulletsForTransition();
    void RemoveAllBullets(i32 mode);
    i32 DespawnBullets(i32 maxScore, i32 awardLaserItems);
    void ClearDrawBuckets();
    i32 SpawnSingleBullet(BulletSpawnDescriptor *descriptor, i32 index1, i32 index2, f32 angleToPlayer);
    void RemoveBulletsInRadius(const Float3 *position, f32 radius);
    i32 SpawnBulletPattern(BulletSpawnDescriptor *descriptor);
    Laser *SpawnLaserPattern(BulletSpawnDescriptor *descriptor);

    static ZunResult RegisterChain(char *bulletAnmPath);
    static ChainCallbackResult OnUpdate(BulletManager *bulletManager);
    static ChainCallbackResult OnDraw(BulletManager *bulletManager);
    static ZunResult AddedCallback(BulletManager *bulletManager);
    static ZunResult DeletedCallback(BulletManager *bulletManager);
    static void CutChain();
};
#if defined(TH08_PSP_STAGE_POOL_ARENA)
C_ASSERT(sizeof(BulletManager) == 0x1a8c8);
C_ASSERT(offsetof(BulletManager, bullets) == 0x1a880);
C_ASSERT(offsetof(BulletManager, lasers) == 0x1a884);
C_ASSERT(offsetof(BulletManager, activeBulletCount) == 0x1a888);
C_ASSERT(offsetof(BulletManager, drawBuckets) == 0x1a8a4);
C_ASSERT(offsetof(BulletManager, bulletAnm) == 0x1a8c4);
#else
C_ASSERT(sizeof(BulletManager) == 0x6ba578);
C_ASSERT(offsetof(BulletManager, bullets) == 0x1a880);
C_ASSERT(offsetof(BulletManager, lasers) == 0x660938);
C_ASSERT(offsetof(BulletManager, activeBulletCount) == 0x6ba538);
C_ASSERT(offsetof(BulletManager, spawnSuppressionFrames) == 0x6ba53c);
C_ASSERT(offsetof(BulletManager, timer) == 0x6ba540);
C_ASSERT(offsetof(BulletManager, frameCounter) == 0x6ba54c);
C_ASSERT(offsetof(BulletManager, drawBuckets) == 0x6ba554);
C_ASSERT(offsetof(BulletManager, bulletCursor) == 0x6ba56c);
C_ASSERT(offsetof(BulletManager, cancelItemType) == 0x6ba570);
C_ASSERT(offsetof(BulletManager, bulletAnm) == 0x6ba574);
#endif

DIFFABLE_EXTERN(BulletManager, g_BulletManager);

} /* namespace th08 */
