#include "item_update_subprofile.hpp"

#if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED

#include "fileio.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

extern "C" std::uint64_t sceKernelGetSystemTimeWide(void);

namespace th08::psp
{
namespace
{
ItemUpdateDurationStat gSectionStats[kItemUpdateSectionCount]{};
std::uint64_t gCounterTotals[kItemUpdateCounterCount]{};
std::uint64_t gItemCountSum = 0U;
std::uint32_t gItemCountMax = 0U;
std::uint32_t gTicks = 0U;
std::uint32_t gTimerReads = 0U;
std::uint32_t gClockRegressionCount = 0U;
std::uint32_t gCounterOverflowCount = 0U;
std::uint32_t gRotation = 0U;
bool gWindowActive = false;
bool gTickActive = false;

void IncrementSaturating(std::uint32_t &value)
{
    if (value != std::numeric_limits<std::uint32_t>::max())
        ++value;
}

void ClearWindowCounters()
{
    std::memset(gSectionStats, 0, sizeof(gSectionStats));
    std::memset(gCounterTotals, 0, sizeof(gCounterTotals));
    gItemCountSum = 0U;
    gItemCountMax = 0U;
    gTicks = 0U;
    gTimerReads = 0U;
    gClockRegressionCount = 0U;
    gCounterOverflowCount = 0U;
    gTickActive = false;
}

const ItemUpdateDurationStat &Section(ItemUpdateSection section)
{
    return gSectionStats[static_cast<std::size_t>(section)];
}
} // namespace

bool ItemUpdateSubprofileBeginTick(std::uint32_t &rotation)
{
    // The rotation advances on every tick, active or not, so the timed list
    // position keeps moving across window boundaries as well.
    gRotation = ItemUpdateNextRotation(gRotation);
    rotation = gRotation;
    gTickActive = gWindowActive;
    if (gTickActive)
        IncrementSaturating(gTicks);
    return gTickActive;
}

void ItemUpdateSubprofileEndTick(std::uint32_t itemCount,
                                 const ItemUpdateTickCounters &counters)
{
    if (!gTickActive)
        return;
    if (ItemUpdateAddCounter(gItemCountSum, itemCount))
        IncrementSaturating(gCounterOverflowCount);
    if (itemCount > gItemCountMax)
        gItemCountMax = itemCount;
    for (std::size_t i = 0U; i < kItemUpdateCounterCount; ++i)
    {
        if (ItemUpdateAddCounter(gCounterTotals[i], counters.values[i]))
            IncrementSaturating(gCounterOverflowCount);
    }
    gTickActive = false;
}

std::uint64_t ItemUpdateSubprofileReadClock()
{
    IncrementSaturating(gTimerReads);
    return sceKernelGetSystemTimeWide();
}

void ItemUpdateSubprofileRecordSection(ItemUpdateSection section,
                                       std::uint64_t startUs)
{
    const std::uint64_t endUs = ItemUpdateSubprofileReadClock();
    if (endUs < startUs)
    {
        IncrementSaturating(gClockRegressionCount);
        return;
    }
    if (ItemUpdateAccumulate(gSectionStats[static_cast<std::size_t>(section)],
                             endUs - startUs))
        IncrementSaturating(gCounterOverflowCount);
}

void ItemUpdateSubprofileResetWindow(bool active)
{
    ClearWindowCounters();
    gWindowActive = active;
}

void ItemUpdateSubprofileCancelWindow()
{
    // stageFrame==0 can persist for every title/menu tick.  Clear the
    // abandoned gameplay window once, not on every idle tick.
    if (gWindowActive || gTickActive)
        ClearWindowCounters();
    gWindowActive = false;
}

void ItemUpdateSubprofileEmitWindow(std::int32_t stage,
                                    std::uint32_t baselineStageFrame,
                                    std::uint32_t stageFrame)
{
    if (!gWindowActive)
        return;

    const ItemUpdateDurationStat &whole = Section(ItemUpdateSection::Whole);
    const ItemUpdateDurationStat &autocollect =
        Section(ItemUpdateSection::Autocollect);
    const ItemUpdateDurationStat &collision =
        Section(ItemUpdateSection::Collision);
    const ItemUpdateDurationStat &script = Section(ItemUpdateSection::Script);

    std::uint64_t nested = 0U;
    bool nestedOverflow = false;
    nestedOverflow |= ItemUpdateAddCounter(nested, autocollect.totalUs);
    nestedOverflow |= ItemUpdateAddCounter(nested, collision.totalUs);
    nestedOverflow |= ItemUpdateAddCounter(nested, script.totalUs);
    const ItemUpdateDifference residual =
        ItemUpdateSubtract(whole.totalUs, nested);
    std::uint32_t underflowMask = 0U;
    underflowMask |= residual.underflow ? 1U << 0U : 0U;
    underflowMask |= nestedOverflow ? 1U << 1U : 0U;

    // One bounded record at the parent PERF_ATTR 600-tick boundary.  BootLog
    // stays buffered; there is deliberately no per-tick or per-item I/O.
    BootLog(
        "ITEM_UPD V1 st=%ld sf=%lu-%lu ticks=%lu rot=32 "
        "sample_rule=list_position_rotating_1of32 "
        "items=%llu/%lu/%lu sampled=%lu timer_reads=%lu cr=%lu ov=%lu "
        "uf=0x%02lx "
        "whole=%llu/%llu/%lu auto=%llu/%llu/%lu coll=%llu/%llu/%lu "
        "script=%llu/%llu/%lu resid=%llu "
        "c_visit=%llu c_auto=%llu c_coll=%llu c_pick=%llu c_off=%llu "
        "c_script=%llu "
        "s_default=%llu s_auto=%llu s_spread=%llu s_rising=%llu s_apex=%llu "
        "s_other=%llu\n",
        static_cast<long>(stage),
        static_cast<unsigned long>(baselineStageFrame),
        static_cast<unsigned long>(stageFrame),
        static_cast<unsigned long>(gTicks),
        static_cast<unsigned long long>(gItemCountSum),
        static_cast<unsigned long>(gItemCountMax),
        static_cast<unsigned long>(gTicks),
        static_cast<unsigned long>(whole.calls),
        static_cast<unsigned long>(gTimerReads),
        static_cast<unsigned long>(gClockRegressionCount),
        static_cast<unsigned long>(gCounterOverflowCount),
        static_cast<unsigned long>(underflowMask),
        static_cast<unsigned long long>(whole.totalUs),
        static_cast<unsigned long long>(whole.maxUs),
        static_cast<unsigned long>(whole.calls),
        static_cast<unsigned long long>(autocollect.totalUs),
        static_cast<unsigned long long>(autocollect.maxUs),
        static_cast<unsigned long>(autocollect.calls),
        static_cast<unsigned long long>(collision.totalUs),
        static_cast<unsigned long long>(collision.maxUs),
        static_cast<unsigned long>(collision.calls),
        static_cast<unsigned long long>(script.totalUs),
        static_cast<unsigned long long>(script.maxUs),
        static_cast<unsigned long>(script.calls),
        static_cast<unsigned long long>(residual.value),
        static_cast<unsigned long long>(gCounterTotals[kItemUpdateCounterVisit]),
        static_cast<unsigned long long>(
            gCounterTotals[kItemUpdateCounterAutocollect]),
        static_cast<unsigned long long>(
            gCounterTotals[kItemUpdateCounterCollisionProbe]),
        static_cast<unsigned long long>(gCounterTotals[kItemUpdateCounterPickup]),
        static_cast<unsigned long long>(
            gCounterTotals[kItemUpdateCounterOffscreen]),
        static_cast<unsigned long long>(gCounterTotals[kItemUpdateCounterScript]),
        static_cast<unsigned long long>(
            gCounterTotals[kItemUpdateCounterStateDefault]),
        static_cast<unsigned long long>(
            gCounterTotals[kItemUpdateCounterStateAutocollect]),
        static_cast<unsigned long long>(
            gCounterTotals[kItemUpdateCounterStateSpread]),
        static_cast<unsigned long long>(
            gCounterTotals[kItemUpdateCounterStateRising]),
        static_cast<unsigned long long>(
            gCounterTotals[kItemUpdateCounterStateApex]),
        static_cast<unsigned long long>(
            gCounterTotals[kItemUpdateCounterStateOther]));
}
} // namespace th08::psp

#endif // TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED
