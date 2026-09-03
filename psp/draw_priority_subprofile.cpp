#include "draw_priority_subprofile.hpp"

#if TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED

#include "draw_priority_subprofile_math.hpp"
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
DrawPriorityDurationStat gPriorityStats[kDrawPriorityBinCount]{};
// GE submission census (sampled frames only, same gate as the timings).
std::uint32_t gDrawCallsRunning = 0U;
std::uint32_t gDrawVerticesRunning = 0U;
std::uint32_t gDrawCallsBaseline = 0U;
std::uint32_t gDrawVerticesBaseline = 0U;
std::uint32_t gBinDrawCalls[kDrawPriorityBinCount]{};
std::uint32_t gBinDrawVertices[kDrawPriorityBinCount]{};
std::uint32_t gChainDrawCalls = 0U;
std::uint32_t gChainDrawVertices = 0U;
// Unscoped GE waits per priority (all presented frames while the window is
// active, not only sampled ones).
int gCurrentPriority = -1;
std::uint64_t gBinGeWaitUs[kDrawPriorityBinCount]{};
std::uint32_t gBinGeWaitCalls[kDrawPriorityBinCount]{};
std::uint32_t gGeWaitFrames = 0U;
DrawPriorityDurationStat gDrawChainStat{};
DrawPriorityDurationStat gEffectBackgroundStat{};
std::uint64_t gPresentedFrameOrdinal = 0U;
std::uint32_t gSampledFrames = 0U;
std::uint32_t gTimerReads = 0U;
std::uint32_t gClockRegressionCount = 0U;
std::uint32_t gCounterOverflowCount = 0U;
bool gWindowActive = false;
bool gCurrentSampleActive = false;

void IncrementSaturating(std::uint32_t &value)
{
    if (value != std::numeric_limits<std::uint32_t>::max())
        ++value;
}

void RecordDuration(DrawPriorityDurationStat &stat, std::uint64_t startUs,
                    std::uint64_t endUs)
{
    if (endUs < startUs)
    {
        IncrementSaturating(gClockRegressionCount);
        return;
    }
    if (DrawPriorityAccumulate(stat, endUs - startUs))
        IncrementSaturating(gCounterOverflowCount);
}

std::uint64_t SaturatingTotal(std::uint64_t total, std::uint64_t addend,
                              bool &overflow)
{
    if (addend > std::numeric_limits<std::uint64_t>::max() - total)
    {
        overflow = true;
        return std::numeric_limits<std::uint64_t>::max();
    }
    return total + addend;
}

std::uint64_t CallbackTotal(bool &overflow)
{
    std::uint64_t total = 0U;
    for (std::size_t i = 0U; i < kDrawPriorityBinCount; ++i)
        total = SaturatingTotal(total, gPriorityStats[i].totalUs, overflow);
    return total;
}

std::uint32_t CallbackCalls(bool &overflow)
{
    std::uint32_t calls = 0U;
    for (std::size_t i = 0U; i < kDrawPriorityBinCount; ++i)
    {
        if (gPriorityStats[i].calls >
            std::numeric_limits<std::uint32_t>::max() - calls)
        {
            overflow = true;
            return std::numeric_limits<std::uint32_t>::max();
        }
        calls += gPriorityStats[i].calls;
    }
    return calls;
}

void ClearWindowCounters()
{
    std::memset(gPriorityStats, 0, sizeof(gPriorityStats));
    gDrawChainStat = DrawPriorityDurationStat{};
    gEffectBackgroundStat = DrawPriorityDurationStat{};
    gSampledFrames = 0U;
    gTimerReads = 0U;
    gClockRegressionCount = 0U;
    gCounterOverflowCount = 0U;
    gCurrentSampleActive = false;
    std::memset(gBinDrawCalls, 0, sizeof(gBinDrawCalls));
    std::memset(gBinDrawVertices, 0, sizeof(gBinDrawVertices));
    gChainDrawCalls = 0U;
    gChainDrawVertices = 0U;
    gCurrentPriority = -1;
    std::memset(gBinGeWaitUs, 0, sizeof(gBinGeWaitUs));
    std::memset(gBinGeWaitCalls, 0, sizeof(gBinGeWaitCalls));
    gGeWaitFrames = 0U;
}

void TakeDrawBaseline()
{
    gDrawCallsBaseline = gDrawCallsRunning;
    gDrawVerticesBaseline = gDrawVerticesRunning;
}
} // namespace

void DrawPrioritySubprofileNoteDraw(std::uint32_t vertices)
{
    ++gDrawCallsRunning;
    gDrawVerticesRunning += vertices;
}

void DrawPrioritySubprofileNoteCallbackPriority(int priority)
{
    gCurrentPriority = priority;
}

void DrawPrioritySubprofileNoteGeWait(std::uint64_t durationUs)
{
    if (!gWindowActive || gCurrentPriority < 0)
        return;
    const std::size_t bin = DrawPriorityBinFor(gCurrentPriority);
    gBinGeWaitUs[bin] += durationUs;
    IncrementSaturating(gBinGeWaitCalls[bin]);
}

std::uint64_t DrawPrioritySubprofileReadClock()
{
    IncrementSaturating(gTimerReads);
    return sceKernelGetSystemTimeWide();
}

bool DrawPrioritySubprofileBeginDrawChain(std::uint64_t &startUs)
{
    const std::uint64_t ordinal = gPresentedFrameOrdinal++;
    if (gWindowActive)
        IncrementSaturating(gGeWaitFrames);
    gCurrentSampleActive =
        gWindowActive && DrawPriorityShouldSampleOrdinal(ordinal);
    if (!gCurrentSampleActive)
    {
        startUs = 0U;
        return false;
    }

    IncrementSaturating(gSampledFrames);
    TakeDrawBaseline();
    startUs = DrawPrioritySubprofileReadClock();
    return true;
}

void DrawPrioritySubprofileRecordCallback(int priority,
                                          std::uint64_t startUs)
{
    const std::uint64_t endUs = DrawPrioritySubprofileReadClock();
    RecordDuration(gPriorityStats[DrawPriorityBinFor(priority)], startUs,
                   endUs);
    const std::size_t bin = DrawPriorityBinFor(priority);
    const std::uint32_t calls = gDrawCallsRunning - gDrawCallsBaseline;
    const std::uint32_t vertices = gDrawVerticesRunning - gDrawVerticesBaseline;
    gBinDrawCalls[bin] += calls;
    gBinDrawVertices[bin] += vertices;
    gChainDrawCalls += calls;
    gChainDrawVertices += vertices;
    TakeDrawBaseline();
}

void DrawPrioritySubprofileEndDrawChain(std::uint64_t startUs)
{
    const std::uint64_t endUs = DrawPrioritySubprofileReadClock();
    RecordDuration(gDrawChainStat, startUs, endUs);
    gCurrentSampleActive = false;
    gCurrentPriority = -1;
}

bool DrawPrioritySubprofileBeginEffectBackground(std::uint64_t &startUs)
{
    if (!gCurrentSampleActive)
    {
        startUs = 0U;
        return false;
    }
    startUs = DrawPrioritySubprofileReadClock();
    return true;
}

void DrawPrioritySubprofileEndEffectBackground(std::uint64_t startUs)
{
    const std::uint64_t endUs = DrawPrioritySubprofileReadClock();
    RecordDuration(gEffectBackgroundStat, startUs, endUs);
}

void DrawPrioritySubprofileResetWindow(bool active)
{
    ClearWindowCounters();
    gWindowActive = active;
}

void DrawPrioritySubprofileCancelWindow()
{
    // stageFrame==0 can persist for every title/menu presentation.  Clear the
    // abandoned gameplay window once, not 600+ bytes on every idle frame.
    if (gWindowActive || gCurrentSampleActive)
        ClearWindowCounters();
    gWindowActive = false;
}

void DrawPrioritySubprofileEmitWindow(std::int32_t stage,
                                      std::uint32_t baselineStageFrame,
                                      std::uint32_t stageFrame,
                                      std::uint32_t presentedFrames,
                                      std::uint8_t cadenceMode)
{
    if (!gWindowActive)
        return;

    bool totalOverflow = false;
    bool callOverflow = false;
    const std::uint64_t callbackTotal = CallbackTotal(totalOverflow);
    const std::uint32_t callbackCalls = CallbackCalls(callOverflow);
    const DrawPriorityDifference dispatchResidual =
        DrawPrioritySubtract(gDrawChainStat.totalUs, callbackTotal);
    const DrawPriorityDifference priority7Exclusive = DrawPrioritySubtract(
        gPriorityStats[7U].totalUs, gEffectBackgroundStat.totalUs);
    std::uint32_t underflowMask = 0U;
    underflowMask |= dispatchResidual.underflow ? 1U << 0U : 0U;
    underflowMask |= priority7Exclusive.underflow ? 1U << 1U : 0U;
    underflowMask |= totalOverflow ? 1U << 2U : 0U;
    underflowMask |= callOverflow ? 1U << 3U : 0U;

    // One bounded record at the parent PERF_ATTR 600-tick boundary. BootLog
    // remains buffered; there is deliberately no per-frame I/O or flush.
    BootLog(
        "DRAW_PRIO V1 st=%ld sf=%lu-%lu presented=%lu cadence_mode=%u "
        "sample_rule=presented_ordinal_private_block_hash_1of16 "
        "ordinal_end=%llu "
        "sampled=%lu chain=%llu/%llu/%lu cb=%llu/%lu residual=%llu "
        "effect_bg=%llu/%llu/%lu p7x=%llu timer_reads=%lu cr=%lu ov=%lu "
        "uf=0x%02lx "
        "p0=%llu/%llu/%lu p1=%llu/%llu/%lu p2=%llu/%llu/%lu "
        "p3=%llu/%llu/%lu p4=%llu/%llu/%lu p5=%llu/%llu/%lu "
        "p6=%llu/%llu/%lu p7=%llu/%llu/%lu p8=%llu/%llu/%lu "
        "p9=%llu/%llu/%lu p10=%llu/%llu/%lu p11=%llu/%llu/%lu "
        "p12=%llu/%llu/%lu p13=%llu/%llu/%lu p14=%llu/%llu/%lu "
        "p15=%llu/%llu/%lu p16=%llu/%llu/%lu p17=%llu/%llu/%lu "
        "p18=%llu/%llu/%lu p19=%llu/%llu/%lu p20=%llu/%llu/%lu "
        "p21=%llu/%llu/%lu po=%llu/%llu/%lu\n",
        static_cast<long>(stage),
        static_cast<unsigned long>(baselineStageFrame),
        static_cast<unsigned long>(stageFrame),
        static_cast<unsigned long>(presentedFrames),
        static_cast<unsigned int>(cadenceMode),
        static_cast<unsigned long long>(gPresentedFrameOrdinal),
        static_cast<unsigned long>(gSampledFrames),
        static_cast<unsigned long long>(gDrawChainStat.totalUs),
        static_cast<unsigned long long>(gDrawChainStat.maxUs),
        static_cast<unsigned long>(gDrawChainStat.calls),
        static_cast<unsigned long long>(callbackTotal),
        static_cast<unsigned long>(callbackCalls),
        static_cast<unsigned long long>(dispatchResidual.value),
        static_cast<unsigned long long>(gEffectBackgroundStat.totalUs),
        static_cast<unsigned long long>(gEffectBackgroundStat.maxUs),
        static_cast<unsigned long>(gEffectBackgroundStat.calls),
        static_cast<unsigned long long>(priority7Exclusive.value),
        static_cast<unsigned long>(gTimerReads),
        static_cast<unsigned long>(gClockRegressionCount),
        static_cast<unsigned long>(gCounterOverflowCount),
        static_cast<unsigned long>(underflowMask),
#define TH08_DRAW_PRIORITY_BIN_ARGS(index)                                      \
        static_cast<unsigned long long>(gPriorityStats[index].totalUs),        \
        static_cast<unsigned long long>(gPriorityStats[index].maxUs),          \
        static_cast<unsigned long>(gPriorityStats[index].calls)
        TH08_DRAW_PRIORITY_BIN_ARGS(0), TH08_DRAW_PRIORITY_BIN_ARGS(1),
        TH08_DRAW_PRIORITY_BIN_ARGS(2), TH08_DRAW_PRIORITY_BIN_ARGS(3),
        TH08_DRAW_PRIORITY_BIN_ARGS(4), TH08_DRAW_PRIORITY_BIN_ARGS(5),
        TH08_DRAW_PRIORITY_BIN_ARGS(6), TH08_DRAW_PRIORITY_BIN_ARGS(7),
        TH08_DRAW_PRIORITY_BIN_ARGS(8), TH08_DRAW_PRIORITY_BIN_ARGS(9),
        TH08_DRAW_PRIORITY_BIN_ARGS(10), TH08_DRAW_PRIORITY_BIN_ARGS(11),
        TH08_DRAW_PRIORITY_BIN_ARGS(12), TH08_DRAW_PRIORITY_BIN_ARGS(13),
        TH08_DRAW_PRIORITY_BIN_ARGS(14), TH08_DRAW_PRIORITY_BIN_ARGS(15),
        TH08_DRAW_PRIORITY_BIN_ARGS(16), TH08_DRAW_PRIORITY_BIN_ARGS(17),
        TH08_DRAW_PRIORITY_BIN_ARGS(18), TH08_DRAW_PRIORITY_BIN_ARGS(19),
        TH08_DRAW_PRIORITY_BIN_ARGS(20), TH08_DRAW_PRIORITY_BIN_ARGS(21),
        TH08_DRAW_PRIORITY_BIN_ARGS(kDrawPriorityOtherBin));
#undef TH08_DRAW_PRIORITY_BIN_ARGS
}

void DrawPrioritySubprofileEmitGeWindow(std::int32_t stage,
                                        std::uint32_t baselineStageFrame,
                                        std::uint32_t stageFrame)
{
    if (!gWindowActive)
        return;
#define TH08_DRAW_PRIORITY_GE_ARGS(bin)                                       \
    static_cast<unsigned long>(gBinDrawCalls[bin]),                          \
        static_cast<unsigned long>(gBinDrawVertices[bin]),                   \
        static_cast<unsigned long long>(gBinGeWaitUs[bin]),                  \
        static_cast<unsigned long>(gBinGeWaitCalls[bin])
    std::uint64_t geWaitTotalUs = 0U;
    std::uint32_t geWaitTotalCalls = 0U;
    for (std::size_t bin = 0U; bin < kDrawPriorityBinCount; ++bin)
    {
        geWaitTotalUs += gBinGeWaitUs[bin];
        geWaitTotalCalls += gBinGeWaitCalls[bin];
    }
    // Draw submissions per sampled present, attributed to the callback that
    // issued them (direct-GE bullet submissions bypass the compat layer and
    // are not counted here).
    BootLog(
        "DRAW_PRIO_GE V1 st=%ld sf=%lu-%lu sampled=%lu chain=%lu/%lu "
        "gwf=%lu gwc=%llu/%lu "
        "p0=%lu/%lu/%llu/%lu p1=%lu/%lu/%llu/%lu p2=%lu/%lu/%llu/%lu "
        "p3=%lu/%lu/%llu/%lu p4=%lu/%lu/%llu/%lu p5=%lu/%lu/%llu/%lu "
        "p6=%lu/%lu/%llu/%lu p7=%lu/%lu/%llu/%lu p8=%lu/%lu/%llu/%lu "
        "p9=%lu/%lu/%llu/%lu p10=%lu/%lu/%llu/%lu p11=%lu/%lu/%llu/%lu "
        "p12=%lu/%lu/%llu/%lu p13=%lu/%lu/%llu/%lu p14=%lu/%lu/%llu/%lu "
        "p15=%lu/%lu/%llu/%lu p16=%lu/%lu/%llu/%lu p17=%lu/%lu/%llu/%lu "
        "p18=%lu/%lu/%llu/%lu p19=%lu/%lu/%llu/%lu p20=%lu/%lu/%llu/%lu "
        "p21=%lu/%lu/%llu/%lu po=%lu/%lu/%llu/%lu\n",
        static_cast<long>(stage),
        static_cast<unsigned long>(baselineStageFrame),
        static_cast<unsigned long>(stageFrame),
        static_cast<unsigned long>(gSampledFrames),
        static_cast<unsigned long>(gChainDrawCalls),
        static_cast<unsigned long>(gChainDrawVertices),
        static_cast<unsigned long>(gGeWaitFrames),
        static_cast<unsigned long long>(geWaitTotalUs),
        static_cast<unsigned long>(geWaitTotalCalls),
        TH08_DRAW_PRIORITY_GE_ARGS(0), TH08_DRAW_PRIORITY_GE_ARGS(1),
        TH08_DRAW_PRIORITY_GE_ARGS(2), TH08_DRAW_PRIORITY_GE_ARGS(3),
        TH08_DRAW_PRIORITY_GE_ARGS(4), TH08_DRAW_PRIORITY_GE_ARGS(5),
        TH08_DRAW_PRIORITY_GE_ARGS(6), TH08_DRAW_PRIORITY_GE_ARGS(7),
        TH08_DRAW_PRIORITY_GE_ARGS(8), TH08_DRAW_PRIORITY_GE_ARGS(9),
        TH08_DRAW_PRIORITY_GE_ARGS(10), TH08_DRAW_PRIORITY_GE_ARGS(11),
        TH08_DRAW_PRIORITY_GE_ARGS(12), TH08_DRAW_PRIORITY_GE_ARGS(13),
        TH08_DRAW_PRIORITY_GE_ARGS(14), TH08_DRAW_PRIORITY_GE_ARGS(15),
        TH08_DRAW_PRIORITY_GE_ARGS(16), TH08_DRAW_PRIORITY_GE_ARGS(17),
        TH08_DRAW_PRIORITY_GE_ARGS(18), TH08_DRAW_PRIORITY_GE_ARGS(19),
        TH08_DRAW_PRIORITY_GE_ARGS(20), TH08_DRAW_PRIORITY_GE_ARGS(21),
        TH08_DRAW_PRIORITY_GE_ARGS(kDrawPriorityOtherBin));
#undef TH08_DRAW_PRIORITY_GE_ARGS
}
} // namespace th08::psp

#endif // TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED
