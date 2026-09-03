#include "perf_env.hpp"

#if TH08_PSP_PERF_ENV_ENABLED

#include "fileio.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include <pspge.h>
#include <psppower.h>
#include <pspthreadman.h>

extern "C" int __real_sceGeListEnQueue(const void *list, void *stall, int cbid,
                                       PspGeListArgs *arg);
extern "C" int __real_sceGeListSync(int qid, int syncType);

namespace th08::psp
{
std::uint8_t gPerfEnvCurrentPhase = kPerfEnvNoScope;

namespace
{
constexpr std::size_t kPhaseBins = kPerfEnvNoScope + 1U;
constexpr std::uint32_t kRecalibrationReads = 1024U;

struct WaitStat
{
    std::uint64_t totalUs;
    std::uint64_t maxUs;
    std::uint32_t calls;
};

WaitStat gPhaseWaits[kPhaseBins]{};
WaitStat gFlipWait{};
WaitStat gMain[kPerfEnvMainSlots]{};

struct GeQueueStat
{
    std::uint32_t enqueues;
    std::uint64_t busyIntervalUs;
    std::uint32_t busyIntervals;
    std::uint64_t idleIntervalUs;
    std::uint32_t idleIntervals;
    std::uint64_t swapWaitUs;
    std::uint64_t spanUs;
    std::uint32_t frames;
};

GeQueueStat gGe{};
int gGeLastQid = -1;
std::uint64_t gGeLastEnqueueUs = 0U;
std::uint64_t gGeFrameFirstUs = 0U;

inline void AddSaturating(std::uint64_t &total, std::uint64_t value)
{
    const std::uint64_t room = std::numeric_limits<std::uint64_t>::max() - total;
    total += value <= room ? value : room;
}
bool gWindowActive = false;
int gMainThreadId = -1;
int gCpuClock0 = 0;
int gBusClock0 = 0;
bool gRunValid0 = false;
std::uint64_t gRunClocks0 = 0U;
std::uint32_t gIntrPreempt0 = 0U;
std::uint32_t gThreadPreempt0 = 0U;

std::uint64_t ReadRunClocks(std::uint32_t *intrPreempt,
                            std::uint32_t *threadPreempt, bool *valid)
{
    SceKernelThreadRunStatus status;
    std::memset(&status, 0, sizeof(status));
    status.size = sizeof(status);
    if (gMainThreadId < 0 ||
        sceKernelReferThreadRunStatus(gMainThreadId, &status) < 0)
    {
        *valid = false;
        *intrPreempt = 0U;
        *threadPreempt = 0U;
        return 0U;
    }
    *valid = true;
    *intrPreempt = status.intrPreemptCount;
    *threadPreempt = status.threadPreemptCount;
    return (static_cast<std::uint64_t>(status.runClocks.hi) << 32U) |
           static_cast<std::uint64_t>(status.runClocks.low);
}

std::uint64_t Recalibrate()
{
    const std::uint64_t start =
        static_cast<std::uint64_t>(sceKernelGetSystemTimeWide());
    std::uint64_t sink = start;
    for (std::uint32_t i = 0U; i < kRecalibrationReads; ++i)
        sink ^= static_cast<std::uint64_t>(sceKernelGetSystemTimeWide());
    const std::uint64_t end =
        static_cast<std::uint64_t>(sceKernelGetSystemTimeWide());
    // Keep an observable dependency so the reads cannot be deleted.
    if (sink == std::numeric_limits<std::uint64_t>::max())
        return 0U;
    return end >= start ? end - start : 0U;
}
} // namespace

void PerfEnvInitialize(int mainThreadId)
{
    gMainThreadId = mainThreadId;
}

void PerfEnvWindowStart(bool active)
{
    std::memset(gPhaseWaits, 0, sizeof(gPhaseWaits));
    gFlipWait = WaitStat{};
    std::memset(gMain, 0, sizeof(gMain));
    gGe = GeQueueStat{};
    gWindowActive = active;
    if (!active)
        return;
    gCpuClock0 = scePowerGetCpuClockFrequencyInt();
    gBusClock0 = scePowerGetBusClockFrequencyInt();
    gRunClocks0 = ReadRunClocks(&gIntrPreempt0, &gThreadPreempt0, &gRunValid0);
}

void PerfEnvCancelWindow()
{
    gWindowActive = false;
}

void PerfEnvNoteUnscopedGeWait(std::uint8_t phase, std::uint64_t durationUs)
{
    if (!gWindowActive)
        return;
    WaitStat &stat = gPhaseWaits[phase < kPhaseBins ? phase : kPerfEnvNoScope];
    const std::uint64_t room = std::numeric_limits<std::uint64_t>::max() - stat.totalUs;
    stat.totalUs += durationUs <= room ? durationUs : room;
    if (durationUs > stat.maxUs)
        stat.maxUs = durationUs;
    if (stat.calls != std::numeric_limits<std::uint32_t>::max())
        ++stat.calls;
}

void PerfEnvNoteFlipWait(std::uint64_t durationUs)
{
    if (!gWindowActive)
        return;
    const std::uint64_t room = std::numeric_limits<std::uint64_t>::max() - gFlipWait.totalUs;
    gFlipWait.totalUs += durationUs <= room ? durationUs : room;
    if (durationUs > gFlipWait.maxUs)
        gFlipWait.maxUs = durationUs;
    if (gFlipWait.calls != std::numeric_limits<std::uint32_t>::max())
        ++gFlipWait.calls;
}

void PerfEnvNoteMain(std::uint8_t slot, std::uint64_t durationUs)
{
    if (!gWindowActive || slot >= kPerfEnvMainSlots)
        return;
    WaitStat &stat = gMain[slot];
    const std::uint64_t room = std::numeric_limits<std::uint64_t>::max() - stat.totalUs;
    stat.totalUs += durationUs <= room ? durationUs : room;
    if (durationUs > stat.maxUs)
        stat.maxUs = durationUs;
    if (stat.calls != std::numeric_limits<std::uint32_t>::max())
        ++stat.calls;
}

void PerfEnvNoteGeSwapWait(std::uint64_t durationUs)
{
    if (!gWindowActive)
        return;
    AddSaturating(gGe.swapWaitUs, durationUs);
}

void PerfEnvNoteGeFrameEnd(std::uint64_t nowUs)
{
    if (gGeFrameFirstUs != 0U)
    {
        if (gWindowActive)
        {
            AddSaturating(gGe.spanUs, nowUs >= gGeFrameFirstUs ? nowUs - gGeFrameFirstUs : 0U);
            if (gGe.frames != std::numeric_limits<std::uint32_t>::max())
                ++gGe.frames;
        }
        gGeFrameFirstUs = 0U;
    }
    // The swap waited for the GE: the next submission starts from idle.
    gGeLastQid = -1;
}

void PerfEnvEmitWindow(std::int32_t stage, std::uint32_t baselineStageFrame,
                       std::uint32_t stageFrame, std::uint64_t wallUs)
{
    if (!gWindowActive)
        return;
    const int cpuClock1 = scePowerGetCpuClockFrequencyInt();
    const int busClock1 = scePowerGetBusClockFrequencyInt();
    std::uint32_t intrPreempt1 = 0U;
    std::uint32_t threadPreempt1 = 0U;
    bool runValid1 = false;
    const std::uint64_t runClocks1 =
        ReadRunClocks(&intrPreempt1, &threadPreempt1, &runValid1);
    const bool runValid = gRunValid0 && runValid1 && runClocks1 >= gRunClocks0;
    const std::uint64_t runUs = runValid ? runClocks1 - gRunClocks0 : 0U;
    const std::uint32_t intrPreempt = runValid ? intrPreempt1 - gIntrPreempt0 : 0U;
    const std::uint32_t threadPreempt = runValid ? threadPreempt1 - gThreadPreempt0 : 0U;
    // The recalibration runs after the window closed and before the next one
    // opens, so its own cost lands in neither.
    const std::uint64_t recalibrationUs = Recalibrate();
    WaitStat total{};
    for (std::size_t i = 0U; i < kPhaseBins; ++i)
    {
        total.totalUs += gPhaseWaits[i].totalUs;
        total.calls += gPhaseWaits[i].calls;
        if (gPhaseWaits[i].maxUs > total.maxUs)
            total.maxUs = gPhaseWaits[i].maxUs;
    }
#define TH08_PERF_ENV_WAIT_ARGS(bin)                                          \
    static_cast<unsigned long long>(gPhaseWaits[bin].totalUs),               \
        static_cast<unsigned long long>(gPhaseWaits[bin].maxUs),             \
        static_cast<unsigned long>(gPhaseWaits[bin].calls)
    BootLog(
        "PERF_ENV V1 st=%ld sf=%lu-%lu wall=%llu clk0=%d/%d clk1=%d/%d "
        "recal_us=%llu run_valid=%d run_us=%llu intr_preempt=%lu "
        "thread_preempt=%lu fw=%llu/%llu/%lu "
        "mo0=%llu/%llu/%lu mo1=%llu/%llu/%lu mo2=%llu/%llu/%lu mo3=%llu/%llu/%lu "
        "mo4=%llu/%llu/%lu mo5=%llu/%llu/%lu mo6=%llu/%llu/%lu mo7=%llu/%llu/%lu "
        "gq=%lu/%llu/%lu/%llu/%lu/%llu/%llu/%lu "
        "gw=%llu/%llu/%lu "
        "gw0=%llu/%llu/%lu gw1=%llu/%llu/%lu gw2=%llu/%llu/%lu "
        "gw3=%llu/%llu/%lu gw4=%llu/%llu/%lu gw5=%llu/%llu/%lu "
        "gw6=%llu/%llu/%lu gw7=%llu/%llu/%lu gw8=%llu/%llu/%lu "
        "gw9=%llu/%llu/%lu gw10=%llu/%llu/%lu gw11=%llu/%llu/%lu "
        "gw12=%llu/%llu/%lu gw13=%llu/%llu/%lu gw14=%llu/%llu/%lu "
        "gw15=%llu/%llu/%lu gw16=%llu/%llu/%lu gw17=%llu/%llu/%lu "
        "gw18=%llu/%llu/%lu gw19=%llu/%llu/%lu gw20=%llu/%llu/%lu "
        "gw21=%llu/%llu/%lu gw22=%llu/%llu/%lu\n",
        static_cast<long>(stage),
        static_cast<unsigned long>(baselineStageFrame),
        static_cast<unsigned long>(stageFrame),
        static_cast<unsigned long long>(wallUs), gCpuClock0, gBusClock0,
        cpuClock1, busClock1,
        static_cast<unsigned long long>(recalibrationUs), runValid ? 1 : 0,
        static_cast<unsigned long long>(runUs),
        static_cast<unsigned long>(intrPreempt),
        static_cast<unsigned long>(threadPreempt),
        static_cast<unsigned long long>(gFlipWait.totalUs),
        static_cast<unsigned long long>(gFlipWait.maxUs),
        static_cast<unsigned long>(gFlipWait.calls),
        static_cast<unsigned long long>(gMain[0].totalUs),
        static_cast<unsigned long long>(gMain[0].maxUs),
        static_cast<unsigned long>(gMain[0].calls),
        static_cast<unsigned long long>(gMain[1].totalUs),
        static_cast<unsigned long long>(gMain[1].maxUs),
        static_cast<unsigned long>(gMain[1].calls),
        static_cast<unsigned long long>(gMain[2].totalUs),
        static_cast<unsigned long long>(gMain[2].maxUs),
        static_cast<unsigned long>(gMain[2].calls),
        static_cast<unsigned long long>(gMain[3].totalUs),
        static_cast<unsigned long long>(gMain[3].maxUs),
        static_cast<unsigned long>(gMain[3].calls),
        static_cast<unsigned long long>(gMain[4].totalUs),
        static_cast<unsigned long long>(gMain[4].maxUs),
        static_cast<unsigned long>(gMain[4].calls),
        static_cast<unsigned long long>(gMain[5].totalUs),
        static_cast<unsigned long long>(gMain[5].maxUs),
        static_cast<unsigned long>(gMain[5].calls),
        static_cast<unsigned long long>(gMain[6].totalUs),
        static_cast<unsigned long long>(gMain[6].maxUs),
        static_cast<unsigned long>(gMain[6].calls),
        static_cast<unsigned long long>(gMain[7].totalUs),
        static_cast<unsigned long long>(gMain[7].maxUs),
        static_cast<unsigned long>(gMain[7].calls),
        static_cast<unsigned long>(gGe.enqueues),
        static_cast<unsigned long long>(gGe.busyIntervalUs),
        static_cast<unsigned long>(gGe.busyIntervals),
        static_cast<unsigned long long>(gGe.idleIntervalUs),
        static_cast<unsigned long>(gGe.idleIntervals),
        static_cast<unsigned long long>(gGe.swapWaitUs),
        static_cast<unsigned long long>(gGe.spanUs),
        static_cast<unsigned long>(gGe.frames),
        static_cast<unsigned long long>(total.totalUs),
        static_cast<unsigned long long>(total.maxUs),
        static_cast<unsigned long>(total.calls),
        TH08_PERF_ENV_WAIT_ARGS(0), TH08_PERF_ENV_WAIT_ARGS(1),
        TH08_PERF_ENV_WAIT_ARGS(2), TH08_PERF_ENV_WAIT_ARGS(3),
        TH08_PERF_ENV_WAIT_ARGS(4), TH08_PERF_ENV_WAIT_ARGS(5),
        TH08_PERF_ENV_WAIT_ARGS(6), TH08_PERF_ENV_WAIT_ARGS(7),
        TH08_PERF_ENV_WAIT_ARGS(8), TH08_PERF_ENV_WAIT_ARGS(9),
        TH08_PERF_ENV_WAIT_ARGS(10), TH08_PERF_ENV_WAIT_ARGS(11),
        TH08_PERF_ENV_WAIT_ARGS(12), TH08_PERF_ENV_WAIT_ARGS(13),
        TH08_PERF_ENV_WAIT_ARGS(14), TH08_PERF_ENV_WAIT_ARGS(15),
        TH08_PERF_ENV_WAIT_ARGS(16), TH08_PERF_ENV_WAIT_ARGS(17),
        TH08_PERF_ENV_WAIT_ARGS(18), TH08_PERF_ENV_WAIT_ARGS(19),
        TH08_PERF_ENV_WAIT_ARGS(20), TH08_PERF_ENV_WAIT_ARGS(21),
        TH08_PERF_ENV_WAIT_ARGS(22));
#undef TH08_PERF_ENV_WAIT_ARGS
}

// PSPGL submits one complete list per 512-word buffer (cbid 0, no stall
// streaming), so every submission is a sampling point for the GE queue.
int PerfEnvGeListEnqueue(const void *list, void *stall, int cbid,
                         PspGeListArgs *arg)
{
    const std::uint64_t nowUs =
        static_cast<std::uint64_t>(sceKernelGetSystemTimeWide());
    if (gGeLastQid >= 0 && gGeFrameFirstUs != 0U)
    {
        // Peek only (syncType 1); an invalid/recycled id counts as done.
        const int state = __real_sceGeListSync(gGeLastQid, 1);
        const std::uint64_t intervalUs =
            nowUs >= gGeLastEnqueueUs ? nowUs - gGeLastEnqueueUs : 0U;
        if (gWindowActive)
        {
            if (state > 0)
            {
                AddSaturating(gGe.busyIntervalUs, intervalUs);
                if (gGe.busyIntervals != std::numeric_limits<std::uint32_t>::max())
                    ++gGe.busyIntervals;
            }
            else
            {
                AddSaturating(gGe.idleIntervalUs, intervalUs);
                if (gGe.idleIntervals != std::numeric_limits<std::uint32_t>::max())
                    ++gGe.idleIntervals;
            }
        }
    }
    if (gGeFrameFirstUs == 0U)
        gGeFrameFirstUs = nowUs;
    const int qid = __real_sceGeListEnQueue(list, stall, cbid, arg);
    gGeLastQid = qid;
    gGeLastEnqueueUs = nowUs;
    if (gWindowActive && gGe.enqueues != std::numeric_limits<std::uint32_t>::max())
        ++gGe.enqueues;
    return qid;
}
} // namespace th08::psp

#endif // TH08_PSP_PERF_ENV_ENABLED
