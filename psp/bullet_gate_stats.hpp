#pragma once

#include <cstdint>

// Window counters for the product bullet collision negative gate
// (TH08_PSP_BULLET_COLLISION_GATE): per parent PERF_ATTR window, how many
// collision-eligible bullets were proven clear (graze/lethal scan skipped,
// bulletCancelItemType=6 reproduced) and why the rest ran canonically.
#if defined(PSP) && defined(TH08_PSP_BULLET_COLLISION_GATE) && \
    TH08_PSP_BULLET_COLLISION_GATE
#define TH08_PSP_BULLET_GATE_STATS_ENABLED 1

namespace th08::psp
{
enum class BulletGateOutcome : std::uint8_t
{
    ClearGrazeSuppressed = 0,
    ClearGrazeSeparate = 1,
    ClearLethalSeparate = 2,
    FallbackCancelUnknown = 3,
    FallbackSnapshotInvalid = 4,
    FallbackBulletInvalid = 5,
    FallbackTouchOrOverlap = 6,
    FallbackBoundsMutated = 7,
    Count = 8,
};
void BulletGateNote(BulletGateOutcome outcome);
void BulletGateStatsResetWindow(bool active);
void BulletGateStatsCancelWindow();
void BulletGateStatsEmitWindow(std::int32_t stage, std::uint32_t baselineStageFrame,
                               std::uint32_t stageFrame);
} // namespace th08::psp

#else
#define TH08_PSP_BULLET_GATE_STATS_ENABLED 0
#endif
