#include "item_atan2_audit.hpp"

#if TH08_PSP_ITEM_ATAN2_STATS_ENABLED

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
constexpr std::size_t kSlotCount = 2097U; // MAX_ITEMS + 1 (ItemManager::items)
constexpr std::uint32_t kSampleMask = 31U;
constexpr std::size_t kMismatchRecords = 4U;
constexpr std::size_t kReasonCount =
    static_cast<std::size_t>(ItemAtan2FastpathReason::Count);

struct DurationStat
{
    std::uint64_t totalUs;
    std::uint64_t maxUs;
    std::uint32_t calls;
};

struct MismatchRecord
{
    std::uint32_t yBits;
    std::uint32_t xBits;
    std::uint32_t fastBits;
    std::uint32_t canonicalBits;
};

struct SlotHistory
{
    std::uint32_t xDeltaBits;
    std::uint32_t yDeltaBits;
    std::uint32_t angleBits;
    std::uint8_t valid;
};

#if TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED
// Audit-only: 2,097 x 16 bytes.  The product switch does not compile it.
SlotHistory gSlots[kSlotCount]{};
#endif
std::uint64_t gCalls = 0U;
std::uint64_t gAccepted = 0U;
std::uint64_t gMismatch = 0U;
std::uint64_t gReasons[kReasonCount]{};
std::uint64_t gDeltaRepeat = 0U;
std::uint64_t gAngleRepeat = 0U;
std::uint64_t gInvalidSlot = 0U;
DurationStat gCanonical{};
DurationStat gFast{};
DurationStat gVelocity{};
MismatchRecord gMismatches[kMismatchRecords]{};
std::uint32_t gMismatchRecorded = 0U;
std::uint32_t gSampled = 0U;
std::uint32_t gTimerReads = 0U;
std::uint32_t gClockRegression = 0U;
#if TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED
std::uint32_t gOrdinal = 0U;
#endif
bool gWindowActive = false;

void IncrementSaturating(std::uint32_t &value)
{
    if (value != std::numeric_limits<std::uint32_t>::max())
        ++value;
}

void IncrementSaturating(std::uint64_t &value)
{
    if (value != std::numeric_limits<std::uint64_t>::max())
        ++value;
}

#if TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED
std::uint32_t Bits(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
#endif

[[maybe_unused]] void Record(DurationStat &stat, std::uint64_t startUs,
                             std::uint64_t endUs)
{
    if (endUs < startUs)
    {
        IncrementSaturating(gClockRegression);
        return;
    }
    const std::uint64_t d = endUs - startUs;
    if (d > std::numeric_limits<std::uint64_t>::max() - stat.totalUs)
        stat.totalUs = std::numeric_limits<std::uint64_t>::max();
    else
        stat.totalUs += d;
    if (d > stat.maxUs)
        stat.maxUs = d;
    IncrementSaturating(stat.calls);
}

void ClearWindowCounters()
{
    gCalls = 0U;
    gAccepted = 0U;
    gMismatch = 0U;
    std::memset(gReasons, 0, sizeof(gReasons));
    gDeltaRepeat = 0U;
    gAngleRepeat = 0U;
    gInvalidSlot = 0U;
    gCanonical = DurationStat{};
    gFast = DurationStat{};
    gVelocity = DurationStat{};
    std::memset(gMismatches, 0, sizeof(gMismatches));
    gMismatchRecorded = 0U;
    gSampled = 0U;
    gTimerReads = 0U;
    gClockRegression = 0U;
}
} // namespace

#if TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED
std::uint64_t ItemAtan2AuditReadClock()
{
    IncrementSaturating(gTimerReads);
    return sceKernelGetSystemTimeWide();
}

bool ItemAtan2AuditBeginCall(std::uint64_t *startUs)
{
    const bool sampled = gWindowActive && ((gOrdinal & kSampleMask) == 0U);
    ++gOrdinal;
    if (sampled)
    {
        IncrementSaturating(gSampled);
        *startUs = ItemAtan2AuditReadClock();
    }
    else
    {
        *startUs = 0U;
    }
    return sampled;
}

void ItemAtan2AuditAfterCanonical(std::uint32_t slot, float playerX,
                                  float playerY, float itemX, float itemY,
                                  float canonicalAngle, bool sampled,
                                  std::uint64_t canonicalStartUs)
{
    if (!gWindowActive)
        return;
    std::uint64_t fastStartUs = 0U;
    if (sampled)
    {
        fastStartUs = ItemAtan2AuditReadClock();
        Record(gCanonical, canonicalStartUs, fastStartUs);
    }
    // Identical f32 subtractions to Player::AngleToPoint (player minus item).
    const float xDelta = playerX - itemX;
    const float yDelta = playerY - itemY;
    float fast = 0.0f;
    ItemAtan2FastpathReason reason = ItemAtan2FastpathReason::Count;
    const bool accepted = ItemAtan2FastpathTry(yDelta, xDelta, &fast, &reason);
    if (sampled)
        Record(gFast, fastStartUs, ItemAtan2AuditReadClock());

    IncrementSaturating(gCalls);
    if (reason < ItemAtan2FastpathReason::Count)
        IncrementSaturating(gReasons[static_cast<std::size_t>(reason)]);
    const std::uint32_t canonicalBits = Bits(canonicalAngle);
    if (accepted)
    {
        IncrementSaturating(gAccepted);
        const std::uint32_t fastBits = Bits(fast);
        if (fastBits != canonicalBits)
        {
            IncrementSaturating(gMismatch);
            if (gMismatchRecorded < kMismatchRecords)
            {
                gMismatches[gMismatchRecorded] = {Bits(yDelta), Bits(xDelta),
                                                   fastBits, canonicalBits};
                ++gMismatchRecorded;
            }
        }
    }

    if (slot >= kSlotCount)
    {
        IncrementSaturating(gInvalidSlot);
        return;
    }
    SlotHistory &history = gSlots[slot];
    const std::uint32_t xBits = Bits(xDelta);
    const std::uint32_t yBits = Bits(yDelta);
    if (history.valid != 0U)
    {
        if (history.xDeltaBits == xBits && history.yDeltaBits == yBits)
            IncrementSaturating(gDeltaRepeat);
        if (history.angleBits == canonicalBits)
            IncrementSaturating(gAngleRepeat);
    }
    history.xDeltaBits = xBits;
    history.yDeltaBits = yBits;
    history.angleBits = canonicalBits;
    history.valid = 1U;
}

void ItemAtan2AuditEndVelocity(std::uint64_t startUs)
{
    Record(gVelocity, startUs, ItemAtan2AuditReadClock());
}
#endif // TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED

void ItemAtan2ProductNote(ItemAtan2FastpathReason reason)
{
    if (!gWindowActive)
        return;
    IncrementSaturating(gCalls);
    if (reason == ItemAtan2FastpathReason::Accepted)
        IncrementSaturating(gAccepted);
    if (reason < ItemAtan2FastpathReason::Count)
        IncrementSaturating(gReasons[static_cast<std::size_t>(reason)]);
}

void ItemAtan2StatsResetWindow(bool active)
{
    ClearWindowCounters();
    // Slot histories survive window boundaries so repeat rates stay
    // continuous; they are diagnostic state only.
    gWindowActive = active;
}

void ItemAtan2StatsCancelWindow()
{
    if (gWindowActive)
        ClearWindowCounters();
    gWindowActive = false;
}

void ItemAtan2StatsEmitWindow(std::int32_t stage,
                              std::uint32_t baselineStageFrame,
                              std::uint32_t stageFrame)
{
    if (!gWindowActive)
        return;
    // One bounded record at the parent PERF_ATTR 600-tick boundary.
    BootLog(
        "ITEM_ATAN2 V1 st=%ld sf=%lu-%lu mode=%s calls=%llu accepted=%llu "
        "mismatch=%llu r_zero=%llu r_nonfinite=%llu r_range=%llu r_tiny=%llu "
        "r_boundary=%llu delta_repeat=%llu angle_repeat=%llu "
        "invalid_slot=%llu sampled=%lu "
        "t_canon=%llu/%llu/%lu t_fast=%llu/%llu/%lu t_vel=%llu/%llu/%lu "
        "timer_reads=%lu cr=%lu mm_recorded=%lu "
        "mm0=%08lx/%08lx/%08lx/%08lx mm1=%08lx/%08lx/%08lx/%08lx "
        "mm2=%08lx/%08lx/%08lx/%08lx mm3=%08lx/%08lx/%08lx/%08lx\n",
        static_cast<long>(stage),
        static_cast<unsigned long>(baselineStageFrame),
        static_cast<unsigned long>(stageFrame),
        TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED ? "audit" : "product",
        static_cast<unsigned long long>(gCalls),
        static_cast<unsigned long long>(gAccepted),
        static_cast<unsigned long long>(gMismatch),
        static_cast<unsigned long long>(gReasons[1]),
        static_cast<unsigned long long>(gReasons[2]),
        static_cast<unsigned long long>(gReasons[3]),
        static_cast<unsigned long long>(gReasons[4]),
        static_cast<unsigned long long>(gReasons[5]),
        static_cast<unsigned long long>(gDeltaRepeat),
        static_cast<unsigned long long>(gAngleRepeat),
        static_cast<unsigned long long>(gInvalidSlot),
        static_cast<unsigned long>(gSampled),
        static_cast<unsigned long long>(gCanonical.totalUs),
        static_cast<unsigned long long>(gCanonical.maxUs),
        static_cast<unsigned long>(gCanonical.calls),
        static_cast<unsigned long long>(gFast.totalUs),
        static_cast<unsigned long long>(gFast.maxUs),
        static_cast<unsigned long>(gFast.calls),
        static_cast<unsigned long long>(gVelocity.totalUs),
        static_cast<unsigned long long>(gVelocity.maxUs),
        static_cast<unsigned long>(gVelocity.calls),
        static_cast<unsigned long>(gTimerReads),
        static_cast<unsigned long>(gClockRegression),
        static_cast<unsigned long>(gMismatchRecorded),
        static_cast<unsigned long>(gMismatches[0].yBits),
        static_cast<unsigned long>(gMismatches[0].xBits),
        static_cast<unsigned long>(gMismatches[0].fastBits),
        static_cast<unsigned long>(gMismatches[0].canonicalBits),
        static_cast<unsigned long>(gMismatches[1].yBits),
        static_cast<unsigned long>(gMismatches[1].xBits),
        static_cast<unsigned long>(gMismatches[1].fastBits),
        static_cast<unsigned long>(gMismatches[1].canonicalBits),
        static_cast<unsigned long>(gMismatches[2].yBits),
        static_cast<unsigned long>(gMismatches[2].xBits),
        static_cast<unsigned long>(gMismatches[2].fastBits),
        static_cast<unsigned long>(gMismatches[2].canonicalBits),
        static_cast<unsigned long>(gMismatches[3].yBits),
        static_cast<unsigned long>(gMismatches[3].xBits),
        static_cast<unsigned long>(gMismatches[3].fastBits),
        static_cast<unsigned long>(gMismatches[3].canonicalBits));
}
} // namespace th08::psp

#endif // TH08_PSP_ITEM_ATAN2_STATS_ENABLED
