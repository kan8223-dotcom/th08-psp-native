#include "perf_attribution.hpp"

#if TH08_PSP_PERF_ATTRIBUTION_ENABLED

#include "draw_priority_subprofile.hpp"
#include "item_atan2_audit.hpp"
#include "item_sincos_audit.hpp"
#include "item_update_subprofile.hpp"
#include "fileio.hpp"
#include "perf_attribution_math.hpp"
#include "softfloat_census.hpp"
#include "audio_cursor_audit.hpp"
#include "trig_df_fastpath.hpp"
#include "perf_env.hpp"
#include "gui_border_replay.hpp"
#include "effect_occupancy_audit.hpp"
#include "bullet_gate_stats.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

// Keep the PSPSDK ABI boundary narrow.  Game translation units use the
// reconstruction's fixed-width aliases, whose spelling conflicts with some
// legacy PSPSDK headers.
extern "C" std::uint64_t sceKernelGetSystemTimeWide(void);
extern "C" int sceKernelGetThreadId(void);
extern "C" int __real_sceGeListSync(int qid, int syncType);
extern "C" int __real_sceDisplayWaitVblankStart(void);

namespace th08::psp
{
namespace
{
constexpr std::uint32_t kWindowStageFrames = 600U;
constexpr std::uint32_t kTimerCalibrationReads = 1024U;
constexpr std::size_t kPhaseCount =
    static_cast<std::size_t>(PerfAttributionPhase::Count);

struct PhaseStat
{
    std::uint64_t totalUs;
    std::uint64_t maxUs;
    std::uint32_t calls;
};

PhaseStat gStats[kPhaseCount]{};
bool gInitialized = false;
bool gWindowActive = false;
int gMainThreadId = -1;
volatile PerfAttributionWaitContext gWaitContext =
    PerfAttributionWaitContext::None;
std::uint64_t gCalibrationUs = 0U;
std::uint64_t gWindowStartUs = 0U;
std::uint32_t gTimerReads = 0U;
std::int32_t gStage = -1;
std::uint32_t gLastStageFrame = 0U;
std::uint32_t gBaselineStageFrame = 0U;
std::uint32_t gTargetStageFrame = 0U;
std::uint32_t gDraws = 0U;
std::uint8_t gCadenceMode = 0U;
bool gReplay = false;
std::uint8_t gDemoReplay = 0U;
std::uint32_t gClockRegressionCount = 0U;

std::uint64_t RawNow()
{
    return sceKernelGetSystemTimeWide();
}

std::uint64_t MeasuredNow()
{
    if (gTimerReads != UINT32_MAX)
        ++gTimerReads;
    return RawNow();
}

std::size_t PhaseIndex(PerfAttributionPhase phase)
{
    return static_cast<std::size_t>(phase);
}

void RecordDuration(PerfAttributionPhase phase, std::uint64_t startUs,
                    std::uint64_t endUs)
{
    if (!gWindowActive)
        return;
    if (endUs < startUs)
    {
        if (gClockRegressionCount != UINT32_MAX)
            ++gClockRegressionCount;
        return;
    }

    PhaseStat &stat = gStats[PhaseIndex(phase)];
    const std::uint64_t durationUs = endUs - startUs;
    const std::uint64_t room = UINT64_MAX - stat.totalUs;
    stat.totalUs += durationUs <= room ? durationUs : room;
    if (durationUs > stat.maxUs)
        stat.maxUs = durationUs;
    if (stat.calls != UINT32_MAX)
        ++stat.calls;
}

const PhaseStat &Stat(PerfAttributionPhase phase)
{
    return gStats[PhaseIndex(phase)];
}

void StartWindow(std::int32_t stage, std::uint32_t stageFrame,
                 std::uint8_t cadenceMode, bool replay,
                 std::uint8_t demoReplay, std::uint64_t startUs)
{
    std::memset(gStats, 0, sizeof(gStats));
    gTimerReads = 0U;
    gClockRegressionCount = 0U;
    gStage = stage;
    gLastStageFrame = stageFrame;
    gBaselineStageFrame = stageFrame;
    gTargetStageFrame =
        stageFrame <= UINT32_MAX - kWindowStageFrames
            ? stageFrame + kWindowStageFrames
            : 0U;
    gDraws = 0U;
    gCadenceMode = cadenceMode;
    gReplay = replay;
    gDemoReplay = demoReplay;
    gWindowStartUs = startUs;
    gWindowActive = gTargetStageFrame != 0U;
#if TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED
    DrawPrioritySubprofileResetWindow(gWindowActive);
#endif
#if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED
    ItemUpdateSubprofileResetWindow(gWindowActive);
#endif
#if TH08_PSP_ITEM_ATAN2_STATS_ENABLED
    ItemAtan2StatsResetWindow(gWindowActive);
#endif
#if TH08_PSP_ITEM_SINCOS_STATS_ENABLED
    ItemSinCosStatsResetWindow(gWindowActive);
#endif
#if TH08_PSP_SOFTFLOAT_CENSUS_ENABLED
    SoftfloatCensusResetWindow(gWindowActive);
#endif
#if TH08_PSP_AUDIO_CURSOR_STATS_ENABLED
    AudioCursorStatsResetWindow(gWindowActive);
#endif
#if TH08_PSP_TRIG_DF_STATS_ENABLED
    TrigDfStatsResetWindow(gWindowActive);
#endif
#if TH08_PSP_PERF_ENV_ENABLED
    PerfEnvWindowStart(gWindowActive);
#endif
#if TH08_PSP_GUI_BORDER_STATS_ENABLED
    GuiBorderStatsResetWindow(gWindowActive);
#endif
#if TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED
    EffectOccupancyStatsResetWindow(gWindowActive);
#endif
#if TH08_PSP_BULLET_GATE_STATS_ENABLED
    BulletGateStatsResetWindow(gWindowActive);
#endif
}

std::uint64_t Total(PerfAttributionPhase phase)
{
    return Stat(phase).totalUs;
}

std::uint64_t EstimateTimerOverheadUs()
{
    if (gCalibrationUs == 0U || gTimerReads == 0U)
        return 0U;
    // Round up: the diagnostic should not under-report its own clock cost.
    return (gCalibrationUs * static_cast<std::uint64_t>(gTimerReads) +
            kTimerCalibrationReads - 1U) /
           kTimerCalibrationReads;
}

void EmitWindow(std::uint32_t stageFrame, std::uint64_t endUs)
{
    const PerfAttributionDifference bulletUpdate = PerfAttributionSubtract(
        Total(PerfAttributionPhase::BulletUpdateInclusive),
        Total(PerfAttributionPhase::ItemUpdate));
    const PerfAttributionDifference bulletDraw = PerfAttributionSubtract(
        Total(PerfAttributionPhase::BulletDrawInclusive),
        Total(PerfAttributionPhase::ItemDraw),
        Total(PerfAttributionPhase::EffectDrawBullet));

    const std::uint64_t effectDraw =
        Total(PerfAttributionPhase::EffectDrawMain) +
        Total(PerfAttributionPhase::EffectDrawBullet) +
        Total(PerfAttributionPhase::EffectDrawBackground);

    const PerfAttributionDifference calcOther0 = PerfAttributionSubtract(
        Total(PerfAttributionPhase::CalcChain),
        Total(PerfAttributionPhase::PlayerUpdate),
        Total(PerfAttributionPhase::EnemyUpdate));
    const PerfAttributionDifference calcOther1 = PerfAttributionSubtract(
        calcOther0.value, Total(PerfAttributionPhase::EffectUpdate),
        Total(PerfAttributionPhase::BulletUpdateInclusive));

    // BulletDrawInclusive already contains ItemDraw and EffectDrawBullet;
    // subtract only the top-level/disjoint callback owners here.
    const PerfAttributionDifference drawOther0 = PerfAttributionSubtract(
        Total(PerfAttributionPhase::DrawChain),
        Total(PerfAttributionPhase::PlayerDraw),
        Total(PerfAttributionPhase::EnemyDraw));
    const PerfAttributionDifference drawOther1 = PerfAttributionSubtract(
        drawOther0.value, Total(PerfAttributionPhase::EffectDrawMain),
        Total(PerfAttributionPhase::BulletDrawInclusive));
    const PerfAttributionDifference drawOther2 = PerfAttributionSubtract(
        drawOther1.value,
        Total(PerfAttributionPhase::EffectDrawBackground));
    const PerfAttributionDifference drawFrameOther = PerfAttributionSubtract(
        Total(PerfAttributionPhase::DrawFrame),
        Total(PerfAttributionPhase::DrawChain));

    const PerfAttributionDifference swapCpu = PerfAttributionSubtract(
        Total(PerfAttributionPhase::PresentSwap),
        Total(PerfAttributionPhase::GeWaitSwap),
        Total(PerfAttributionPhase::VblankWaitSwap));
    const PerfAttributionDifference presentOther0 = PerfAttributionSubtract(
        Total(PerfAttributionPhase::PresentOuter),
        Total(PerfAttributionPhase::PresentPreSwap),
        Total(PerfAttributionPhase::PresentSwap));
    const PerfAttributionDifference presentOther1 = PerfAttributionSubtract(
        presentOther0.value,
        Total(PerfAttributionPhase::PresentPostSwap));

    std::uint32_t underflowMask = 0U;
    underflowMask |= bulletUpdate.underflow ? 1U << 0 : 0U;
    underflowMask |= bulletDraw.underflow ? 1U << 1 : 0U;
    underflowMask |= (calcOther0.underflow || calcOther1.underflow)
                         ? 1U << 2
                         : 0U;
    underflowMask |= (drawOther0.underflow || drawOther1.underflow ||
                      drawOther2.underflow)
                         ? 1U << 3
                         : 0U;
    underflowMask |= swapCpu.underflow ? 1U << 4 : 0U;
    underflowMask |= (presentOther0.underflow || presentOther1.underflow)
                         ? 1U << 5
                         : 0U;
    underflowMask |= drawFrameOther.underflow ? 1U << 6 : 0U;

    const std::uint64_t wallUs =
        endUs >= gWindowStartUs ? endUs - gWindowStartUs : 0U;
    if (endUs < gWindowStartUs && gClockRegressionCount != UINT32_MAX)
        ++gClockRegressionCount;
    const PerfAttributionDifference wallOther0 = PerfAttributionSubtract(
        wallUs, Total(PerfAttributionPhase::CalcChain),
        Total(PerfAttributionPhase::DrawFrame));
    const PerfAttributionDifference wallOther1 = PerfAttributionSubtract(
        wallOther0.value, Total(PerfAttributionPhase::PresentOuter),
        Total(PerfAttributionPhase::VblankWaitCadence));
    underflowMask |= (wallOther0.underflow || wallOther1.underflow)
                         ? 1U << 7
                         : 0U;

    // One bounded (<4 KiB) record per 600 stage ticks.  BootLog buffers it;
    // deliberately do not FlushBootLog() on the gameplay path.
    BootLog(
        "PERF_ATTR V1 st=%ld sf=%lu-%lu sim_frames=600 sim_hz=60 "
        "rendered_frames=%lu render_target_fps=%u cadence_mode=%u "
        "replay=%d demo=%u wall=%llu wo=%llu tc=%llu/1024 tr=%lu te=%llu cr=%lu "
        "calc=%llu/%llu/%lu pu=%llu/%llu/%lu eu=%llu/%llu/%lu "
        "fxu=%llu/%llu/%lu bui=%llu/%llu/%lu iu=%llu/%llu/%lu "
        "bux=%llu co=%llu "
        "drawf=%llu/%llu/%lu drawc=%llu/%llu/%lu dfo=%llu "
        "pd=%llu/%llu/%lu ed=%llu/%llu/%lu "
        "fxd=%llu/%llu/%llu/%llu bd_i=%llu/%llu/%lu "
        "id=%llu/%llu/%lu bdx=%llu do=%llu "
        "pres=%llu/%llu/%lu pre=%llu sw=%llu ge=%llu vbs=%llu "
        "swcpu=%llu post=%llu po=%llu vbc=%llu uf=0x%02lx\n",
        static_cast<long>(gStage),
        static_cast<unsigned long>(gBaselineStageFrame),
        static_cast<unsigned long>(stageFrame),
        static_cast<unsigned long>(gDraws),
        static_cast<unsigned int>(60U / (gCadenceMode + 1U)),
        static_cast<unsigned int>(gCadenceMode), gReplay ? 1 : 0,
        static_cast<unsigned int>(gDemoReplay),
        static_cast<unsigned long long>(wallUs),
        static_cast<unsigned long long>(wallOther1.value),
        static_cast<unsigned long long>(gCalibrationUs),
        static_cast<unsigned long>(gTimerReads),
        static_cast<unsigned long long>(EstimateTimerOverheadUs()),
        static_cast<unsigned long>(gClockRegressionCount),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::CalcChain)),
        static_cast<unsigned long long>(Stat(PerfAttributionPhase::CalcChain).maxUs),
        static_cast<unsigned long>(Stat(PerfAttributionPhase::CalcChain).calls),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::PlayerUpdate)),
        static_cast<unsigned long long>(Stat(PerfAttributionPhase::PlayerUpdate).maxUs),
        static_cast<unsigned long>(Stat(PerfAttributionPhase::PlayerUpdate).calls),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::EnemyUpdate)),
        static_cast<unsigned long long>(Stat(PerfAttributionPhase::EnemyUpdate).maxUs),
        static_cast<unsigned long>(Stat(PerfAttributionPhase::EnemyUpdate).calls),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::EffectUpdate)),
        static_cast<unsigned long long>(Stat(PerfAttributionPhase::EffectUpdate).maxUs),
        static_cast<unsigned long>(Stat(PerfAttributionPhase::EffectUpdate).calls),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::BulletUpdateInclusive)),
        static_cast<unsigned long long>(Stat(PerfAttributionPhase::BulletUpdateInclusive).maxUs),
        static_cast<unsigned long>(Stat(PerfAttributionPhase::BulletUpdateInclusive).calls),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::ItemUpdate)),
        static_cast<unsigned long long>(Stat(PerfAttributionPhase::ItemUpdate).maxUs),
        static_cast<unsigned long>(Stat(PerfAttributionPhase::ItemUpdate).calls),
        static_cast<unsigned long long>(bulletUpdate.value),
        static_cast<unsigned long long>(calcOther1.value),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::DrawFrame)),
        static_cast<unsigned long long>(Stat(PerfAttributionPhase::DrawFrame).maxUs),
        static_cast<unsigned long>(Stat(PerfAttributionPhase::DrawFrame).calls),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::DrawChain)),
        static_cast<unsigned long long>(Stat(PerfAttributionPhase::DrawChain).maxUs),
        static_cast<unsigned long>(Stat(PerfAttributionPhase::DrawChain).calls),
        static_cast<unsigned long long>(drawFrameOther.value),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::PlayerDraw)),
        static_cast<unsigned long long>(Stat(PerfAttributionPhase::PlayerDraw).maxUs),
        static_cast<unsigned long>(Stat(PerfAttributionPhase::PlayerDraw).calls),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::EnemyDraw)),
        static_cast<unsigned long long>(Stat(PerfAttributionPhase::EnemyDraw).maxUs),
        static_cast<unsigned long>(Stat(PerfAttributionPhase::EnemyDraw).calls),
        static_cast<unsigned long long>(effectDraw),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::EffectDrawMain)),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::EffectDrawBullet)),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::EffectDrawBackground)),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::BulletDrawInclusive)),
        static_cast<unsigned long long>(Stat(PerfAttributionPhase::BulletDrawInclusive).maxUs),
        static_cast<unsigned long>(Stat(PerfAttributionPhase::BulletDrawInclusive).calls),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::ItemDraw)),
        static_cast<unsigned long long>(Stat(PerfAttributionPhase::ItemDraw).maxUs),
        static_cast<unsigned long>(Stat(PerfAttributionPhase::ItemDraw).calls),
        static_cast<unsigned long long>(bulletDraw.value),
        static_cast<unsigned long long>(drawOther2.value),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::PresentOuter)),
        static_cast<unsigned long long>(Stat(PerfAttributionPhase::PresentOuter).maxUs),
        static_cast<unsigned long>(Stat(PerfAttributionPhase::PresentOuter).calls),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::PresentPreSwap)),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::PresentSwap)),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::GeWaitSwap)),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::VblankWaitSwap)),
        static_cast<unsigned long long>(swapCpu.value),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::PresentPostSwap)),
        static_cast<unsigned long long>(presentOther1.value),
        static_cast<unsigned long long>(Total(PerfAttributionPhase::VblankWaitCadence)),
        static_cast<unsigned long>(underflowMask));
#if TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED
    DrawPrioritySubprofileEmitWindow(
        gStage, gBaselineStageFrame, stageFrame, gDraws, gCadenceMode);
    DrawPrioritySubprofileEmitGeWindow(gStage, gBaselineStageFrame, stageFrame);
#endif
#if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED
    ItemUpdateSubprofileEmitWindow(gStage, gBaselineStageFrame, stageFrame);
#endif
#if TH08_PSP_ITEM_ATAN2_STATS_ENABLED
    ItemAtan2StatsEmitWindow(gStage, gBaselineStageFrame, stageFrame);
#endif
#if TH08_PSP_ITEM_SINCOS_STATS_ENABLED
    ItemSinCosStatsEmitWindow(gStage, gBaselineStageFrame, stageFrame);
#endif
#if TH08_PSP_SOFTFLOAT_CENSUS_ENABLED
    SoftfloatCensusEmitWindow(gStage, gBaselineStageFrame, stageFrame);
#endif
#if TH08_PSP_AUDIO_CURSOR_STATS_ENABLED
    AudioCursorStatsEmitWindow(gStage, gBaselineStageFrame, stageFrame);
#endif
#if TH08_PSP_TRIG_DF_STATS_ENABLED
    TrigDfStatsEmitWindow(gStage, gBaselineStageFrame, stageFrame);
#endif
#if TH08_PSP_PERF_ENV_ENABLED
    PerfEnvEmitWindow(gStage, gBaselineStageFrame, stageFrame, wallUs);
#endif
#if TH08_PSP_GUI_BORDER_STATS_ENABLED
    GuiBorderStatsEmitWindow(gStage, gBaselineStageFrame, stageFrame);
#endif
#if TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED
    EffectOccupancyStatsEmitWindow(gStage, gBaselineStageFrame, stageFrame);
#endif
#if TH08_PSP_BULLET_GATE_STATS_ENABLED
    BulletGateStatsEmitWindow(gStage, gBaselineStageFrame, stageFrame);
#endif
}

bool BeginWrappedWait(PerfAttributionPhase &phase, std::uint64_t &startUs)
{
    if (!gWindowActive || gWaitContext == PerfAttributionWaitContext::None ||
        sceKernelGetThreadId() != gMainThreadId)
        return false;

    if (gWaitContext == PerfAttributionWaitContext::Swap)
    {
        phase = PerfAttributionPhase::GeWaitSwap;
    }
    else
    {
        // sceGeListSync is not expected in the cadence-only context.  Ignore
        // it rather than mislabeling an unexpected call as display wait.
        return false;
    }
    startUs = MeasuredNow();
    return true;
}

#if TH08_PSP_PERF_ENV_ENABLED
// A sceGeListSync reached outside every wait context (PSPGL display-list
// rollover inside the calc/draw chains) is timed for the environment record
// only; it never enters the phase totals.
bool BeginUnscopedGeWait(std::uint64_t &startUs)
{
    if (!gWindowActive || gWaitContext != PerfAttributionWaitContext::None ||
        sceKernelGetThreadId() != gMainThreadId)
        return false;
    startUs = MeasuredNow();
    return true;
}
#endif
bool BeginWrappedVblank(PerfAttributionPhase &phase, std::uint64_t &startUs)
{
    if (!gWindowActive || gWaitContext == PerfAttributionWaitContext::None ||
        sceKernelGetThreadId() != gMainThreadId)
        return false;
    phase = gWaitContext == PerfAttributionWaitContext::Swap
                ? PerfAttributionPhase::VblankWaitSwap
                : PerfAttributionPhase::VblankWaitCadence;
    startUs = MeasuredNow();
    return true;
}
} // namespace

PerfAttributionScope::PerfAttributionScope(PerfAttributionPhase phase)
    : phase_(phase), startUs_(0U), active_(gWindowActive), previousPhase_(0U)
{
#if TH08_PSP_SOFTFLOAT_CENSUS_ENABLED
    previousPhase_ = gSoftfloatCurrentPhase;
    gSoftfloatCurrentPhase = static_cast<std::uint8_t>(phase);
#endif
#if TH08_PSP_PERF_ENV_ENABLED
    // Both observers track the same innermost phase, so sharing the saved
    // value is exact when the census is also on.
    previousPhase_ = gPerfEnvCurrentPhase;
    gPerfEnvCurrentPhase = static_cast<std::uint8_t>(phase);
#endif
    if (active_)
        startUs_ = MeasuredNow();
}

PerfAttributionScope::~PerfAttributionScope()
{
    if (active_)
        RecordDuration(phase_, startUs_, MeasuredNow());
#if TH08_PSP_SOFTFLOAT_CENSUS_ENABLED
    gSoftfloatCurrentPhase = previousPhase_;
#endif
#if TH08_PSP_PERF_ENV_ENABLED
    gPerfEnvCurrentPhase = previousPhase_;
#endif
}

PerfAttributionWaitContextScope::PerfAttributionWaitContextScope(
    PerfAttributionWaitContext context)
    : previous_(gWaitContext)
{
    gWaitContext = context;
}

PerfAttributionWaitContextScope::~PerfAttributionWaitContextScope()
{
    gWaitContext = previous_;
}

void PerfAttributionInitialize()
{
    if (gInitialized)
        return;

    gMainThreadId = sceKernelGetThreadId();
#if TH08_PSP_PERF_ENV_ENABLED
    PerfEnvInitialize(gMainThreadId);
#endif
    const std::uint64_t calibrationStart = RawNow();
    std::uint64_t calibrationSink = calibrationStart;
    for (std::uint32_t i = 0U; i < kTimerCalibrationReads; ++i)
        calibrationSink ^= RawNow();
    const std::uint64_t calibrationEnd = RawNow();
    gCalibrationUs = calibrationEnd >= calibrationStart
                         ? calibrationEnd - calibrationStart
                         : 0U;
    // Retain an observable dependency so LTO cannot delete calibration reads.
    if (calibrationSink == UINT64_MAX)
        ++gClockRegressionCount;

    gInitialized = true;
    BootLog(
        "PERF_ATTR_CONFIG V1 enabled=1 period_stage_frames=600 "
        "clock=sceKernelGetSystemTimeWide cal_reads=1024 cal_us=%llu "
        "runtime_telemetry_independent=1 log_flush_per_sample=0\n",
        static_cast<unsigned long long>(gCalibrationUs));
}

void PerfAttributionAfterPresent(std::int32_t stage,
                                 std::uint32_t stageFrame,
                                 std::uint8_t cadenceMode,
                                 bool replay,
                                 std::uint8_t demoReplay)
{
    if (!gInitialized)
        return;

    const std::uint64_t now = gWindowActive ? MeasuredNow() : RawNow();
    if (stageFrame == 0U)
    {
        gWindowActive = false;
#if TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED
        DrawPrioritySubprofileCancelWindow();
#endif
#if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED
        ItemUpdateSubprofileCancelWindow();
#endif
#if TH08_PSP_ITEM_ATAN2_STATS_ENABLED
        ItemAtan2StatsCancelWindow();
#endif
#if TH08_PSP_ITEM_SINCOS_STATS_ENABLED
        ItemSinCosStatsCancelWindow();
#endif
#if TH08_PSP_SOFTFLOAT_CENSUS_ENABLED
        SoftfloatCensusCancelWindow();
#endif
#if TH08_PSP_AUDIO_CURSOR_STATS_ENABLED
        AudioCursorStatsCancelWindow();
#endif
#if TH08_PSP_TRIG_DF_STATS_ENABLED
        TrigDfStatsCancelWindow();
#endif
#if TH08_PSP_PERF_ENV_ENABLED
        PerfEnvCancelWindow();
#endif
#if TH08_PSP_GUI_BORDER_STATS_ENABLED
        GuiBorderStatsCancelWindow();
#endif
#if TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED
        EffectOccupancyStatsCancelWindow();
#endif
#if TH08_PSP_BULLET_GATE_STATS_ENABLED
        BulletGateStatsCancelWindow();
#endif
        return;
    }

    if (!gWindowActive || stage != gStage || stageFrame <= gLastStageFrame ||
        cadenceMode != gCadenceMode || replay != gReplay ||
        demoReplay != gDemoReplay)
    {
        StartWindow(stage, stageFrame, cadenceMode, replay, demoReplay, now);
        return;
    }

    ++gDraws;
    gLastStageFrame = stageFrame;
    if (stageFrame < gTargetStageFrame)
        return;
    if (stageFrame > gTargetStageFrame)
    {
        StartWindow(stage, stageFrame, cadenceMode, replay, demoReplay, now);
        return;
    }

    // Stop before formatting/I/O so neither this record nor its lock/open/write
    // latency contaminates the next measurement interval.
    gWindowActive = false;
    EmitWindow(stageFrame, now);
    StartWindow(stage, stageFrame, cadenceMode, replay, demoReplay, RawNow());
}
} // namespace th08::psp

extern "C" int __wrap_sceGeListSync(int qid, int syncType)
{
    th08::psp::PerfAttributionPhase phase =
        th08::psp::PerfAttributionPhase::GeWaitSwap;
    std::uint64_t startUs = 0U;
    const bool measured = th08::psp::BeginWrappedWait(phase, startUs);
#if TH08_PSP_PERF_ENV_ENABLED
    const bool envMeasured =
        !measured && th08::psp::BeginUnscopedGeWait(startUs);
#endif
    const int result = __real_sceGeListSync(qid, syncType);
    if (measured)
    {
        const std::uint64_t endUs = th08::psp::MeasuredNow();
        th08::psp::RecordDuration(phase, startUs, endUs);
#if TH08_PSP_PERF_ENV_ENABLED
        if (phase == th08::psp::PerfAttributionPhase::GeWaitSwap)
            th08::psp::PerfEnvNoteGeSwapWait(endUs >= startUs ? endUs - startUs : 0U);
#endif
    }
#if TH08_PSP_PERF_ENV_ENABLED
    else if (envMeasured)
    {
        const std::uint64_t endUs = th08::psp::MeasuredNow();
        const std::uint64_t durationUs = endUs >= startUs ? endUs - startUs : 0U;
        th08::psp::PerfEnvNoteUnscopedGeWait(th08::psp::gPerfEnvCurrentPhase,
                                             durationUs);
#if TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED
        th08::psp::DrawPrioritySubprofileNoteGeWait(durationUs);
#endif
    }
#endif
    return result;
}

extern "C" int __wrap_sceDisplayWaitVblankStart(void)
{
    th08::psp::PerfAttributionPhase phase =
        th08::psp::PerfAttributionPhase::VblankWaitSwap;
    std::uint64_t startUs = 0U;
    const bool measured = th08::psp::BeginWrappedVblank(phase, startUs);
    const int result = __real_sceDisplayWaitVblankStart();
    if (measured)
        th08::psp::RecordDuration(phase, startUs,
                                  th08::psp::MeasuredNow());
    return result;
}

#endif // TH08_PSP_PERF_ATTRIBUTION_ENABLED
