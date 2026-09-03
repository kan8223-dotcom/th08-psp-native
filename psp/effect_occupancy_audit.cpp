#include "effect_occupancy_audit.hpp"

#if TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED

#include "fileio.hpp"

#include <cstdint>
#include <limits>

namespace th08::psp
{
namespace
{
std::uint64_t gVisits = 0U;
std::uint64_t gActive = 0U;
std::uint64_t gActiveTested = 0U;
std::uint64_t gFalseNegatives = 0U;
std::uint64_t gInactive = 0U;
std::uint64_t gInactiveTested = 0U; // stale positives (harmless)
std::uint64_t gCleanupMisses = 0U;  // bit clear but vertices still owned
std::uint64_t gSkippable = 0U;      // bit clear, inactive, nothing to free
bool gWindowActive = false;

void Inc(std::uint64_t &value)
{
    if (value != std::numeric_limits<std::uint64_t>::max())
        ++value;
}
} // namespace

void EffectOccupancyAuditNoteActive(bool tested)
{
    if (!gWindowActive)
        return;
    Inc(gVisits);
    Inc(gActive);
    if (tested)
        Inc(gActiveTested);
    else
        Inc(gFalseNegatives);
}

void EffectOccupancyAuditNoteInactive(bool tested, bool ownsVertices)
{
    if (!gWindowActive)
        return;
    Inc(gVisits);
    Inc(gInactive);
    if (tested)
        Inc(gInactiveTested);
    else if (ownsVertices)
        Inc(gCleanupMisses);
    else
        Inc(gSkippable);
}

void EffectOccupancyStatsResetWindow(bool active)
{
    gVisits = gActive = gActiveTested = gFalseNegatives = 0U;
    gInactive = gInactiveTested = gCleanupMisses = gSkippable = 0U;
    gWindowActive = active;
}

void EffectOccupancyStatsCancelWindow()
{
    gWindowActive = false;
}

void EffectOccupancyStatsEmitWindow(std::int32_t stage,
                                    std::uint32_t baselineStageFrame,
                                    std::uint32_t stageFrame)
{
    if (!gWindowActive)
        return;
    BootLog("EFFECT_OCC V1 st=%ld sf=%lu-%lu visits=%llu active=%llu "
            "active_tested=%llu false_neg=%llu inactive=%llu stale_pos=%llu "
            "cleanup_miss=%llu skippable=%llu\n",
            static_cast<long>(stage),
            static_cast<unsigned long>(baselineStageFrame),
            static_cast<unsigned long>(stageFrame),
            static_cast<unsigned long long>(gVisits),
            static_cast<unsigned long long>(gActive),
            static_cast<unsigned long long>(gActiveTested),
            static_cast<unsigned long long>(gFalseNegatives),
            static_cast<unsigned long long>(gInactive),
            static_cast<unsigned long long>(gInactiveTested),
            static_cast<unsigned long long>(gCleanupMisses),
            static_cast<unsigned long long>(gSkippable));
}
} // namespace th08::psp

#endif // TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED
