#pragma once
#include "Supervisor.hpp"

#include <stddef.h>
#include "ZunColor.hpp"
#include "ZunMath.hpp"
#include "ZunResult.hpp"
#include "diffbuild.hpp"
#include "dxutil.hpp"
#include "inttypes.hpp"
#include "utils.hpp"
#include <d3d8.h>
#if defined(PSP)
#include "anm_scratch.hpp"
#endif

#define GAME_WINDOW_WIDTH 640
#define GAME_WINDOW_HEIGHT 480

namespace th08
{
struct AsciiManagerPopup;

#if defined(PSP) && defined(TH08_PSP_EFFECT_INDEXED_QUADS) && \
        TH08_PSP_EFFECT_INDEXED_QUADS && \
    defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
        TH08_PSP_BULLET_UNIFIED_QUADS
#define TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED 1
#else
#define TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED 0
#endif

#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED
// Presentation-only interval counters.  Keeping the complete declaration and
// every writer behind the product gate makes the default-OFF build pay no BSS,
// branch, or counter-update cost.  None of these fields is manager, VM, RNG,
// gameplay, or replay state.
struct PspEffectIndexedQuadStats
{
    u32 passes;
    u32 flushes;
    u32 batches;
    u32 successfulOrdinaryQuads;
    u32 verticesSaved;
    u32 bytesSaved;
    u32 fallbacks;
    u32 fallbackQuads;
    u32 ownerConflicts;
    u32 abandonedPasses;
    u32 abandonedQuads;
    u32 maxBatchQuads;
};

void PspQueryEffectIndexedQuadStats(PspEffectIndexedQuadStats *stats);
void PspResetEffectIndexedQuadStats();
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_NATURAL_QUADS) && \
    TH08_PSP_ITEM_NATURAL_QUADS
#define TH08_PSP_ITEM_NATURAL_QUADS_ENABLED 1
#else
#define TH08_PSP_ITEM_NATURAL_QUADS_ENABLED 0
#endif

#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED && \
    (!defined(TH08_PSP_BULLET_UNIFIED_QUADS) || \
     !TH08_PSP_BULLET_UNIFIED_QUADS)
#error "ITEM natural quads require TH08_PSP_BULLET_UNIFIED_QUADS"
#endif

#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED && \
    (((defined(TH08_PSP_ITEM_DIRECT_GE) && TH08_PSP_ITEM_DIRECT_GE)) || \
     ((defined(TH08_PSP_ITEM_MIXED_QUADS_AUDIT) && \
       TH08_PSP_ITEM_MIXED_QUADS_AUDIT)) || \
     ((defined(TH08_PSP_ITEM_MIXED_QUADS_FASTPATH) && \
       TH08_PSP_ITEM_MIXED_QUADS_FASTPATH)) || \
     ((defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) && \
       TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT)) || \
     ((defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH) && \
       TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH)))
#error "ITEM natural quads are isolated from prior Item topology experiments"
#endif

#if defined(PSP)
// Fixed OFF/ON telemetry reservation for the natural-batch product.  The
// counters are presentation-only and reset at telemetry window boundaries;
// no field is part of AnmManager, Item, simulation, VM, RNG, or replay state.
struct PspItemNaturalQuadStats
{
    u32 passes;
    u32 canonicalBatches;
    u32 itemTimeCandidates;
    u32 visibleItemTime;
    u32 culledItemTime;
    u32 triggerBatches;
    u32 triggerQuads;
    u32 coalescedQuads;
    u32 eligibleQuads;
    u32 submittedBatches;
    u32 submittedQuads;
    u32 nativeSubmits;
    u32 nativeSubmittedQuads;
    u32 clientFallbackSubmits;
    u32 clientFallbackQuads;
    u32 canonicalInputVertices;
    u32 packedOutputVertices;
    u32 duplicateVerticesAvoided;
    u32 fallbackBatches;
    u32 pointerFallbacks;
    u32 spanFallbacks;
    u32 capacityFallbacks;
    u32 topologyFallbacks;
    u32 stateFallbacks;
    u32 extraTopologyBatches;
    u32 indexFallbacks;
    u32 nativeFallbacks;
    u32 extraSplitBatches;
    u32 extraFlushes;
    u32 abandonedBatches;
    u32 abandonedQuads;
    u32 maxBatchQuads;
    u32 topologyChecks;
    u32 topologyCheckedQuads;
};

#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
void PspItemNaturalQuadNotePass();
void PspItemNaturalQuadSetCurrentTarget(bool active);
void PspQueryItemNaturalQuadStats(PspItemNaturalQuadStats *stats);
void PspResetItemNaturalQuadStats();
#endif
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) && \
        TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT && \
    defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH) && \
        TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH
#error "ITEM_TIME draw-pair audit and fast path are mutually exclusive"
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_DIRECT_GE) && \
        TH08_PSP_ITEM_DIRECT_GE && \
    (((defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) && \
       TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT)) || \
     ((defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH) && \
       TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH)))
#error "ITEM_TIME draw-pair and Item direct-GE owners are mutually exclusive"
#endif

#if defined(PSP) && \
    (((defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) && \
       TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT)) || \
     ((defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH) && \
       TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH)))
#define TH08_PSP_ITEM_TIME_DRAW_PAIR_ENABLED 1
#else
#define TH08_PSP_ITEM_TIME_DRAW_PAIR_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH) && \
    TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH
#define TH08_PSP_ITEM_TIME_DRAW_PAIR_PRODUCT_ENABLED 1
#else
#define TH08_PSP_ITEM_TIME_DRAW_PAIR_PRODUCT_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_BULLET_MIXED_QUADS_AUDIT) && \
        TH08_PSP_BULLET_MIXED_QUADS_AUDIT && \
    defined(TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) && \
        TH08_PSP_BULLET_MIXED_QUADS_FASTPATH
#error "Bullet mixed-quad audit and fast path are mutually exclusive"
#endif

#if defined(PSP) && \
    (((defined(TH08_PSP_BULLET_MIXED_QUADS_AUDIT) && \
       TH08_PSP_BULLET_MIXED_QUADS_AUDIT)) || \
     ((defined(TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) && \
       TH08_PSP_BULLET_MIXED_QUADS_FASTPATH)))
#define TH08_PSP_BULLET_MIXED_QUADS_ENABLED 1
#else
#define TH08_PSP_BULLET_MIXED_QUADS_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) && \
    TH08_PSP_BULLET_MIXED_QUADS_FASTPATH
#define TH08_PSP_BULLET_MIXED_QUADS_PRODUCT_ENABLED 1
#else
#define TH08_PSP_BULLET_MIXED_QUADS_PRODUCT_ENABLED 0
#endif

#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED && \
    (!defined(TH08_PSP_BULLET_UNIFIED_QUADS) || \
     !TH08_PSP_BULLET_UNIFIED_QUADS)
#error "Bullet mixed quads require TH08_PSP_BULLET_UNIFIED_QUADS"
#endif

#if TH08_PSP_BULLET_MIXED_QUADS_PRODUCT_ENABLED && \
    (!defined(TH08_PSP_BULLET_DIRECT_GE) || !TH08_PSP_BULLET_DIRECT_GE)
#error "Bullet mixed-quad fast path requires TH08_PSP_BULLET_DIRECT_GE"
#endif

#if defined(PSP) && defined(TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH) && \
        TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH && \
    (!defined(TH08_PSP_BULLET_UNIFIED_QUADS) || \
     !TH08_PSP_BULLET_UNIFIED_QUADS || \
     !defined(TH08_PSP_BULLET_DIRECT_GE) || !TH08_PSP_BULLET_DIRECT_GE)
#error "Bullet packed-vertex fast path requires unified/direct GE"
#endif

#if defined(PSP) && defined(TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH) && \
        TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH && \
    (((defined(TH08_PSP_BULLET_PACKED_VERTEX_AUDIT) && \
       TH08_PSP_BULLET_PACKED_VERTEX_AUDIT)) || \
     ((defined(TH08_PSP_BULLET_MIXED_QUADS_AUDIT) && \
       TH08_PSP_BULLET_MIXED_QUADS_AUDIT)) || \
     ((defined(TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) && \
       TH08_PSP_BULLET_MIXED_QUADS_FASTPATH)) || \
     ((defined(TH08_PSP_ITEM_MIXED_QUADS_AUDIT) && \
       TH08_PSP_ITEM_MIXED_QUADS_AUDIT)) || \
     ((defined(TH08_PSP_ITEM_MIXED_QUADS_FASTPATH) && \
       TH08_PSP_ITEM_MIXED_QUADS_FASTPATH)))
#error "Bullet packed-vertex product is isolated from audit/mixed 2V modes"
#endif

#if defined(PSP) && defined(TH08_PSP_BULLET_ONEPASS_4V_AUDIT) && \
        TH08_PSP_BULLET_ONEPASS_4V_AUDIT && \
    defined(TH08_PSP_BULLET_ONEPASS_4V_FASTPATH) && \
        TH08_PSP_BULLET_ONEPASS_4V_FASTPATH
#error "Bullet one-pass 4V audit and fast path are mutually exclusive"
#endif

#if defined(PSP) && \
    (((defined(TH08_PSP_BULLET_ONEPASS_4V_AUDIT) && \
       TH08_PSP_BULLET_ONEPASS_4V_AUDIT)) || \
     ((defined(TH08_PSP_BULLET_ONEPASS_4V_FASTPATH) && \
       TH08_PSP_BULLET_ONEPASS_4V_FASTPATH)))
#define TH08_PSP_BULLET_ONEPASS_4V_ENABLED 1
#else
#define TH08_PSP_BULLET_ONEPASS_4V_ENABLED 0
#endif

#if TH08_PSP_BULLET_ONEPASS_4V_ENABLED && \
    (!defined(TH08_PSP_BULLET_FASTPATH) || \
     !TH08_PSP_BULLET_FASTPATH || \
     !defined(TH08_PSP_BULLET_UNIFIED_QUADS) || \
     !TH08_PSP_BULLET_UNIFIED_QUADS || \
     !defined(TH08_PSP_BULLET_DIRECT_GE) || \
     !TH08_PSP_BULLET_DIRECT_GE)
#error "Bullet one-pass 4V requires the rotation sidecar, unified quads, and direct GE"
#endif

#if TH08_PSP_BULLET_ONEPASS_4V_ENABLED && \
    (((defined(TH08_PSP_BULLET_PACKED_VERTEX_AUDIT) && \
       TH08_PSP_BULLET_PACKED_VERTEX_AUDIT)) || \
     ((defined(TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH) && \
       TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH)) || \
     ((defined(TH08_PSP_BULLET_MIXED_QUADS_AUDIT) && \
       TH08_PSP_BULLET_MIXED_QUADS_AUDIT)) || \
     ((defined(TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) && \
       TH08_PSP_BULLET_MIXED_QUADS_FASTPATH)))
#error "Bullet one-pass 4V is isolated from packed/mixed Bullet experiments"
#endif

#if defined(PSP)
// Fixed OFF/AUDIT/PRODUCT reservation.  M0 counters are presentation-only and
// never enter AnmManager, Bullet, RNG, simulation, or replay state.
struct PspBulletOnePass4VStats
{
    u32 attempts;
    u32 canonicalDraws;
    u32 inputFallbacks;
    u32 ownerFallbacks;
    u32 stateFallbacks;
    u32 capacityFallbacks;
    u32 builtQuads;
    u32 visibleQuads;
    u32 culledQuads;
    u32 wouldAccept;
    u32 quadMatches;
    u32 quadMismatches;
    u32 bufferMatches;
    u32 bufferMismatches;
    u32 vmMatches;
    u32 vmMismatches;
    u32 stateMatches;
    u32 stateMismatches;
    u32 productAccepts;
    u32 reserved;
};

static_assert(sizeof(PspBulletOnePass4VStats) == 80U,
              "Bullet one-pass fixed stats reservation changed");
PspBulletOnePass4VStats PspPeekBulletOnePass4VStats();
PspBulletOnePass4VStats PspTakeBulletOnePass4VStats();
void PspResetBulletOnePass4VStats();
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_MIXED_QUADS_AUDIT) && \
        TH08_PSP_ITEM_MIXED_QUADS_AUDIT && \
    defined(TH08_PSP_ITEM_MIXED_QUADS_FASTPATH) && \
        TH08_PSP_ITEM_MIXED_QUADS_FASTPATH
#error "Item mixed-quad audit and fast path are mutually exclusive"
#endif

#if defined(PSP) && \
    (((defined(TH08_PSP_ITEM_MIXED_QUADS_AUDIT) && \
       TH08_PSP_ITEM_MIXED_QUADS_AUDIT)) || \
     ((defined(TH08_PSP_ITEM_MIXED_QUADS_FASTPATH) && \
       TH08_PSP_ITEM_MIXED_QUADS_FASTPATH)))
#define TH08_PSP_ITEM_MIXED_QUADS_ENABLED 1
#else
#define TH08_PSP_ITEM_MIXED_QUADS_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_MIXED_QUADS_FASTPATH) && \
    TH08_PSP_ITEM_MIXED_QUADS_FASTPATH
#define TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED 1
#else
#define TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED 0
#endif

#if TH08_PSP_ITEM_MIXED_QUADS_ENABLED && \
    (!defined(TH08_PSP_BULLET_UNIFIED_QUADS) || \
     !TH08_PSP_BULLET_UNIFIED_QUADS)
#error "Item mixed quads require TH08_PSP_BULLET_UNIFIED_QUADS"
#endif

#if TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED && \
    (!defined(TH08_PSP_BULLET_DIRECT_GE) || !TH08_PSP_BULLET_DIRECT_GE)
#error "Item mixed-quad fast path requires TH08_PSP_BULLET_DIRECT_GE"
#endif

#if TH08_PSP_ITEM_MIXED_QUADS_ENABLED && \
    defined(TH08_PSP_ITEM_DIRECT_GE) && TH08_PSP_ITEM_DIRECT_GE
#error "Item mixed quads and Item direct-GE are mutually exclusive"
#endif

#if TH08_PSP_ITEM_MIXED_QUADS_ENABLED && TH08_PSP_ITEM_TIME_DRAW_PAIR_ENABLED
#error "Item mixed quads and ITEM_TIME draw-pair are mutually exclusive"
#endif

#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED || \
    TH08_PSP_ITEM_MIXED_QUADS_ENABLED
#define TH08_PSP_ANY_MIXED_QUADS_ENABLED 1
#else
#define TH08_PSP_ANY_MIXED_QUADS_ENABLED 0
#endif

#if TH08_PSP_BULLET_MIXED_QUADS_PRODUCT_ENABLED || \
    TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED
#define TH08_PSP_ANY_MIXED_QUADS_PRODUCT_ENABLED 1
#else
#define TH08_PSP_ANY_MIXED_QUADS_PRODUCT_ENABLED 0
#endif

#if defined(PSP)
// A reason is supplied before the generic axis-aligned pair machinery sees a
// VM, or produced by that machinery while proving the canonical Draw2D
// contract.  Keeping this vocabulary stable makes audit/product logs directly
// comparable and lets every failure take the untouched six-vertex path.
enum PspItemTimeDrawPairRejectReason
{
    PSP_ITEM_TIME_DRAW_PAIR_ACCEPT = 0,
    PSP_ITEM_TIME_DRAW_PAIR_REJECT_OWNER,
    PSP_ITEM_TIME_DRAW_PAIR_REJECT_LOAD,
    PSP_ITEM_TIME_DRAW_PAIR_REJECT_SCRIPT,
    PSP_ITEM_TIME_DRAW_PAIR_REJECT_SPRITE,
    PSP_ITEM_TIME_DRAW_PAIR_REJECT_VISIBILITY,
    PSP_ITEM_TIME_DRAW_PAIR_REJECT_ROTATION,
    PSP_ITEM_TIME_DRAW_PAIR_REJECT_SCALE,
    PSP_ITEM_TIME_DRAW_PAIR_REJECT_NONFINITE,
    PSP_ITEM_TIME_DRAW_PAIR_REJECT_TEXTURE,
    PSP_ITEM_TIME_DRAW_PAIR_REJECT_STATE,
    PSP_ITEM_TIME_DRAW_PAIR_REJECT_AXIS,
    PSP_ITEM_TIME_DRAW_PAIR_REJECT_ENDPOINT,
    PSP_ITEM_TIME_DRAW_PAIR_REJECT_CAPACITY,
    PSP_ITEM_TIME_DRAW_PAIR_REJECT_BACKEND,
};

struct ItemTimeDrawPairStats
{
    u32 passes;
    u32 candidates;
    u32 canonicalDraws;
    u32 visibleCandidates;
    u32 culledCandidates;
    u32 eligiblePairs;
    u32 compatibleRuns;
    u32 submittedRuns;
    u32 submittedPairs;
    u32 endpointMatches;
    u32 endpointMismatches;
    u32 canonicalFallbacks;
    u32 ownerFallbacks;
    u32 loadFallbacks;
    u32 scriptFallbacks;
    u32 spriteFallbacks;
    u32 visibilityFallbacks;
    u32 rotationFallbacks;
    u32 scaleFallbacks;
    u32 nonfiniteFallbacks;
    u32 textureFallbacks;
    u32 stateFallbacks;
    u32 axisFallbacks;
    u32 endpointFallbacks;
    u32 capacityFallbacks;
    u32 backendFallbacks;
    u32 verticesSaved;
    u32 frontendBytesSaved;
    u32 backendBytesSaved;
    u32 maxRunLength;
    u32 peakCandidatesPerPass;
    u32 peakVisiblePerPass;
    u32 peakEligiblePerPass;
    u32 peakRunsPerPass;
    u32 peakStageFrame;
    u32 cacheHits;
    u32 cacheRevalidations;
    u32 cacheGenerationChanges;
    u32 cacheValidationFailures;
    u32 semanticHash;
};

#if TH08_PSP_ITEM_TIME_DRAW_PAIR_ENABLED
const ItemTimeDrawPairStats &GetItemTimeDrawPairStats();
#endif

#if TH08_PSP_ANY_MIXED_QUADS_ENABLED
enum PspBulletMixedQuadRejectReason
{
    PSP_BULLET_MIXED_QUAD_ACCEPT = 0,
    PSP_BULLET_MIXED_QUAD_REJECT_NONFINITE,
    PSP_BULLET_MIXED_QUAD_REJECT_AXIS,
    PSP_BULLET_MIXED_QUAD_REJECT_AREA_OR_MIRROR,
    PSP_BULLET_MIXED_QUAD_REJECT_Z_OR_W,
    PSP_BULLET_MIXED_QUAD_REJECT_UV,
    PSP_BULLET_MIXED_QUAD_REJECT_DIFFUSE,
};

struct BulletMixedQuadStats
{
    u32 passes;
    u32 ownerConflictPasses;
    u32 stateRuns;
    u32 batches;
    u32 candidates;
    u32 eligiblePrefixQuads;
    u32 generalQuads;
    u32 stickyGeneralQuads;
    u32 nonfiniteFallbacks;
    u32 axisFallbacks;
    u32 areaOrMirrorFallbacks;
    u32 zOrWFallbacks;
    u32 uvFallbacks;
    u32 diffuseFallbacks;
    u32 submittedBatches;
    u32 submittedPairQuads;
    u32 submittedGeneralQuads;
    u32 backendFallbackBatches;
    u32 failClosedBatches;
    u32 missingRunBatches;
    u32 invalidRangeBatches;
    u32 canonicalRecoveryDrawFailures;
    u32 canonicalRecoveryQuads;
    // Potential savings proved by classification/staging. Bullet pair quads
    // save 2 frontend and 4 GE vertices; Bullet general quads save none. Item
    // pair quads save 4 frontend and 4 GE vertices, while Item general quads
    // save 2 frontend vertices. Backend fallback does not subtract from these
    // audit-facing opportunity counters.
    u32 frontendVerticesSaved;
    u32 geVerticesSaved;
    u32 maxPairPrefix;
    u32 maxGeneralSuffix;
};

#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
const BulletMixedQuadStats &GetBulletMixedQuadStats();
#endif
#if TH08_PSP_ITEM_MIXED_QUADS_ENABLED
using ItemMixedQuadStats = BulletMixedQuadStats;
const ItemMixedQuadStats &GetItemMixedQuadStats();
#endif
#endif
#endif

struct VertexDiffuseXyzrhw
{
    VertexDiffuseXyzrhw();

    Float3 pos;
    f32 w;
    D3DCOLOR diffuse;
};

struct VertexTex1DiffuseXyzrhw
{
    Float3 pos;
    float w;
    D3DCOLOR diffuse;
    Float2 textureUV;
};

// Target-observed 4-vertex textured quad without a diffuse color field.
struct VertexTex0Xyzrhw
{
    Float3 pos;
    float w;
    Float2 textureUV;
};
C_ASSERT(sizeof(VertexTex0Xyzrhw) == 0x18);

// Touhou 8 uses DirectX 8.1, but evidently Zun used some mismatched DirectX 8 headers as well
// D3DXIMAGE_INFO changed from 20 to 28 bytes between DX8 and DX8.1, but somehow IN uses the the DX8 version
// This struct is a redefinition of the DX8 D3DXIMAGE_INFO for that
// The only reason this ABI mismatch doesn't cause issues is because no surface indices are ever loaded other than 0
struct ZunImageInfo
{
    u32 Width;
    u32 Height;
    u32 Depth;
    u32 MipLevels;
    D3DFORMAT Format;
};
C_ASSERT(sizeof(ZunImageInfo) == 0x14);

enum AnmBlendMode
{
    AnmBlendMode_Unset = -1,
    AnmBlendMode_Normal,
    AnmBlendMode_Additive
};

enum AnmColorOp
{
    AnmColorOp_Unset = -1
};

enum AnmVertexShader
{
    AnmVertexShader_Unset = -1
};

enum AnmCameraMode
{
    AnmCameraMode_Unset = -1
};

enum AnmZWriteMode
{
    AnmZWriteMode_Unset = -1
};

enum AnmVariable
{
    AnmVariable_I0 = 10000,
    AnmVariable_I1,
    AnmVariable_I2,
    AnmVariable_I3,
    AnmVariable_F0,
    AnmVariable_F1,
    AnmVariable_F2,
    AnmVariable_F3,
    AnmVariable_IC0,
    AnmVariable_IC1,
};

enum AnmInterp
{
    AnmInterp_Pos,
    AnmInterp_RGB1,
    AnmInterp_Alpha1,
    AnmInterp_Rotate,
    AnmInterp_Scale,
    AnmInterp_RGB2,
    AnmInterp_Alpha2,
    AnmInterp_Last
};

enum AnmInterpMode
{
    AnmInterpMode_Linear = 0,
    AnmInterpMode_EaseIn = 1,
    AnmInterpMode_EaseInCubic = 2,
    AnmInterpMode_EaseInQuartic = 3,
    AnmInterpMode_EaseOut = 4,
    AnmInterpMode_EaseOutCubic = 5,
    AnmInterpMode_EaseOutQuartic = 6
};

enum AnmOpcode
{
    AnmOpcode_EndOfScript = -1,
    AnmOpcode_Nop = 0,
    AnmOpcode_Delete = 1,
    AnmOpcode_Static = 2,
    AnmOpcode_Sprite = 3,
    AnmOpcode_Jmp = 4,
    AnmOpcode_JmpDec = 5,
    AnmOpcode_Pos = 6,
    AnmOpcode_Scale = 7,
    AnmOpcode_Alpha = 8,
    AnmOpcode_Color = 9,
    AnmOpcode_FlipX = 10,
    AnmOpcode_FlipY = 11,
    AnmOpcode_Rotate = 12,
    AnmOpcode_AngularVelocity = 13,
    AnmOpcode_ScaleGrowth = 14,
    AnmOpcode_AlphaTimeLinear = 15,
    AnmOpcode_AdditiveBlendMode = 16,
    AnmOpcode_PosTimeLinear = 17,
    AnmOpcode_PosTimeDecel = 18,
    AnmOpcode_PosTimeDecel2 = 19,
    AnmOpcode_Stop = 20,
    AnmOpcode_InterruptLabel = 21,
    AnmOpcode_AnchorTopLeft = 22,
    AnmOpcode_StopHide = 23,
    AnmOpcode_PosMode = 24,
    AnmOpcode_Ins25 = 25,
    AnmOpcode_AddU = 26,
    AnmOpcode_AddV = 27,
    AnmOpcode_Visible = 28,
    AnmOpcode_ScaleTimeLinear = 29,
    AnmOpcode_ZWriteDisable = 30,
    AnmOpcode_Ins31 = 31,
    AnmOpcode_PosTime = 32,
    AnmOpcode_ColorTime = 33,
    AnmOpcode_AlphaTime = 34,
    AnmOpcode_RotateTime = 35,
    AnmOpcode_ScaleTime = 36,
    AnmOpcode_ISet = 37,
    AnmOpcode_FSet = 38,
    AnmOpcode_IAdd = 39,
    AnmOpcode_FAdd = 40,
    AnmOpcode_ISub = 41,
    AnmOpcode_FSub = 42,
    AnmOpcode_IMul = 43,
    AnmOpcode_FMul = 44,
    AnmOpcode_IDiv = 45,
    AnmOpcode_FDiv = 46,
    AnmOpcode_IMod = 47,
    AnmOpcode_FMod = 48,
    AnmOpcode_ISetAdd = 49,
    AnmOpcode_FSetAdd = 50,
    AnmOpcode_ISetSub = 51,
    AnmOpcode_FSetSub = 52,
    AnmOpcode_ISetMul = 53,
    AnmOpcode_FSetMul = 54,
    AnmOpcode_ISetDiv = 55,
    AnmOpcode_FSetDiv = 56,
    AnmOpcode_ISetMod = 57,
    AnmOpcode_FSetMod = 58,
    AnmOpcode_ISetRand = 59,
    AnmOpcode_FSetRand = 60,
    AnmOpcode_FSin = 61,
    AnmOpcode_FCos = 62,
    AnmOpcode_FTan = 63,
    AnmOpcode_FAcos = 64,
    AnmOpcode_FAtan = 65,
    AnmOpcode_NormalizeAngle = 66,
    AnmOpcode_IJmpEq = 67,
    AnmOpcode_FJmpEq = 68,
    AnmOpcode_IJmpNeq = 69,
    AnmOpcode_FJmpNeq = 70,
    AnmOpcode_IJmpLess = 71,
    AnmOpcode_FJmpLess = 72,
    AnmOpcode_IJmpLessOrEq = 73,
    AnmOpcode_FJmpLessOrEq = 74,
    AnmOpcode_IJmpGreater = 75,
    AnmOpcode_FJmpGreater = 76,
    AnmOpcode_IJmpGreaterOrEq = 77,
    AnmOpcode_FJmpGreaterOrEq = 78,
    AnmOpcode_Wait = 79,
    AnmOpcode_UScroll = 80,
    AnmOpcode_VScroll = 81,
    AnmOpcode_BlendMode = 82,
    AnmOpcode_Ins83 = 83,
    AnmOpcode_Color2 = 84,
    AnmOpcode_Alpha2 = 85,
    AnmOpcode_Color2Time = 86,
    AnmOpcode_Alpha2Time = 87,
    AnmOpcode_Ins88 = 88,
    AnmOpcode_ReturnFromInterrupt = 89
};

struct AnmEntry
{
    IDirect3DTexture8 *texture;
    u8 *rawData;
    i32 size;
};

C_ASSERT(sizeof(AnmEntry) == 0xc);

struct AnmRawEntry
{
    i32 numSprites;
    i32 numScripts;
    u32 textureIdx;
    i32 width;
    i32 height;
    u32 format;
    u32 colorKey;
    u32 nameOffset;
    u32 spriteIdxOffset;
    u32 mipmapNameOffset;
    u32 version;
    u32 priority;
    u32 textureOffset;
    u8 hasData;
    /* 3 bytes pad for alignment */
    u32 nextOffset;
    u32 serializedReserved3C;
};

C_ASSERT(sizeof(AnmRawEntry) == 0x40);
C_ASSERT(offsetof(AnmRawEntry, serializedReserved3C) == 0x3C);

struct AnmTextureHeader
{
    char magic[4]; /* THTX */
    u16 serializedReserved04;
    i16 format;
    i16 width;
    i16 height;
    u16 serializedReserved0C;
    u16 serializedReserved0E;
};

C_ASSERT(sizeof(AnmTextureHeader) == 0x10);
C_ASSERT(offsetof(AnmTextureHeader, serializedReserved04) == 0x04);
C_ASSERT(offsetof(AnmTextureHeader, serializedReserved0C) == 0x0C);
C_ASSERT(offsetof(AnmTextureHeader, serializedReserved0E) == 0x0E);

struct AnmLoadedSprite
{
    i32 anmIdx;
    IDirect3DTexture8 *texture;
    Float2 startPixelInclusive;
    Float2 endPixelInclusive;
    float height;
    float width;
    Float2 uvStart;
    Float2 uvEnd;
    float heightPx;
    float widthPx;
    Float2 scaleFactor;
    u32 unconsumedDword40;
};

C_ASSERT(sizeof(AnmLoadedSprite) == 0x44);
C_ASSERT(offsetof(AnmLoadedSprite, unconsumedDword40) == 0x40);

#define ANM_MAX_ARGS 10

struct AnmRawInstr
{
    i16 opcode;
    u16 instructionSize;
    i16 time;
    u16 varMask;
    union {
        i32 intArgs[ANM_MAX_ARGS];
        f32 floatArgs[ANM_MAX_ARGS];
        u8 byteArgs[ANM_MAX_ARGS * sizeof(i32)];
    };
};

struct AnmVmBase
{
    void SetBlendModeAdditive();
    void SetBlendModeNormal();

    void Initialize();

    ZunBool IsVisible()
    {
        return this->visible;
    }

    void SetInvisible()
    {
        this->visible = FALSE;
    }

    void SetInterrupt(i16 interrupt)
    {
        this->pendingInterrupt = interrupt;
    }

    Float3 rotation;
    Float3 angleVel;
    Float2 scale;
    Float2 scaleGrowth;
    Float2 spriteSize;
    Float2 uvScrollPos;
    ZunTimer currentTimeInScript;
    ZunTimer waitTimer;
    ZunTimer interpCurrentTimers[AnmInterp_Last];
    ZunTimer interpEndTimers[AnmInterp_Last];
    u8 interpModes[AnmInterp_Last];
    i32 intVar0;
    i32 intVar1;
    i32 intVar2;
    i32 intVar3;
    f32 floatVar0;
    f32 floatVar1;
    f32 floatVar2;
    f32 floatVar3;
    i32 counterVar0;
    i32 counterVar1;
    Float2 uvScrollVel;
    D3DXMATRIX matrix1;
    D3DXMATRIX matrix2;
    D3DXMATRIX matrix3;
    ZunColor color1;
    ZunColor color2;
    union {
        u16 flags;
        u32 flagsWord;
        struct
        {
            u32 visible : 1;
            u32 flag1 : 1;
            u32 updateRotation : 1;
            u32 updateScale : 1;
            u32 blendMode : 2;
            u32 flag6 : 1;
            u32 flag7 : 1;
            u32 usePosOffset : 1;
            u32 flip : 2;
            u32 anchor : 2;
            u32 zWriteDisabled : 1;
            u32 stopped : 1;
            u32 flag15 : 1;
            u32 flag16 : 1;
            u32 flag17 : 1;
            u32 flag18 : 1;
            u32 flag19 : 1;
        };
    };
    i16 type;
    i16 pendingInterrupt;
    i32 playerBulletHitAnimationType;
    AnmLoaded *anmFile;
};

C_ASSERT(sizeof(AnmVmBase) == 0x208);
C_ASSERT(offsetof(AnmVmBase, scale) == 0x18);
C_ASSERT(offsetof(AnmVmBase, color1) == 0x1F0);
C_ASSERT(offsetof(AnmVmBase, color2) == 0x1F4);
C_ASSERT(offsetof(AnmVmBase, flagsWord) == 0x1F8);

struct AnmVm : AnmVmBase
{
    void SetZRotation(f32 z)
    {
        this->rotation.z = z;
        this->updateRotation = 1;
    }

    Float3 pos;
    i16 activeSpriteIndex;
    i16 anmFileIndex;
    i16 baseSpriteIndex;
    i16 scriptIndex;
    AnmRawInstr *beginningOfScript;
    AnmRawInstr *currentInstruction;
    AnmLoadedSprite *loadedSprite;
    ZunTimer interruptReturnTime;
    AnmRawInstr *interruptReturnInstruction;

    Float3 posInitial;
    Float3 posFinal;
    Float3 rotateInitial;
    Float3 rotateFinal;
    Float2 scaleInitial;
    Float2 scaleFinal;
    ZunColor color1Initial;
    ZunColor color1Final;
    ZunColor color2Initial;
    ZunColor color2Final;

    Float3 pos2;
    i32 timeOfLastSpriteSet;
    u8 fontWidth;
    u8 fontHeight;
    u8 unconsumedTail29A[0xA];

    AnmVm()
    {
        memset(this, 0, sizeof(AnmVm));
        this->activeSpriteIndex = -1;
    }

    ZunBool IsStopped();
    i32 UpdatePulsingRadialTrail();
    void StartPositionInterpolation(i32 duration, i32 mode, Float3 *initial, Float3 *final);
    void StartColor1RgbInterpolation(i32 duration, i32 mode, u32 initial, u32 final);
    void StartColor1AlphaInterpolation(i32 duration, i32 mode, i32 initial, i32 final);
    void StartScaleInterpolation(i32 duration, i32 mode, Float2 *initial, Float2 *final);

    f32 GetFloatVar(f32 varId);
    i32 GetIntVar(i32 varId);
    f32 *GetFloatVarPtr(f32 *varPtr, u16 varMask, u32 variableNumber);
    i32 *GetIntVarPtr(i32 *varPtr, u16 varMask, u32 variableNumber);
};

C_ASSERT(sizeof(AnmVm) == 0x2a4);
C_ASSERT(offsetof(AnmVm, pos) == 0x208);
C_ASSERT(offsetof(AnmVm, unconsumedTail29A) == 0x29A);

typedef void (__fastcall *AnmProjectedPositionCallback)(
    AnmVm *vm, D3DXVECTOR3 *projectedPosition);

struct AnmLoaded
{
    i32 anmIdx;
    AnmRawEntry *rawData;
    i32 totalEntries;
    AnmLoadedSprite *sprites;
    AnmRawInstr **scripts;
    AnmEntry *textures;
    int numberEntriesToBeLoaded;

    void LoadSprite(i32 spriteIdx, AnmLoadedSprite *loadedSprite);

    void ExecuteAnmIdx(AnmVm *vm, int scriptIdx)
    {
        vm->scriptIndex = scriptIdx;

        vm->pos = Float3(0, 0, 0);
        vm->pos2 = Float3(0, 0, 0);

        vm->fontHeight = 15;
        vm->fontWidth = 15;

        this->SetAndExecuteScript(vm, this->scripts[scriptIdx]);
    }

    AnmLoadedSprite *GetSprite(int sprite)
    {
        return &this->sprites[sprite];
    }

    void InitializeAndSetSprite(AnmVm *vm, i32 sprite)
    {
        vm->Initialize();
        vm->anmFile = this;
        this->SetSprite(vm, sprite);
    }

    void SetAndExecuteScriptIdx(AnmVm *vm, int scriptIdx)
    {
        vm->anmFile = this;
        vm->scriptIndex = scriptIdx;
        this->SetAndExecuteScript(vm, this->scripts[scriptIdx]);
    }

    void ExecuteAnmIdxArray(AnmVm *vm, i32 scriptIdx, i32 count);
    ZunResult SetSprite(AnmVm *vm, int spriteIdx);
    void SetAndExecuteScript(AnmVm *vm, AnmRawInstr *beginningOfScript);
#if defined(PSP) && \
    ((defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT) && \
      TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT) || \
     (defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH) && \
      TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH) || \
     (defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) && \
      TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) || \
     (defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH) && \
      TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH))
    // Read-only generation/phase/table/range witnesses shared by the shadow
    // audit and its separately gated product.  They never wait for, finalize,
    // or otherwise mutate an ANM load.  Table bounds are proved before either
    // scripts[68] or sprites[179] is dereferenced; byte bounds are proved
    // before the four-instruction fingerprint is walked.
    u32 PspLoadGenerationForItemTimeSpawnInit() const;
    bool PspLoadReadyForItemTimeSpawnInit(u32 expectedGeneration) const;
    bool PspItemTimeSpawnInitTablesContain(u32 expectedGeneration,
                                           i32 scriptIndex,
                                           i32 spriteIndex) const;
    bool PspItemTimeSpawnInitScriptRangeContains(
        u32 expectedGeneration, const void *script, u32 byteCount) const;
#if defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT) && \
    TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT
    u32 PspLoadGenerationForSpawnInitAudit() const
    {
        return PspLoadGenerationForItemTimeSpawnInit();
    }
    bool PspLoadReadyForSpawnInitAudit(u32 expectedGeneration) const
    {
        return PspLoadReadyForItemTimeSpawnInit(expectedGeneration);
    }
#endif
#endif
};

C_ASSERT(sizeof(AnmLoaded) == 0x1c);

struct AnmRawSprite
{
    u32 id;
    float x;
    float y;
    float width;
    float height;
};

struct AnmRawScript
{
    u32 id;
    u32 offset;
};

struct AnmManager
{
    AnmManager();
    void SetupVertexBuffer();

    // FUNCTION: th08 0x43ef40 FOLDED
    ~AnmManager()
    {
    }
    ZunBool ExecuteScript(AnmVm *vm);
    void ExecuteScriptArray(AnmVm *sprites, int count);
    void SetRenderStateForVm(AnmVm *vm);
    void SetRenderStateForVm3D(AnmVm *vm);
    ZunResult ProjectCameraFacingQuad(AnmVm *vm);
    void Project3DQuad(AnmVm *vm);
    ZunResult ProjectCameraFacingQuadWithCallback(
        AnmVm *vm, AnmProjectedPositionCallback callback);
    ZunResult DrawInner(AnmVm *vm, i32 flags);
    ZunResult AddSpriteToDrawBuffer(VertexTex1DiffuseXyzrhw *vertices);
    ZunResult DrawNoRotation(AnmVm *vm);
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_BATCH) && \
    TH08_PSP_ASCII_POPUP_BATCH
    // Presentation-only score popup path. It validates the complete frame and
    // reserves backend storage before changing the shared VM, so any rejected
    // condition returns to AsciiManager's untouched canonical digit loop.
    ZunResult DrawPspAsciiPopupBatch(AnmVm *vm, AnmLoaded *asciiAnm,
                                     AsciiManagerPopup *popups,
                                     i32 popupCount, f32 playerX, f32 playerY,
                                     f32 popupScaleX, f32 popupScaleY);
#endif
    ZunResult Draw2DRotatedOrAxisAligned(AnmVm *vm);
    void TranslateRotation(VertexTex1DiffuseXyzrhw *vertex, float x, float y, float sine, float cosine, float xOffset,
                           float yOffset);
    ZunResult Draw2D(AnmVm *vm);
    // Draw2D-equivalent presentation path for callers which already own the
    // sine/cosine of vm->rotation.z. It still emits the same four-corner input
    // and six-vertex triangle list through DrawInner.
    ZunResult Draw2DWithPrecomputedRotation(AnmVm *vm, f32 sine, f32 cosine);
#if defined(PSP) && defined(TH08_PSP_BULLET_ONEPASS_4V_AUDIT) && \
    TH08_PSP_BULLET_ONEPASS_4V_AUDIT
    // M0 remains canonical-authoritative and compares the complete persistent
    // quad plus the exact 4V staging/state effects after every eligible draw.
    ZunResult DrawPspBulletOnePass4VAudit(AnmVm *vm, f32 sine, f32 cosine);
#endif
#if defined(PSP) && defined(TH08_PSP_BULLET_ONEPASS_4V_FASTPATH) && \
    TH08_PSP_BULLET_ONEPASS_4V_FASTPATH
    // Returns false before changing authoritative output when any owner,
    // state, input, or capacity proof fails; the caller then draws canonically.
    bool TryDrawPspBulletOnePass4V(AnmVm *vm, f32 sine, f32 cosine,
                                   ZunResult *result);
#endif
    void DrawPlayerBullet(AnmVm *vm);
    ZunResult DrawCameraFacingQuad(AnmVm *vm);
    ZunResult DrawProjected3DQuad(AnmVm *vm);
    ZunResult DrawWithCallback(AnmVm *vm, AnmProjectedPositionCallback callback);
    void SetCameraMode(i32 mode)
    {
        this->cameraMode = mode;
    }
    void Draw2DAndFlush(AnmVm *vm)
    {
        this->Draw2D(vm);
        this->FlushVertexBuffer();
    }
    ZunResult DrawNoRotationNoRound(AnmVm *vm);
    ZunResult Draw3D(AnmVm *vm);
    ZunResult DrawVertices(AnmVm *vm, VertexTex1DiffuseXyzrhw *vertices, i32 vertexCount);
    ZunResult InitializeHorizontalTextureStrip(AnmVm *vm, VertexTex1DiffuseXyzrhw *vertices, i32 vertexCount);
    ZunResult InitializeVerticalTextureStrip(AnmVm *vm, VertexTex1DiffuseXyzrhw *vertices, i32 vertexCount);
    ZunResult QueueSpriteQuad(AnmVm *vm, VertexTex1DiffuseXyzrhw *vertices);
    ZunResult DrawTriangleFan(AnmVm *vm, VertexDiffuseXyzrhw *vertices, i32 vertexCount);
    ZunResult CreateTextureFromFile(AnmEntry *entry, i32 format, i32 colorKey);
    ZunResult CreateTextureFromAnm(IDirect3DTexture8 **outTexture, AnmTextureHeader *textureData, i32 format);
    ZunResult CreateEmptyTexture(IDirect3DTexture8 **outTexture, i32 width, i32 height, i32 format);
    AnmLoaded *LoadAnm(i32 anmIdx, const char *filename);
    AnmLoaded *ReadAnmEntries(i32 anmIdx, const char *filename);
    AnmLoaded *GetAnm(i32 anmIdx)
    {
        return &this->anmFiles[anmIdx];
    }
    AnmLoaded *PreloadAnm(i32 anmIdx, const char *filename);
    i32 LoadExternalTextureData(AnmLoaded *anmLoaded, i32 entryNumber, i32 *sprites, i32 *scripts,
                                AnmRawEntry *rawEntry);
    AnmLoaded *PostloadAnmEntry(AnmLoaded *anm);
    BOOL LoadTextureData(AnmLoaded *anmLoaded, i32 entryNumber, i32 sprites, i32 scripts, AnmRawEntry *rawEntry);
    ZunResult ServicePreloadedAnims();
    void ReleaseAnm(i32 anmIdx);
    void ReleaseAnmEntry(AnmEntry *anmEntry);

    void DrawTextInner(IDirect3DTexture8 *outTexture, i32 x, i32 y, i32 width, i32 height, i32 fontWidth,
                       i32 fontHeight, COLORREF textColor, COLORREF outlineColor, const char *buffer,
                       float scaleFactorX, float scaleFactorY);
    void DrawTextLeft(AnmVm *vm, COLORREF textColor, COLORREF shadowColor, const char *fmt, ...);
    void DrawTextRight(AnmVm *vm, COLORREF textColor, COLORREF shadowColor, const char *fmt, ...);
    void DrawTextCentered(AnmVm *vm, COLORREF textColor, COLORREF shadowColor, const char *fmt, ...);
    ZunResult LoadSurface(i32 surfaceIdx, const char *path);
    ZunResult PreloadSurface(i32 surfaceIdx, const char *path);
    void ReleaseSurface(i32 surfaceIdx);
    void CopySurfaceToBackbuffer(int surfaceIdx, int left, int top, int x, int y);
    void CopySurfaceToBackbuffer2(i32 surfaceIdx, i32 destX, i32 destY, i32 srcX, i32 srcY, i32 width, i32 height);

    void ReleaseVertexBuffer()
    {
        SAFE_RELEASE(this->quadVertexBuffer);
    }

    void ClearBlendMode()
    {
        this->currentBlendMode = 3;
    }

    void ClearColorOp()
    {
        this->currentColorOp = AnmColorOp_Unset;
    }

    void ClearSprite()
    {
        this->currentSprite = NULL;
    }

    void ClearVertexShader()
    {
        this->currentVertexShader = AnmVertexShader_Unset;
    }

    void ClearTexture()
    {
        this->currentTexture = NULL;
    }

    void ClearCameraSettings()
    {
        this->cameraMode = AnmCameraMode_Unset;
    }

    void ClearZWrite()
    {
        this->disableZWrite = AnmZWriteMode_Unset;
    }

    void ResetFrameDebugInfo()
    {
        this->scriptsExecutedThisFrame = 0;
        this->renderStateChangesThisFrame = 0;
        this->scriptsStartedThisFrame = 0;
        this->flushesThisFrame = 0;
    }

    void SetInterruptArray(AnmVm *vm, int count, i16 interrupt);

    ZunBool SpriteHasTexture(AnmVm *vm);

    void ReleaseSurfaces()
    {
        i32 i;

        for (i = 0; i < ARRAY_SIZE_SIGNED(this->surfaces); i++)
        {
            if (this->surfaces[i] != NULL || this->surfacesBis[i] != NULL)
            {
                this->ReleaseSurface(i);
            }
        }
    }

    void TakeScreencaptures()
    {
        if (this->captureAnmIdx >= 0)
        {
            CaptureToTexture(this->captureAnmIdx, this->textureCaptureSrcX, this->textureCaptureSrcY,
                             this->textureCaptureSrcW, this->textureCaptureSrcH, this->textureCaptureDstX,
                             this->textureCaptureDstY, this->textureCaptureDstW, this->textureCaptureDstH);
            this->captureAnmIdx = -1;
        }

        TakePendingSurfaceCapture(false);
    }

    void TakePendingSurfaceCapture(bool readDisplayedFrame)
    {
        if (this->captureSurfaceIdx < 0)
            return;

        CaptureToSurface(this->captureSurfaceIdx, this->surfaceCaptureSrcX, this->surfaceCaptureSrcY,
                         this->surfaceCaptureSrcW, this->surfaceCaptureSrcH, this->surfaceCaptureDstX,
                         this->surfaceCaptureDstY, this->surfaceCaptureDstW, this->surfaceCaptureDstH,
                         readDisplayedFrame);
        this->captureSurfaceIdx = -1;
    }

    void SetMixColorDefault()
    {
        this->useMixColor = FALSE;
        this->color.d3dColor = 0x80808080;
    }

    void SetMixColor(D3DCOLOR color)
    {
        this->useMixColor = TRUE;
        this->color.d3dColor = color;
    }

    void RequestCapture(i32 captureSurfaceIdx, i32 srcX, i32 srcY, i32 srcW, i32 srcH, i32 dstX, i32 dstY, i32 dstW,
                        i32 dstH)
    {
        if (this->captureSurfaceIdx >= 0)
        {
            return;
        }

#if defined(PSP)
        // Win the phase scratch before the setup worker is allowed to begin
        // loading ANMs. Capture returns it in the same Present() that consumes
        // this request, so no gameplay state or frame timing is altered.
        if (!th08::psp::AnmScratchReserveTransition(
                "frame transition capture"))
        {
            // Never present an old title/logo surface as a failed transition.
            if (captureSurfaceIdx >= 0 &&
                captureSurfaceIdx < ARRAY_SIZE_SIGNED(this->surfaces) &&
                (this->surfaces[captureSurfaceIdx] != NULL ||
                 this->surfacesBis[captureSurfaceIdx] != NULL))
            {
                this->ReleaseSurface(captureSurfaceIdx);
            }
            return;
        }
#endif
        this->captureSurfaceIdx = captureSurfaceIdx;
        this->surfaceCaptureSrcX = srcX;
        this->surfaceCaptureSrcY = srcY;
        this->surfaceCaptureSrcW = srcW;
        this->surfaceCaptureSrcH = srcH;
        this->surfaceCaptureDstX = dstX;
        this->surfaceCaptureDstY = dstY;
        this->surfaceCaptureDstW = dstW;
        this->surfaceCaptureDstH = dstH;
    }

    void ReplaceSurface(i32 destIndex, i32 srcIndex);

    void CaptureToTexture(i32 captureAnmIdx, i32 srcX, i32 srcY, i32 srcW, i32 srcH, i32 dstX, i32 dstY, i32 dstW,
                          i32 dstH);
    ZunResult SetTextureCaptureParams(u32 captureAnmIdx, u32 srcX, u32 srcY, u32 srcW, u32 srcH, u32 dstX,
                                      u32 dstY, u32 dstW, u32 dstH)
    {
        if (this->captureAnmIdx >= 0)
            return ZUN_ERROR;

        this->captureAnmIdx = captureAnmIdx;
        this->textureCaptureSrcX = srcX;
        this->textureCaptureSrcY = srcY;
        this->textureCaptureSrcW = srcW;
        this->textureCaptureSrcH = srcH;
        this->textureCaptureDstX = dstX;
        this->textureCaptureDstY = dstY;
        this->textureCaptureDstW = dstW;
        this->textureCaptureDstH = dstH;
        return ZUN_SUCCESS;
    }
    void CopyTextureRect(i32 dstAnmIdx, i32 dstEntryIdx, i32 srcAnmIdx, i32 srcEntryIdx, RECT *dstRect,
                         RECT *srcRect);
    void CaptureToSurface(i32 captureSurfaceIdx, i32 srcX, i32 srcY, i32 srcW, i32 srcH, i32 dstX, i32 dstY, i32 dstW,
                          i32 dstH, bool readDisplayedFrame = false);

    void ClearVertexBuffer();
    void FlushVertexBuffer();
#if TH08_PSP_ITEM_TIME_DRAW_PAIR_ENABLED
    // ItemManager brackets one linked-list traversal.  Internally the staging
    // owner is generic (ItemTime today, Bullet later), while this public gate
    // keeps the current experiment isolated to ITEM_TIME.
    void ResetPspItemTimeDrawPairStats();
    void BeginPspItemTimeDrawPairPass();
    void EndPspItemTimeDrawPairPass();
    void PspItemTimeDrawPairBoundary();
    PspItemTimeDrawPairRejectReason PspValidateItemTimeDrawPairIdentity(
        const AnmVm *vm, AnmLoaded *owner, i32 expectedSpriteIndex);
#if defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) && \
    TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT
    ZunResult DrawPspItemTimePairAudit(
        AnmVm *vm, PspItemTimeDrawPairRejectReason identityReason);
#endif
#if TH08_PSP_ITEM_TIME_DRAW_PAIR_PRODUCT_ENABLED
    bool TryDrawPspItemTimeSpritePair(
        AnmVm *vm, PspItemTimeDrawPairRejectReason identityReason);
#endif
#endif
#if defined(PSP) && defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
    TH08_PSP_BULLET_UNIFIED_QUADS
    void BeginPspBulletUnifiedQuadBatch();
    void EndPspBulletUnifiedQuadBatch();
#if defined(TH08_PSP_EFFECT_INDEXED_QUADS) && \
    TH08_PSP_EFFECT_INDEXED_QUADS
    void BeginPspEffectIndexedQuadBatch();
    void EndPspEffectIndexedQuadBatch();
#endif
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
    void ResetPspBulletMixedQuadStats();
#endif
#if TH08_PSP_ITEM_MIXED_QUADS_ENABLED
    void BeginPspItemMixedQuadBatch();
    void EndPspItemMixedQuadBatch();
    void ResetPspItemMixedQuadStats();
#endif
#endif
#if defined(PSP) && defined(TH08_PSP_ITEM_DIRECT_GE) && \
    TH08_PSP_ITEM_DIRECT_GE
    void BeginPspItemUnifiedQuadBatch();
    void EndPspItemUnifiedQuadBatch();
#endif

    ZunColor color;
    ZunBool useMixColor;
    i32 captureSurfaceIdx;
    u32 scriptsStartedThisFrame;
    u32 scriptsExecutedThisFrame;
    u32 renderStateChangesThisFrame;
    u32 flushesThisFrame;
    Float2 screenShakeOffset;
    AnmLoaded anmFiles[256];
    D3DXMATRIX cachedWorldMatrix;
    AnmVm unconsumedVm1C64;
    u8 unconsumedBytes1F08[0x130];

    IDirect3DSurface8 *surfaces[32];
    IDirect3DSurface8 *surfacesBis[32];
    u8 *surfaceData[32];
    u32 surfaceDataSizes[32];
    ZunImageInfo surfaceInfo[32];

    D3DCOLOR currentTextureFactor;

    IDirect3DTexture8 *currentTexture;
    u8 currentBlendMode;
    u8 currentColorOp;
    u8 currentVertexShader;
    u8 disableZWrite;
    u8 cameraMode;
    u8 needsTextureFactorSetup;
    AnmLoadedSprite *currentSprite;
    IDirect3DVertexBuffer8 *quadVertexBuffer;
    VertexDiffuseXyzrhw untexturedVector[4];
    u32 spritesToDraw;
    VertexTex1DiffuseXyzrhw vertexBuffer[0x18000];
    VertexTex1DiffuseXyzrhw *vertexBufferEndPtr;
    VertexTex1DiffuseXyzrhw *vertexBufferStartPtr;
    i32 captureAnmIdx;
    i32 textureCaptureSrcX;
    i32 textureCaptureSrcY;
    i32 textureCaptureSrcW;
    i32 textureCaptureSrcH;
    i32 textureCaptureDstX;
    i32 textureCaptureDstY;
    i32 textureCaptureDstW;
    i32 textureCaptureDstH;
    i32 surfaceCaptureSrcX;
    i32 surfaceCaptureSrcY;
    i32 surfaceCaptureSrcW;
    i32 surfaceCaptureSrcH;
    i32 surfaceCaptureDstX;
    i32 surfaceCaptureDstY;
    i32 surfaceCaptureDstW;
    i32 surfaceCaptureDstH;
};
C_ASSERT(sizeof(AnmManager) == 0x2a2570);
C_ASSERT(offsetof(AnmManager, currentTextureFactor) == 0x24B8);
C_ASSERT(offsetof(AnmManager, scriptsStartedThisFrame) == 0x0C);
C_ASSERT(offsetof(AnmManager, unconsumedVm1C64) == 0x1C64);
C_ASSERT(offsetof(AnmManager, unconsumedBytes1F08) == 0x1F08);
C_ASSERT(offsetof(AnmManager, surfaces) == 0x2038);
C_ASSERT(offsetof(AnmManager, needsTextureFactorSetup) == 0x24C5);
C_ASSERT(offsetof(AnmManager, currentSprite) == 0x24C8);

DIFFABLE_EXTERN(AnmManager *, g_AnmManager);

}; // namespace th08
