#include "render_perf_telemetry.hpp"

namespace th08::psp
{
RenderPerfFrameCounters gRenderPerfCurrentFrame{};
std::uint32_t gRenderPerfFpsOverlayExpectedVertices = 0U;
std::uint32_t gRenderPerfFpsOverlaySubmittedVertices = 0U;

namespace
{
RenderPerfCounters gIntervalTotal{};
RenderPerfFrameCounters gIntervalPeak{};
std::uint32_t gIntervalFrames = 0;
std::uint32_t gQueuedFpsOverlayVertices = 0U;
std::uint32_t gRenderPerfFpsOverlayOwnedVertices = 0U;

void Accumulate(RenderPerfCounters *destination, const RenderPerfFrameCounters &source)
{
    destination->draws += source.draws;
    destination->vertices += source.vertices;
    destination->fpsOverlayVertices += source.fpsOverlayVertices;
    destination->gameVertices += source.gameVertices;
    destination->stateRequested += source.stateRequested;
    destination->stateEmitted += source.stateEmitted;
    destination->matrixRecomputes += source.matrixRecomputes;
    destination->vfpuSinCos += source.vfpuSinCos;
    destination->backgroundInstanceVisits += source.backgroundInstanceVisits;
    destination->backgroundCandidates += source.backgroundCandidates;
    destination->backgroundInstancesDrawn += source.backgroundInstancesDrawn;
    destination->backgroundProjectCalls += source.backgroundProjectCalls;
    destination->backgroundRadiusCacheHits += source.backgroundRadiusCacheHits;
    destination->effectSpawnRequests += source.effectSpawnRequests;
    destination->effectsActive += source.effectsActive;
    destination->effectsDrawn += source.effectsDrawn;
    destination->itemSpawnRequests += source.itemSpawnRequests;
    destination->itemTimeSpawnInitCandidates +=
        source.itemTimeSpawnInitCandidates;
    destination->itemTimeSpawnInitEligible +=
        source.itemTimeSpawnInitEligible;
    destination->itemTimeSpawnInitFullVmMismatches +=
        source.itemTimeSpawnInitFullVmMismatches;
    destination->itemTimeSpawnInitFieldMismatches +=
        source.itemTimeSpawnInitFieldMismatches;
    destination->itemTimeSpawnInitFallbacks +=
        source.itemTimeSpawnInitFallbacks;
    destination->itemsDrawn += source.itemsDrawn;
    destination->popupsActive += source.popupsActive;
    destination->popupDigits += source.popupDigits;
    destination->asciiPopupBatchCalls += source.asciiPopupBatchCalls;
    destination->asciiPopupBatchDigits += source.asciiPopupBatchDigits;
    destination->asciiPopupBatchSprites += source.asciiPopupBatchSprites;
    destination->asciiPopupBatchVerticesSaved +=
        source.asciiPopupBatchVerticesSaved;
    destination->asciiPopupBatchBytesSaved += source.asciiPopupBatchBytesSaved;
    destination->asciiPopupBatchFallbacks += source.asciiPopupBatchFallbacks;
    destination->asciiPopupDirectActivePopups +=
        source.asciiPopupDirectActivePopups;
    destination->asciiPopupDirectValidationQuadsAvoided +=
        source.asciiPopupDirectValidationQuadsAvoided;
    destination->asciiPopupDirectValidationCullTestsAvoided +=
        source.asciiPopupDirectValidationCullTestsAvoided;
    destination->asciiPopupDirectNearbyintCallsAvoided +=
        source.asciiPopupDirectNearbyintCallsAvoided;
    destination->uploadAttempts += source.uploadAttempts;
    destination->actualUploads += source.actualUploads;
    destination->uploadBytes += source.uploadBytes;
    destination->textBytes += source.textBytes;
}

void Maximize(RenderPerfFrameCounters *destination, const RenderPerfFrameCounters &source)
{
#define TH08_PSP_RENDER_PERF_MAX(field)       \
    do                                        \
    {                                         \
        if (source.field > destination->field) \
            destination->field = source.field; \
    } while (0)
    TH08_PSP_RENDER_PERF_MAX(draws);
    TH08_PSP_RENDER_PERF_MAX(vertices);
    TH08_PSP_RENDER_PERF_MAX(fpsOverlayVertices);
    TH08_PSP_RENDER_PERF_MAX(gameVertices);
    TH08_PSP_RENDER_PERF_MAX(stateRequested);
    TH08_PSP_RENDER_PERF_MAX(stateEmitted);
    TH08_PSP_RENDER_PERF_MAX(matrixRecomputes);
    TH08_PSP_RENDER_PERF_MAX(vfpuSinCos);
    TH08_PSP_RENDER_PERF_MAX(backgroundInstanceVisits);
    TH08_PSP_RENDER_PERF_MAX(backgroundCandidates);
    TH08_PSP_RENDER_PERF_MAX(backgroundInstancesDrawn);
    TH08_PSP_RENDER_PERF_MAX(backgroundProjectCalls);
    TH08_PSP_RENDER_PERF_MAX(backgroundRadiusCacheHits);
    TH08_PSP_RENDER_PERF_MAX(effectSpawnRequests);
    TH08_PSP_RENDER_PERF_MAX(effectsActive);
    TH08_PSP_RENDER_PERF_MAX(effectsDrawn);
    TH08_PSP_RENDER_PERF_MAX(itemSpawnRequests);
    TH08_PSP_RENDER_PERF_MAX(itemTimeSpawnInitCandidates);
    TH08_PSP_RENDER_PERF_MAX(itemTimeSpawnInitEligible);
    TH08_PSP_RENDER_PERF_MAX(itemTimeSpawnInitFullVmMismatches);
    TH08_PSP_RENDER_PERF_MAX(itemTimeSpawnInitFieldMismatches);
    TH08_PSP_RENDER_PERF_MAX(itemTimeSpawnInitFallbacks);
    TH08_PSP_RENDER_PERF_MAX(itemsDrawn);
    TH08_PSP_RENDER_PERF_MAX(popupsActive);
    TH08_PSP_RENDER_PERF_MAX(popupDigits);
    TH08_PSP_RENDER_PERF_MAX(asciiPopupBatchCalls);
    TH08_PSP_RENDER_PERF_MAX(asciiPopupBatchDigits);
    TH08_PSP_RENDER_PERF_MAX(asciiPopupBatchSprites);
    TH08_PSP_RENDER_PERF_MAX(asciiPopupBatchVerticesSaved);
    TH08_PSP_RENDER_PERF_MAX(asciiPopupBatchBytesSaved);
    TH08_PSP_RENDER_PERF_MAX(asciiPopupBatchFallbacks);
    TH08_PSP_RENDER_PERF_MAX(asciiPopupDirectActivePopups);
    TH08_PSP_RENDER_PERF_MAX(asciiPopupDirectValidationQuadsAvoided);
    TH08_PSP_RENDER_PERF_MAX(asciiPopupDirectValidationCullTestsAvoided);
    TH08_PSP_RENDER_PERF_MAX(asciiPopupDirectNearbyintCallsAvoided);
    TH08_PSP_RENDER_PERF_MAX(uploadAttempts);
    TH08_PSP_RENDER_PERF_MAX(actualUploads);
    TH08_PSP_RENDER_PERF_MAX(uploadBytes);
    TH08_PSP_RENDER_PERF_MAX(textBytes);
#undef TH08_PSP_RENDER_PERF_MAX
}
} // namespace

RenderPerfFpsOverlayDrawScope::RenderPerfFpsOverlayDrawScope(
    std::uint32_t expectedVertices)
    : savedExpectedVertices_(gRenderPerfFpsOverlayExpectedVertices),
      savedSubmittedVertices_(gRenderPerfFpsOverlaySubmittedVertices),
      savedOwnedVertices_(gRenderPerfFpsOverlayOwnedVertices), active_(false)
{
    const std::uint32_t ownedVertices = gQueuedFpsOverlayVertices;
    gQueuedFpsOverlayVertices = 0U;
    gRenderPerfFpsOverlayExpectedVertices = 0U;
    gRenderPerfFpsOverlaySubmittedVertices = 0U;
    gRenderPerfFpsOverlayOwnedVertices = 0U;
    if (ownedVertices != 0U)
    {
        active_ = true;
        gRenderPerfFpsOverlayExpectedVertices = expectedVertices;
        gRenderPerfFpsOverlayOwnedVertices = ownedVertices;
    }
}

RenderPerfFpsOverlayDrawScope::~RenderPerfFpsOverlayDrawScope()
{
    if (active_ &&
        gRenderPerfFpsOverlayOwnedVertices <=
            gRenderPerfFpsOverlayExpectedVertices &&
        gRenderPerfFpsOverlaySubmittedVertices ==
            gRenderPerfFpsOverlayExpectedVertices)
    {
        gRenderPerfCurrentFrame.fpsOverlayVertices +=
            gRenderPerfFpsOverlayOwnedVertices;
    }
    gRenderPerfFpsOverlayExpectedVertices = savedExpectedVertices_;
    gRenderPerfFpsOverlaySubmittedVertices = savedSubmittedVertices_;
    gRenderPerfFpsOverlayOwnedVertices = savedOwnedVertices_;
}

void RenderPerfQueueFpsOverlayVertices(std::uint32_t vertices)
{
    gQueuedFpsOverlayVertices += vertices;
}

void RenderPerfDiscardQueuedFpsOverlayVertices()
{
    gQueuedFpsOverlayVertices = 0U;
}

void RenderPerfTelemetryReset()
{
    gRenderPerfCurrentFrame = RenderPerfFrameCounters{};
    gIntervalTotal = RenderPerfCounters{};
    gIntervalPeak = RenderPerfFrameCounters{};
    gIntervalFrames = 0;
    gQueuedFpsOverlayVertices = 0U;
    gRenderPerfFpsOverlayExpectedVertices = 0U;
    gRenderPerfFpsOverlaySubmittedVertices = 0U;
    gRenderPerfFpsOverlayOwnedVertices = 0U;
}

void RenderPerfTelemetryEndFrame()
{
    RenderPerfFrameCounters completed{};
    completed.draws = gRenderPerfCurrentFrame.draws;
    completed.vertices = gRenderPerfCurrentFrame.vertices;
    completed.fpsOverlayVertices =
        gRenderPerfCurrentFrame.fpsOverlayVertices;
    completed.gameVertices =
        completed.vertices >= completed.fpsOverlayVertices
            ? completed.vertices - completed.fpsOverlayVertices
            : 0U;
    completed.stateRequested = gRenderPerfCurrentFrame.stateRequested;
    completed.stateEmitted = gRenderPerfCurrentFrame.stateEmitted;
    completed.matrixRecomputes = gRenderPerfCurrentFrame.matrixRecomputes;
    completed.vfpuSinCos = gRenderPerfCurrentFrame.vfpuSinCos;
    completed.backgroundInstanceVisits = gRenderPerfCurrentFrame.backgroundInstanceVisits;
    completed.backgroundCandidates = gRenderPerfCurrentFrame.backgroundCandidates;
    completed.backgroundInstancesDrawn = gRenderPerfCurrentFrame.backgroundInstancesDrawn;
    completed.backgroundProjectCalls = gRenderPerfCurrentFrame.backgroundProjectCalls;
    completed.backgroundRadiusCacheHits = gRenderPerfCurrentFrame.backgroundRadiusCacheHits;
    completed.effectSpawnRequests = gRenderPerfCurrentFrame.effectSpawnRequests;
    completed.effectsActive = gRenderPerfCurrentFrame.effectsActive;
    completed.effectsDrawn = gRenderPerfCurrentFrame.effectsDrawn;
    completed.itemSpawnRequests = gRenderPerfCurrentFrame.itemSpawnRequests;
    completed.itemTimeSpawnInitCandidates =
        gRenderPerfCurrentFrame.itemTimeSpawnInitCandidates;
    completed.itemTimeSpawnInitEligible =
        gRenderPerfCurrentFrame.itemTimeSpawnInitEligible;
    completed.itemTimeSpawnInitFullVmMismatches =
        gRenderPerfCurrentFrame.itemTimeSpawnInitFullVmMismatches;
    completed.itemTimeSpawnInitFieldMismatches =
        gRenderPerfCurrentFrame.itemTimeSpawnInitFieldMismatches;
    completed.itemTimeSpawnInitFallbacks =
        gRenderPerfCurrentFrame.itemTimeSpawnInitFallbacks;
    completed.itemsDrawn = gRenderPerfCurrentFrame.itemsDrawn;
    completed.popupsActive = gRenderPerfCurrentFrame.popupsActive;
    completed.popupDigits = gRenderPerfCurrentFrame.popupDigits;
    completed.asciiPopupBatchCalls =
        gRenderPerfCurrentFrame.asciiPopupBatchCalls;
    completed.asciiPopupBatchDigits =
        gRenderPerfCurrentFrame.asciiPopupBatchDigits;
    completed.asciiPopupBatchSprites =
        gRenderPerfCurrentFrame.asciiPopupBatchSprites;
    completed.asciiPopupBatchVerticesSaved =
        gRenderPerfCurrentFrame.asciiPopupBatchVerticesSaved;
    completed.asciiPopupBatchBytesSaved =
        gRenderPerfCurrentFrame.asciiPopupBatchBytesSaved;
    completed.asciiPopupBatchFallbacks =
        gRenderPerfCurrentFrame.asciiPopupBatchFallbacks;
    completed.asciiPopupDirectActivePopups =
        gRenderPerfCurrentFrame.asciiPopupDirectActivePopups;
    completed.asciiPopupDirectValidationQuadsAvoided =
        gRenderPerfCurrentFrame.asciiPopupDirectValidationQuadsAvoided;
    completed.asciiPopupDirectValidationCullTestsAvoided =
        gRenderPerfCurrentFrame.asciiPopupDirectValidationCullTestsAvoided;
    completed.asciiPopupDirectNearbyintCallsAvoided =
        gRenderPerfCurrentFrame.asciiPopupDirectNearbyintCallsAvoided;
    gRenderPerfCurrentFrame.draws = 0;
    gRenderPerfCurrentFrame.vertices = 0;
    gRenderPerfCurrentFrame.fpsOverlayVertices = 0;
    gRenderPerfCurrentFrame.gameVertices = 0;
    gRenderPerfCurrentFrame.stateRequested = 0;
    gRenderPerfCurrentFrame.stateEmitted = 0;
    gRenderPerfCurrentFrame.matrixRecomputes = 0;
    gRenderPerfCurrentFrame.vfpuSinCos = 0;
    gRenderPerfCurrentFrame.backgroundInstanceVisits = 0;
    gRenderPerfCurrentFrame.backgroundCandidates = 0;
    gRenderPerfCurrentFrame.backgroundInstancesDrawn = 0;
    gRenderPerfCurrentFrame.backgroundProjectCalls = 0;
    gRenderPerfCurrentFrame.backgroundRadiusCacheHits = 0;
    gRenderPerfCurrentFrame.effectSpawnRequests = 0;
    gRenderPerfCurrentFrame.effectsActive = 0;
    gRenderPerfCurrentFrame.effectsDrawn = 0;
    gRenderPerfCurrentFrame.itemSpawnRequests = 0;
    gRenderPerfCurrentFrame.itemTimeSpawnInitCandidates = 0;
    gRenderPerfCurrentFrame.itemTimeSpawnInitEligible = 0;
    gRenderPerfCurrentFrame.itemTimeSpawnInitFullVmMismatches = 0;
    gRenderPerfCurrentFrame.itemTimeSpawnInitFieldMismatches = 0;
    gRenderPerfCurrentFrame.itemTimeSpawnInitFallbacks = 0;
    gRenderPerfCurrentFrame.itemsDrawn = 0;
    gRenderPerfCurrentFrame.popupsActive = 0;
    gRenderPerfCurrentFrame.popupDigits = 0;
    gRenderPerfCurrentFrame.asciiPopupBatchCalls = 0;
    gRenderPerfCurrentFrame.asciiPopupBatchDigits = 0;
    gRenderPerfCurrentFrame.asciiPopupBatchSprites = 0;
    gRenderPerfCurrentFrame.asciiPopupBatchVerticesSaved = 0;
    gRenderPerfCurrentFrame.asciiPopupBatchBytesSaved = 0;
    gRenderPerfCurrentFrame.asciiPopupBatchFallbacks = 0;
    gRenderPerfCurrentFrame.asciiPopupDirectActivePopups = 0;
    gRenderPerfCurrentFrame.asciiPopupDirectValidationQuadsAvoided = 0;
    gRenderPerfCurrentFrame.asciiPopupDirectValidationCullTestsAvoided = 0;
    gRenderPerfCurrentFrame.asciiPopupDirectNearbyintCallsAvoided = 0;

    // The stage setup thread may upload while the main thread presents its
    // loading screen. Atomic exchanges assign every completed increment to
    // exactly one Present interval without a mutex or heap-backed TLS.
    completed.uploadAttempts =
        __sync_lock_test_and_set(&gRenderPerfCurrentFrame.uploadAttempts, 0U);
    completed.actualUploads =
        __sync_lock_test_and_set(&gRenderPerfCurrentFrame.actualUploads, 0U);
    completed.uploadBytes =
        __sync_lock_test_and_set(&gRenderPerfCurrentFrame.uploadBytes, 0U);
    completed.textBytes =
        __sync_lock_test_and_set(&gRenderPerfCurrentFrame.textBytes, 0U);

    Accumulate(&gIntervalTotal, completed);
    Maximize(&gIntervalPeak, completed);
    ++gIntervalFrames;
}

RenderPerfIntervalSnapshot RenderPerfTelemetryTakeInterval()
{
    RenderPerfIntervalSnapshot snapshot{};
    snapshot.frames = gIntervalFrames;
    snapshot.total = gIntervalTotal;
    snapshot.peak = gIntervalPeak;
    gIntervalTotal = RenderPerfCounters{};
    gIntervalPeak = RenderPerfFrameCounters{};
    gIntervalFrames = 0;
    return snapshot;
}
} // namespace th08::psp
