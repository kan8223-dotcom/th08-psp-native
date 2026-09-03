#include "bullet_gate_stats.hpp"

#if TH08_PSP_BULLET_GATE_STATS_ENABLED

#include "fileio.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace th08::psp
{
namespace
{
std::uint64_t gCounts[static_cast<std::size_t>(BulletGateOutcome::Count)]{};
bool gWindowActive = false;
} // namespace

void BulletGateNote(BulletGateOutcome outcome)
{
    if (!gWindowActive || outcome >= BulletGateOutcome::Count)
        return;
    std::uint64_t &value = gCounts[static_cast<std::size_t>(outcome)];
    if (value != std::numeric_limits<std::uint64_t>::max())
        ++value;
}

void BulletGateStatsResetWindow(bool active)
{
    std::memset(gCounts, 0, sizeof(gCounts));
    gWindowActive = active;
}

void BulletGateStatsCancelWindow()
{
    gWindowActive = false;
}

void BulletGateStatsEmitWindow(std::int32_t stage, std::uint32_t baselineStageFrame,
                               std::uint32_t stageFrame)
{
    if (!gWindowActive)
        return;
    std::uint64_t eligible = 0U;
    for (std::size_t i = 0U; i < static_cast<std::size_t>(BulletGateOutcome::Count); ++i)
        eligible += gCounts[i];
    BootLog("BULLET_GATE V1 st=%ld sf=%lu-%lu eligible=%llu clear_graze_suppressed=%llu "
            "clear_graze_separate=%llu clear_lethal_separate=%llu fb_cancel_unknown=%llu "
            "fb_snapshot_invalid=%llu fb_bullet_invalid=%llu fb_touch=%llu fb_bounds_mutated=%llu\n",
            static_cast<long>(stage),
            static_cast<unsigned long>(baselineStageFrame),
            static_cast<unsigned long>(stageFrame),
            static_cast<unsigned long long>(eligible),
            static_cast<unsigned long long>(gCounts[0]),
            static_cast<unsigned long long>(gCounts[1]),
            static_cast<unsigned long long>(gCounts[2]),
            static_cast<unsigned long long>(gCounts[3]),
            static_cast<unsigned long long>(gCounts[4]),
            static_cast<unsigned long long>(gCounts[5]),
            static_cast<unsigned long long>(gCounts[6]),
            static_cast<unsigned long long>(gCounts[7]));
}
} // namespace th08::psp

#endif // TH08_PSP_BULLET_GATE_STATS_ENABLED
