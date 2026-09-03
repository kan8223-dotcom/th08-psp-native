#include "radial_trail_telemetry.hpp"

namespace th08::psp
{
namespace
{
RadialTrailTelemetrySnapshot gRadialTrailTelemetry{};

std::uint32_t ClampSegments(std::int32_t segmentCount)
{
    return segmentCount > 0 ? static_cast<std::uint32_t>(segmentCount) : 0U;
}
} // namespace

void RadialTrailTelemetryReset()
{
    gRadialTrailTelemetry = RadialTrailTelemetrySnapshot{};
}

void RadialTrailTelemetryNoteDraw(std::int32_t segmentCount)
{
    const std::uint32_t segments = ClampSegments(segmentCount);
    const std::uint32_t vertices = segments * 2U + 2U;
    ++gRadialTrailTelemetry.drawCalls;
    gRadialTrailTelemetry.submittedVertices += vertices;
    if (vertices > gRadialTrailTelemetry.peakSubmittedVertices)
        gRadialTrailTelemetry.peakSubmittedVertices = vertices;
}

void RadialTrailTelemetryNoteRebuild(std::int32_t segmentCount,
                                     RadialTrailBranch branch,
                                     bool reusedCanonicalTrig)
{
    const std::uint32_t segments = ClampSegments(segmentCount);
    const std::uint32_t samples = segments + 1U;
    ++gRadialTrailTelemetry.dirtyRebuilds;
    gRadialTrailTelemetry.angularSamples += samples;
    gRadialTrailTelemetry.rebuiltVertices += samples * 2U;
    if (segments > gRadialTrailTelemetry.peakSegments)
        gRadialTrailTelemetry.peakSegments = segments;

    switch (branch)
    {
    case RadialTrailBranch::ZeroSecondary:
    {
        ++gRadialTrailTelemetry.zeroSecondaryRebuilds;
        gRadialTrailTelemetry.reusableTrigPairs += samples;
        // Keep identical counter-store traffic in OFF and ON builds so the
        // observer cannot bias the timing comparison against the candidate.
        // The all-zero/all-one mask avoids a feature-dependent branch.
        const std::uint32_t enabledMask =
            0U - static_cast<std::uint32_t>(reusedCanonicalTrig);
        const std::uint32_t reusedSamples = samples & enabledMask;
        gRadialTrailTelemetry.reusedTrigPairs += reusedSamples;
        // The canonical path used cos+sin for each of two radii.  Reuse
        // removes one cos and one sin evaluation per angular sample.
        gRadialTrailTelemetry.trigEvaluationsAvoided += reusedSamples * 2U;
        break;
    }
    case RadialTrailBranch::Ellipse:
        ++gRadialTrailTelemetry.ellipseRebuilds;
        break;
    case RadialTrailBranch::Wavy:
        ++gRadialTrailTelemetry.wavyRebuilds;
        break;
    }
}

RadialTrailTelemetrySnapshot RadialTrailTelemetryPeek()
{
    return gRadialTrailTelemetry;
}

RadialTrailTelemetrySnapshot RadialTrailTelemetryTake()
{
    const RadialTrailTelemetrySnapshot snapshot = gRadialTrailTelemetry;
    RadialTrailTelemetryReset();
    return snapshot;
}
} // namespace th08::psp
