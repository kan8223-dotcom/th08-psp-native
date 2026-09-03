#include "th_pch.h"

#include "EclManager.hpp"
#include "EclOperands.hpp"
#include "AnmManager.hpp"
#include "Background.hpp"
#include "ItemManager.hpp"
#include "ReplayManager.hpp"
#include "GameManager.hpp"
#include "EnemyManager.hpp"
#include "Player.hpp"

#if defined(PSP)
#include "perf_attribution.hpp"
#include "radial_trail_telemetry.hpp"
#include "radial_trig_reuse.hpp"
#include "render_perf_telemetry.hpp"
#endif

#if defined(PSP) && defined(TH08_PSP_EFFECT_OCCUPANCY_FASTPATH) && \
    TH08_PSP_EFFECT_OCCUPANCY_FASTPATH
#define TH08_PSP_EFFECT_OCCUPANCY_FASTPATH_ENABLED 1
#else
#define TH08_PSP_EFFECT_OCCUPANCY_FASTPATH_ENABLED 0
#endif
#if defined(PSP) && defined(TH08_PSP_EFFECT_OCCUPANCY_AUDIT) && \
    TH08_PSP_EFFECT_OCCUPANCY_AUDIT
#define TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED 1
#else
#define TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED 0
#endif
#if TH08_PSP_EFFECT_OCCUPANCY_FASTPATH_ENABLED && TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED
#error "EFFECT_OCCUPANCY fastpath and audit switches are mutually exclusive"
#endif
// The sidecar (Mark/Reset/Forget) is maintained for both the product skip
// and the shadow audit; only the product may skip a slot.
#define TH08_PSP_EFFECT_OCCUPANCY_SIDECAR_ENABLED \
    (TH08_PSP_EFFECT_OCCUPANCY_FASTPATH_ENABLED || TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED)
#if TH08_PSP_EFFECT_OCCUPANCY_SIDECAR_ENABLED
#include "PspEffectOccupancy.hpp"
#if TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED
#include "effect_occupancy_audit.hpp"
#endif
#endif

#if defined(PSP) && \
    (((defined(TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT) && \
       TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT)) || \
     ((defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
       TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH)))
#include "effect_sprite_pair_audit.hpp"
#include "fileio.hpp"
#endif

#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
#include "modern/linux/d3d8_internal.hpp"
#endif

namespace th08
{

ZunBool IsDisableResourceReload();

void __fastcall AdjustStageEffectDrawPosition(AnmVm *effect, D3DXVECTOR3 *base);
i32 __fastcall HasAnimationEnded(Effect *effect);
i32 __fastcall DrawRadialTrail(Effect *effect);


























DIFFABLE_STATIC(EffectManager, g_EffectManager);
DIFFABLE_STATIC(ChainElem, g_EffectManagerCalcChain);
DIFFABLE_STATIC(ChainElem, g_EffectManagerDrawChain);

#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT && \
    defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
#error "Effect sprite-pair M0 and product modes are mutually exclusive"
#endif

#if defined(PSP)
namespace
{
// Keep OFF and product BSS geometry identical so a performance A/B does not
// move any later gameplay object.  The reservation contains counters only;
// pair vertices and replay-authority VM pointers reuse AnmManager's existing
// canonical staging array.
constexpr u32 kPspEffectSpritePairProductStorageBytes = 256U;
alignas(4) u8 g_PspEffectSpritePairProductStorage
    [kPspEffectSpritePairProductStorageBytes] __attribute__((used));
} // namespace
#endif

#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT
#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED
#error "Effect sprite-pair M0 requires the canonical six-vertex Effect path"
#endif

// DrawNoRotation owns this persistent scratch in AnmManager.cpp.  M0 observes
// it only after first taking an independent copy and always lets Draw2D remain
// the sole authoritative writer/submission path.
extern VertexTex1DiffuseXyzrhw g_QuadVertices[4];

namespace
{
enum PspEffectSpritePairFallback : u32
{
    PSP_EFFECT_PAIR_FALLBACK_NONE = 0,
    PSP_EFFECT_PAIR_FALLBACK_SLOT,
    PSP_EFFECT_PAIR_FALLBACK_ORDER,
    PSP_EFFECT_PAIR_FALLBACK_CALLBACK,
    PSP_EFFECT_PAIR_FALLBACK_ROTATION,
    PSP_EFFECT_PAIR_FALLBACK_VISIBILITY,
    PSP_EFFECT_PAIR_FALLBACK_SPRITE,
    PSP_EFFECT_PAIR_FALLBACK_TEXTURE,
    PSP_EFFECT_PAIR_FALLBACK_SCALE,
    PSP_EFFECT_PAIR_FALLBACK_NONFINITE,
    PSP_EFFECT_PAIR_FALLBACK_Z_OR_W,
    PSP_EFFECT_PAIR_FALLBACK_DIFFUSE,
    PSP_EFFECT_PAIR_FALLBACK_AXIS,
    PSP_EFFECT_PAIR_FALLBACK_UV,
    PSP_EFFECT_PAIR_FALLBACK_AREA_OR_MIRROR,
    PSP_EFFECT_PAIR_FALLBACK_STATE,
    PSP_EFFECT_PAIR_FALLBACK_CANONICAL_MISMATCH,
    PSP_EFFECT_PAIR_FALLBACK_COUNT,
};

struct PspEffectSpritePairAuditStats
{
    u32 passes;
    u32 candidates;
    u32 canonicalDraws;
    u32 builtQuads;
    u32 visibleQuads;
    u32 culledQuads;
    u32 eligiblePairs;
    u32 eligibleCulls;
    u32 canonicalFallbacks;
    u32 slotMatches;
    u32 slotMismatches;
    u32 orderMatches;
    u32 orderMismatches;
    u32 effectMatches;
    u32 effectMismatches;
    u32 vmMatches;
    u32 vmMismatches;
    u32 quadMatches;
    u32 quadMismatches;
    u32 positionMatches;
    u32 positionMismatches;
    u32 uvMatches;
    u32 uvMismatches;
    u32 colorMatches;
    u32 colorMismatches;
    u32 pairMatches;
    u32 pairMismatches;
    u32 bufferMatches;
    u32 bufferMismatches;
    u32 stateMatches;
    u32 stateMismatches;
    u32 textureMatches;
    u32 textureMismatches;
    u32 blendMatches;
    u32 blendMismatches;
    u32 mismatchDetailLogs;
    u32 orderHash;
    u32 fallbackCounts[PSP_EFFECT_PAIR_FALLBACK_COUNT];
};

struct PspEffectSpritePairManagerState
{
    IDirect3DTexture8 *texture;
    AnmLoadedSprite *sprite;
    VertexTex1DiffuseXyzrhw *bufferStart;
    VertexTex1DiffuseXyzrhw *bufferEnd;
    D3DCOLOR textureFactor;
    ZunColor mixColor;
    Float2 screenShake;
    u32 spritesToDraw;
    u32 renderStateChanges;
    u32 flushes;
    ZunBool useMixColor;
    u8 blendMode;
    u8 colorOp;
    u8 vertexShader;
    u8 disableZWrite;
    u8 cameraMode;
    u8 needsTextureFactorSetup;
};

constexpr u32 kPspEffectSpritePairOrderHashOffset = 2166136261U;
constexpr u32 kPspEffectSpritePairOrderHashPrime = 16777619U;
constexpr u32 kPspEffectSpritePairReportPeriod = 600U;
PspEffectSpritePairAuditStats g_PspEffectSpritePairAuditStats{};
u32 g_PspEffectSpritePairAuditFrame = 0xffffffffU;
u32 g_PspEffectSpritePairAuditNextReportFrame =
    kPspEffectSpritePairReportPeriod;
u32 g_PspEffectSpritePairAuditLastReportedPasses = 0U;
u32 g_PspEffectSpritePairAuditGeneration = 0U;

void PspEffectSpritePairOrderHashByte(u8 value)
{
    g_PspEffectSpritePairAuditStats.orderHash ^= value;
    g_PspEffectSpritePairAuditStats.orderHash *=
        kPspEffectSpritePairOrderHashPrime;
}

void PspEffectSpritePairOrderHashU32(u32 value)
{
    PspEffectSpritePairOrderHashByte(static_cast<u8>(value));
    PspEffectSpritePairOrderHashByte(static_cast<u8>(value >> 8));
    PspEffectSpritePairOrderHashByte(static_cast<u8>(value >> 16));
    PspEffectSpritePairOrderHashByte(static_cast<u8>(value >> 24));
}

void PspEffectSpritePairReport(u32 lastDrawFrame, u32 crossedStageFrame,
                               const char *reason)
{
    const PspEffectSpritePairAuditStats &stats =
        g_PspEffectSpritePairAuditStats;
    th08::psp::BootLog(
            "TH08PSP EFFECT_SPRITE_PAIR_M0 frame=%lu generation=%lu reason=%s "
            "last_draw_frame=%lu crossed_stage_frame=%lu passes=%lu "
            "candidates=%lu eligible_pairs=%lu eligible_culls=%lu "
            "fallbacks=%lu slot_mismatch=%lu order_mismatch=%lu "
            "effect_mismatch=%lu vm_mismatch=%lu quad_mismatch=%lu "
            "position_mismatch=%lu uv_mismatch=%lu color_mismatch=%lu "
            "pair_mismatch=%lu buffer_mismatch=%lu state_mismatch=%lu "
            "texture_mismatch=%lu blend_mismatch=%lu order_hash=%08lx\n",
            static_cast<unsigned long>(lastDrawFrame),
            static_cast<unsigned long>(g_PspEffectSpritePairAuditGeneration),
            reason,
            static_cast<unsigned long>(lastDrawFrame),
            static_cast<unsigned long>(crossedStageFrame),
            static_cast<unsigned long>(stats.passes),
            static_cast<unsigned long>(stats.candidates),
            static_cast<unsigned long>(stats.eligiblePairs),
            static_cast<unsigned long>(stats.eligibleCulls),
            static_cast<unsigned long>(stats.canonicalFallbacks),
            static_cast<unsigned long>(stats.slotMismatches),
            static_cast<unsigned long>(stats.orderMismatches),
            static_cast<unsigned long>(stats.effectMismatches),
            static_cast<unsigned long>(stats.vmMismatches),
            static_cast<unsigned long>(stats.quadMismatches),
            static_cast<unsigned long>(stats.positionMismatches),
            static_cast<unsigned long>(stats.uvMismatches),
            static_cast<unsigned long>(stats.colorMismatches),
            static_cast<unsigned long>(stats.pairMismatches),
            static_cast<unsigned long>(stats.bufferMismatches),
            static_cast<unsigned long>(stats.stateMismatches),
            static_cast<unsigned long>(stats.textureMismatches),
            static_cast<unsigned long>(stats.blendMismatches),
            static_cast<unsigned long>(stats.orderHash));
    th08::psp::BootLog(
            "TH08PSP EFFECT_SPRITE_PAIR_M0_FALLBACK frame=%lu generation=%lu "
            "reason=%s last_draw_frame=%lu crossed_stage_frame=%lu "
            "slot=%lu order=%lu callback=%lu rotation=%lu "
            "visibility=%lu sprite=%lu texture=%lu scale=%lu "
            "nonfinite=%lu z_or_w=%lu diffuse=%lu axis=%lu uv=%lu "
            "area_or_mirror=%lu state=%lu canonical_mismatch=%lu\n",
            static_cast<unsigned long>(lastDrawFrame),
            static_cast<unsigned long>(g_PspEffectSpritePairAuditGeneration),
            reason,
            static_cast<unsigned long>(lastDrawFrame),
            static_cast<unsigned long>(crossedStageFrame),
            static_cast<unsigned long>(
                stats.fallbackCounts[PSP_EFFECT_PAIR_FALLBACK_SLOT]),
            static_cast<unsigned long>(
                stats.fallbackCounts[PSP_EFFECT_PAIR_FALLBACK_ORDER]),
            static_cast<unsigned long>(
                stats.fallbackCounts[PSP_EFFECT_PAIR_FALLBACK_CALLBACK]),
            static_cast<unsigned long>(
                stats.fallbackCounts[PSP_EFFECT_PAIR_FALLBACK_ROTATION]),
            static_cast<unsigned long>(
                stats.fallbackCounts[PSP_EFFECT_PAIR_FALLBACK_VISIBILITY]),
            static_cast<unsigned long>(
                stats.fallbackCounts[PSP_EFFECT_PAIR_FALLBACK_SPRITE]),
            static_cast<unsigned long>(
                stats.fallbackCounts[PSP_EFFECT_PAIR_FALLBACK_TEXTURE]),
            static_cast<unsigned long>(
                stats.fallbackCounts[PSP_EFFECT_PAIR_FALLBACK_SCALE]),
            static_cast<unsigned long>(
                stats.fallbackCounts[PSP_EFFECT_PAIR_FALLBACK_NONFINITE]),
            static_cast<unsigned long>(
                stats.fallbackCounts[PSP_EFFECT_PAIR_FALLBACK_Z_OR_W]),
            static_cast<unsigned long>(
                stats.fallbackCounts[PSP_EFFECT_PAIR_FALLBACK_DIFFUSE]),
            static_cast<unsigned long>(
                stats.fallbackCounts[PSP_EFFECT_PAIR_FALLBACK_AXIS]),
            static_cast<unsigned long>(
                stats.fallbackCounts[PSP_EFFECT_PAIR_FALLBACK_UV]),
            static_cast<unsigned long>(stats.fallbackCounts[
                PSP_EFFECT_PAIR_FALLBACK_AREA_OR_MIRROR]),
            static_cast<unsigned long>(
                stats.fallbackCounts[PSP_EFFECT_PAIR_FALLBACK_STATE]),
            static_cast<unsigned long>(stats.fallbackCounts[
                PSP_EFFECT_PAIR_FALLBACK_CANONICAL_MISMATCH]));
    g_PspEffectSpritePairAuditLastReportedPasses = stats.passes;
}

void PspEffectSpritePairResetGeneration()
{
    memset(&g_PspEffectSpritePairAuditStats, 0,
           sizeof(g_PspEffectSpritePairAuditStats));
    g_PspEffectSpritePairAuditStats.orderHash =
        kPspEffectSpritePairOrderHashOffset;
    g_PspEffectSpritePairAuditFrame = 0xffffffffU;
    g_PspEffectSpritePairAuditNextReportFrame =
        kPspEffectSpritePairReportPeriod;
    g_PspEffectSpritePairAuditLastReportedPasses = 0U;
    ++g_PspEffectSpritePairAuditGeneration;
}

void PspEffectSpritePairFinalize(const char *reason)
{
    if (g_PspEffectSpritePairAuditFrame != 0xffffffffU &&
        g_PspEffectSpritePairAuditStats.passes !=
            g_PspEffectSpritePairAuditLastReportedPasses)
    {
        PspEffectSpritePairReport(g_PspEffectSpritePairAuditFrame, 0U,
                                  reason);
    }
    g_PspEffectSpritePairAuditFrame = 0xffffffffU;
    g_PspEffectSpritePairAuditNextReportFrame =
        kPspEffectSpritePairReportPeriod;
}

void PspEffectSpritePairBeginFrame()
{
    const u32 frame = g_GameManager.stageActiveFrames;
    if (g_PspEffectSpritePairAuditStats.orderHash == 0U)
        g_PspEffectSpritePairAuditStats.orderHash =
            kPspEffectSpritePairOrderHashOffset;
    if (frame == g_PspEffectSpritePairAuditFrame)
        return;

    if (g_PspEffectSpritePairAuditFrame != 0xffffffffU)
    {
        if (frame > g_PspEffectSpritePairAuditFrame &&
            frame >= g_PspEffectSpritePairAuditNextReportFrame)
        {
            // Rendering at 1/2 or 1/3 cadence can skip the exact 600-frame
            // residue forever. Report the last fully observed draw as soon
            // as a stage-frame boundary is crossed instead of requiring an
            // exact modulo hit.
            PspEffectSpritePairReport(
                g_PspEffectSpritePairAuditFrame,
                g_PspEffectSpritePairAuditNextReportFrame, "periodic");
            g_PspEffectSpritePairAuditNextReportFrame =
                (frame / kPspEffectSpritePairReportPeriod + 1U) *
                kPspEffectSpritePairReportPeriod;
        }
        else if (frame < g_PspEffectSpritePairAuditFrame)
        {
            // Normal lifetimes flush through ReleaseEffectResources. Keep a
            // fail-safe for a stage-frame rewind so an unusual chain reset
            // cannot merge two generations into one diagnostic total.
            PspEffectSpritePairFinalize("frame_rewind");
            PspEffectSpritePairResetGeneration();
        }
    }
    g_PspEffectSpritePairAuditFrame = frame;
}

void PspEffectSpritePairNoteFallback(PspEffectSpritePairFallback reason)
{
    ++g_PspEffectSpritePairAuditStats.canonicalFallbacks;
    if (reason > PSP_EFFECT_PAIR_FALLBACK_NONE &&
        reason < PSP_EFFECT_PAIR_FALLBACK_COUNT)
    {
        ++g_PspEffectSpritePairAuditStats.fallbackCounts[reason];
    }
}

void PspEffectSpritePairNoteMismatch(const char *kind, u32 group, u32 slot,
                                     u32 ordinal)
{
    const PspEffectSpritePairAuditStats &stats =
        g_PspEffectSpritePairAuditStats;
    const u32 mismatches = stats.effectMismatches + stats.vmMismatches +
        stats.quadMismatches + stats.positionMismatches +
        stats.uvMismatches + stats.colorMismatches +
        stats.pairMismatches + stats.bufferMismatches +
        stats.stateMismatches + stats.textureMismatches +
        stats.blendMismatches + stats.slotMismatches +
        stats.orderMismatches;
    if (g_PspEffectSpritePairAuditStats.mismatchDetailLogs >= 8U)
        return;
    ++g_PspEffectSpritePairAuditStats.mismatchDetailLogs;
    th08::psp::BootLog(
            "TH08PSP EFFECT_SPRITE_PAIR_M0 mismatch=%s group=%lu "
            "slot=%lu ordinal=%lu detail=%lu/8 total_direct=%lu\n",
            kind, static_cast<unsigned long>(group),
            static_cast<unsigned long>(slot),
            static_cast<unsigned long>(ordinal),
            static_cast<unsigned long>(
                g_PspEffectSpritePairAuditStats.mismatchDetailLogs),
            static_cast<unsigned long>(mismatches));
}

PspEffectSpritePairManagerState PspCaptureEffectSpritePairManagerState(
    const AnmManager *manager)
{
    PspEffectSpritePairManagerState state{};
    state.texture = manager->currentTexture;
    state.sprite = manager->currentSprite;
    state.bufferStart = manager->vertexBufferStartPtr;
    state.bufferEnd = manager->vertexBufferEndPtr;
    state.textureFactor = manager->currentTextureFactor;
    state.mixColor = manager->color;
    state.screenShake = manager->screenShakeOffset;
    state.spritesToDraw = manager->spritesToDraw;
    state.renderStateChanges = manager->renderStateChangesThisFrame;
    state.flushes = manager->flushesThisFrame;
    state.useMixColor = manager->useMixColor;
    state.blendMode = manager->currentBlendMode;
    state.colorOp = manager->currentColorOp;
    state.vertexShader = manager->currentVertexShader;
    state.disableZWrite = manager->disableZWrite;
    state.cameraMode = manager->cameraMode;
    state.needsTextureFactorSetup = manager->needsTextureFactorSetup;
    return state;
}

bool PspEffectSpritePairImmutableManagerStateMatches(
    const PspEffectSpritePairManagerState &before,
    const AnmManager *manager)
{
    return manager->currentSprite == before.sprite &&
           manager->currentTextureFactor == before.textureFactor &&
           manager->color.d3dColor == before.mixColor.d3dColor &&
           manager->useMixColor == before.useMixColor &&
           th08::psp::EffectSpritePairFloatBitsEqual(
               manager->screenShakeOffset.x, before.screenShake.x) &&
           th08::psp::EffectSpritePairFloatBitsEqual(
               manager->screenShakeOffset.y, before.screenShake.y) &&
           manager->currentColorOp == before.colorOp &&
           manager->cameraMode == before.cameraMode &&
           manager->needsTextureFactorSetup ==
               before.needsTextureFactorSetup;
}

bool PspEffectSpritePairSlotIndex(const EffectManager *effectManager,
                                  const Effect *effect, u32 *slot)
{
    if (effectManager == NULL || effect == NULL || slot == NULL)
        return false;
    const uintptr_t begin =
        reinterpret_cast<uintptr_t>(&effectManager->effects[0]);
    const uintptr_t end =
        reinterpret_cast<uintptr_t>(&effectManager->effects[653]);
    const uintptr_t address = reinterpret_cast<uintptr_t>(effect);
    if (address < begin || address >= end ||
        (address - begin) % sizeof(Effect) != 0U)
    {
        return false;
    }
    *slot = static_cast<u32>((address - begin) / sizeof(Effect));
    return true;
}

bool PspEffectSpritePairPositionBytesEqual(
    const VertexTex1DiffuseXyzrhw *left,
    const VertexTex1DiffuseXyzrhw *right)
{
    for (u32 index = 0U; index < 4U; ++index)
    {
        if (memcmp(&left[index].pos, &right[index].pos,
                   sizeof(left[index].pos)) != 0)
        {
            return false;
        }
    }
    return true;
}

bool PspEffectSpritePairUvBytesEqual(
    const VertexTex1DiffuseXyzrhw *left,
    const VertexTex1DiffuseXyzrhw *right)
{
    for (u32 index = 0U; index < 4U; ++index)
    {
        if (memcmp(&left[index].textureUV, &right[index].textureUV,
                   sizeof(left[index].textureUV)) != 0)
        {
            return false;
        }
    }
    return true;
}

bool PspEffectSpritePairColorBytesEqual(
    const VertexTex1DiffuseXyzrhw *left,
    const VertexTex1DiffuseXyzrhw *right)
{
    for (u32 index = 0U; index < 4U; ++index)
    {
        if (left[index].diffuse != right[index].diffuse)
            return false;
    }
    return true;
}

u8 PspEffectSpritePairMixColors(u8 left, u8 right)
{
    u32 color = (static_cast<u32>(left) *
                 static_cast<u32>(right)) / 128U;
    if (color >= 256U)
        color = 255U;
    return static_cast<u8>(color);
}

void PspEffectSpritePairSetDiffuse(AnmManager *manager, const AnmVm *vm,
                                   VertexTex1DiffuseXyzrhw *quad)
{
    ZunColor color;
    color.d3dColor = vm->flag17 ? vm->color2.d3dColor
                                : vm->color1.d3dColor;
    if (manager->useMixColor)
    {
        color.r = PspEffectSpritePairMixColors(
            color.r, manager->color.r);
        color.g = PspEffectSpritePairMixColors(
            color.g, manager->color.g);
        color.b = PspEffectSpritePairMixColors(
            color.b, manager->color.b);
        color.a = PspEffectSpritePairMixColors(
            color.a, manager->color.a);
    }
    quad[0].diffuse = quad[1].diffuse =
        quad[2].diffuse = quad[3].diffuse = color.d3dColor;
}

PspEffectSpritePairFallback PspEffectSpritePairClassFallback(
    th08::psp::EffectSpritePairQuadClass quadClass)
{
    switch (quadClass)
    {
    case th08::psp::EffectSpritePairQuadClass::Accept:
        return PSP_EFFECT_PAIR_FALLBACK_NONE;
    case th08::psp::EffectSpritePairQuadClass::Nonfinite:
        return PSP_EFFECT_PAIR_FALLBACK_NONFINITE;
    case th08::psp::EffectSpritePairQuadClass::ZOrW:
        return PSP_EFFECT_PAIR_FALLBACK_Z_OR_W;
    case th08::psp::EffectSpritePairQuadClass::Diffuse:
        return PSP_EFFECT_PAIR_FALLBACK_DIFFUSE;
    case th08::psp::EffectSpritePairQuadClass::Axis:
        return PSP_EFFECT_PAIR_FALLBACK_AXIS;
    case th08::psp::EffectSpritePairQuadClass::Uv:
        return PSP_EFFECT_PAIR_FALLBACK_UV;
    case th08::psp::EffectSpritePairQuadClass::AreaOrMirror:
        return PSP_EFFECT_PAIR_FALLBACK_AREA_OR_MIRROR;
    }
    return PSP_EFFECT_PAIR_FALLBACK_STATE;
}

bool PspEffectSpritePairCanonicalBufferMatches(
    const PspEffectSpritePairManagerState &before, const AnmVm *vm,
    const VertexTex1DiffuseXyzrhw *candidate, bool visible,
    ZunResult result, const AnmManager *manager)
{
    const bool boundaryRequested =
        before.texture != vm->loadedSprite->texture ||
        before.vertexShader != 1U || before.blendMode != vm->blendMode;
    const bool boundarySubmitted =
        visible && before.spritesToDraw != 0U && boundaryRequested;
    const VertexTex1DiffuseXyzrhw *const expectedStart =
        boundarySubmitted ? before.bufferEnd : before.bufferStart;
    const u32 expectedSprites = visible
        ? (boundarySubmitted ? 1U : before.spritesToDraw + 1U)
        : before.spritesToDraw;
    const u32 expectedFlushes =
        before.flushes + (boundarySubmitted ? 1U : 0U);
    const uintptr_t bufferBegin =
        reinterpret_cast<uintptr_t>(manager->vertexBuffer);
    const uintptr_t bufferLimit = reinterpret_cast<uintptr_t>(
        manager->vertexBuffer + ARRAY_SIZE(manager->vertexBuffer));
    const uintptr_t appendBegin =
        reinterpret_cast<uintptr_t>(before.bufferEnd);
    const uintptr_t appendBytes = visible
        ? 6U * sizeof(VertexTex1DiffuseXyzrhw)
        : 0U;
    if (appendBegin < bufferBegin || appendBegin > bufferLimit ||
        bufferLimit - appendBegin < appendBytes)
    {
        return false;
    }
    const VertexTex1DiffuseXyzrhw *const expectedEnd =
        reinterpret_cast<const VertexTex1DiffuseXyzrhw *>(
            appendBegin + appendBytes);
    if (result != ZUN_SUCCESS ||
        manager->vertexBufferStartPtr != expectedStart ||
        manager->vertexBufferEndPtr != expectedEnd ||
        manager->spritesToDraw != expectedSprites ||
        manager->flushesThisFrame != expectedFlushes)
    {
        return false;
    }
    if (!visible)
        return true;

    VertexTex1DiffuseXyzrhw expected[6];
    expected[0] = candidate[0];
    expected[1] = candidate[1];
    expected[2] = candidate[2];
    expected[3] = candidate[1];
    expected[4] = candidate[2];
    expected[5] = candidate[3];
    return memcmp(before.bufferEnd, expected, sizeof(expected)) == 0;
}

bool PspEffectSpritePairCanonicalStateMatches(
    const PspEffectSpritePairManagerState &before, const AnmVm *vm,
    bool visible, const AnmManager *manager)
{
    if (!PspEffectSpritePairImmutableManagerStateMatches(before, manager))
        return false;
    if (!visible)
    {
        return manager->currentTexture == before.texture &&
               manager->currentBlendMode == before.blendMode &&
               manager->currentVertexShader == before.vertexShader &&
               manager->disableZWrite == before.disableZWrite &&
               manager->renderStateChangesThisFrame ==
                   before.renderStateChanges;
    }
    const u8 expectedZWrite = g_Supervisor.cfg.opts.disableDepthTest
        ? before.disableZWrite
        : static_cast<u8>(vm->zWriteDisabled);
    return manager->currentTexture == vm->loadedSprite->texture &&
           manager->currentBlendMode == vm->blendMode &&
           manager->currentVertexShader == 1U &&
           manager->disableZWrite == expectedZWrite &&
           manager->renderStateChangesThisFrame ==
               before.renderStateChanges + 1U;
}

ZunResult PspAuditOrdinaryEffectSpritePair(
    EffectManager *effectManager, Effect *effect, u32 group, u32 ordinal,
    bool orderMatches)
{
    PspEffectSpritePairAuditStats &stats =
        g_PspEffectSpritePairAuditStats;
    ++stats.candidates;
    ++stats.canonicalDraws;

    u32 slot = 0xffffffffU;
    const bool slotMatches =
        PspEffectSpritePairSlotIndex(effectManager, effect, &slot);
    if (slotMatches)
        ++stats.slotMatches;
    else
    {
        ++stats.slotMismatches;
        PspEffectSpritePairNoteMismatch(
            "slot_range", group, slot, ordinal);
    }
    if (orderMatches)
        ++stats.orderMatches;
    else
    {
        ++stats.orderMismatches;
        PspEffectSpritePairNoteMismatch(
            "slot_order", group, slot, ordinal);
    }
    PspEffectSpritePairOrderHashU32(group);
    PspEffectSpritePairOrderHashU32(slot);
    PspEffectSpritePairOrderHashU32(ordinal);

    alignas(4) u8 effectBefore[sizeof(Effect)];
    alignas(4) u8 vmBefore[sizeof(AnmVm)];
    memcpy(effectBefore, effect, sizeof(effectBefore));
    memcpy(vmBefore, &effect->vm, sizeof(vmBefore));
    VertexTex1DiffuseXyzrhw quadBefore[4];
    memcpy(quadBefore, g_QuadVertices, sizeof(quadBefore));
    const PspEffectSpritePairManagerState managerBefore =
        PspCaptureEffectSpritePairManagerState(g_AnmManager);

    VertexTex1DiffuseXyzrhw candidate[4];
    memcpy(candidate, quadBefore, sizeof(candidate));
    bool candidateBuilt = false;
    bool candidateFinite = false;
    bool visible = false;
    PspEffectSpritePairFallback fallback = PSP_EFFECT_PAIR_FALLBACK_NONE;
    if (!slotMatches)
        fallback = PSP_EFFECT_PAIR_FALLBACK_SLOT;
    else if (!orderMatches)
        fallback = PSP_EFFECT_PAIR_FALLBACK_ORDER;
    else if (effect->drawCallback != NULL)
        fallback = PSP_EFFECT_PAIR_FALLBACK_CALLBACK;
    else if (effect->vm.rotation.z != 0.0f)
        fallback = PSP_EFFECT_PAIR_FALLBACK_ROTATION;
    else if (!effect->vm.visible || !effect->vm.flag1 ||
             effect->vm.color1.a == 0U)
        fallback = PSP_EFFECT_PAIR_FALLBACK_VISIBILITY;
    else if (effect->vm.loadedSprite == NULL)
        fallback = PSP_EFFECT_PAIR_FALLBACK_SPRITE;
    else
    {
        th08::psp::BuildEffectSpritePairCanonicalQuad(
            candidate, effect->vm,
            g_AnmManager->screenShakeOffset.x,
            g_AnmManager->screenShakeOffset.y);
        candidateBuilt = true;
        ++stats.builtQuads;
        candidateFinite =
            th08::psp::EffectSpritePairQuadFinite(candidate);
        if (!candidateFinite)
        {
            fallback = PSP_EFFECT_PAIR_FALLBACK_NONFINITE;
        }
        else
        {
            visible = th08::psp::EffectSpritePairCanonicalQuadVisible(
                candidate,
                static_cast<f32>(g_Supervisor.viewport.X),
                static_cast<f32>(g_Supervisor.viewport.Y),
                static_cast<f32>(g_Supervisor.viewport.X +
                                 g_Supervisor.viewport.Width),
                static_cast<f32>(g_Supervisor.viewport.Y +
                                 g_Supervisor.viewport.Height));
            if (visible)
            {
                ++stats.visibleQuads;
                PspEffectSpritePairSetDiffuse(
                    g_AnmManager, &effect->vm, candidate);
            }
            else
            {
                ++stats.culledQuads;
            }

            if (effect->vm.loadedSprite->texture == NULL)
                fallback = PSP_EFFECT_PAIR_FALLBACK_TEXTURE;
            else if (!(effect->vm.scale.x > 0.0f) ||
                     !(effect->vm.scale.y > 0.0f))
                fallback = PSP_EFFECT_PAIR_FALLBACK_SCALE;
            else if (g_Supervisor.d3dDevice == NULL ||
                     effect->vm.blendMode > AnmBlendMode_Additive)
                fallback = PSP_EFFECT_PAIR_FALLBACK_STATE;
            else if (visible)
            {
                const th08::psp::EffectSpritePairQuadClass quadClass =
                    th08::psp::ClassifyEffectSpritePairQuad(candidate);
                fallback = PspEffectSpritePairClassFallback(quadClass);
                if (fallback == PSP_EFFECT_PAIR_FALLBACK_NONE)
                    ++stats.eligiblePairs;
            }
            else
            {
                ++stats.eligibleCulls;
            }
        }
    }

    // The experiment ends here: canonical Draw2D is always the only writer
    // and the only renderer called in M0, for both eligible and rejected VMs.
    const ZunResult result = g_AnmManager->Draw2D(&effect->vm);

    const bool effectMatches =
        memcmp(effectBefore, effect, sizeof(effectBefore)) == 0;
    const bool vmMatches =
        memcmp(vmBefore, &effect->vm, sizeof(vmBefore)) == 0;
    if (effectMatches)
        ++stats.effectMatches;
    else
    {
        ++stats.effectMismatches;
        PspEffectSpritePairNoteMismatch(
            "effect_bytes", group, slot, ordinal);
    }
    if (vmMatches)
        ++stats.vmMatches;
    else
    {
        ++stats.vmMismatches;
        PspEffectSpritePairNoteMismatch("vm_bytes", group, slot, ordinal);
    }

    if (candidateBuilt && candidateFinite)
    {
        const bool quadMatches =
            memcmp(candidate, g_QuadVertices, sizeof(candidate)) == 0;
        const bool positionMatches = PspEffectSpritePairPositionBytesEqual(
            candidate, g_QuadVertices);
        const bool uvMatches = PspEffectSpritePairUvBytesEqual(
            candidate, g_QuadVertices);
        const bool colorMatches = PspEffectSpritePairColorBytesEqual(
            candidate, g_QuadVertices);
        if (quadMatches)
            ++stats.quadMatches;
        else
            ++stats.quadMismatches;
        if (positionMatches)
            ++stats.positionMatches;
        else
        {
            ++stats.positionMismatches;
            PspEffectSpritePairNoteMismatch(
                "position_bytes", group, slot, ordinal);
        }
        if (uvMatches)
            ++stats.uvMatches;
        else
        {
            ++stats.uvMismatches;
            PspEffectSpritePairNoteMismatch(
                "uv_bytes", group, slot, ordinal);
        }
        if (colorMatches)
            ++stats.colorMatches;
        else
        {
            ++stats.colorMismatches;
            PspEffectSpritePairNoteMismatch(
                "color_bytes", group, slot, ordinal);
        }
        if (!quadMatches)
        {
            PspEffectSpritePairNoteMismatch(
                "quad_bytes", group, slot, ordinal);
            fallback = PSP_EFFECT_PAIR_FALLBACK_CANONICAL_MISMATCH;
        }

        if (visible &&
            fallback == PSP_EFFECT_PAIR_FALLBACK_NONE)
        {
            VertexTex1DiffuseXyzrhw pair[2];
            VertexTex1DiffuseXyzrhw reconstructed[4];
            th08::psp::BuildEffectSpritePair(candidate, pair);
            th08::psp::ReconstructEffectSpritePairQuad(
                pair, reconstructed);
            if (memcmp(candidate, reconstructed,
                       sizeof(reconstructed)) == 0)
            {
                ++stats.pairMatches;
            }
            else
            {
                ++stats.pairMismatches;
                PspEffectSpritePairNoteMismatch(
                    "pair_reconstruction", group, slot, ordinal);
                fallback = PSP_EFFECT_PAIR_FALLBACK_CANONICAL_MISMATCH;
            }
        }

        const bool bufferMatches =
            PspEffectSpritePairCanonicalBufferMatches(
                managerBefore, &effect->vm, candidate, visible,
                result, g_AnmManager);
        if (bufferMatches)
            ++stats.bufferMatches;
        else
        {
            ++stats.bufferMismatches;
            PspEffectSpritePairNoteMismatch(
                "canonical_buffer", group, slot, ordinal);
            fallback = PSP_EFFECT_PAIR_FALLBACK_CANONICAL_MISMATCH;
        }

        const bool stateMatches =
            PspEffectSpritePairCanonicalStateMatches(
                managerBefore, &effect->vm, visible, g_AnmManager);
        if (stateMatches)
            ++stats.stateMatches;
        else
        {
            ++stats.stateMismatches;
            PspEffectSpritePairNoteMismatch(
                "renderer_state", group, slot, ordinal);
            fallback = PSP_EFFECT_PAIR_FALLBACK_CANONICAL_MISMATCH;
        }

        const bool textureMatches = visible
            ? g_AnmManager->currentTexture ==
                  effect->vm.loadedSprite->texture
            : g_AnmManager->currentTexture == managerBefore.texture;
        if (textureMatches)
            ++stats.textureMatches;
        else
        {
            ++stats.textureMismatches;
            PspEffectSpritePairNoteMismatch(
                "texture_state", group, slot, ordinal);
        }
        const bool blendMatches = visible
            ? g_AnmManager->currentBlendMode == effect->vm.blendMode
            : g_AnmManager->currentBlendMode == managerBefore.blendMode;
        if (blendMatches)
            ++stats.blendMatches;
        else
        {
            ++stats.blendMismatches;
            PspEffectSpritePairNoteMismatch(
                "blend_state", group, slot, ordinal);
        }
    }

    if (fallback != PSP_EFFECT_PAIR_FALLBACK_NONE)
        PspEffectSpritePairNoteFallback(fallback);
    return result;
}

class PspEffectSpritePairAuditPass final
{
public:
    PspEffectSpritePairAuditPass(EffectManager *effectManager, u32 group)
        : effectManager_(effectManager), group_(group), ordinal_(0U),
          order_{}
    {
        PspEffectSpritePairBeginFrame();
        ++g_PspEffectSpritePairAuditStats.passes;
    }

    ZunResult Draw(Effect *effect)
    {
        u32 slot = 0xffffffffU;
        const bool validSlot =
            PspEffectSpritePairSlotIndex(effectManager_, effect, &slot);
        const bool orderMatches = validSlot &&
            th08::psp::EffectSpritePairNoteOrder(
                &order_, slot, ordinal_);
        const ZunResult result = PspAuditOrdinaryEffectSpritePair(
            effectManager_, effect, group_, ordinal_, orderMatches);
        ++ordinal_;
        return result;
    }

    PspEffectSpritePairAuditPass(
        const PspEffectSpritePairAuditPass &) = delete;
    PspEffectSpritePairAuditPass &operator=(
        const PspEffectSpritePairAuditPass &) = delete;

private:
    EffectManager *effectManager_;
    u32 group_;
    u32 ordinal_;
    th08::psp::EffectSpritePairOrderState order_;
};
} // namespace
#endif

#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED
#error "Effect sprite-pair product requires the canonical Effect frontend"
#endif

extern VertexTex1DiffuseXyzrhw g_QuadVertices[4];

namespace
{
enum PspEffectSpritePairProductFallback : u32
{
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_NONE = 0,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_SLOT,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_ORDER,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_CALLBACK,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_ROTATION,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_VISIBILITY,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_SPRITE,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_TEXTURE,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_SCALE,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_NONFINITE,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_Z_OR_W,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_DIFFUSE,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_AXIS,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_UV,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_AREA_OR_MIRROR,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_STATE,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_CAPACITY,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_BACKEND,
    PSP_EFFECT_PAIR_PRODUCT_FALLBACK_COUNT,
};

struct PspEffectSpritePairProductStats
{
    u32 generation;
    u32 passes;
    u32 groupPasses[5];
    u32 candidates;
    u32 eligiblePairs;
    u32 eligibleCulls;
    u32 canonicalFallbacks;
    u32 canonicalDraws;
    u32 submittedRuns;
    u32 submittedPairs;
    u32 verticesSaved;
    u32 frontendBytesSaved;
    u32 backendBytesSaved;
    u32 backendFallbackRuns;
    u32 backendReplayDraws;
    u32 stateBoundaries;
    u32 callbackBoundaries;
    u32 capacityBoundaries;
    u32 maxRunPairs;
    u32 fallbackCounts[PSP_EFFECT_PAIR_PRODUCT_FALLBACK_COUNT];
};

static_assert(sizeof(PspEffectSpritePairProductStats) <=
                  kPspEffectSpritePairProductStorageBytes,
              "Effect sprite-pair product stats exceeded fixed storage");
static_assert(alignof(PspEffectSpritePairProductStats) <= 4U,
              "Effect sprite-pair product stats alignment changed");
static_assert(sizeof(void *) == 4U,
              "Effect sprite-pair VM tail requires the PSP 32-bit ABI");

constexpr u32 kPspEffectSpritePairProductCapacity = 653U;

PspEffectSpritePairProductStats &PspEffectSpritePairProductStatsRef()
{
    return *reinterpret_cast<PspEffectSpritePairProductStats *>(
        g_PspEffectSpritePairProductStorage);
}

void PspEffectSpritePairProductReset()
{
    PspEffectSpritePairProductStats &stats =
        PspEffectSpritePairProductStatsRef();
    const u32 generation = stats.generation + 1U;
    memset(&stats, 0, sizeof(stats));
    stats.generation = generation;
}

void PspEffectSpritePairProductReport()
{
    const PspEffectSpritePairProductStats &stats =
        PspEffectSpritePairProductStatsRef();
    th08::psp::BootLog(
        "TH08PSP EFFECT_SPRITE_PAIR_PRODUCT generation=%lu passes=%lu "
        "group0=%lu group1=%lu group3=%lu group4=%lu candidates=%lu "
        "eligible_pairs=%lu eligible_culls=%lu fallbacks=%lu "
        "canonical_draws=%lu submitted_runs=%lu submitted_pairs=%lu "
        "vertices_saved=%lu frontend_bytes_saved=%lu "
        "backend_bytes_saved=%lu backend_fallback_runs=%lu "
        "backend_replay_draws=%lu state_boundaries=%lu "
        "callback_boundaries=%lu capacity_boundaries=%lu max_run=%lu\n",
        static_cast<unsigned long>(stats.generation),
        static_cast<unsigned long>(stats.passes),
        static_cast<unsigned long>(stats.groupPasses[0]),
        static_cast<unsigned long>(stats.groupPasses[1]),
        static_cast<unsigned long>(stats.groupPasses[3]),
        static_cast<unsigned long>(stats.groupPasses[4]),
        static_cast<unsigned long>(stats.candidates),
        static_cast<unsigned long>(stats.eligiblePairs),
        static_cast<unsigned long>(stats.eligibleCulls),
        static_cast<unsigned long>(stats.canonicalFallbacks),
        static_cast<unsigned long>(stats.canonicalDraws),
        static_cast<unsigned long>(stats.submittedRuns),
        static_cast<unsigned long>(stats.submittedPairs),
        static_cast<unsigned long>(stats.verticesSaved),
        static_cast<unsigned long>(stats.frontendBytesSaved),
        static_cast<unsigned long>(stats.backendBytesSaved),
        static_cast<unsigned long>(stats.backendFallbackRuns),
        static_cast<unsigned long>(stats.backendReplayDraws),
        static_cast<unsigned long>(stats.stateBoundaries),
        static_cast<unsigned long>(stats.callbackBoundaries),
        static_cast<unsigned long>(stats.capacityBoundaries),
        static_cast<unsigned long>(stats.maxRunPairs));
    th08::psp::BootLog(
        "TH08PSP EFFECT_SPRITE_PAIR_PRODUCT_FALLBACK generation=%lu "
        "slot=%lu order=%lu callback=%lu rotation=%lu visibility=%lu "
        "sprite=%lu texture=%lu scale=%lu nonfinite=%lu z_or_w=%lu "
        "diffuse=%lu axis=%lu uv=%lu area_or_mirror=%lu state=%lu "
        "capacity=%lu backend=%lu\n",
        static_cast<unsigned long>(stats.generation),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_SLOT]),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_ORDER]),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_CALLBACK]),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_ROTATION]),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_VISIBILITY]),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_SPRITE]),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_TEXTURE]),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_SCALE]),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_NONFINITE]),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_Z_OR_W]),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_DIFFUSE]),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_AXIS]),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_UV]),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_AREA_OR_MIRROR]),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_STATE]),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_CAPACITY]),
        static_cast<unsigned long>(stats.fallbackCounts[
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_BACKEND]));
}

void PspEffectSpritePairProductNoteFallback(
    PspEffectSpritePairProductFallback reason, u32 count = 1U)
{
    PspEffectSpritePairProductStats &stats =
        PspEffectSpritePairProductStatsRef();
    stats.canonicalFallbacks += count;
    if (reason > PSP_EFFECT_PAIR_PRODUCT_FALLBACK_NONE &&
        reason < PSP_EFFECT_PAIR_PRODUCT_FALLBACK_COUNT)
    {
        stats.fallbackCounts[reason] += count;
    }
}

bool PspEffectSpritePairProductSlotIndex(
    const EffectManager *effectManager, const Effect *effect, u32 *slot)
{
    if (effectManager == NULL || effect == NULL || slot == NULL)
        return false;
    const uintptr_t begin =
        reinterpret_cast<uintptr_t>(&effectManager->effects[0]);
    const uintptr_t end =
        reinterpret_cast<uintptr_t>(&effectManager->effects[653]);
    const uintptr_t address = reinterpret_cast<uintptr_t>(effect);
    if (address < begin || address >= end ||
        (address - begin) % sizeof(Effect) != 0U)
    {
        return false;
    }
    *slot = static_cast<u32>((address - begin) / sizeof(Effect));
    return true;
}

u8 PspEffectSpritePairProductMixColors(u8 left, u8 right)
{
    u32 color = (static_cast<u32>(left) *
                 static_cast<u32>(right)) / 128U;
    if (color >= 256U)
        color = 255U;
    return static_cast<u8>(color);
}

void PspEffectSpritePairProductSetDiffuse(
    const AnmManager *manager, const AnmVm *vm,
    VertexTex1DiffuseXyzrhw *quad)
{
    ZunColor color;
    color.d3dColor = vm->flag17 ? vm->color2.d3dColor
                                : vm->color1.d3dColor;
    if (manager->useMixColor)
    {
        color.r = PspEffectSpritePairProductMixColors(
            color.r, manager->color.r);
        color.g = PspEffectSpritePairProductMixColors(
            color.g, manager->color.g);
        color.b = PspEffectSpritePairProductMixColors(
            color.b, manager->color.b);
        color.a = PspEffectSpritePairProductMixColors(
            color.a, manager->color.a);
    }
    quad[0].diffuse = quad[1].diffuse =
        quad[2].diffuse = quad[3].diffuse = color.d3dColor;
}

PspEffectSpritePairProductFallback PspEffectSpritePairProductClassFallback(
    th08::psp::EffectSpritePairQuadClass quadClass)
{
    switch (quadClass)
    {
    case th08::psp::EffectSpritePairQuadClass::Accept:
        return PSP_EFFECT_PAIR_PRODUCT_FALLBACK_NONE;
    case th08::psp::EffectSpritePairQuadClass::Nonfinite:
        return PSP_EFFECT_PAIR_PRODUCT_FALLBACK_NONFINITE;
    case th08::psp::EffectSpritePairQuadClass::ZOrW:
        return PSP_EFFECT_PAIR_PRODUCT_FALLBACK_Z_OR_W;
    case th08::psp::EffectSpritePairQuadClass::Diffuse:
        return PSP_EFFECT_PAIR_PRODUCT_FALLBACK_DIFFUSE;
    case th08::psp::EffectSpritePairQuadClass::Axis:
        return PSP_EFFECT_PAIR_PRODUCT_FALLBACK_AXIS;
    case th08::psp::EffectSpritePairQuadClass::Uv:
        return PSP_EFFECT_PAIR_PRODUCT_FALLBACK_UV;
    case th08::psp::EffectSpritePairQuadClass::AreaOrMirror:
        return PSP_EFFECT_PAIR_PRODUCT_FALLBACK_AREA_OR_MIRROR;
    }
    return PSP_EFFECT_PAIR_PRODUCT_FALLBACK_STATE;
}

bool PspEffectSpritePairProductCanStore(const AnmManager *manager,
                                        u32 pairIndex)
{
    if (manager == NULL ||
        pairIndex >= kPspEffectSpritePairProductCapacity)
    {
        return false;
    }
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

void PspEffectSpritePairProductStoreVm(AnmManager *manager, u32 pairIndex,
                                       AnmVm *vm)
{
    u8 *const bufferEnd = reinterpret_cast<u8 *>(
        manager->vertexBuffer + ARRAY_SIZE(manager->vertexBuffer));
    memcpy(bufferEnd - static_cast<size_t>(pairIndex + 1U) * sizeof(vm),
           &vm, sizeof(vm));
}

AnmVm *PspEffectSpritePairProductLoadVm(AnmManager *manager, u32 pairIndex)
{
    AnmVm *vm = NULL;
    const u8 *const bufferEnd = reinterpret_cast<const u8 *>(
        manager->vertexBuffer + ARRAY_SIZE(manager->vertexBuffer));
    memcpy(&vm,
           bufferEnd - static_cast<size_t>(pairIndex + 1U) * sizeof(vm),
           sizeof(vm));
    return vm;
}

class PspEffectSpritePairProductPass final
{
public:
    PspEffectSpritePairProductPass(EffectManager *effectManager, u32 group)
        : effectManager_(effectManager), manager_(g_AnmManager),
          ordinal_(0U), order_{}, backendReady_(
              g_AnmManager != NULL && g_Supervisor.d3dDevice != NULL),
          runActive_(false), finished_(false), pairCount_(0U),
          lastVm_(NULL), texture_(NULL), runKey_{}
    {
        PspEffectSpritePairProductStats &stats =
            PspEffectSpritePairProductStatsRef();
        ++stats.passes;
        if (group < ARRAY_SIZE(stats.groupPasses))
            ++stats.groupPasses[group];
    }

    ~PspEffectSpritePairProductPass()
    {
        Finish();
    }

    ZunResult Draw(Effect *effect)
    {
        PspEffectSpritePairProductStats &stats =
            PspEffectSpritePairProductStatsRef();
        ++stats.candidates;
        const u32 ordinal = ordinal_++;
        u32 slot = 0xffffffffU;
        const bool validSlot = PspEffectSpritePairProductSlotIndex(
            effectManager_, effect, &slot);
        const bool orderMatches = validSlot &&
            th08::psp::EffectSpritePairNoteOrder(
                &order_, slot, ordinal);

        VertexTex1DiffuseXyzrhw candidate[4];
        memcpy(candidate, g_QuadVertices, sizeof(candidate));
        bool visible = false;
        PspEffectSpritePairProductFallback fallback =
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_NONE;
        if (!validSlot)
            fallback = PSP_EFFECT_PAIR_PRODUCT_FALLBACK_SLOT;
        else if (!orderMatches)
            fallback = PSP_EFFECT_PAIR_PRODUCT_FALLBACK_ORDER;
        else if (effect->drawCallback != NULL)
            fallback = PSP_EFFECT_PAIR_PRODUCT_FALLBACK_CALLBACK;
        else if (effect->vm.rotation.z != 0.0f)
            fallback = PSP_EFFECT_PAIR_PRODUCT_FALLBACK_ROTATION;
        else if (!effect->vm.visible || !effect->vm.flag1 ||
                 effect->vm.color1.a == 0U)
            fallback = PSP_EFFECT_PAIR_PRODUCT_FALLBACK_VISIBILITY;
        else if (effect->vm.loadedSprite == NULL)
            fallback = PSP_EFFECT_PAIR_PRODUCT_FALLBACK_SPRITE;
        else
        {
            th08::psp::BuildEffectSpritePairCanonicalQuad(
                candidate, effect->vm,
                manager_->screenShakeOffset.x,
                manager_->screenShakeOffset.y);
            if (!th08::psp::EffectSpritePairQuadFinite(candidate))
            {
                fallback = PSP_EFFECT_PAIR_PRODUCT_FALLBACK_NONFINITE;
            }
            else
            {
                visible = th08::psp::EffectSpritePairCanonicalQuadVisible(
                    candidate,
                    static_cast<f32>(g_Supervisor.viewport.X),
                    static_cast<f32>(g_Supervisor.viewport.Y),
                    static_cast<f32>(g_Supervisor.viewport.X +
                                     g_Supervisor.viewport.Width),
                    static_cast<f32>(g_Supervisor.viewport.Y +
                                     g_Supervisor.viewport.Height));
                if (visible)
                {
                    PspEffectSpritePairProductSetDiffuse(
                        manager_, &effect->vm, candidate);
                }

                // Keep the accepted set byte-for-byte aligned with M0.  In
                // particular, even an exact cull is not accepted when the
                // original proof rejected its texture/scale/state contract.
                if (effect->vm.loadedSprite->texture == NULL)
                    fallback = PSP_EFFECT_PAIR_PRODUCT_FALLBACK_TEXTURE;
                else if (!(effect->vm.scale.x > 0.0f) ||
                         !(effect->vm.scale.y > 0.0f))
                    fallback = PSP_EFFECT_PAIR_PRODUCT_FALLBACK_SCALE;
                else if (g_Supervisor.d3dDevice == NULL ||
                         effect->vm.blendMode > AnmBlendMode_Additive)
                    fallback = PSP_EFFECT_PAIR_PRODUCT_FALLBACK_STATE;
                else if (visible)
                    fallback = PspEffectSpritePairProductClassFallback(
                        th08::psp::ClassifyEffectSpritePairQuad(candidate));
            }
        }

        if (fallback != PSP_EFFECT_PAIR_PRODUCT_FALLBACK_NONE)
            return CanonicalFallback(effect, fallback);

        // DrawNoRotation/DrawInner own this persistent scratch contract even
        // for a cull.  Commit the exact M0-proven bytes before omitting either
        // the six duplicated vertices or the no-op canonical call.
        memcpy(g_QuadVertices, candidate, sizeof(candidate));
        if (!visible)
        {
            ++stats.eligibleCulls;
            return ZUN_SUCCESS;
        }

        ++stats.eligiblePairs;
        return QueueVisible(effect, candidate);
    }

    void CallbackBoundary()
    {
        ++PspEffectSpritePairProductStatsRef().callbackBoundaries;
        Flush();
    }

    void Boundary()
    {
        Flush();
    }

    void Finish()
    {
        if (finished_)
            return;
        Flush();
        finished_ = true;
    }

    PspEffectSpritePairProductPass(
        const PspEffectSpritePairProductPass &) = delete;
    PspEffectSpritePairProductPass &operator=(
        const PspEffectSpritePairProductPass &) = delete;

private:
    ZunResult CanonicalFallback(
        Effect *effect, PspEffectSpritePairProductFallback reason)
    {
        Flush();
        PspEffectSpritePairProductStats &stats =
            PspEffectSpritePairProductStatsRef();
        ++stats.canonicalDraws;
        PspEffectSpritePairProductNoteFallback(reason);
        return manager_->Draw2D(&effect->vm);
    }

    void BeginRun(Effect *effect,
                  const th08::psp::EffectSpritePairRunKey &key)
    {
#if TH08_PSP_ITEM_TIME_DRAW_PAIR_ENABLED
        // Close the established shared-buffer owner, if any, before borrowing
        // the same canonical array.  In normal draw-chain order this is a no-op.
        manager_->PspItemTimeDrawPairBoundary();
#endif
        // This is the canonical ordering barrier.  No private GE primitive is
        // emitted until all earlier six-vertex work has been submitted.
        manager_->FlushVertexBuffer();
        manager_->spritesToDraw = 0U;
        manager_->vertexBufferStartPtr =
            manager_->vertexBufferEndPtr = manager_->vertexBuffer;
        runActive_ = true;
        pairCount_ = 0U;
        lastVm_ = &effect->vm;
        texture_ = effect->vm.loadedSprite->texture;
        runKey_ = key;
    }

    ZunResult QueueVisible(Effect *effect,
                           const VertexTex1DiffuseXyzrhw *quad)
    {
        const th08::psp::EffectSpritePairRunKey key =
            th08::psp::MakeEffectSpritePairRunKey(
                effect->vm.loadedSprite->texture,
                static_cast<u32>(effect->vm.blendMode),
                static_cast<u32>(effect->vm.zWriteDisabled),
                static_cast<u32>(
                    g_Supervisor.IsDepthTestDisabled() != 0));
        if (runActive_ &&
            !th08::psp::EffectSpritePairRunKeysEqual(runKey_, key))
        {
            ++PspEffectSpritePairProductStatsRef().stateBoundaries;
            Flush();
        }
        if (!backendReady_)
        {
            return CanonicalFallback(
                effect, PSP_EFFECT_PAIR_PRODUCT_FALLBACK_BACKEND);
        }
        if (!runActive_)
            BeginRun(effect, key);

        if (!PspEffectSpritePairProductCanStore(manager_, pairCount_))
        {
            ++PspEffectSpritePairProductStatsRef().capacityBoundaries;
            Flush();
            if (!backendReady_)
            {
                return CanonicalFallback(
                    effect, PSP_EFFECT_PAIR_PRODUCT_FALLBACK_BACKEND);
            }
            BeginRun(effect, key);
            if (!PspEffectSpritePairProductCanStore(manager_, pairCount_))
            {
                return CanonicalFallback(
                    effect, PSP_EFFECT_PAIR_PRODUCT_FALLBACK_CAPACITY);
            }
        }

        VertexTex1DiffuseXyzrhw *const pair =
            manager_->vertexBuffer + pairCount_ * 2U;
        th08::psp::BuildEffectSpritePair(quad, pair);
        PspEffectSpritePairProductStoreVm(
            manager_, pairCount_, &effect->vm);
        ++pairCount_;
        lastVm_ = &effect->vm;
        manager_->vertexBufferEndPtr =
            manager_->vertexBuffer + pairCount_ * 2U;
        return ZUN_SUCCESS;
    }

    void Flush()
    {
        if (!runActive_ || pairCount_ == 0U)
            return;

        PspEffectSpritePairProductStats &stats =
            PspEffectSpritePairProductStatsRef();
        const u32 pairCount = pairCount_;
        VertexTex1DiffuseXyzrhw preservedQuad[4];
        memcpy(preservedQuad, g_QuadVertices, sizeof(preservedQuad));
        if (stats.maxRunPairs < pairCount)
            stats.maxRunPairs = pairCount;

        bool submitted = backendReady_ && manager_ != NULL &&
                         lastVm_ != NULL && texture_ != NULL &&
                         g_Supervisor.d3dDevice != NULL;
        bool logicalStateCountAdded = false;
        if (submitted)
        {
            if (manager_->currentTexture != texture_)
            {
                manager_->currentTexture = texture_;
                manager_->FlushVertexBuffer();
                g_Supervisor.d3dDevice->SetTexture(
                    0, manager_->currentTexture);
            }
            if (manager_->currentVertexShader != 1U)
            {
                manager_->FlushVertexBuffer();
                manager_->currentVertexShader = 1U;
            }

            // The shared run key already split every texture/blend boundary and
            // every effective Z-write boundary proven by canonical DrawInner.
            manager_->SetRenderStateForVm(lastVm_);
            logicalStateCountAdded = true;
            if (pairCount > 1U)
                manager_->renderStateChangesThisFrame += pairCount - 1U;
            g_Supervisor.d3dDevice->SetTextureStageState(
                0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            g_Supervisor.d3dDevice->SetTextureStageState(
                0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            g_Supervisor.d3dDevice->SetVertexShader(
                D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            submitted = th08_psp_draw_sprite_pairs_in_place(
                g_Supervisor.d3dDevice, manager_->vertexBuffer,
                pairCount, sizeof(VertexTex1DiffuseXyzrhw));
        }

        runActive_ = false;
        pairCount_ = 0U;
        lastVm_ = NULL;
        texture_ = NULL;
        manager_->spritesToDraw = 0U;
        manager_->vertexBufferStartPtr =
            manager_->vertexBufferEndPtr = manager_->vertexBuffer;

        if (submitted)
        {
            ++stats.submittedRuns;
            stats.submittedPairs += pairCount;
            stats.verticesSaved += pairCount * 4U;
            stats.frontendBytesSaved +=
                pairCount * 4U * sizeof(VertexTex1DiffuseXyzrhw);
            stats.backendBytesSaved += pairCount * 4U * 24U;
            ++manager_->flushesThisFrame;
            return;
        }

        // The D3D guards and PSPGL private hook both reject before PRIM.  The
        // hook may have packed the front in place, but the independent pointer
        // tail remains intact and replays every VM once in original list order.
        backendReady_ = false;
        if (logicalStateCountAdded &&
            manager_->renderStateChangesThisFrame >= pairCount)
        {
            manager_->renderStateChangesThisFrame -= pairCount;
        }
        ++stats.backendFallbackRuns;
        stats.backendReplayDraws += pairCount;
        stats.canonicalDraws += pairCount;
        PspEffectSpritePairProductNoteFallback(
            PSP_EFFECT_PAIR_PRODUCT_FALLBACK_BACKEND, pairCount);
        for (u32 pair = 0U; pair < pairCount; ++pair)
        {
            AnmVm *const vm = PspEffectSpritePairProductLoadVm(
                manager_, pair);
            if (vm != NULL)
                manager_->Draw2D(vm);
        }
        memcpy(g_QuadVertices, preservedQuad, sizeof(preservedQuad));
    }

    EffectManager *effectManager_;
    AnmManager *manager_;
    u32 ordinal_;
    th08::psp::EffectSpritePairOrderState order_;
    bool backendReady_;
    bool runActive_;
    bool finished_;
    u32 pairCount_;
    AnmVm *lastVm_;
    IDirect3DTexture8 *texture_;
    th08::psp::EffectSpritePairRunKey runKey_;
};
} // namespace
#endif

#if TH08_PSP_EFFECT_OCCUPANCY_SIDECAR_ENABLED
// ABI-external presentation-side cache.  A set bit means that the canonical
// slot is active OR still needs the inactive-slot vertices cleanup performed
// by the next EffectManager::OnUpdate visit.  Stale positives are safe; false
// negatives are forbidden.
static psp::PspEffectOccupancyBits g_PspEffectOccupancy;

static inline void PspMarkEffectOccupied(EffectManager *effectManager,
                                         Effect *effect)
{
    g_PspEffectOccupancy.Mark(
        static_cast<u32>(effect - effectManager->effects));
}
#endif

#if defined(PSP) && defined(TH08_PSP_EFFECT_INDEXED_QUADS) && \
    TH08_PSP_EFFECT_INDEXED_QUADS && \
    defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
    TH08_PSP_BULLET_UNIFIED_QUADS
namespace
{
class PspEffectIndexedQuadPass final
{
public:
    explicit PspEffectIndexedQuadPass(AnmManager *manager)
        : manager_(manager)
    {
        if (manager_ != NULL)
            manager_->BeginPspEffectIndexedQuadBatch();
    }

    ~PspEffectIndexedQuadPass()
    {
        if (manager_ != NULL)
            manager_->EndPspEffectIndexedQuadBatch();
    }

    PspEffectIndexedQuadPass(const PspEffectIndexedQuadPass &) = delete;
    PspEffectIndexedQuadPass &operator=(
        const PspEffectIndexedQuadPass &) = delete;

private:
    AnmManager *manager_;
};
} // namespace
#endif

// Target 0x004E4B64 is owned by Gui.cpp but participates in effect-resource setup.
#if defined(PSP)
#define g_GuiMessageStageMode g_Background.spellVmScriptBase
#else
extern i32 g_GuiMessageStageMode;
#endif





















struct EffectTemplate
{
    i32 scriptIdx;
    EffectUpdateCallback updateCallback;
    EffectInitializeCallback initializeCallback;
};
C_ASSERT(sizeof(EffectTemplate) == 0xc);
C_ASSERT(offsetof(EffectTemplate, updateCallback) == 0x4);
C_ASSERT(offsetof(EffectTemplate, initializeCallback) == 0x8);
DIFFABLE_STATIC_ARRAY(EffectTemplate, 66, g_EffectTemplates);

// FUNCTION: th08 0x423d70
Float3 *Float3::operator*=(f32 scalar)
{
    this->x *= scalar;
    this->y *= scalar;
    this->z *= scalar;
    return this;
}

// FUNCTION: th08 0x4253e0
Effect *EffectManager::GetFixedSlotEffect(i32 index)
{
    return &this->effects[index + 0x280];
}

// FUNCTION: th08 0x425410
void EffectManager::ResetEffects()
{
    memset(this, 0, 0x8B05C);
#if TH08_PSP_EFFECT_OCCUPANCY_SIDECAR_ENABLED
    g_PspEffectOccupancy.Reset();
#endif
}

// FUNCTION: th08 0x425430
#pragma var_order(effect, i)
Effect *EffectManager::SpawnEffect(i32 id, D3DXVECTOR3 *position, i32 count, i32 color)
{
#if defined(PSP)
    if (count > 0)
        th08::psp::RenderPerfNoteEffectSpawnRequest(static_cast<u32>(count));
#endif
    Effect *effect = this->effects + this->nextEffectIndex;
    i32 i;

    for (i = 0; i < 0x200; i++)
    {
        this->nextEffectIndex = this->nextEffectIndex + 1;
        if (this->nextEffectIndex >= 0x200)
        {
            this->nextEffectIndex = 0;
        }

        if (effect->active != 0)
        {
            if (this->nextEffectIndex == 0)
            {
                effect = this->effects;
            }
            else
            {
                effect++;
            }
            continue;
        }

        if (effect->vertices != NULL)
        {
            g_ZunMemory.Free(effect->vertices);
        }

        memset(effect, 0, sizeof(Effect));
        effect->active = 1;
#if TH08_PSP_EFFECT_OCCUPANCY_SIDECAR_ENABLED
        PspMarkEffectOccupied(this, effect);
#endif
        effect->effectId = id;
        effect->position = *reinterpret_cast<Float3 *>(position);
        this->effectAnm->SetAndExecuteScriptIdx(&effect->vm, g_EffectTemplates[id].scriptIdx);
        *reinterpret_cast<u32 *>(&effect->vm.flags) |= 0x2000;
        effect->vm.color1.d3dColor = color;
        effect->vm.pos2.x = 0.0f;
        effect->vm.pos2.y = 0.0f;
        effect->vm.pos2.z = 0.0f;
        effect->updateCallback = g_EffectTemplates[id].updateCallback;

        if (g_EffectTemplates[id].initializeCallback != NULL)
        {
            if (g_EffectTemplates[id].initializeCallback(effect) != 0)
            {
                effect->active = 0;
            }
        }

        count--;
        if (count == 0)
        {
            break;
        }

        if (this->nextEffectIndex == 0)
        {
            effect = this->effects;
        }
        else
        {
            effect++;
        }
    }

    g_ReplayManager->frameEventFlags |= 0x400;
    return i >= 0x200 ? &this->effects[653] : effect;
}

// FUNCTION: th08 0x425650
#pragma var_order(effect, i)
Effect *EffectManager::SpawnEffectWithVelocity(i32 id, D3DXVECTOR3 *position, D3DXVECTOR3 *velocity, i32 count, i32 color)
{
#if defined(PSP)
    if (count > 0)
        th08::psp::RenderPerfNoteEffectSpawnRequest(static_cast<u32>(count));
#endif
    Effect *effect = this->effects + this->nextEffectIndex;
    i32 i;

    for (i = 0; i < 0x200; i++)
    {
        this->nextEffectIndex = this->nextEffectIndex + 1;
        if (this->nextEffectIndex >= 0x200)
        {
            this->nextEffectIndex = 0;
        }

        if (effect->active != 0)
        {
            if (this->nextEffectIndex == 0)
            {
                effect = this->effects;
            }
            else
            {
                effect++;
            }
            continue;
        }

        if (effect->vertices != NULL)
        {
            g_ZunMemory.Free(effect->vertices);
        }

        memset(effect, 0, sizeof(Effect));
        effect->active = 1;
#if TH08_PSP_EFFECT_OCCUPANCY_SIDECAR_ENABLED
        PspMarkEffectOccupied(this, effect);
#endif
        effect->effectId = id;
        effect->position = *reinterpret_cast<Float3 *>(position);
        this->effectAnm->SetAndExecuteScriptIdx(&effect->vm, g_EffectTemplates[id].scriptIdx);
        effect->vm.color1.d3dColor = color;
        effect->vm.pos2.x = 0.0f;
        effect->vm.pos2.y = 0.0f;
        effect->vm.pos2.z = 0.0f;
        effect->updateCallback = g_EffectTemplates[id].updateCallback;
        effect->vector1 = *reinterpret_cast<Float3 *>(velocity);

        if (g_EffectTemplates[id].initializeCallback != NULL)
        {
            if (g_EffectTemplates[id].initializeCallback(effect) != 0)
            {
                effect->active = 0;
            }
        }

        count--;
        if (count == 0)
        {
            break;
        }

        if (this->nextEffectIndex == 0)
        {
            effect = this->effects;
        }
        else
        {
            effect++;
        }
    }

    g_ReplayManager->frameEventFlags |= 0x400;
    return i >= 0x200 ? &this->effects[653] : effect;
}

// FUNCTION: th08 0x425870
#pragma var_order(effect)
Effect *EffectManager::SpawnEffectInFixedSlot(i32 id, D3DXVECTOR3 *position, i32 slotIndex, i32 unused, i32 color)
{
#if defined(PSP)
    th08::psp::RenderPerfNoteEffectSpawnRequest();
#endif
    Effect *effect = &this->effects[slotIndex + 0x280];

    if (effect->vertices != NULL)
    {
        g_ZunMemory.Free(effect->vertices);
    }

    memset(effect, 0, sizeof(Effect));
    effect->slotIndex = slotIndex;
    effect->active = 1;
#if TH08_PSP_EFFECT_OCCUPANCY_SIDECAR_ENABLED
    PspMarkEffectOccupied(this, effect);
#endif
    effect->effectId = id;
    effect->position = *reinterpret_cast<Float3 *>(position);

    if (g_EffectTemplates[id].scriptIdx >= 0)
    {
        this->effectAnm->SetAndExecuteScriptIdx(&effect->vm, g_EffectTemplates[id].scriptIdx);
    }

    *reinterpret_cast<u32 *>(&effect->vm.flags) |= 0x2000;
    effect->vm.color1.d3dColor = color;
    effect->vm.pos2.x = 0.0f;
    effect->vm.pos2.y = 0.0f;
    effect->vm.pos2.z = 0.0f;
    effect->updateCallback = g_EffectTemplates[id].updateCallback;

    if (g_EffectTemplates[id].initializeCallback != NULL &&
        g_EffectTemplates[id].initializeCallback(effect) != 0)
    {
        effect->active = 0;
    }

    g_ReplayManager->frameEventFlags |= 0x400;
    return effect;
}

// FUNCTION: th08 0x4259e0
#pragma var_order(effect)
Effect *EffectManager::SpawnEffectInFixedSlotWithVelocity(i32 id, D3DXVECTOR3 *position, D3DXVECTOR3 *velocity, i32 slotIndex,
                                   i32 unused, i32 color)
{
#if defined(PSP)
    th08::psp::RenderPerfNoteEffectSpawnRequest();
#endif
    Effect *effect = &this->effects[slotIndex + 0x280];

    if (effect->vertices != NULL)
        g_ZunMemory.Free(effect->vertices);

    memset(effect, 0, sizeof(Effect));
    effect->slotIndex = slotIndex;
    effect->vector1 = *reinterpret_cast<Float3 *>(velocity);
    effect->active = 1;
#if TH08_PSP_EFFECT_OCCUPANCY_SIDECAR_ENABLED
    PspMarkEffectOccupied(this, effect);
#endif
    effect->effectId = id;
    effect->position = *reinterpret_cast<Float3 *>(position);

    if (g_EffectTemplates[id].scriptIdx >= 0)
    {
        this->effectAnm->SetAndExecuteScriptIdx(&effect->vm, g_EffectTemplates[id].scriptIdx);
    }

    *reinterpret_cast<u32 *>(&effect->vm.flags) |= 0x2000;
    effect->vm.color1.d3dColor = color;
    effect->vm.pos2.x = 0.0f;
    effect->vm.pos2.y = 0.0f;
    effect->vm.pos2.z = 0.0f;
    effect->updateCallback = g_EffectTemplates[id].updateCallback;

    if (g_EffectTemplates[id].initializeCallback != NULL &&
        g_EffectTemplates[id].initializeCallback(effect) != 0)
    {
        effect->active = 0;
    }

    g_ReplayManager->frameEventFlags |= 0x400;
    return effect;
}

// FUNCTION: th08 0x425b70
#pragma var_order(effect, i, zeroVector)
Effect *EffectManager::SpawnEffectInSecondaryPool(i32 id, D3DXVECTOR3 *position, i32 count, i32 color)
{
#if defined(PSP)
    if (count > 0)
        th08::psp::RenderPerfNoteEffectSpawnRequest(static_cast<u32>(count));
#endif
    Effect *effect = this->effects + 0x200;
    i32 i;

    for (i = 0; i < 0x80; i++, effect++)
    {
        if (effect->active != 0)
        {
            continue;
        }

        if (effect->vertices != NULL)
        {
            g_ZunMemory.Free(effect->vertices);
        }
        effect->vertices = NULL;
        effect->drawCallback = NULL;
        effect->drawGroup = 0;
        effect->active = 1;
#if TH08_PSP_EFFECT_OCCUPANCY_SIDECAR_ENABLED
        PspMarkEffectOccupied(this, effect);
#endif
        effect->effectId = id;
        effect->position = *reinterpret_cast<Float3 *>(position);
        this->effectAnm->SetAndExecuteScriptIdx(&effect->vm, g_EffectTemplates[id].scriptIdx);
        effect->vm.color1.d3dColor = color;
        effect->vm.pos2.x = 0.0f;
        effect->vm.pos2.y = 0.0f;
        effect->vm.pos2.z = 0.0f;
        effect->updateCallback = g_EffectTemplates[id].updateCallback;
        effect->timer = 0;
        effect->releaseRequested = 0;
        effect->releaseTimer = 0;
        *reinterpret_cast<D3DXVECTOR3 *>(&effect->vector1) = D3DXVECTOR3(0, 0, 0);

        if (g_EffectTemplates[id].initializeCallback != NULL)
        {
            if (g_EffectTemplates[id].initializeCallback(effect) != 0)
            {
                effect->active = 0;
            }
        }

        count--;
        if (count == 0)
        {
            break;
        }
    }

    g_ReplayManager->frameEventFlags |= 0x400;
    return i >= 0x80 ? &this->effects[653] : effect;
}

// FUNCTION: th08 0x425d70
i32 __fastcall EffectRandomSplashInit(Effect *effect)
{
    effect->vector2.operator float *()[0] = (g_Rng.GetRandomF32InRange(256.0f) - 128.0f) / 12.0f;
    effect->vector2.operator float *()[1] = (g_Rng.GetRandomF32InRange(256.0f) - 128.0f) / 12.0f;
    effect->vector2.operator float *()[2] = 0.0f;
    effect->vector3 = -effect->vector2 / 19.0f;
    effect->vector2 *= g_Supervisor.framerateMultiplier;
    effect->vector3 *= g_Supervisor.framerateMultiplier;
    return 0;
}

// FUNCTION: th08 0x425e60
i32 __fastcall EffectRandomSplashUpdate(Effect *effect)
{
    effect->position += effect->vector2;
    effect->vector2 += effect->vector3;
    return 1;
}

// FUNCTION: th08 0x425ea0
i32 __fastcall EffectRandomSplashBigInit(Effect *effect)
{
    effect->vector2.operator float *()[0] = (g_Rng.GetRandomF32InRange(256.0f) - 128.0f) * 4.0f / 33.0f;
    effect->vector2.operator float *()[1] = (g_Rng.GetRandomF32InRange(256.0f) - 128.0f) * 4.0f / 33.0f;
    effect->vector2.operator float *()[2] = 0.0f;
    effect->vector3 = -effect->vector2 / 20.0f;
    effect->vector2 *= g_Supervisor.framerateMultiplier;
    effect->vector3 *= g_Supervisor.framerateMultiplier;
    return 0;
}

// FUNCTION: th08 0x425fa0
Float3 Float3::operator-() const
{
    return Float3(-this->x, -this->y, -this->z);
}

// FUNCTION: th08 0x425fe0
i32 __fastcall EffectOrbitInit(Effect *effect)
{
    effect->drawGroup = 2;
    effect->vector6.x = 0.0f;
    effect->vector6.y = 0.0f;
    effect->vector6.z = 0.0f;
    effect->radius = 0.0f;
    return 0;
}

// FUNCTION: th08 0x426030
#pragma var_order(posOffset, verticalAngle, localMatrix, horizontalAngle, normalizedPos, alpha, this)
i32 __fastcall EffectOrbitUpdate(Effect *effect)
{
    Float3 posOffset;
    f32 verticalAngle;
    Float3 normalizedPos;
    D3DXMATRIX localMatrix;
    f32 horizontalAngle;
    f32 alpha;
    D3DXVec3Normalize(reinterpret_cast<D3DXVECTOR3 *>(&normalizedPos),
                      reinterpret_cast<D3DXVECTOR3 *>(&effect->vector6));
    verticalAngle = sinf(effect->angle);
    horizontalAngle = cosf(effect->angle);
    effect->orientationAxis.x = normalizedPos.x * verticalAngle;
    effect->orientationAxis.y = normalizedPos.y * verticalAngle;
    effect->orientationAxis.z = normalizedPos.z * verticalAngle;
    effect->orientationW = horizontalAngle;
    D3DXMatrixRotationQuaternion(
        &localMatrix, reinterpret_cast<D3DXQUATERNION *>(&effect->orientationAxis));
    posOffset.x = normalizedPos.y * 1.0f - normalizedPos.z * 0.0f;
    posOffset.y = normalizedPos.z * 0.0f - normalizedPos.x * 1.0f;
    posOffset.z = normalizedPos.x * 0.0f - normalizedPos.y * 0.0f;
    if (D3DXVec3LengthSq(reinterpret_cast<D3DXVECTOR3 *>(&posOffset)) < 0.00001f)
        normalizedPos = Float3(1.0f, 0.0f, 0.0f);
    else
        D3DXVec3Normalize(reinterpret_cast<D3DXVECTOR3 *>(&posOffset), reinterpret_cast<D3DXVECTOR3 *>(&posOffset));
    posOffset *= effect->radius;
    D3DXVec3TransformCoord(reinterpret_cast<D3DXVECTOR3 *>(&posOffset), reinterpret_cast<D3DXVECTOR3 *>(&posOffset), &localMatrix);
    posOffset.z *= 6.0f;
    effect->position = posOffset + effect->vector5;
    effect->position.z = 0.0f;
    if (effect->releaseRequested != 0)
    {
        ++effect->releaseTimer;
        if (effect->releaseTimer >= 16)
            return 0;
        alpha = 1.0f - (f32)effect->releaseTimer / 16.0f;
        effect->vm.color1.d3dColor = (effect->vm.color1.d3dColor & 0xffffff) |
            ((i32)(alpha * 255.0f) << 24);
        effect->vm.scale.y = 2.0f - alpha;
        effect->vm.scale.x = effect->vm.scale.y;
    }
    return 1;
}

// FUNCTION: th08 0x426280
#pragma var_order(backgroundOffset, effect)
i32 __fastcall InitializeTintedBossTrackingCameraParticle(Effect *effect)
{
    Float3 backgroundOffset;

    backgroundOffset = -g_Background.cameraCurrent.lookAtOffset;
    effect->vector4 = g_Background.cameraCurrent.lookAtOffset + g_Background.cameraCurrent.position;
    effect->vector4.x += g_Rng.GetRandomF32SignedInRange(60.0f) + backgroundOffset.x / 2.0f;
    effect->vector4.y += g_Rng.GetRandomF32SignedInRange(100.0f) - 50.0f + backgroundOffset.y / 2.0f;
    effect->vector4.z += g_Rng.GetRandomF32InRange(100.0f) - 100.0f + backgroundOffset.z / 2.0f;

    effect->vector2.x = g_Rng.GetRandomF32SignedInRange(0.001f) + effect->vector1.x;
    effect->vector2.y = g_Rng.GetRandomF32SignedInRange(0.03f) + effect->vector1.y;
    effect->vector2.z = -g_Rng.GetRandomF32InRange(0.1f) - 0.3f + effect->vector1.z;
    effect->vector3.x = g_Rng.GetRandomF32SignedInRange(0.0001f);
    effect->vector3.y = g_Rng.GetRandomF32SignedInRange(0.0001f);
    effect->vector3.z = -0.0003f;
    effect->vector2 = effect->vector2 * g_Supervisor.framerateMultiplier;
    effect->vector3 = effect->vector3 * g_Supervisor.framerateMultiplier;
    effect->drawGroup = 1;
    effect->vm.pos2.x = -9999.0f;
    effect->vm.posInitial.x = 0.0f;
    effect->vm.posFinal.x = 0.0f;
    effect->vm.posFinal.y = 0.0f;
    effect->vm.posFinal.z = 0.0f;
    effect->vm.rotateInitial.x = 0.0f;
    effect->vm.rotateInitial.y = 0.0f;
    effect->vm.rotateInitial.z = 0.0f;
    return 0;
}

// FUNCTION: th08 0x4264f0
#pragma var_order(delta, dot, effect)
i32 __fastcall UpdateTintedBossTrackingCameraParticle(Effect *effect)
{
    f32 dot;

    effect->vector2 += effect->vector3;
    effect->vector4 += effect->vector2;
    effect->position = effect->vector4;

    Float3 delta;
    delta = effect->position - g_Background.cameraCurrent.position;
    D3DXVec3Normalize(reinterpret_cast<D3DXVECTOR3 *>(&delta), reinterpret_cast<D3DXVECTOR3 *>(&delta));
    dot = D3DXVec3Dot(reinterpret_cast<D3DXVECTOR3 *>(&g_Background.cameraCurrent.forward),
                      reinterpret_cast<D3DXVECTOR3 *>(&delta));
    if (dot < 0.94f)
        return 0;

    if (g_EnemyManager.HasBoss())
    {
        if (((g_EnemyManager.bosses[0]->flags1 >>
              ENEMY_FLAG_DAMAGEABLE_SHIFT) & 1) != 0)
        {
            if (effect->vm.pos2.x <= -9999.0f)
            {
                effect->vm.pos2 = g_EnemyManager.bosses[0]->position;
            }
            else
            {
                effect->vm.pos2 =
                    (g_EnemyManager.bosses[0]->position -
                     effect->vm.pos2) * 0.1f + effect->vm.pos2;
            }
        }
    }

    *reinterpret_cast<u32 *>(&effect->vm.flags) |= 0x20000;
    effect->vm.color2.r = ((u32)effect->vm.color1.r * g_Background.stageTextVm.color1.r) >> 8;
    effect->vm.color2.g = ((u32)effect->vm.color1.g * g_Background.stageTextVm.color1.g) >> 8;
    effect->vm.color2.b = ((u32)effect->vm.color1.b * g_Background.stageTextVm.color1.b) >> 8;
    effect->vm.color2.a = ((u32)effect->vm.color1.a * g_Background.stageTextVm.color1.a) >> 8;
    return 1;
}

// FUNCTION: th08 0x426720
#pragma var_order(backgroundOffset, effect)
i32 __fastcall InitializeRisingBossTrackingCameraParticle(Effect *effect)
{
    Float3 backgroundOffset;

    backgroundOffset = -g_Background.cameraCurrent.lookAtOffset;
    effect->vector4 = g_Background.cameraCurrent.lookAtOffset + g_Background.cameraCurrent.position;
    effect->vector4.x += g_Rng.GetRandomF32SignedInRange(60.0f) + backgroundOffset.x / 2.0f;
    effect->vector4.y += g_Rng.GetRandomF32SignedInRange(200.0f) - 200.0f + backgroundOffset.y / 2.0f;
    effect->vector4.z += g_Rng.GetRandomF32InRange(100.0f) - 100.0f + backgroundOffset.z / 2.0f;

    effect->vector2.x = g_Rng.GetRandomF32SignedInRange(0.001f) + effect->vector1.x;
    effect->vector2.y = g_Rng.GetRandomF32SignedInRange(0.03f) + 0.4f;
    effect->vector2.z = -g_Rng.GetRandomF32InRange(0.1f) - 0.3f + effect->vector1.z;
    effect->vector3.x = g_Rng.GetRandomF32SignedInRange(0.0001f);
    effect->vector3.y = g_Rng.GetRandomF32SignedInRange(0.0001f);
    effect->vector3.z = -0.0003f;
    effect->vector2 = effect->vector2 * g_Supervisor.framerateMultiplier;
    effect->vector3 = effect->vector3 * g_Supervisor.framerateMultiplier;
    effect->drawGroup = 1;
    effect->vm.pos2.x = -9999.0f;
    effect->vm.posInitial.x = 0.0f;
    effect->vm.posFinal.x = 0.0f;
    effect->vm.posFinal.y = 0.0f;
    effect->vm.posFinal.z = 0.0f;
    effect->vm.rotateInitial.x = 0.0f;
    effect->vm.rotateInitial.y = 0.0f;
    effect->vm.rotateInitial.z = 0.0f;
    return 0;
}

// FUNCTION: th08 0x426990
#pragma var_order(delta, dot, effect)
i32 __fastcall UpdateRisingBossTrackingCameraParticle(Effect *effect)
{
    f32 dot;

    effect->vector2 += effect->vector3;
    effect->vector4 += effect->vector2;
    effect->position = effect->vector4;

    Float3 delta;
    delta = effect->position - g_Background.cameraCurrent.position;
    D3DXVec3Normalize(reinterpret_cast<D3DXVECTOR3 *>(&delta), reinterpret_cast<D3DXVECTOR3 *>(&delta));
    dot = D3DXVec3Dot(reinterpret_cast<D3DXVECTOR3 *>(&g_Background.cameraCurrent.forward),
                      reinterpret_cast<D3DXVECTOR3 *>(&delta));
    if (dot < 0.94f)
        return 0;

    if (g_EnemyManager.bosses[0] != NULL)
    {
        if (((g_EnemyManager.bosses[0]->flags1 >>
              ENEMY_FLAG_DAMAGEABLE_SHIFT) & 1) != 0)
        {
            if (effect->vm.pos2.x <= -9999.0f)
            {
                effect->vm.pos2 = g_EnemyManager.bosses[0]->position;
            }
            else
            {
                effect->vm.pos2 =
                    (g_EnemyManager.bosses[0]->position -
                     effect->vm.pos2) * 0.1f + effect->vm.pos2;
            }
        }
    }
    return 1;
}

// FUNCTION: th08 0x426b20
#pragma var_order(angle, effect)
i32 __fastcall InitializeRandomDirectionalOffset(Effect *effect)
{
    f32 angle;

    effect->vector5 = effect->position;
    effect->vector5.z = 0.0f;
    angle = g_Rng.GetRandomF32InRange(ZUN_2PI) - ZUN_PI;
    effect->vector6.x = cosf(angle);
    effect->vector6.y = sinf(angle);
    effect->vector6.z = 0.0f;
    return 0;
}

// FUNCTION: th08 0x426bb0
#pragma var_order(alpha, effect)
i32 __fastcall UpdateDirectionalOffset60(Effect *effect)
{
    f32 alpha;

    alpha = 256.0f - (f32)effect->timer * 256.0f / 60.0f;
    effect->position = effect->vector6 * alpha + effect->vector5;
    effect->position.z = 0.0f;
    return 1;
}

// FUNCTION: th08 0x426c40
i32 __fastcall TrackPlayerUntilAnimationEnds(Effect *effect)
{
    if (HasAnimationEnded(effect))
        return 0;

    effect->position = g_Player.position;
    return 1;
}

// FUNCTION: th08 0x426c90
#pragma var_order(alpha, effect)
i32 __fastcall UpdateDirectionalOffset240(Effect *effect)
{
    f32 alpha;

    alpha = 256.0f - (f32)effect->timer * 256.0f / 240.0f;
    effect->position = effect->vector6 * alpha + effect->vector5;
    return 1;
}

// FUNCTION: th08 0x426d10
#pragma var_order(effect, i, delta)
void __fastcall ShiftStageEffectOrigins(Float3 *delta)
{
    Effect *effect = g_EffectManager.effects;
    i32 i;

    for (i = 0; i < 0x200; i++, effect++)
    {
        if (effect->effectId == 0x33)
        {
            effect->vector4 += *delta;
        }
    }
}

// FUNCTION: th08 0x426d70
#pragma var_order(delta, dot, effect)
i32 __fastcall UpdateSpinningCameraParticle(Effect *effect)
{
    f32 dot;

    effect->vector2 += effect->vector3;
    effect->vector4 += effect->vector2;
    effect->position = effect->vector4;

    Float3 delta;
    delta = effect->position - g_Background.cameraCurrent.position;
    D3DXVec3Normalize(reinterpret_cast<D3DXVECTOR3 *>(&delta), reinterpret_cast<D3DXVECTOR3 *>(&delta));
    dot = D3DXVec3Dot(reinterpret_cast<D3DXVECTOR3 *>(&g_Background.cameraCurrent.forward),
                      reinterpret_cast<D3DXVECTOR3 *>(&delta));
    if (dot < 0.94f)
        return 0;

    effect->vm.SetZRotation(AddNormalizeAngle(effect->vm.rotation.z, effect->vm.rotation.x));
    if (effect->position.z >= 0.0f)
        return 0;
    return 1;
}

// FUNCTION: th08 0x426e70
#pragma var_order(backgroundOffset, effect)
i32 __fastcall InitializeSpinningCameraParticle(Effect *effect)
{
    Float3 backgroundOffset;

    backgroundOffset = -g_Background.cameraCurrent.lookAtOffset;
    effect->vector4 = g_Background.cameraCurrent.lookAtOffset + g_Background.cameraCurrent.position;
    effect->vector4.x += g_Rng.GetRandomF32InRange(120.0f) - 60.0f + backgroundOffset.x / 2.0f;
    effect->vector4.y += g_Rng.GetRandomF32InRange(200.0f) - 100.0f + backgroundOffset.y / 2.0f;
    effect->vector4.z += g_Rng.GetRandomF32InRange(100.0f) - 100.0f + backgroundOffset.z / 2.0f;

    effect->vector2.x = g_Rng.GetRandomF32InRange(0.06f) - 0.03f + effect->vector1.x;
    effect->vector2.y = g_Rng.GetRandomF32InRange(0.06f) - 0.03f + effect->vector1.y;
    effect->vector2.z = g_Rng.GetRandomF32InRange(0.1f) + 0.03f + effect->vector1.z;
    effect->vector3.x = g_Rng.GetRandomF32InRange(0.0002f) - 0.0001f;
    effect->vector3.y = g_Rng.GetRandomF32InRange(0.0002f) - 0.0001f;
    effect->vector2 = effect->vector2 * g_Supervisor.framerateMultiplier;
    effect->vector3 = effect->vector3 * g_Supervisor.framerateMultiplier;
    effect->drawGroup = 1;
    effect->vm.rotation.z = g_Rng.GetRandomF32InRange(ZUN_2PI) - ZUN_PI;
    effect->vm.rotation.x = g_Rng.GetRandomF32InRange(0.03141592815518379f) - 0.015707964077591896f;
    return 0;
}

// FUNCTION: th08 0x4270c0
#pragma var_order(angle, effect)
i32 __fastcall InitializeDirectionalOffset(Effect *effect)
{
    f32 angle;

    if (effect->vector1.x > -990.0)
        angle = AddNormalizeAngle(effect->vector1.x, 0.0f);
    else
        angle = g_Rng.GetRandomF32InRange(ZUN_2PI) - ZUN_PI;

    effect->vector5 = effect->position;
    effect->vector5.z = 0.0f;
    effect->vector6.x = cosf(angle);
    effect->vector6.y = sinf(angle);
    effect->vector6.z = 0.0f;
    effect->vector6 *= g_Rng.GetRandomF32InRange(1.5f) + 0.0f;
    return 0;
}

// FUNCTION: th08 0x4271a0
#pragma var_order(alpha, effect)
i32 __fastcall UpdateEasedDirectionalOffset(Effect *effect)
{
    f32 alpha;

    alpha = (f32)effect->timer / 90.0f;
    alpha = 1.0f - (1.0f - alpha) * (1.0f - alpha);
    effect->position = effect->vector6 * alpha * 128.0f + effect->vector5;
    effect->position.z = 0.0f;
    return 1;
}

// FUNCTION: th08 0x427250
i32 __fastcall KeepTrailAlive(Effect *effect)
{
    return 1;
}

// FUNCTION: th08 0x427260
#pragma var_order(offset, effect)
i32 __fastcall InitializeTrailOffset(Effect *effect)
{
    Float3 offset;

    offset.FromAngleMagnitude(effect->vector1.x, 256.0f);
    effect->position.x += offset.x;
    effect->position.y += offset.y;
    effect->vm.rotation.z = AddNormalizeAngle(effect->vector1.x, ZUN_PI / 2.0f);
    return 0;
}

// FUNCTION: th08 0x4272e0
i32 __fastcall InitializeRadialTrail(Effect *effect)
{
    effect->vertices = static_cast<VertexTex1DiffuseXyzrhw *>(g_ZunMemory.Alloc(0x1c38, "Effect"));
    if (effect->vertices == NULL)
        return -1;

    effect->vertexSegmentCount = 3;
    effect->vector5 = effect->position;
    effect->vector6.x = 0.0f;
    effect->vector6.y = 0.0f;
    effect->vector6.z = 1.0f;
    effect->vector7.x = 0.0f;
    effect->vector7.y = -1.0f;
    effect->vector7.z = 0.0f;
    effect->angle = effect->vector1.x;
    effect->radius = effect->vector1.y;
    effect->shapeThickness = effect->vector1.z;

    g_AnmManager->InitializeHorizontalTextureStrip(&effect->vm, effect->vertices, effect->vertexSegmentCount * 2);
    effect->verticesDirty = 1;
    effect->drawCallback = DrawRadialTrail;
    effect->secondaryRadius = 0.0f;
    effect->secondaryAngle = 0.0f;
    effect->radialWaveCount = 0.0f;
    effect->vertexSegmentCount = 24;
    return 0;
}

// FUNCTION: th08 0x427450
#pragma var_order(i, innerRadius, vertex, angleStep, radius)
i32 __fastcall DrawRadialTrail(Effect *effect)
{
    i32 i;
    f32 innerRadius;
    VertexTex1DiffuseXyzrhw *vertex;
    f32 angleStep;
    f32 radius;

#if defined(PSP)
    th08::psp::RadialTrailTelemetryNoteDraw(effect->vertexSegmentCount);
#endif

    if (effect->verticesDirty)
    {
        angleStep = ZUN_2PI / effect->vertexSegmentCount;
        radius = effect->shapeThickness /
                 sinf((ZUN_PI - angleStep) / 2.0f);
        vertex = effect->vertices;
        g_AnmManager->InitializeVerticalTextureStrip(
            &effect->vm, effect->vertices, effect->vertexSegmentCount * 2 + 2);

        if (effect->secondaryRadius == 0.0f)
        {
#if defined(PSP)
            th08::psp::RadialTrailTelemetryNoteRebuild(
                effect->vertexSegmentCount,
                th08::psp::RadialTrailBranch::ZeroSecondary,
#if defined(TH08_PSP_RADIAL_TRAIL_TRIG_REUSE) && \
    TH08_PSP_RADIAL_TRAIL_TRIG_REUSE
                true);
#else
                false);
#endif
#endif
            f32 angle;
            angle = effect->angle;
            innerRadius = effect->radius - radius;
            radius += effect->radius;
            for (i = effect->vertexSegmentCount + 1; i > 0; --i)
            {
                if (angle >= ZUN_PI)
                    angle -= ZUN_2PI;

                vertex->pos.z = 0.0f;
#if defined(PSP) && defined(TH08_PSP_RADIAL_TRAIL_TRIG_REUSE) && \
    TH08_PSP_RADIAL_TRAIL_TRIG_REUSE
                // Presentation-only M1: both radii use the same canonical
                // binary64 sin/cos pair.  Keep the Float3 stores/adds in their
                // original order; no value feeds simulation.
                th08::psp::CanonicalRadialSinCos radialTrig;
                radialTrig.cosine =
                    th08::psp::EvaluateCanonicalRadialCos(angle);
                vertex->pos.x =
                    th08::psp::CanonicalRadialCosMul(radialTrig, radius);
                radialTrig.sine =
                    th08::psp::EvaluateCanonicalRadialSin(angle);
                vertex->pos.y =
                    th08::psp::CanonicalRadialSinMul(radialTrig, radius);
#else
                vertex->pos.FromAngleMagnitude(angle, radius);
#endif
                vertex->pos += effect->vector5;
                vertex->pos.x += g_GameManager.arcadeRegionTopLeftPos.x;
                vertex->pos.y += g_GameManager.arcadeRegionTopLeftPos.y;
                vertex++;

                vertex->pos.z = 0.0f;
#if defined(PSP) && defined(TH08_PSP_RADIAL_TRAIL_TRIG_REUSE) && \
    TH08_PSP_RADIAL_TRAIL_TRIG_REUSE
                vertex->pos.x =
                    th08::psp::CanonicalRadialCosMul(radialTrig, innerRadius);
                vertex->pos.y =
                    th08::psp::CanonicalRadialSinMul(radialTrig, innerRadius);
#else
                vertex->pos.FromAngleMagnitude(angle, innerRadius);
#endif
                vertex->pos += effect->vector5;
                vertex->pos.x += g_GameManager.arcadeRegionTopLeftPos.x;
                vertex->pos.y += g_GameManager.arcadeRegionTopLeftPos.y;
                vertex++;

                angle += angleStep;
            }
        }
        else if (effect->radialWaveCount == 0.0f)
        {
#if defined(PSP)
            th08::psp::RadialTrailTelemetryNoteRebuild(
                effect->vertexSegmentCount,
                th08::psp::RadialTrailBranch::Ellipse, false);
#endif
#pragma var_order(innerEllipseRadius, outerEllipseRadius, angle, point)
            f32 angle = 0.0f;
            Float3 point;
            f32 outerEllipseRadius;
            f32 innerEllipseRadius;

            outerEllipseRadius = radius + effect->secondaryRadius;
            innerEllipseRadius = effect->secondaryRadius - radius;
            innerRadius = effect->radius - radius;
            radius += effect->radius;

            for (i = effect->vertexSegmentCount + 1; i > 0; --i)
            {
                point.FromRotatedVec2(angle, radius, outerEllipseRadius);
                Rotate(&vertex->pos, &point, effect->secondaryAngle);
                vertex->pos += effect->vector5;
                vertex->pos.x += g_GameManager.arcadeRegionTopLeftPos.x;
                vertex->pos.y += g_GameManager.arcadeRegionTopLeftPos.y;
                vertex->pos.z = 0.0f;
                vertex++;

                point.FromRotatedVec2(angle, innerRadius, innerEllipseRadius);
                Rotate(&vertex->pos, &point, effect->secondaryAngle);
                vertex->pos += effect->vector5;
                vertex->pos.x += g_GameManager.arcadeRegionTopLeftPos.x;
                vertex->pos.y += g_GameManager.arcadeRegionTopLeftPos.y;
                vertex->pos.z = 0.0f;
                vertex++;

                angle += angleStep;
            }
        }
        else
        {
#if defined(PSP)
            th08::psp::RadialTrailTelemetryNoteRebuild(
                effect->vertexSegmentCount,
                th08::psp::RadialTrailBranch::Wavy, false);
#endif
#pragma var_order(secondAngleStep, secondAngle, radialOffset, angle, unused)
            f32 secondAngle;
            f32 angle;
            f32 secondAngleStep;
            f32 radialOffset;

            secondAngle = effect->secondaryAngle;
            angle = effect->angle;
            secondAngleStep = ZUN_2PI * effect->radialWaveCount / effect->vertexSegmentCount;
            Float3 unused;
            innerRadius = effect->radius - radius;
            radius += effect->radius;

            for (i = effect->vertexSegmentCount + 1; i > 0; --i)
            {
                if (angle >= ZUN_PI)
                    angle -= ZUN_2PI;
                if (secondAngle >= ZUN_PI)
                    secondAngle -= ZUN_2PI;

                radialOffset = cosf(secondAngle) * effect->secondaryRadius;
                vertex->pos.FromAngleMagnitude(angle, radius + radialOffset);
                vertex->pos += effect->vector5;
                vertex->pos.x += g_GameManager.arcadeRegionTopLeftPos.x;
                vertex->pos.y += g_GameManager.arcadeRegionTopLeftPos.y;
                vertex->pos.z = 0.0f;
                vertex++;

                vertex->pos.FromAngleMagnitude(angle, innerRadius + radialOffset);
                vertex->pos += effect->vector5;
                vertex->pos.x += g_GameManager.arcadeRegionTopLeftPos.x;
                vertex->pos.y += g_GameManager.arcadeRegionTopLeftPos.y;
                vertex->pos.z = 0.0f;
                vertex++;

                angle += angleStep;
                secondAngle += secondAngleStep;
            }
        }
        effect->verticesDirty = 0;
    }

    g_AnmManager->DrawVertices(&effect->vm, effect->vertices, effect->vertexSegmentCount * 2 + 2);
    return 1;
}

// FUNCTION: th08 0x427970
i32 __fastcall InitializeAlternateLayerRadialTrail(Effect *effect)
{
    InitializeRadialTrail(effect);
    effect->alternateDrawGroup = 1;
    return 0;
}

// FUNCTION: th08 0x427990
i32 __fastcall SyncRadialTrailRadius(Effect *effect)
{
    effect->verticesDirty = 1;
    effect->shapeThickness = effect->vm.scale.x;
    effect->radius = effect->vm.pos.x;
    return 1;
}

// FUNCTION: th08 0x4279d0
i32 __fastcall SyncRadialTrailShape(Effect *effect)
{
    effect->vertexSegmentCount = effect->vm.intVar0;
    effect->radialWaveCount = (f32)effect->vm.intVar1;
    effect->shapeThickness = effect->vm.scale.x;
    effect->radius = effect->vm.pos.x;
    effect->secondaryRadius = effect->vm.pos.y;
    effect->angle = effect->vm.rotation.z;
    effect->secondaryAngle = effect->vm.rotation.y;
    effect->verticesDirty = 1;
    return 1;
}

// FUNCTION: th08 0x427a60
i32 __fastcall UpdateTimedRadialTrail(Effect *effect)
{
    effect->vertexSegmentCount = 32;
    effect->shapeThickness = effect->vm.scale.x;
    effect->radius = effect->vm.pos.x;
    effect->secondaryRadius = effect->vm.pos.y;
    effect->verticesDirty = 1;
    if (effect->timer >= 120)
        return 0;
    return 1;
}

// FUNCTION: th08 0x427ae0
i32 __fastcall UpdateFadingRadialTrail(Effect *effect)
{
    effect->verticesDirty = 1;
    effect->shapeThickness = effect->vm.scale.x;
    effect->radius = effect->vm.pos.x;
    effect->secondaryRadius = effect->vm.pos.y;
    effect->angle = effect->vm.rotation.z;
    if (effect->vm.color1.a == 0)
        return 0;
    return 1;
}

// FUNCTION: th08 0x427b50
i32 __fastcall SyncAnchoredRadialTrail(Effect *effect)
{
    effect->vertexSegmentCount = effect->vm.intVar0;
    effect->radialWaveCount = (f32)effect->vm.intVar1;
    effect->shapeThickness = effect->vm.scale.x;
    effect->radius = effect->vm.floatVar1;
    effect->angle = effect->vm.rotation.z;
    effect->secondaryAngle = effect->vm.rotation.y;
    effect->verticesDirty = 1;
    effect->vector5 = effect->vm.pos;
    return 1;
}

// FUNCTION: th08 0x427bf0
#pragma var_order(effect, i)
ChainCallbackResult EffectManager::OnUpdate(EffectManager *effectManager)
{
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
    th08::psp::PerfAttributionScope perfScope(
        th08::psp::PerfAttributionPhase::EffectUpdate);
#endif
    Effect *effect = effectManager->effects;
    i32 i;

    effectManager->activeCount = 0;
    effectManager->drawGroupTails[0] = &effectManager->drawGroupSentinel0;
    effectManager->drawGroupTails[1] = &effectManager->drawGroupSentinel1;
    effectManager->drawGroupTails[2] = &effectManager->drawGroupSentinel2;
    effectManager->drawGroupTails[3] = &effectManager->drawGroupSentinel3;
    effectManager->drawGroupTails[4] = &effectManager->drawGroupSentinel4;

    effectManager->drawGroupSentinel0.nextInDrawGroup = NULL;
    effectManager->drawGroupSentinel1.nextInDrawGroup = NULL;
    effectManager->drawGroupSentinel2.nextInDrawGroup = NULL;
    effectManager->drawGroupSentinel3.nextInDrawGroup = NULL;
    effectManager->drawGroupSentinel4.nextInDrawGroup = NULL;

    for (i = 0; i < 653; i++, effect++)
    {
#if TH08_PSP_EFFECT_OCCUPANCY_FASTPATH_ENABLED
        // Test the 84-byte sidecar before touching an 864-byte Effect.  Mark()
        // is published by every activation path, so a higher-index effect
        // spawned by an update callback is still visited later this frame.
        if (!g_PspEffectOccupancy.Test(static_cast<u32>(i)))
            continue;
#elif TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED
        // Shadow only: the canonical visit decides; the bit is compared.
        const bool pspOccupancyTested =
            g_PspEffectOccupancy.Test(static_cast<u32>(i));
#endif
        if (effect->active == 0)
        {
#if TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED
            th08::psp::EffectOccupancyAuditNoteInactive(
                pspOccupancyTested, effect->vertices != NULL);
#endif
            if (effect->vertices != NULL)
            {
                g_ZunMemory.Free(effect->vertices);
                effect->vertices = NULL;
            }
#if TH08_PSP_EFFECT_OCCUPANCY_SIDECAR_ENABLED
            // Clear only after the canonical inactive-slot cleanup.  Update
            // callbacks, ANM completion, and external deactivation deliberately
            // leave a stale positive until this next visit.
            g_PspEffectOccupancy.Forget(static_cast<u32>(i));
#endif
            continue;
        }

#if TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED
        th08::psp::EffectOccupancyAuditNoteActive(pspOccupancyTested);
#endif
        effectManager->activeCount++;
        if (!g_GameManager.flags.deathbombFreezeActive ||
            effect->updateDuringFreeze != 0)
        {
            if (effect->updateCallback != NULL && effect->updateCallback(effect) != 1)
            {
                effect->active = 0;
                continue;
            }
            if (g_AnmManager->ExecuteScript(&effect->vm))
            {
                effect->active = 0;
                continue;
            }
            effect->timer++;
        }

        effect->nextInDrawGroup = NULL;
        if (effect->effectId == 0x40)
            continue;

        if (effect->drawGroup == 1 || effect->drawGroup >= 3)
        {
            effectManager->drawGroupTails[1]->nextInDrawGroup = effect;
            effectManager->drawGroupTails[1] = effect;
        }
        else if (effect->drawGroup == 0)
        {
            if (effect->alternateDrawGroup != 0)
            {
                effectManager->drawGroupTails[3]->nextInDrawGroup = effect;
                effectManager->drawGroupTails[3] = effect;
            }
            else if (((*reinterpret_cast<u32 *>(&effect->vm.flags) >> 4) & 3) == 1)
            {
                effectManager->drawGroupTails[4]->nextInDrawGroup = effect;
                effectManager->drawGroupTails[4] = effect;
            }
            else
            {
                effectManager->drawGroupTails[0]->nextInDrawGroup = effect;
                effectManager->drawGroupTails[0] = effect;
            }
        }
        else
        {
            effectManager->drawGroupTails[2]->nextInDrawGroup = effect;
            effectManager->drawGroupTails[2] = effect;
        }
    }

#if defined(PSP)
    th08::psp::RenderPerfNoteEffectsActive(
        effectManager->activeCount > 0
            ? static_cast<u32>(effectManager->activeCount)
            : 0U);
#endif

    if (++effectManager->tamperCheckCounter % 300 == 100 &&
        g_GameManager.IsTampered())
        return CHAIN_CALLBACK_RESULT_EXIT_GAME_SUCCESS;
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

// FUNCTION: th08 0x427f00
#pragma var_order(effect)
ChainCallbackResult EffectManager::OnDraw(EffectManager *effectManager)
{
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
    th08::psp::PerfAttributionScope perfScope(
        th08::psp::PerfAttributionPhase::EffectDrawMain);
#endif
    Effect *effect;

#if defined(PSP) && defined(TH08_PSP_EFFECT_INDEXED_QUADS) && \
    TH08_PSP_EFFECT_INDEXED_QUADS && \
    defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
    TH08_PSP_BULLET_UNIFIED_QUADS
    PspEffectIndexedQuadPass pspEffectIndexedQuadPass(g_AnmManager);
#endif

#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT
    PspEffectSpritePairAuditPass pspEffectSpritePairGroup0(
        effectManager, 0U);
#elif defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
    PspEffectSpritePairProductPass pspEffectSpritePairGroup0(
        effectManager, 0U);
#endif

    effect = effectManager->drawGroupSentinel0.nextInDrawGroup;
    while (effect != NULL)
    {
#if defined(PSP)
        th08::psp::RenderPerfNoteEffectDrawn();
#endif
        if (effect->drawCallback != NULL)
        {
#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
            pspEffectSpritePairGroup0.CallbackBoundary();
#endif
            effect->drawCallback(effect);
        }
        else
        {
            effect->vm.pos = effect->position;
            effect->vm.pos.x += g_GameManager.arcadeRegionTopLeftPos.x;
            effect->vm.pos.y += g_GameManager.arcadeRegionTopLeftPos.y;
            effect->vm.pos.z = 0.07f;
            effect->vm.pos += effect->vm.pos2;
#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT
            pspEffectSpritePairGroup0.Draw(effect);
#elif defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
            pspEffectSpritePairGroup0.Draw(effect);
#else
            g_AnmManager->Draw2D(&effect->vm);
#endif
        }
        effect = effect->nextInDrawGroup;
    }

#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
    pspEffectSpritePairGroup0.Finish();
#endif

    effect = effectManager->drawGroupSentinel2.nextInDrawGroup;
    while (effect != NULL)
    {
#if defined(PSP)
        th08::psp::RenderPerfNoteEffectDrawn();
#endif
        effect->vm.pos = effect->position;
        g_AnmManager->DrawCameraFacingQuad(&effect->vm);
        effect = effect->nextInDrawGroup;
    }

#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT
    PspEffectSpritePairAuditPass pspEffectSpritePairGroup4(
        effectManager, 4U);
#elif defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
    PspEffectSpritePairProductPass pspEffectSpritePairGroup4(
        effectManager, 4U);
#endif
    effect = effectManager->drawGroupSentinel4.nextInDrawGroup;
    while (effect != NULL)
    {
#if defined(PSP)
        th08::psp::RenderPerfNoteEffectDrawn();
#endif
        if (effect->drawCallback != NULL)
        {
#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
            pspEffectSpritePairGroup4.CallbackBoundary();
#endif
            effect->drawCallback(effect);
        }
        else
        {
            effect->vm.pos = effect->position;
            effect->vm.pos.x += g_GameManager.arcadeRegionTopLeftPos.x;
            effect->vm.pos.y += g_GameManager.arcadeRegionTopLeftPos.y;
            effect->vm.pos.z = 0.07f;
            effect->vm.pos += effect->vm.pos2;
#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT
            pspEffectSpritePairGroup4.Draw(effect);
#elif defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
            pspEffectSpritePairGroup4.Draw(effect);
#else
            g_AnmManager->Draw2D(&effect->vm);
#endif
        }
        effect = effect->nextInDrawGroup;
    }

#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
    pspEffectSpritePairGroup4.Finish();
#endif

    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

// FUNCTION: th08 0x428100
#pragma var_order(effect, this)
i32 EffectManager::DrawBulletLayerEffects()
{
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
    th08::psp::PerfAttributionScope perfScope(
        th08::psp::PerfAttributionPhase::EffectDrawBullet);
#endif
    Effect *effect = this->drawGroupSentinel3.nextInDrawGroup;

#if defined(PSP) && defined(TH08_PSP_EFFECT_INDEXED_QUADS) && \
    TH08_PSP_EFFECT_INDEXED_QUADS && \
    defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
    TH08_PSP_BULLET_UNIFIED_QUADS
    PspEffectIndexedQuadPass pspEffectIndexedQuadPass(g_AnmManager);
#endif

#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT
    PspEffectSpritePairAuditPass pspEffectSpritePairGroup3(this, 3U);
#elif defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
    PspEffectSpritePairProductPass pspEffectSpritePairGroup3(this, 3U);
#endif

    while (effect != NULL)
    {
#if defined(PSP)
        th08::psp::RenderPerfNoteEffectDrawn();
#endif
        if (effect->drawCallback != NULL)
        {
#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
            pspEffectSpritePairGroup3.CallbackBoundary();
#endif
            effect->drawCallback(effect);
        }
        else
        {
            effect->vm.pos = effect->position;
            effect->vm.pos.x += g_GameManager.arcadeRegionTopLeftPos.x;
            effect->vm.pos.y += g_GameManager.arcadeRegionTopLeftPos.y;
            effect->vm.pos += effect->vm.pos2;
            effect->vm.pos.z = 0.04f;
#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT
            pspEffectSpritePairGroup3.Draw(effect);
#elif defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
            pspEffectSpritePairGroup3.Draw(effect);
#else
            g_AnmManager->Draw2D(&effect->vm);
#endif
        }

        effect = effect->nextInDrawGroup;
    }

#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
    pspEffectSpritePairGroup3.Finish();
#endif

    return 1;
}

// FUNCTION: th08 0x4281e0
#pragma var_order(effect, i, this)
i32 EffectManager::DrawBackgroundEffects()
{
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
    th08::psp::PerfAttributionScope perfScope(
        th08::psp::PerfAttributionPhase::EffectDrawBackground);
#endif
    Effect *effect = this->drawGroupSentinel1.nextInDrawGroup;
    i32 i = 0;

    if (g_Supervisor.cfg.effectQuality == MINIMUM)
    {
        return 1;
    }

#if defined(PSP) && defined(TH08_PSP_EFFECT_INDEXED_QUADS) && \
    TH08_PSP_EFFECT_INDEXED_QUADS && \
    defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
    TH08_PSP_BULLET_UNIFIED_QUADS
    PspEffectIndexedQuadPass pspEffectIndexedQuadPass(g_AnmManager);
#endif


#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT
    PspEffectSpritePairAuditPass pspEffectSpritePairGroup1(this, 1U);
#elif defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
    PspEffectSpritePairProductPass pspEffectSpritePairGroup1(this, 1U);
#endif

    while (effect != NULL)
    {
        i++;
        if (g_Supervisor.cfg.effectQuality == MODERATE && (i & 1) != 0)
        {
            return 1;
        }

#if defined(PSP)
        th08::psp::RenderPerfNoteEffectDrawn();
#endif

        effect->vm.pos = effect->position;
        if (effect->drawGroup == 4)
        {
#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT
            pspEffectSpritePairGroup1.Draw(effect);
#elif defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
            pspEffectSpritePairGroup1.Draw(effect);
#else
            g_AnmManager->Draw2D(&effect->vm);
#endif
        }
        else if (effect->drawGroup == 1)
        {
#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
            pspEffectSpritePairGroup1.Boundary();
#endif
            if (effect->effectId == 0x33 || effect->effectId == 0x3F)
            {
                g_AnmManager->DrawWithCallback(
                    &effect->vm, AdjustStageEffectDrawPosition);
            }
            else
            {
                g_AnmManager->DrawCameraFacingQuad(&effect->vm);
            }
        }
        else
        {
#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
            pspEffectSpritePairGroup1.Boundary();
#endif
            g_AnmManager->DrawProjected3DQuad(&effect->vm);
        }

        effect = effect->nextInDrawGroup;
    }
#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
    pspEffectSpritePairGroup1.Finish();
#endif
    return 1;
}

// FUNCTION: th08 0x428310
#pragma var_order(delta, point)
void __fastcall AdjustStageEffectDrawPosition(AnmVm *effect, D3DXVECTOR3 *base)
{
    D3DXVECTOR3 delta;
    D3DXVECTOR3 point;

    if (!g_GameManager.isInGameMenu && !g_GameManager.showRetryMenu)
    {
        point = *base + *reinterpret_cast<D3DXVECTOR3 *>(&effect->posFinal);
        delta = *reinterpret_cast<D3DXVECTOR3 *>(&effect->pos2) - point;
        if (effect->pos2.x > -9999.0f)
        {
            delta.x += 32.0f;
            delta.y += 16.0f;
            delta.z = 0.0f;
            if (D3DXVec3LengthSq(&delta) < 25600.0f)
            {
                effect->posInitial.x += 0.0005000000237487257f;
                *reinterpret_cast<D3DXVECTOR3 *>(&effect->posFinal) += delta * effect->posInitial.x;
            }
        }

        delta = point - reinterpret_cast<const D3DXVECTOR3 &>(g_Player.position);
        delta.x -= 32.0f;
        delta.y -= 16.0f;
        delta.z = 0.0f;
        if (D3DXVec3LengthSq(&delta) < 7744.0f)
        {
            *reinterpret_cast<D3DXVECTOR3 *>(&effect->posFinal) += delta * 0.019999999552965164f;
        }
    }
    *base += *reinterpret_cast<D3DXVECTOR3 *>(&effect->posFinal);
}

// FUNCTION: th08 0x4284b0
ZunResult EffectManager::LoadEffectResources(EffectManager *effectManager)
{
#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT
    PspEffectSpritePairResetGeneration();
#elif defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
    PspEffectSpritePairProductReset();
#endif
    effectManager->ResetEffects();
    effectManager->effectAnm = g_AnmManager->GetAnm(6);
    g_GuiMessageStageMode = 0;
    g_Background.spellVmCount = 2;

    if (!IsDisableResourceReload())
    {
        if (!g_GameManager.IsSpellPractice() || g_GameManager.currentSpellCardNumber < 216)
        {
            effectManager->stageEffectAnm = g_AnmManager->PreloadAnm(9, g_EffectAnms[g_GameManager.currentStage]);
        }
        else
        {
            effectManager->stageEffectAnm =
                g_AnmManager->PreloadAnm(9, g_EffectAnms[g_GameManager.currentSpellCardNumber - 216 + 9]);
        }
        if (effectManager->stageEffectAnm == NULL)
            return ZUN_ERROR;
    }
    else
    {
        effectManager->stageEffectAnm = g_AnmManager->GetAnm(9);
    }
    return ZUN_SUCCESS;
}

// FUNCTION: th08 0x428590
#pragma var_order(effect, i)
ZunResult EffectManager::ReleaseEffectResources(EffectManager *effectManager)
{
#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT
    // Flush before touching Effect storage. The aggregate observer must
    // describe the complete canonical lifetime even when cadence skipped the
    // exact periodic boundary or the stage ended before 600 frames.
    PspEffectSpritePairFinalize("teardown");
#elif defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
    // Product hot paths only increment counters.  One teardown snapshot keeps
    // normal frames free of logging and captures the complete stage lifetime.
    PspEffectSpritePairProductReport();
#endif
    Effect *effect = effectManager->effects;
    i32 i;
    for (i = 0; i < 653; i++, effect++)
    {
        if (effect->vertices != NULL)
        {
            g_ZunMemory.Free(effect->vertices);
            effect->vertices = NULL;
        }
    }
    if (!IsDisableResourceReload())
        g_AnmManager->ReleaseAnm(9);
    return ZUN_SUCCESS;
}

// FUNCTION: th08 0x428620
ZunResult EffectManager::RegisterChain()
{
    EffectManager *effectManager = &g_EffectManager;
    effectManager->ResetEffects();
    g_EffectManagerCalcChain.SetCallback((ChainCallback)EffectManager::OnUpdate);
    g_EffectManagerCalcChain.addedCallback = (ChainLifetimeCallback)EffectManager::LoadEffectResources;
    g_EffectManagerCalcChain.deletedCallback = (ChainLifetimeCallback)EffectManager::ReleaseEffectResources;
    g_EffectManagerCalcChain.arg = effectManager;
    if (g_Chain.AddToCalcChain(&g_EffectManagerCalcChain, CHAIN_PRIO_CALC_EFFECTMANAGER) != ZUN_SUCCESS)
        return ZUN_ERROR;

    g_EffectManagerDrawChain.SetCallback((ChainCallback)EffectManager::OnDraw);
    g_EffectManagerDrawChain.arg = effectManager;
    g_Chain.AddToDrawChain(&g_EffectManagerDrawChain, CHAIN_PRIO_DRAW_EFFECTMANAGER);
    return ZUN_SUCCESS;
}

// FUNCTION: th08 0x4286b0
void EffectManager::CutChain()
{
    g_Chain.Cut(&g_EffectManagerCalcChain);
    g_Chain.Cut(&g_EffectManagerDrawChain);
}

// FUNCTION: th08 0x428720
i32 __fastcall HasAnimationEnded(Effect *effect)
{
    return effect->vm.currentInstruction == NULL;
}

// FUNCTION: th08 0x428740
EffectManager::EffectManager()
{
    this->ResetEffects();
    this->scaleX = 1.0f;
    this->scaleY = 1.0f;
    this->scaleZ = 1.0f;
    this->scaleW = 1.0f;
}

// FUNCTION: th08 0x4287e0
Effect::Effect()
{
}


} // namespace th08
