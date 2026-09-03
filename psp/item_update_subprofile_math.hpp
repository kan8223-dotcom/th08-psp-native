#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace th08::psp
{
// One active-list position out of every 32 is timed per simulation tick.  The
// phase rotates by one position every tick, so a fixed list position is timed
// once per 32 ticks and, against TH08's 48-tick animation period
// (gcd(32, 48) = 16), cycles through three animation phases instead of
// locking to one.  Whole-population counters never depend on this gate.
constexpr std::uint32_t kItemUpdateSampleRotation = 32U;

enum class ItemUpdateSection : std::uint8_t
{
    Whole = 0,
    Autocollect,
    Collision,
    Script,
};
constexpr std::size_t kItemUpdateSectionCount = 4U;

// Whole-population per-tick counters (every visited item, sampled or not).
constexpr std::size_t kItemUpdateCounterVisit = 0U;
constexpr std::size_t kItemUpdateCounterAutocollect = 1U;
constexpr std::size_t kItemUpdateCounterCollisionProbe = 2U;
constexpr std::size_t kItemUpdateCounterPickup = 3U;
constexpr std::size_t kItemUpdateCounterOffscreen = 4U;
constexpr std::size_t kItemUpdateCounterScript = 5U;
constexpr std::size_t kItemUpdateCounterStateDefault = 6U;
constexpr std::size_t kItemUpdateCounterStateAutocollect = 7U;
constexpr std::size_t kItemUpdateCounterStateSpread = 8U;
constexpr std::size_t kItemUpdateCounterStateRising = 9U;
constexpr std::size_t kItemUpdateCounterStateApex = 10U;
constexpr std::size_t kItemUpdateCounterStateOther = 11U;
constexpr std::size_t kItemUpdateCounterCount = 12U;

struct ItemUpdateTickCounters
{
    std::uint32_t values[kItemUpdateCounterCount];
};

struct ItemUpdateDurationStat
{
    std::uint64_t totalUs;
    std::uint64_t maxUs;
    std::uint32_t calls;
};

struct ItemUpdateDifference
{
    std::uint64_t value;
    bool underflow;
};

constexpr bool ItemUpdateShouldSampleOrdinal(std::uint32_t ordinal,
                                             std::uint32_t rotation)
{
    return ((ordinal + rotation) & (kItemUpdateSampleRotation - 1U)) == 0U;
}

constexpr std::uint32_t ItemUpdateNextRotation(std::uint32_t rotation)
{
    return (rotation + 1U) & (kItemUpdateSampleRotation - 1U);
}

// ITEM_STATE_DEFAULT=0, ITEM_STATE_AUTOCOLLECT=1, ITEM_STATE_DEATH_DROP_SPREAD=2,
// ITEM_STATE_TIME_RISING=3, ITEM_STATE_RESERVED_4=4,
// ITEM_STATE_TIME_RISING_TO_APEX=5 (src/ItemManager.hpp).
constexpr std::size_t ItemUpdateStateCounter(int state)
{
    return state == 0   ? kItemUpdateCounterStateDefault
           : state == 1 ? kItemUpdateCounterStateAutocollect
           : state == 2 ? kItemUpdateCounterStateSpread
           : state == 3 ? kItemUpdateCounterStateRising
           : state == 5 ? kItemUpdateCounterStateApex
                        : kItemUpdateCounterStateOther;
}

inline bool ItemUpdateAccumulate(ItemUpdateDurationStat &stat,
                                 std::uint64_t durationUs)
{
    const bool totalOverflow =
        durationUs > std::numeric_limits<std::uint64_t>::max() - stat.totalUs;
    stat.totalUs = totalOverflow ? std::numeric_limits<std::uint64_t>::max()
                                 : stat.totalUs + durationUs;
    if (durationUs > stat.maxUs)
        stat.maxUs = durationUs;
    if (stat.calls != std::numeric_limits<std::uint32_t>::max())
        ++stat.calls;
    return totalOverflow;
}

inline bool ItemUpdateAddCounter(std::uint64_t &total, std::uint64_t addend)
{
    if (addend > std::numeric_limits<std::uint64_t>::max() - total)
    {
        total = std::numeric_limits<std::uint64_t>::max();
        return true;
    }
    total += addend;
    return false;
}

constexpr ItemUpdateDifference ItemUpdateSubtract(std::uint64_t inclusive,
                                                  std::uint64_t nested)
{
    return {inclusive >= nested ? inclusive - nested : 0U,
            inclusive < nested};
}
} // namespace th08::psp
