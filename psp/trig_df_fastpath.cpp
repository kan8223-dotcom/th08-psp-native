#include "trig_df_fastpath.hpp"

#if TH08_PSP_TRIG_DF_STATS_ENABLED

#include "fileio.hpp"
#include "item_atan2_fastpath_math.hpp"
#include "item_sincos_fastpath_math.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace th08::psp
{
namespace
{
constexpr std::size_t kSiteCount = static_cast<std::size_t>(TrigDfSite::Count);
constexpr std::size_t kReasonCount = 6U; // both reason enums are 6 wide
constexpr std::size_t kMismatchRecords = 2U;

struct SiteStat
{
    std::uint64_t calls;
    std::uint64_t accepted;
    std::uint64_t mismatch;
};

struct MismatchRecord
{
    std::uint32_t site;
    std::uint32_t in0;
    std::uint32_t in1;
    std::uint32_t fast0;
    std::uint32_t canonical0;
    std::uint32_t fast1;
    std::uint32_t canonical1;
};

SiteStat gSites[kSiteCount]{};
std::uint64_t gSinCosReasons[kReasonCount]{};
std::uint64_t gAtan2Reasons[kReasonCount]{};
MismatchRecord gMismatches[kMismatchRecords]{};
std::uint32_t gMismatchRecorded = 0U;
bool gWindowActive = false;

void IncrementSaturating(std::uint64_t &value)
{
    if (value != std::numeric_limits<std::uint64_t>::max())
        ++value;
}

void NoteSinCos(TrigDfSite site, ItemSinCosFastpathReason reason)
{
    if (!gWindowActive)
        return;
    SiteStat &s = gSites[static_cast<std::size_t>(site)];
    IncrementSaturating(s.calls);
    if (reason == ItemSinCosFastpathReason::Accepted)
        IncrementSaturating(s.accepted);
    if (reason < ItemSinCosFastpathReason::Count)
        IncrementSaturating(gSinCosReasons[static_cast<std::size_t>(reason)]);
}

void NoteAtan2(ItemAtan2FastpathReason reason)
{
    if (!gWindowActive)
        return;
    SiteStat &s = gSites[static_cast<std::size_t>(TrigDfSite::Atan2)];
    IncrementSaturating(s.calls);
    if (reason == ItemAtan2FastpathReason::Accepted)
        IncrementSaturating(s.accepted);
    if (reason < ItemAtan2FastpathReason::Count)
        IncrementSaturating(gAtan2Reasons[static_cast<std::size_t>(reason)]);
}

#if TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED
std::uint32_t Bits(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void RecordMismatch(TrigDfSite site, float in0, float in1, float fast0,
                    float canonical0, float fast1, float canonical1)
{
    SiteStat &s = gSites[static_cast<std::size_t>(site)];
    IncrementSaturating(s.mismatch);
    if (gMismatchRecorded < kMismatchRecords)
    {
        gMismatches[gMismatchRecorded] = {
            static_cast<std::uint32_t>(site), Bits(in0), Bits(in1),
            Bits(fast0), Bits(canonical0), Bits(fast1), Bits(canonical1)};
        ++gMismatchRecorded;
    }
}
#endif

void ClearWindowCounters()
{
    std::memset(gSites, 0, sizeof(gSites));
    std::memset(gSinCosReasons, 0, sizeof(gSinCosReasons));
    std::memset(gAtan2Reasons, 0, sizeof(gAtan2Reasons));
    std::memset(gMismatches, 0, sizeof(gMismatches));
    gMismatchRecorded = 0U;
}
} // namespace

#if TH08_PSP_TRIG_DF_FASTPATH_ENABLED
bool TrigDfSinCosMul(float angle, float magnitude, float *cosMul,
                     float *sinMul, TrigDfSite site)
{
    float x = 0.0f;
    float y = 0.0f;
    ItemSinCosFastpathReason reason = ItemSinCosFastpathReason::Count;
    const bool accepted =
        ItemSinCosFastpathTry(angle, magnitude, &x, &y, &reason);
    NoteSinCos(site, reason);
    if (!accepted)
        return false;
    *cosMul = x;
    *sinMul = y;
    return true;
}

bool TrigDfAtan2(float y, float x, float *out)
{
    float angle = 0.0f;
    ItemAtan2FastpathReason reason = ItemAtan2FastpathReason::Count;
    const bool accepted = ItemAtan2FastpathTry(y, x, &angle, &reason);
    NoteAtan2(reason);
    if (!accepted)
        return false;
    *out = angle;
    return true;
}
#endif // TH08_PSP_TRIG_DF_FASTPATH_ENABLED

#if TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED
void TrigDfAuditSinCosMul(float angle, float magnitude,
                          float canonicalCosMul, float canonicalSinMul,
                          TrigDfSite site)
{
    if (!gWindowActive)
        return;
    float x = 0.0f;
    float y = 0.0f;
    ItemSinCosFastpathReason reason = ItemSinCosFastpathReason::Count;
    const bool accepted =
        ItemSinCosFastpathTry(angle, magnitude, &x, &y, &reason);
    NoteSinCos(site, reason);
    if (!accepted)
        return;
    if (Bits(x) != Bits(canonicalCosMul) || Bits(y) != Bits(canonicalSinMul))
        RecordMismatch(site, angle, magnitude, x, canonicalCosMul, y,
                       canonicalSinMul);
}

void TrigDfAuditAtan2(float y, float x, float canonical)
{
    if (!gWindowActive)
        return;
    float angle = 0.0f;
    ItemAtan2FastpathReason reason = ItemAtan2FastpathReason::Count;
    const bool accepted = ItemAtan2FastpathTry(y, x, &angle, &reason);
    NoteAtan2(reason);
    if (!accepted)
        return;
    if (Bits(angle) != Bits(canonical))
        RecordMismatch(TrigDfSite::Atan2, y, x, angle, canonical, 0.0f, 0.0f);
}
#endif // TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED

void TrigDfStatsResetWindow(bool active)
{
    ClearWindowCounters();
    gWindowActive = active;
}

void TrigDfStatsCancelWindow()
{
    if (gWindowActive)
        ClearWindowCounters();
    gWindowActive = false;
}

void TrigDfStatsEmitWindow(std::int32_t stage, std::uint32_t baselineStageFrame,
                           std::uint32_t stageFrame)
{
    if (!gWindowActive)
        return;
    // One bounded record at the parent PERF_ATTR 600-tick boundary; no timer.
    BootLog(
        "TRIG_DF V1 st=%ld sf=%lu-%lu mode=%s "
        "fa=%llu/%llu/%llu rv=%llu/%llu/%llu sm=%llu/%llu/%llu "
        "su=%llu/%llu/%llu at=%llu/%llu/%llu "
        "sc_zero=%llu sc_nonfinite=%llu sc_range=%llu sc_tiny=%llu "
        "sc_boundary=%llu at_zero=%llu at_nonfinite=%llu at_range=%llu "
        "at_tiny=%llu at_boundary=%llu mm_recorded=%lu "
        "mm0=%lu/%08lx/%08lx/%08lx/%08lx/%08lx/%08lx "
        "mm1=%lu/%08lx/%08lx/%08lx/%08lx/%08lx/%08lx\n",
        static_cast<long>(stage),
        static_cast<unsigned long>(baselineStageFrame),
        static_cast<unsigned long>(stageFrame),
        TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED ? "audit" : "product",
        static_cast<unsigned long long>(gSites[0].calls),
        static_cast<unsigned long long>(gSites[0].accepted),
        static_cast<unsigned long long>(gSites[0].mismatch),
        static_cast<unsigned long long>(gSites[1].calls),
        static_cast<unsigned long long>(gSites[1].accepted),
        static_cast<unsigned long long>(gSites[1].mismatch),
        static_cast<unsigned long long>(gSites[2].calls),
        static_cast<unsigned long long>(gSites[2].accepted),
        static_cast<unsigned long long>(gSites[2].mismatch),
        static_cast<unsigned long long>(gSites[3].calls),
        static_cast<unsigned long long>(gSites[3].accepted),
        static_cast<unsigned long long>(gSites[3].mismatch),
        static_cast<unsigned long long>(gSites[4].calls),
        static_cast<unsigned long long>(gSites[4].accepted),
        static_cast<unsigned long long>(gSites[4].mismatch),
        static_cast<unsigned long long>(gSinCosReasons[1]),
        static_cast<unsigned long long>(gSinCosReasons[2]),
        static_cast<unsigned long long>(gSinCosReasons[3]),
        static_cast<unsigned long long>(gSinCosReasons[4]),
        static_cast<unsigned long long>(gSinCosReasons[5]),
        static_cast<unsigned long long>(gAtan2Reasons[1]),
        static_cast<unsigned long long>(gAtan2Reasons[2]),
        static_cast<unsigned long long>(gAtan2Reasons[3]),
        static_cast<unsigned long long>(gAtan2Reasons[4]),
        static_cast<unsigned long long>(gAtan2Reasons[5]),
        static_cast<unsigned long>(gMismatchRecorded),
        static_cast<unsigned long>(gMismatches[0].site),
        static_cast<unsigned long>(gMismatches[0].in0),
        static_cast<unsigned long>(gMismatches[0].in1),
        static_cast<unsigned long>(gMismatches[0].fast0),
        static_cast<unsigned long>(gMismatches[0].canonical0),
        static_cast<unsigned long>(gMismatches[0].fast1),
        static_cast<unsigned long>(gMismatches[0].canonical1),
        static_cast<unsigned long>(gMismatches[1].site),
        static_cast<unsigned long>(gMismatches[1].in0),
        static_cast<unsigned long>(gMismatches[1].in1),
        static_cast<unsigned long>(gMismatches[1].fast0),
        static_cast<unsigned long>(gMismatches[1].canonical0),
        static_cast<unsigned long>(gMismatches[1].fast1),
        static_cast<unsigned long>(gMismatches[1].canonical1));
}
} // namespace th08::psp

#endif // TH08_PSP_TRIG_DF_STATS_ENABLED
