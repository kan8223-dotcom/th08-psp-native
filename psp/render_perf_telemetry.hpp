#pragma once

#include "runtime_telemetry_config.hpp"

#include <cstdint>

namespace th08::psp
{
// Draw/state counters are owned by the SC render thread. Texture loading can
// run on TH08's SC setup thread while the loading screen is presented, so the
// transfer/text fields use lock-free atomic additions and frame exchanges.
// No hook performs allocation, clock reads, or I/O.
struct RenderPerfCounters
{
    std::uint64_t draws;
    std::uint64_t vertices;
    // Successful logical vertices owned by Supervisor's wall-clock FPS text
    // and replay-FPS diagnostic text.  These remain a subset of vertices;
    // gameVertices is derived per completed Present so A/B gates can remove
    // only this explicitly-owned presentation noise.
    std::uint64_t fpsOverlayVertices;
    std::uint64_t gameVertices;
    // Logical D3D compatibility-state setter invocations. Rejected or ignored
    // requests still count because this measures caller-side state traffic.
    std::uint64_t stateRequested;
    // GL state/client-array calls actually issued by the instrumented draw
    // path. Texture image transfers, the draw call itself, and immediate-mode
    // per-vertex attribute calls are not included.
    std::uint64_t stateEmitted;
    // CPU-derived matrices actually rebuilt, not matrix setter requests.
    std::uint64_t matrixRecomputes;
    // Presentation-only sin/cos pairs evaluated by the Allegrex VFPU.  This
    // proves that the renderer uses VFPU on a real hot path; merely linking
    // libpspvfpu is not evidence of execution.
    std::uint64_t vfpuSinCos;
    std::uint64_t backgroundInstanceVisits;
    std::uint64_t backgroundCandidates;
    std::uint64_t backgroundInstancesDrawn;
    std::uint64_t backgroundProjectCalls;
    std::uint64_t backgroundRadiusCacheHits;
    std::uint64_t effectSpawnRequests;
    std::uint64_t effectsActive;
    std::uint64_t effectsDrawn;
    std::uint64_t itemSpawnRequests;
    // Audit-only script-68 spawn initializer workload.  These counters are
    // still present when the gate is off (and remain zero), keeping sampler
    // layout and reset/take behavior simple and deterministic.
    std::uint64_t itemTimeSpawnInitCandidates;
    std::uint64_t itemTimeSpawnInitEligible;
    std::uint64_t itemTimeSpawnInitFullVmMismatches;
    std::uint64_t itemTimeSpawnInitFieldMismatches;
    std::uint64_t itemTimeSpawnInitFallbacks;
    std::uint64_t itemsDrawn;
    std::uint64_t popupsActive;
    std::uint64_t popupDigits;
    // Score-only PSP sprite-pair experiment.  A submitted sprite replaces six
    // 28-byte frontend vertices with two; the saved fields make that work
    // reduction explicit instead of inferring it from scene-dependent totals.
    std::uint64_t asciiPopupBatchCalls;
    std::uint64_t asciiPopupBatchDigits;
    std::uint64_t asciiPopupBatchSprites;
    std::uint64_t asciiPopupBatchVerticesSaved;
    std::uint64_t asciiPopupBatchBytesSaved;
    std::uint64_t asciiPopupBatchFallbacks;
    // Work removed by the independently gated direct-pair frontend relative
    // to the accepted two-pass score batch.  Active score popups make the
    // shared-Y nearbyintf algebra independently checkable; remaining units
    // are full validation quads, viewport-cull tests, and nearbyintf calls.
    std::uint64_t asciiPopupDirectActivePopups;
    std::uint64_t asciiPopupDirectValidationQuadsAvoided;
    std::uint64_t asciiPopupDirectValidationCullTestsAvoided;
    std::uint64_t asciiPopupDirectNearbyintCallsAvoided;
    // High-level texture-transfer entry attempts, including clean early-outs
    // and rejected requests.
    std::uint64_t uploadAttempts;
    // PSPGL texture image/subimage submissions in d3d8_compat. A NULL backing
    // definition has zero uploadBytes but still counts as a submission.
    std::uint64_t actualUploads;
    std::uint64_t uploadBytes;
    // PSP text-raster source-rectangle bytes submitted to the
    // upload/resample path (not an estimate of repeated filter reads).
    std::uint64_t textBytes;
};

struct RenderPerfFrameCounters
{
    std::uint32_t draws;
    std::uint32_t vertices;
    std::uint32_t fpsOverlayVertices;
    std::uint32_t gameVertices;
    std::uint32_t stateRequested;
    std::uint32_t stateEmitted;
    std::uint32_t matrixRecomputes;
    std::uint32_t vfpuSinCos;
    std::uint32_t backgroundInstanceVisits;
    std::uint32_t backgroundCandidates;
    std::uint32_t backgroundInstancesDrawn;
    std::uint32_t backgroundProjectCalls;
    std::uint32_t backgroundRadiusCacheHits;
    std::uint32_t effectSpawnRequests;
    std::uint32_t effectsActive;
    std::uint32_t effectsDrawn;
    std::uint32_t itemSpawnRequests;
    std::uint32_t itemTimeSpawnInitCandidates;
    std::uint32_t itemTimeSpawnInitEligible;
    std::uint32_t itemTimeSpawnInitFullVmMismatches;
    std::uint32_t itemTimeSpawnInitFieldMismatches;
    std::uint32_t itemTimeSpawnInitFallbacks;
    std::uint32_t itemsDrawn;
    std::uint32_t popupsActive;
    std::uint32_t popupDigits;
    std::uint32_t asciiPopupBatchCalls;
    std::uint32_t asciiPopupBatchDigits;
    std::uint32_t asciiPopupBatchSprites;
    std::uint32_t asciiPopupBatchVerticesSaved;
    std::uint32_t asciiPopupBatchBytesSaved;
    std::uint32_t asciiPopupBatchFallbacks;
    std::uint32_t asciiPopupDirectActivePopups;
    std::uint32_t asciiPopupDirectValidationQuadsAvoided;
    std::uint32_t asciiPopupDirectValidationCullTestsAvoided;
    std::uint32_t asciiPopupDirectNearbyintCallsAvoided;
    std::uint32_t uploadAttempts;
    std::uint32_t actualUploads;
    std::uint32_t uploadBytes;
    std::uint32_t textBytes;
};

struct RenderPerfIntervalSnapshot
{
    std::uint32_t frames;
    RenderPerfCounters total;
    RenderPerfFrameCounters peak;
};

#if TH08_PSP_RUNTIME_TELEMETRY
extern RenderPerfFrameCounters gRenderPerfCurrentFrame;

// AsciiManager queues FPS-overlay glyphs into AnmManager's ordinary shared
// sprite batch.  The pending count is attached to that batch only after the
// canonical DrawNoRotation path actually appends the glyph.  Flush then opens
// this scope: only a completely submitted logical batch commits its owned
// vertices.  Saving the previous attribution state makes nested flushes safe.
class RenderPerfFpsOverlayDrawScope
{
public:
    explicit RenderPerfFpsOverlayDrawScope(std::uint32_t expectedVertices);
    ~RenderPerfFpsOverlayDrawScope();

    RenderPerfFpsOverlayDrawScope(const RenderPerfFpsOverlayDrawScope &) = delete;
    RenderPerfFpsOverlayDrawScope &operator=(
        const RenderPerfFpsOverlayDrawScope &) = delete;

private:
    std::uint32_t savedExpectedVertices_;
    std::uint32_t savedSubmittedVertices_;
    std::uint32_t savedOwnedVertices_;
    bool active_;
};

extern std::uint32_t gRenderPerfFpsOverlayExpectedVertices;
extern std::uint32_t gRenderPerfFpsOverlaySubmittedVertices;

void RenderPerfQueueFpsOverlayVertices(std::uint32_t vertices);
void RenderPerfDiscardQueuedFpsOverlayVertices();

inline void RenderPerfNoteDraw(std::uint32_t vertices)
{
    ++gRenderPerfCurrentFrame.draws;
    gRenderPerfCurrentFrame.vertices += vertices;
    if (gRenderPerfFpsOverlayExpectedVertices != 0U)
        gRenderPerfFpsOverlaySubmittedVertices += vertices;
}

inline void RenderPerfNoteStateRequested(std::uint32_t count = 1U)
{
    gRenderPerfCurrentFrame.stateRequested += count;
}

inline void RenderPerfNoteStateEmitted(std::uint32_t count)
{
    gRenderPerfCurrentFrame.stateEmitted += count;
}

inline void RenderPerfNoteMatrixRecompute(std::uint32_t count = 1U)
{
    gRenderPerfCurrentFrame.matrixRecomputes += count;
}

inline void RenderPerfNoteVfpuSinCos(std::uint32_t count = 1U)
{
    gRenderPerfCurrentFrame.vfpuSinCos += count;
}

inline void RenderPerfNoteBackgroundInstanceVisit(std::uint32_t count = 1U)
{
    gRenderPerfCurrentFrame.backgroundInstanceVisits += count;
}

inline void RenderPerfNoteBackgroundCandidate(std::uint32_t count = 1U)
{
    gRenderPerfCurrentFrame.backgroundCandidates += count;
}

inline void RenderPerfNoteBackgroundDrawn(std::uint32_t count = 1U)
{
    gRenderPerfCurrentFrame.backgroundInstancesDrawn += count;
}

inline void RenderPerfNoteBackgroundProject(std::uint32_t count = 1U)
{
    gRenderPerfCurrentFrame.backgroundProjectCalls += count;
}

inline void RenderPerfNoteBackgroundRadiusCacheHit(std::uint32_t count = 1U)
{
    gRenderPerfCurrentFrame.backgroundRadiusCacheHits += count;
}

inline void RenderPerfNoteEffectSpawnRequest(std::uint32_t count = 1U)
{
    gRenderPerfCurrentFrame.effectSpawnRequests += count;
}

inline void RenderPerfNoteEffectsActive(std::uint32_t count)
{
    gRenderPerfCurrentFrame.effectsActive += count;
}

inline void RenderPerfNoteEffectDrawn(std::uint32_t count = 1U)
{
    gRenderPerfCurrentFrame.effectsDrawn += count;
}

inline void RenderPerfNoteItemSpawnRequest(std::uint32_t count = 1U)
{
    gRenderPerfCurrentFrame.itemSpawnRequests += count;
}

inline void RenderPerfNoteItemTimeSpawnInitAudit(bool eligible,
                                                  bool fullVmMismatch,
                                                  bool fieldMismatch,
                                                  bool fallback)
{
    ++gRenderPerfCurrentFrame.itemTimeSpawnInitCandidates;
    if (eligible)
        ++gRenderPerfCurrentFrame.itemTimeSpawnInitEligible;
    if (fullVmMismatch)
        ++gRenderPerfCurrentFrame.itemTimeSpawnInitFullVmMismatches;
    if (fieldMismatch)
        ++gRenderPerfCurrentFrame.itemTimeSpawnInitFieldMismatches;
    if (fallback)
        ++gRenderPerfCurrentFrame.itemTimeSpawnInitFallbacks;
}

inline void RenderPerfNoteItemDrawn(std::uint32_t count = 1U)
{
    gRenderPerfCurrentFrame.itemsDrawn += count;
}

inline void RenderPerfNotePopup(std::uint32_t digits)
{
    ++gRenderPerfCurrentFrame.popupsActive;
    gRenderPerfCurrentFrame.popupDigits += digits;
}

inline void RenderPerfNoteAsciiPopupBatchSuccess(std::uint32_t digits,
                                                  std::uint32_t sprites)
{
    ++gRenderPerfCurrentFrame.asciiPopupBatchCalls;
    gRenderPerfCurrentFrame.asciiPopupBatchDigits += digits;
    gRenderPerfCurrentFrame.asciiPopupBatchSprites += sprites;
    gRenderPerfCurrentFrame.asciiPopupBatchVerticesSaved += sprites * 4U;
    gRenderPerfCurrentFrame.asciiPopupBatchBytesSaved += sprites * 4U * 28U;
}

inline void RenderPerfNoteAsciiPopupBatchFallback()
{
    ++gRenderPerfCurrentFrame.asciiPopupBatchFallbacks;
}

inline void RenderPerfNoteAsciiPopupDirectPairSavings(
    std::uint32_t digits, std::uint32_t activePopups)
{
    gRenderPerfCurrentFrame.asciiPopupDirectActivePopups += activePopups;
    gRenderPerfCurrentFrame.asciiPopupDirectValidationQuadsAvoided += digits;
    gRenderPerfCurrentFrame.asciiPopupDirectValidationCullTestsAvoided +=
        digits;
    // Old batch: four nearbyintf calls in each of validation and generation.
    // Direct batch: two X calls per digit plus two shared Y calls per popup.
    const std::uint32_t oldCalls = digits * 8U;
    const std::uint32_t directCalls = digits * 2U + activePopups * 2U;
    if (oldCalls >= directCalls)
    {
        gRenderPerfCurrentFrame.asciiPopupDirectNearbyintCallsAvoided +=
            oldCalls - directCalls;
    }
}

inline void RenderPerfNoteUploadAttempt(std::uint32_t count = 1U)
{
    __sync_fetch_and_add(&gRenderPerfCurrentFrame.uploadAttempts, count);
}

inline void RenderPerfNoteActualUpload(std::uint32_t bytes)
{
    __sync_fetch_and_add(&gRenderPerfCurrentFrame.actualUploads, 1U);
    __sync_fetch_and_add(&gRenderPerfCurrentFrame.uploadBytes, bytes);
}

inline void RenderPerfNoteTextBytes(std::uint32_t bytes)
{
    __sync_fetch_and_add(&gRenderPerfCurrentFrame.textBytes, bytes);
}

void RenderPerfTelemetryReset();
void RenderPerfTelemetryEndFrame();
RenderPerfIntervalSnapshot RenderPerfTelemetryTakeInterval();
#else
class RenderPerfFpsOverlayDrawScope
{
public:
    explicit RenderPerfFpsOverlayDrawScope(std::uint32_t) {}
    ~RenderPerfFpsOverlayDrawScope() = default;

    RenderPerfFpsOverlayDrawScope(const RenderPerfFpsOverlayDrawScope &) = delete;
    RenderPerfFpsOverlayDrawScope &operator=(
        const RenderPerfFpsOverlayDrawScope &) = delete;
};

inline void RenderPerfQueueFpsOverlayVertices(std::uint32_t) {}
inline void RenderPerfDiscardQueuedFpsOverlayVertices() {}
inline void RenderPerfNoteDraw(std::uint32_t) {}
inline void RenderPerfNoteStateRequested(std::uint32_t = 1U) {}
inline void RenderPerfNoteStateEmitted(std::uint32_t) {}
inline void RenderPerfNoteMatrixRecompute(std::uint32_t = 1U) {}
inline void RenderPerfNoteVfpuSinCos(std::uint32_t = 1U) {}
inline void RenderPerfNoteBackgroundInstanceVisit(std::uint32_t = 1U) {}
inline void RenderPerfNoteBackgroundCandidate(std::uint32_t = 1U) {}
inline void RenderPerfNoteBackgroundDrawn(std::uint32_t = 1U) {}
inline void RenderPerfNoteBackgroundProject(std::uint32_t = 1U) {}
inline void RenderPerfNoteBackgroundRadiusCacheHit(std::uint32_t = 1U) {}
inline void RenderPerfNoteEffectSpawnRequest(std::uint32_t = 1U) {}
inline void RenderPerfNoteEffectsActive(std::uint32_t) {}
inline void RenderPerfNoteEffectDrawn(std::uint32_t = 1U) {}
inline void RenderPerfNoteItemSpawnRequest(std::uint32_t = 1U) {}
inline void RenderPerfNoteItemTimeSpawnInitAudit(bool, bool, bool, bool) {}
inline void RenderPerfNoteItemDrawn(std::uint32_t = 1U) {}
inline void RenderPerfNotePopup(std::uint32_t) {}
inline void RenderPerfNoteAsciiPopupBatchSuccess(std::uint32_t,
                                                  std::uint32_t) {}
inline void RenderPerfNoteAsciiPopupBatchFallback() {}
inline void RenderPerfNoteAsciiPopupDirectPairSavings(std::uint32_t,
                                                       std::uint32_t) {}
inline void RenderPerfNoteUploadAttempt(std::uint32_t = 1U) {}
inline void RenderPerfNoteActualUpload(std::uint32_t) {}
inline void RenderPerfNoteTextBytes(std::uint32_t) {}
inline void RenderPerfTelemetryReset() {}
inline void RenderPerfTelemetryEndFrame() {}
inline RenderPerfIntervalSnapshot RenderPerfTelemetryTakeInterval()
{
    return RenderPerfIntervalSnapshot{};
}
#endif
} // namespace th08::psp
