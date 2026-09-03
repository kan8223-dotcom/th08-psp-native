#pragma once

#include "runtime_telemetry_config.hpp"

#include <cstdint>

namespace th08::psp
{
enum class RadialTrailBranch : std::uint32_t
{
    ZeroSecondary = 0U,
    Ellipse = 1U,
    Wavy = 2U,
};

// SC-render-thread counters only.  They contain no pointers, game state,
// clocks, allocation, or atomics and are identical in radial OFF/ON builds.
struct RadialTrailTelemetrySnapshot
{
    std::uint64_t drawCalls;
    std::uint64_t submittedVertices;
    std::uint64_t dirtyRebuilds;
    std::uint64_t zeroSecondaryRebuilds;
    std::uint64_t ellipseRebuilds;
    std::uint64_t wavyRebuilds;
    std::uint64_t angularSamples;
    std::uint64_t rebuiltVertices;
    std::uint64_t reusableTrigPairs;
    std::uint64_t reusedTrigPairs;
    std::uint64_t trigEvaluationsAvoided;
    std::uint32_t peakSegments;
    std::uint32_t peakSubmittedVertices;
};

#if TH08_PSP_RUNTIME_TELEMETRY
void RadialTrailTelemetryReset();
void RadialTrailTelemetryNoteDraw(std::int32_t segmentCount);
void RadialTrailTelemetryNoteRebuild(std::int32_t segmentCount,
                                     RadialTrailBranch branch,
                                     bool reusedCanonicalTrig);
RadialTrailTelemetrySnapshot RadialTrailTelemetryPeek();
RadialTrailTelemetrySnapshot RadialTrailTelemetryTake();
#else
inline void RadialTrailTelemetryReset() {}
inline void RadialTrailTelemetryNoteDraw(std::int32_t) {}
inline void RadialTrailTelemetryNoteRebuild(std::int32_t, RadialTrailBranch,
                                            bool) {}
inline RadialTrailTelemetrySnapshot RadialTrailTelemetryPeek()
{
    return RadialTrailTelemetrySnapshot{};
}
inline RadialTrailTelemetrySnapshot RadialTrailTelemetryTake()
{
    return RadialTrailTelemetrySnapshot{};
}
#endif
} // namespace th08::psp
