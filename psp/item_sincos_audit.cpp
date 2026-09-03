#include "item_sincos_audit.hpp"

#if TH08_PSP_ITEM_SINCOS_STATS_ENABLED

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
constexpr std::uint32_t kSampleMask = 31U;
constexpr std::size_t kMismatchRecords = 2U;
constexpr std::size_t kReasonCount =
    static_cast<std::size_t>(ItemSinCosFastpathReason::Count);

struct DurationStat
{
    std::uint64_t totalUs;
    std::uint64_t maxUs;
    std::uint32_t calls;
};

struct MismatchRecord
{
    std::uint32_t angleBits;
    std::uint32_t magnitudeBits;
    std::uint32_t fastXBits;
    std::uint32_t canonicalXBits;
    std::uint32_t fastYBits;
    std::uint32_t canonicalYBits;
};

std::uint64_t gCalls = 0U;
std::uint64_t gAccepted = 0U;
std::uint64_t gMismatch = 0U;
std::uint64_t gReasons[kReasonCount]{};
DurationStat gCanonical{};
DurationStat gFast{};
MismatchRecord gMismatches[kMismatchRecords]{};
std::uint32_t gMismatchRecorded = 0U;
std::uint32_t gSampled = 0U;
std::uint32_t gTimerReads = 0U;
std::uint32_t gClockRegression = 0U;
#if TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED
std::uint32_t gOrdinal = 0U;
#endif
bool gWindowActive = false;

[[maybe_unused]] void IncrementSaturating(std::uint32_t &value)
{
    if (value != std::numeric_limits<std::uint32_t>::max())
        ++value;
}

void IncrementSaturating(std::uint64_t &value)
{
    if (value != std::numeric_limits<std::uint64_t>::max())
        ++value;
}

#if TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED
std::uint32_t Bits(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::uint64_t ReadClock()
{
    IncrementSaturating(gTimerReads);
    return sceKernelGetSystemTimeWide();
}

void Record(DurationStat &stat, std::uint64_t startUs, std::uint64_t endUs)
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
#endif

void ClearWindowCounters()
{
    gCalls = 0U;
    gAccepted = 0U;
    gMismatch = 0U;
    std::memset(gReasons, 0, sizeof(gReasons));
    gCanonical = DurationStat{};
    gFast = DurationStat{};
    std::memset(gMismatches, 0, sizeof(gMismatches));
    gMismatchRecorded = 0U;
    gSampled = 0U;
    gTimerReads = 0U;
    gClockRegression = 0U;
}
} // namespace

#if TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED
bool ItemSinCosAuditBeginCall(std::uint64_t *startUs)
{
    const bool sampled = gWindowActive && ((gOrdinal & kSampleMask) == 0U);
    ++gOrdinal;
    if (sampled)
    {
        IncrementSaturating(gSampled);
        *startUs = ReadClock();
    }
    else
    {
        *startUs = 0U;
    }
    return sampled;
}

void ItemSinCosAuditAfterCanonical(float angle, float magnitude,
                                   float canonicalX, float canonicalY,
                                   bool sampled,
                                   std::uint64_t canonicalStartUs)
{
    if (!gWindowActive)
        return;
    std::uint64_t fastStartUs = 0U;
    if (sampled)
    {
        fastStartUs = ReadClock();
        Record(gCanonical, canonicalStartUs, fastStartUs);
    }
    float fastX = 0.0f;
    float fastY = 0.0f;
    ItemSinCosFastpathReason reason = ItemSinCosFastpathReason::Count;
    const bool accepted =
        ItemSinCosFastpathTry(angle, magnitude, &fastX, &fastY, &reason);
    if (sampled)
        Record(gFast, fastStartUs, ReadClock());

    IncrementSaturating(gCalls);
    if (reason < ItemSinCosFastpathReason::Count)
        IncrementSaturating(gReasons[static_cast<std::size_t>(reason)]);
    if (!accepted)
        return;
    IncrementSaturating(gAccepted);
    const std::uint32_t cx = Bits(canonicalX);
    const std::uint32_t cy = Bits(canonicalY);
    const std::uint32_t fx = Bits(fastX);
    const std::uint32_t fy = Bits(fastY);
    if (fx != cx || fy != cy)
    {
        IncrementSaturating(gMismatch);
        if (gMismatchRecorded < kMismatchRecords)
        {
            gMismatches[gMismatchRecorded] = {Bits(angle), Bits(magnitude),
                                               fx, cx, fy, cy};
            ++gMismatchRecorded;
        }
    }
}
#endif // TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED

void ItemSinCosProductNote(ItemSinCosFastpathReason reason)
{
    if (!gWindowActive)
        return;
    IncrementSaturating(gCalls);
    if (reason == ItemSinCosFastpathReason::Accepted)
        IncrementSaturating(gAccepted);
    if (reason < ItemSinCosFastpathReason::Count)
        IncrementSaturating(gReasons[static_cast<std::size_t>(reason)]);
}

void ItemSinCosStatsResetWindow(bool active)
{
    ClearWindowCounters();
    gWindowActive = active;
}

void ItemSinCosStatsCancelWindow()
{
    if (gWindowActive)
        ClearWindowCounters();
    gWindowActive = false;
}

void ItemSinCosStatsEmitWindow(std::int32_t stage,
                               std::uint32_t baselineStageFrame,
                               std::uint32_t stageFrame)
{
    if (!gWindowActive)
        return;
    // One bounded record at the parent PERF_ATTR 600-tick boundary.
    BootLog(
        "ITEM_SINCOS V1 st=%ld sf=%lu-%lu mode=%s calls=%llu accepted=%llu "
        "mismatch=%llu r_zero=%llu r_nonfinite=%llu r_range=%llu r_tiny=%llu "
        "r_boundary=%llu sampled=%lu "
        "t_canon=%llu/%llu/%lu t_fast=%llu/%llu/%lu timer_reads=%lu cr=%lu "
        "mm_recorded=%lu mm0=%08lx/%08lx/%08lx/%08lx/%08lx/%08lx "
        "mm1=%08lx/%08lx/%08lx/%08lx/%08lx/%08lx\n",
        static_cast<long>(stage),
        static_cast<unsigned long>(baselineStageFrame),
        static_cast<unsigned long>(stageFrame),
        TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED ? "audit" : "product",
        static_cast<unsigned long long>(gCalls),
        static_cast<unsigned long long>(gAccepted),
        static_cast<unsigned long long>(gMismatch),
        static_cast<unsigned long long>(gReasons[1]),
        static_cast<unsigned long long>(gReasons[2]),
        static_cast<unsigned long long>(gReasons[3]),
        static_cast<unsigned long long>(gReasons[4]),
        static_cast<unsigned long long>(gReasons[5]),
        static_cast<unsigned long>(gSampled),
        static_cast<unsigned long long>(gCanonical.totalUs),
        static_cast<unsigned long long>(gCanonical.maxUs),
        static_cast<unsigned long>(gCanonical.calls),
        static_cast<unsigned long long>(gFast.totalUs),
        static_cast<unsigned long long>(gFast.maxUs),
        static_cast<unsigned long>(gFast.calls),
        static_cast<unsigned long>(gTimerReads),
        static_cast<unsigned long>(gClockRegression),
        static_cast<unsigned long>(gMismatchRecorded),
        static_cast<unsigned long>(gMismatches[0].angleBits),
        static_cast<unsigned long>(gMismatches[0].magnitudeBits),
        static_cast<unsigned long>(gMismatches[0].fastXBits),
        static_cast<unsigned long>(gMismatches[0].canonicalXBits),
        static_cast<unsigned long>(gMismatches[0].fastYBits),
        static_cast<unsigned long>(gMismatches[0].canonicalYBits),
        static_cast<unsigned long>(gMismatches[1].angleBits),
        static_cast<unsigned long>(gMismatches[1].magnitudeBits),
        static_cast<unsigned long>(gMismatches[1].fastXBits),
        static_cast<unsigned long>(gMismatches[1].canonicalXBits),
        static_cast<unsigned long>(gMismatches[1].fastYBits),
        static_cast<unsigned long>(gMismatches[1].canonicalYBits));
}
} // namespace th08::psp

#endif // TH08_PSP_ITEM_SINCOS_STATS_ENABLED
