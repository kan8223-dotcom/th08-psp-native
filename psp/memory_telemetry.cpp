#include "memory_telemetry.hpp"

#include "audio_telemetry.hpp"
#include "anm_scratch.hpp"
#include "ecl_child_memory.hpp"
#include "newlib_heap_geometry.hpp"
#include "radial_trail_telemetry.hpp"
#include "render_resource_arena.hpp"
#include "render_perf_telemetry.hpp"
#include "stage_pool_arena.hpp"
#if defined(TH08_PSP_X87_TRIG_CACHE) && TH08_PSP_X87_TRIG_CACHE
#include "x87_trig_cache.hpp"
#define TH08_PSP_X87_TRIG_CACHE_ENABLED 1
#else
#define TH08_PSP_X87_TRIG_CACHE_ENABLED 0
#endif

#include "BulletManager.hpp"
#include "AnmManager.hpp"
#include "EnemyManager.hpp"
#include "GameManager.hpp"
#include "Global.hpp"
#include "ItemManager.hpp"
#if (defined(TH08_PSP_SCORE_POPUP_NATIVE_GE) && \
     TH08_PSP_SCORE_POPUP_NATIVE_GE) || \
    (defined(TH08_PSP_BULLET_PACKED_VERTEX_AUDIT) && \
     TH08_PSP_BULLET_PACKED_VERTEX_AUDIT) || \
    (defined(TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH) && \
     TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH) || \
    (defined(TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) && \
     TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) || \
    (defined(TH08_PSP_ITEM_MIXED_QUADS_FASTPATH) && \
     TH08_PSP_ITEM_MIXED_QUADS_FASTPATH)
#include "modern/linux/d3d8_internal.hpp"
#endif

#include <malloc.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(TH08_PSP_ITEM_NATURAL_NATIVE_COPY) && \
    TH08_PSP_ITEM_NATURAL_NATIVE_COPY
#define TH08_PSP_ITEM_NATURAL_NATIVE_COPY_ENABLED 1
#else
#define TH08_PSP_ITEM_NATURAL_NATIVE_COPY_ENABLED 0
#endif

// Game headers preserve the original executable's `u32 == unsigned int`
// contract, while current PSPSDK headers typedef u32 through stdint as an
// unsigned long.  Mixing both typedef sets in one translation unit is an
// error, so keep this narrow ABI boundary instead of including PSPSDK here.
extern "C" std::uint64_t sceKernelGetSystemTimeWide(void);
extern "C" std::uint32_t sceKernelTotalFreeMemSize(void);
extern "C" std::uint32_t sceKernelMaxFreeMemSize(void);

namespace
{
constexpr std::uint32_t kSamplePeriodFrames = 300;
#if TH08_PSP_STAGE_RELATIVE_PERF_SAMPLING
constexpr std::uint32_t kMaximumStageFrame = static_cast<std::uint32_t>(-1);
#endif

#if TH08_PSP_ITEM_MIXED_QUADS_ENABLED
void ResetItemMixedQuadIntervalIfReady()
{
    if (th08::g_AnmManager != nullptr)
        th08::g_AnmManager->ResetPspItemMixedQuadStats();
}
#endif
#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
void ResetItemNaturalQuadInterval()
{
    th08::PspResetItemNaturalQuadStats();
}
#endif
#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED
void ResetEffectIndexedQuadInterval()
{
    th08::PspResetEffectIndexedQuadStats();
}
#endif
#if defined(TH08_PSP_BULLET_CANCEL_SPATIAL) && \
    TH08_PSP_BULLET_CANCEL_SPATIAL
void ResetBulletCancelSpatialInterval()
{
    th08::PspResetBulletCancelSpatialTelemetry();
}
#endif
constexpr std::size_t kLargeAllocationThreshold = 64U * 1024U;
constexpr std::size_t kAllocationEventCapacity = 128;

enum class AllocationEventKind : std::uint8_t
{
    Allocate,
    Free,
    Failure,
    Resize,
};

struct AllocationEvent
{
    std::uint32_t sequence;
    std::uint32_t frame;
    std::uintptr_t address;
    std::uint32_t requestedBytes;
    std::uint32_t usableBytes;
    AllocationEventKind kind;
    char owner[48];
};

char gTelemetryPath[640] = "ms0:/PSP/GAME/TH08PSP/TH08PSP_MEMORY.LOG";
volatile std::uint32_t gTrackedLiveBytes = 0;
volatile std::uint32_t gTrackedPeakBytes = 0;
volatile std::uint32_t gTrackedAllocationCount = 0;
volatile std::uint32_t gTrackedFailureCount = 0;
volatile std::uint32_t gGameFrame = 0;
volatile int gEventLock = 0;
volatile int gIoLock = 0;
AllocationEvent gAllocationEvents[kAllocationEventCapacity]{};
std::uint32_t gNextEventSequence = 0;
std::uint32_t gFirstUnflushedSequence = 0;
std::uint32_t gDroppedAllocationEvents = 0;

std::uint32_t gPeakMallinfoUsed = 0;
std::uint32_t gPeakEnemyCount = 0;
std::uint32_t gPeakBulletCount = 0;
std::uint32_t gPeakLaserCount = 0;
std::uint32_t gPeakItemCount = 0;
std::uint32_t gSampledPeakEnemyCount = 0;
std::uint32_t gSampledPeakBulletCount = 0;
std::uint32_t gSampledPeakLaserCount = 0;
std::uint32_t gSampledPeakItemCount = 0;
std::uint64_t gLastSampleTimeUs = 0;
std::uint32_t gLastSampleFrame = 0;
bool gInitialized = false;

#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
void ResetBulletMixedQuadIntervalIfReady()
{
    // Telemetry starts before WinMain constructs the original managers.  The
    // fixed sidecar is static-zeroed even then; once the manager exists use
    // its narrow Bullet-only reset so ItemManager's shared cache/stats are not
    // disturbed by measurement-window boundaries.
    if (th08::g_AnmManager != nullptr)
        th08::g_AnmManager->ResetPspBulletMixedQuadStats();
}
#endif

#if TH08_PSP_STAGE_RELATIVE_PERF_SAMPLING
void AppendLine(const char *format, ...);

struct StageRelativePerfSamplingState
{
    std::int32_t stage;
    std::uint32_t lastObservedStageFrame;
    std::uint32_t baselineStageFrame;
    std::uint32_t nextSampleStageFrame;
    bool stageObserved;
    bool baselineReady;
};

StageRelativePerfSamplingState gStageRelativePerfSampling{};

void RearmStageRelativePerfSampling(std::int32_t stage, std::uint32_t stageFrame)
{
    gStageRelativePerfSampling.stage = stage;
    gStageRelativePerfSampling.lastObservedStageFrame = stageFrame;
    gStageRelativePerfSampling.baselineStageFrame = 0;
    gStageRelativePerfSampling.nextSampleStageFrame = 0;
    gStageRelativePerfSampling.stageObserved = true;
    gStageRelativePerfSampling.baselineReady = false;
}

void BeginStageRelativePerfBaseline(std::int32_t stage, std::uint32_t stageFrame,
                                    const char *reason)
{
    // MemoryTelemetryAfterPresent runs only after EndFrame has closed the
    // current real Present.  Discard through that complete boundary frame;
    // the interval starting here therefore contains only the completed
    // Presents after stage frame N, never a partial current-frame carry.
    (void)th08::psp::RenderPerfTelemetryTakeInterval();
    th08::psp::RadialTrailTelemetryReset();
#if TH08_PSP_X87_TRIG_CACHE_ENABLED
    // Consume counters only.  The exact-value cache remains warm so choosing
    // a diagnostic sampling boundary cannot change workload timing.
    (void)th08::psp::X87TrigCacheTake();
#endif
    // Keep the Bullet enumerator interval on the identical stage-relative
    // boundary.  MARK snapshots only peek, so this is the sole pre-sample
    // discard and title/loading work cannot bleed into the first 300 frames.
    th08::PspResetBulletLiveEnumTelemetry();
#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT) && \
    TH08_PSP_BULLET_TRANSFORM_AUDIT
    th08::PspResetBulletTransformAuditTelemetry();
#endif
#if defined(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH) && \
    TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH
    th08::PspResetBulletTransformTerminalTelemetry();
#endif
#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT) && \
    TH08_PSP_BULLET_COLLISION_GATE_AUDIT
    th08::PspResetBulletCollisionGateAuditTelemetry();
#endif
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
    ResetBulletMixedQuadIntervalIfReady();
#endif
#if TH08_PSP_ITEM_MIXED_QUADS_ENABLED
    ResetItemMixedQuadIntervalIfReady();
#endif
#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
    ResetItemNaturalQuadInterval();
#endif
#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED
    ResetEffectIndexedQuadInterval();
#endif
#if defined(TH08_PSP_BULLET_CANCEL_SPATIAL) && \
    TH08_PSP_BULLET_CANCEL_SPATIAL
    ResetBulletCancelSpatialInterval();
#endif
    gStageRelativePerfSampling.stage = stage;
    gStageRelativePerfSampling.lastObservedStageFrame = stageFrame;
    gStageRelativePerfSampling.baselineStageFrame = stageFrame;
    gStageRelativePerfSampling.baselineReady = true;
    if (stageFrame <= kMaximumStageFrame - kSamplePeriodFrames)
        gStageRelativePerfSampling.nextSampleStageFrame =
            stageFrame + kSamplePeriodFrames;
    else
        gStageRelativePerfSampling.nextSampleStageFrame = 0;

    AppendLine(
        "STAGE_RELATIVE_PERF_BASELINE stage=%ld baseline_stage_frame=%lu "
        "next_sample_stage_frame=%lu game_frame=%lu reason=%s\n",
        static_cast<long>(stage),
        static_cast<unsigned long>(stageFrame),
        static_cast<unsigned long>(gStageRelativePerfSampling.nextSampleStageFrame),
        static_cast<unsigned long>(gGameFrame), reason);
    // Exclude the diagnostic baseline record itself from the measured window;
    // the next render counter belongs to the next real Present.
    gLastSampleTimeUs = sceKernelGetSystemTimeWide();
    gLastSampleFrame = gGameFrame;
}
#endif

bool TryLockEvents()
{
    return __sync_bool_compare_and_swap(&gEventLock, 0, 1);
}

void UnlockEvents()
{
    __sync_lock_release(&gEventLock);
}

void CopyOwner(char *destination, std::size_t destinationSize, const char *owner)
{
    if (destinationSize == 0)
        return;
    if (owner == nullptr)
        owner = "unknown";

    std::size_t cursor = 0;
    while (cursor + 1 < destinationSize && owner[cursor] != '\0')
    {
        const char ch = owner[cursor];
        destination[cursor] = (ch == '\r' || ch == '\n') ? '_' : ch;
        ++cursor;
    }
    destination[cursor] = '\0';
}

void RecordAllocationEvent(AllocationEventKind kind, void *memory, std::size_t requested,
                           std::size_t usable, const char *owner)
{
    // Allocation hooks run on the main, loader and audio threads.  Never spin
    // here: a higher-priority PSP thread could otherwise starve the lock owner
    // and deadlock the game.  Losing a diagnostic event is preferable and is
    // reported explicitly at the next flush.
    if (!TryLockEvents())
    {
        __sync_fetch_and_add(&gDroppedAllocationEvents, 1);
        return;
    }
    const std::uint32_t sequence = gNextEventSequence++;
    if (sequence - gFirstUnflushedSequence >= kAllocationEventCapacity)
    {
        gFirstUnflushedSequence = sequence - static_cast<std::uint32_t>(kAllocationEventCapacity) + 1;
        ++gDroppedAllocationEvents;
    }

    AllocationEvent &event = gAllocationEvents[sequence % kAllocationEventCapacity];
    event.sequence = sequence;
    event.frame = gGameFrame;
    event.address = reinterpret_cast<std::uintptr_t>(memory);
    event.requestedBytes = static_cast<std::uint32_t>(requested);
    event.usableBytes = static_cast<std::uint32_t>(usable);
    event.kind = kind;
    CopyOwner(event.owner, sizeof(event.owner), owner);
    UnlockEvents();
}

void UpdatePeak(volatile std::uint32_t *peak, std::uint32_t candidate)
{
    std::uint32_t observed = *peak;
    while (candidate > observed)
    {
        if (__sync_bool_compare_and_swap(peak, observed, candidate))
            return;
        observed = *peak;
    }
}

bool AppendText(const char *text, std::size_t length)
{
    if (!__sync_bool_compare_and_swap(&gIoLock, 0, 1))
        return false;
    std::FILE *file = std::fopen(gTelemetryPath, "ab");
    if (file == nullptr)
    {
        __sync_lock_release(&gIoLock);
        return false;
    }
    const std::size_t written = std::fwrite(text, 1, length, file);
    std::fclose(file);
    __sync_lock_release(&gIoLock);
    return written == length;
}

void AppendLine(const char *format, ...)
{
    // Snapshot lines include heap, arena and all pool counters. Keep each
    // record atomic and newline-terminated instead of concatenating truncated
    // fragments, which obscures the first failing phase.  The popup and Bullet
    // diagnostics take the complete snapshot slightly past 4 KiB, so retain
    // enough stack space for the current schema and still fail closed with a
    // newline if a later schema outgrows this buffer.
    char line[6144];
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (length <= 0)
        return;
    std::size_t boundedLength = static_cast<std::size_t>(length);
    if (boundedLength >= sizeof(line))
    {
        boundedLength = sizeof(line) - 1;
        line[boundedLength - 1] = '\n';
    }
    AppendText(line, boundedLength);
}

void FlushAllocationEvents()
{
    AllocationEvent local[kAllocationEventCapacity];
    std::uint32_t count = 0;
    std::uint32_t dropped = 0;

    if (!TryLockEvents())
        return;
    const std::uint32_t end = gNextEventSequence;
    std::uint32_t begin = gFirstUnflushedSequence;
    if (end - begin > kAllocationEventCapacity)
        begin = end - static_cast<std::uint32_t>(kAllocationEventCapacity);
    for (std::uint32_t sequence = begin; sequence < end; ++sequence)
        local[count++] = gAllocationEvents[sequence % kAllocationEventCapacity];
    gFirstUnflushedSequence = end;
    dropped = __sync_lock_test_and_set(&gDroppedAllocationEvents, 0);
    UnlockEvents();

    if (dropped != 0)
        AppendLine("ALLOC_EVENT_DROP count=%lu\n", static_cast<unsigned long>(dropped));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const AllocationEvent &event = local[i];
        const char *kind = event.kind == AllocationEventKind::Allocate
                               ? "ALLOC"
                               : (event.kind == AllocationEventKind::Free
                                      ? "FREE"
                                      : (event.kind == AllocationEventKind::Failure ? "FAIL"
                                                                                    : "RESIZE"));
        AppendLine("%s seq=%lu frame=%lu ptr=0x%08lx request=%lu usable=%lu owner=%s\n",
                   kind,
                   static_cast<unsigned long>(event.sequence),
                   static_cast<unsigned long>(event.frame),
                   static_cast<unsigned long>(event.address),
                   static_cast<unsigned long>(event.requestedBytes),
                   static_cast<unsigned long>(event.usableBytes),
                   event.owner);
    }
}

struct ExactPoolUsage
{
    std::uint32_t enemies;
    std::uint32_t enemyHighestSlot;
    std::uint32_t bullets;
    std::uint32_t bulletHighestSlot;
    std::uint32_t lasers;
    std::uint32_t laserHighestSlot;
    std::uint32_t itemArray;
    std::uint32_t itemHighestSlot;
    std::uint32_t itemList;
};

ExactPoolUsage ScanExactPoolUsage()
{
    ExactPoolUsage usage{};
    usage.enemyHighestSlot = usage.bulletHighestSlot = usage.laserHighestSlot =
        usage.itemHighestSlot = 0xffffffffU;

    if (!th08::psp::StagePoolArenaIsBound())
        return usage;

    for (std::size_t i = 0; i < 480; ++i)
    {
        if ((th08::g_EnemyManager.enemies[i].flags1 & th08::ENEMY_FLAG_ACTIVE) != 0)
        {
            ++usage.enemies;
            usage.enemyHighestSlot = static_cast<std::uint32_t>(i);
        }
    }
    for (std::size_t i = 0; i < 0x600; ++i)
    {
        if (th08::g_BulletManager.bullets[i].state != th08::BULLET_STATE_UNUSED)
        {
            ++usage.bullets;
            usage.bulletHighestSlot = static_cast<std::uint32_t>(i);
        }
    }
    for (std::size_t i = 0; i < 0x100; ++i)
    {
        if (th08::g_BulletManager.lasers[i].inUse != 0)
        {
            ++usage.lasers;
            usage.laserHighestSlot = static_cast<std::uint32_t>(i);
        }
    }
    for (std::size_t i = 0; i < MAX_ITEMS; ++i)
    {
        if (th08::g_ItemManager.items[i].isInUse != 0)
        {
            ++usage.itemArray;
            usage.itemHighestSlot = static_cast<std::uint32_t>(i);
        }
    }
    for (th08::Item *item = th08::g_ItemManager.itemListHead.next;
         item != nullptr && usage.itemList <= MAX_ITEMS; item = item->next)
    {
        ++usage.itemList;
    }
    return usage;
}

void UpdatePoolHighWater()
{
    if (!th08::psp::StagePoolArenaIsBound())
        return;

    const std::uint32_t enemies = th08::g_EnemyManager.activeEnemyCount > 0
                                      ? static_cast<std::uint32_t>(th08::g_EnemyManager.activeEnemyCount)
                                      : 0;
    const std::uint32_t bullets = th08::g_BulletManager.activeBulletCount > 0
                                      ? static_cast<std::uint32_t>(th08::g_BulletManager.activeBulletCount)
                                      : 0;
    const std::uint32_t items = th08::g_ItemManager.itemCount;
    UpdatePeak(&gPeakEnemyCount, enemies);
    UpdatePeak(&gPeakBulletCount, bullets);
    UpdatePeak(&gPeakItemCount, items);
}

void LogSnapshot(const char *kind, const char *phase, std::uint32_t elapsedFrames,
                 std::uint64_t elapsedUs,
                 const th08::psp::RenderPerfIntervalSnapshot *renderPerf)
{
    // This capture is allocator read-only and releases the malloc lock before
    // any arena lock or output.  AppendText's fopen/fwrite/fclose path may
    // itself allocate in newlib; that is pre-existing telemetry-sink debt and
    // is intentionally not hidden by the geometry numbers captured here.
    const th08::psp::NewlibHeapGeometrySnapshot heap =
        th08::psp::CaptureNewlibHeapGeometry();
    const std::uint32_t heapUsed = heap.usedBytes;
    if (heap.scanValid != 0U && heapUsed > gPeakMallinfoUsed)
        gPeakMallinfoUsed = heapUsed;

    const ExactPoolUsage exact = ScanExactPoolUsage();
    UpdatePeak(&gPeakLaserCount, exact.lasers);
    UpdatePeak(&gSampledPeakEnemyCount, exact.enemies);
    UpdatePeak(&gSampledPeakBulletCount, exact.bullets);
    UpdatePeak(&gSampledPeakLaserCount, exact.lasers);
    UpdatePeak(&gSampledPeakItemCount, exact.itemArray);

    std::uint64_t fpsMilli = 0;
    if (elapsedFrames != 0 && elapsedUs != 0)
        fpsMilli = static_cast<std::uint64_t>(elapsedFrames) * 1000000000ULL / elapsedUs;

    const bool stagePoolBound = th08::psp::StagePoolArenaIsBound();
    const th08::psp::RenderResourceArenaSnapshot renderArena =
        th08::psp::CaptureRenderResourceArenaSnapshot();
    const th08::psp::EnemyChildEclMemorySnapshot eclChild =
        th08::psp::CaptureEnemyChildEclMemorySnapshot();
    const i32 enemyReported = stagePoolBound ? th08::g_EnemyManager.activeEnemyCount : 0;
    const i32 bulletReported = stagePoolBound ? th08::g_BulletManager.activeBulletCount : 0;
    const u32 itemReported = stagePoolBound ? th08::g_ItemManager.itemCount : 0U;
    const th08::psp::RenderPerfIntervalSnapshot emptyRenderPerf{};
    const th08::psp::RenderPerfIntervalSnapshot &render =
        renderPerf != nullptr ? *renderPerf : emptyRenderPerf;
    const th08::psp::AudioTelemetrySnapshot audio =
        th08::psp::CaptureAudioTelemetrySnapshot();
    const th08::psp::RadialTrailTelemetrySnapshot radialTrail =
        renderPerf != nullptr ? th08::psp::RadialTrailTelemetryTake()
                              : th08::psp::RadialTrailTelemetryPeek();
    // A nullptr render interval identifies an observational MARK.  Match the
    // render-counter policy: MARK peeks and only a real SAMPLE consumes the
    // interval, so unrelated phase logging cannot split an A/B window.
    const th08::PspBulletLiveEnumTelemetrySnapshot bulletLiveEnum =
        renderPerf != nullptr ? th08::PspTakeBulletLiveEnumTelemetry()
                              : th08::PspPeekBulletLiveEnumTelemetry();
#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT) && \
    TH08_PSP_BULLET_TRANSFORM_AUDIT
    const th08::PspBulletTransformAuditTelemetrySnapshot bulletTransformAudit =
        renderPerf != nullptr ? th08::PspTakeBulletTransformAuditTelemetry()
                              : th08::PspPeekBulletTransformAuditTelemetry();
#endif
#if defined(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH) && \
    TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH
    const th08::PspBulletTransformTerminalTelemetrySnapshot
        bulletTransformTerminal =
            renderPerf != nullptr
                ? th08::PspTakeBulletTransformTerminalTelemetry()
                : th08::PspPeekBulletTransformTerminalTelemetry();
#endif
#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT) && \
    TH08_PSP_BULLET_COLLISION_GATE_AUDIT
    const th08::PspBulletCollisionGateAuditTelemetrySnapshot
        bulletCollisionGateAudit =
            renderPerf != nullptr
                ? th08::PspTakeBulletCollisionGateAuditTelemetry()
                : th08::PspPeekBulletCollisionGateAuditTelemetry();
#endif

#if defined(TH08_PSP_SCORE_POPUP_NATIVE_GE) && \
    TH08_PSP_SCORE_POPUP_NATIVE_GE
    // Emit and close this bounded record before the large general snapshot.
    // A harness waiting for the corresponding stage-teardown MARK therefore
    // cannot stop PPSSPP before the native-submit evidence reaches storage.
    PspScorePopupNativeGeStats scorePopupNativeGe{};
    const bool scorePopupNativeGeValid =
        th08_psp_query_score_popup_native_ge_stats(
            th08::g_Supervisor.d3dDevice, &scorePopupNativeGe);
    AppendLine(
        "SCORE_POPUP_NATIVE_GE_TELEMETRY kind=%s phase=%s frame=%lu "
        "stage=%ld stage_frame=%lu valid=%d counter_scope=device_lifetime "
        "cumulative=1 counter_bits=32_wrap attempts=%lu submits=%lu "
        "client_fallbacks=%lu\n",
        kind, phase != nullptr ? phase : "periodic",
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        scorePopupNativeGeValid ? 1 : 0,
        scorePopupNativeGe.attempts, scorePopupNativeGe.submits,
        scorePopupNativeGe.clientFallbacks);
#endif

#if defined(TH08_PSP_BULLET_PACKED_VERTEX_AUDIT) && \
    TH08_PSP_BULLET_PACKED_VERTEX_AUDIT
    PspBulletPackedVertexAuditStats bulletPackedVertexAudit{};
    const bool bulletPackedVertexAuditValid =
        th08_psp_query_bullet_packed_vertex_audit_stats(
            th08::g_Supervisor.d3dDevice, &bulletPackedVertexAudit);
    AppendLine(
        "BULLET_PACKED_VERTEX_AUDIT_TELEMETRY kind=%s phase=%s frame=%lu "
        "stage=%ld stage_frame=%lu valid=%d counter_scope=device_lifetime "
        "cumulative=1 counter_bits=32_wrap attempts=%lu attempted_quads=%lu "
        "eligible_batches=%lu eligible_quads=%lu compared_quads=%lu "
        "matched_quads=%lu mismatch_quads=%lu compared_vertices=%lu "
        "mismatch_vertices=%lu mismatch_bytes=%lu "
        "uniform_diffuse_quads=%lu per_vertex_diffuse_quads=%lu "
        "canonical_fallbacks=%lu owner_fallbacks=%lu "
        "state_fallbacks=%lu index_fallbacks=%lu "
        "capacity_fallbacks=%lu submit_fallbacks=%lu "
        "canonical_native_submits=%lu canonical_native_submitted_quads=%lu "
        "mismatch_u_bytes=%lu mismatch_v_bytes=%lu "
        "mismatch_color_bytes=%lu mismatch_x_bytes=%lu "
        "mismatch_y_bytes=%lu mismatch_z_bytes=%lu "
        "mismatch_other_bytes=%lu first_mismatch_valid=%lu "
        "first_mismatch_batch=%lu first_mismatch_quad=%lu "
        "first_mismatch_vertex=%lu first_mismatch_byte=%lu "
        "candidate_submits=0 canonical_authority=1 product_enabled=0\n",
        kind, phase != nullptr ? phase : "periodic",
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        bulletPackedVertexAuditValid ? 1 : 0,
        bulletPackedVertexAudit.attempts,
        bulletPackedVertexAudit.attemptedQuads,
        bulletPackedVertexAudit.eligibleBatches,
        bulletPackedVertexAudit.eligibleQuads,
        bulletPackedVertexAudit.comparedQuads,
        bulletPackedVertexAudit.matchedQuads,
        bulletPackedVertexAudit.mismatchQuads,
        bulletPackedVertexAudit.comparedVertices,
        bulletPackedVertexAudit.mismatchVertices,
        bulletPackedVertexAudit.mismatchBytes,
        bulletPackedVertexAudit.uniformDiffuseQuads,
        bulletPackedVertexAudit.perVertexDiffuseQuads,
        bulletPackedVertexAudit.canonicalFallbacks,
        bulletPackedVertexAudit.ownerFallbacks,
        bulletPackedVertexAudit.stateFallbacks,
        bulletPackedVertexAudit.indexFallbacks,
        bulletPackedVertexAudit.capacityFallbacks,
        bulletPackedVertexAudit.submitFallbacks,
        bulletPackedVertexAudit.canonicalNativeSubmits,
        bulletPackedVertexAudit.canonicalNativeSubmittedQuads,
        bulletPackedVertexAudit.mismatchUBytes,
        bulletPackedVertexAudit.mismatchVBytes,
        bulletPackedVertexAudit.mismatchColorBytes,
        bulletPackedVertexAudit.mismatchXBytes,
        bulletPackedVertexAudit.mismatchYBytes,
        bulletPackedVertexAudit.mismatchZBytes,
        bulletPackedVertexAudit.mismatchOtherBytes,
        bulletPackedVertexAudit.firstMismatchValid,
        bulletPackedVertexAudit.firstMismatchBatch,
        bulletPackedVertexAudit.firstMismatchQuad,
        bulletPackedVertexAudit.firstMismatchVertex,
        bulletPackedVertexAudit.firstMismatchByte);
#endif

#if defined(TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH) && \
    TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH
    PspBulletPackedVertexFastpathStats bulletPackedVertexFastpath{};
    const bool bulletPackedVertexFastpathValid =
        th08_psp_query_bullet_packed_vertex_fastpath_stats(
            th08::g_Supervisor.d3dDevice,
            &bulletPackedVertexFastpath);
    AppendLine(
        "BULLET_PACKED_VERTEX_FASTPATH_TELEMETRY kind=%s phase=%s "
        "frame=%lu stage=%ld stage_frame=%lu valid=%d "
        "counter_scope=device_lifetime cumulative=1 counter_bits=32_wrap "
        "begin_attempts=%lu accepted_batches=%lu "
        "canonical_fallback_batches=%lu append_attempts=%lu "
        "appended_quads=%lu packed_vertices=%lu "
        "uniform_diffuse_quads=%lu per_vertex_diffuse_quads=%lu "
        "submit_attempts=%lu submitted_runs=%lu submitted_quads=%lu "
        "native_submits=%lu native_submitted_quads=%lu "
        "client_fallback_submits=%lu client_fallback_quads=%lu "
        "owner_fallbacks=%lu state_fallbacks=%lu index_fallbacks=%lu "
        "capacity_fallbacks=%lu contract_fallbacks=%lu "
        "abandoned_runs=%lu abandoned_quads=%lu "
        "recovery_split_runs=%lu recovery_split_quads=%lu "
        "max_run_quads=%lu "
        "arena_high_water_vertices=%lu arena_capacity_vertices=%lu "
        "frontend_28b_bytes_avoided=%lu packed_24b_bytes=%lu "
        "manager_28B_staging_writes=0 packed_generation_passes=1 "
        "extra_flush=0 topology=4v_indexed\n",
        kind, phase != nullptr ? phase : "periodic",
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        bulletPackedVertexFastpathValid ? 1 : 0,
        bulletPackedVertexFastpath.beginAttempts,
        bulletPackedVertexFastpath.acceptedBatches,
        bulletPackedVertexFastpath.canonicalFallbackBatches,
        bulletPackedVertexFastpath.appendAttempts,
        bulletPackedVertexFastpath.appendedQuads,
        bulletPackedVertexFastpath.packedVertices,
        bulletPackedVertexFastpath.uniformDiffuseQuads,
        bulletPackedVertexFastpath.perVertexDiffuseQuads,
        bulletPackedVertexFastpath.submitAttempts,
        bulletPackedVertexFastpath.submittedRuns,
        bulletPackedVertexFastpath.submittedQuads,
        bulletPackedVertexFastpath.nativeSubmits,
        bulletPackedVertexFastpath.nativeSubmittedQuads,
        bulletPackedVertexFastpath.clientFallbackSubmits,
        bulletPackedVertexFastpath.clientFallbackQuads,
        bulletPackedVertexFastpath.ownerFallbacks,
        bulletPackedVertexFastpath.stateFallbacks,
        bulletPackedVertexFastpath.indexFallbacks,
        bulletPackedVertexFastpath.capacityFallbacks,
        bulletPackedVertexFastpath.contractFallbacks,
        bulletPackedVertexFastpath.abandonedRuns,
        bulletPackedVertexFastpath.abandonedQuads,
        bulletPackedVertexFastpath.recoverySplitRuns,
        bulletPackedVertexFastpath.recoverySplitQuads,
        bulletPackedVertexFastpath.maxRunQuads,
        bulletPackedVertexFastpath.arenaHighWaterVertices,
        bulletPackedVertexFastpath.arenaCapacityVertices,
        bulletPackedVertexFastpath.appendedQuads * 112UL,
        bulletPackedVertexFastpath.packedVertices * 24UL);
#endif

#if (defined(TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) && \
     TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) || \
    (defined(TH08_PSP_ITEM_MIXED_QUADS_FASTPATH) && \
     TH08_PSP_ITEM_MIXED_QUADS_FASTPATH)
    PspMixedGeBackendStats mixedGeBackend{};
    const bool mixedGeBackendValid =
        th08_psp_query_mixed_ge_backend_stats(
            th08::g_Supervisor.d3dDevice, &mixedGeBackend);
    AppendLine(
        "MIXED_GE_BACKEND_TELEMETRY kind=%s phase=%s frame=%lu "
        "stage=%ld stage_frame=%lu valid=%d counter_scope=device_lifetime "
        "cumulative=1 counter_bits=32_wrap bullet_attempts=%lu "
        "bullet_submitted_batches=%lu bullet_submitted_quads=%lu "
        "bullet_fallbacks=%lu bullet_arena_exhaustions=%lu "
        "item_attempts=%lu item_submitted_batches=%lu "
        "item_submitted_quads=%lu item_fallbacks=%lu "
        "item_arena_exhaustions=%lu shared_arena_high_water_vertices=%lu "
        "shared_arena_capacity_vertices=%lu\n",
        kind, phase != nullptr ? phase : "periodic",
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        mixedGeBackendValid ? 1 : 0,
        mixedGeBackend.bulletAttempts,
        mixedGeBackend.bulletSubmittedBatches,
        mixedGeBackend.bulletSubmittedQuads,
        mixedGeBackend.bulletFallbacks,
        mixedGeBackend.bulletArenaExhaustions,
        mixedGeBackend.itemAttempts,
        mixedGeBackend.itemSubmittedBatches,
        mixedGeBackend.itemSubmittedQuads,
        mixedGeBackend.itemFallbacks,
        mixedGeBackend.itemArenaExhaustions,
        mixedGeBackend.sharedArenaHighWaterVertices,
        mixedGeBackend.sharedArenaCapacityVertices);
#endif

#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
    th08::PspItemNaturalQuadStats itemNaturalQuads{};
    th08::PspQueryItemNaturalQuadStats(&itemNaturalQuads);
    AppendLine(
        "ITEM_NATURAL_QUADS_TELEMETRY kind=%s phase=%s frame=%lu "
        "stage=%ld stage_frame=%lu valid=1 mode=product "
        "counter_scope=stage_relative_interval cumulative=0 "
        "existing_flush=1 begin_end_added=0 topology=6v_to_4v_indexed "
        "passes=%lu canonical_batches=%lu item_time_candidates=%lu "
        "visible_item_time=%lu culled_item_time=%lu "
        "trigger_batches=%lu trigger_quads=%lu coalesced_quads=%lu "
        "eligible_quads=%lu submitted_batches=%lu submitted_quads=%lu "
        "native_submits=%lu native_submitted_quads=%lu "
        "client_fallback_submits=%lu client_fallback_quads=%lu "
        "canonical_input_vertices=%lu packed_output_vertices=%lu "
        "duplicate_vertices_avoided=%lu fallback_batches=%lu "
        "pointer_fallbacks=%lu span_fallbacks=%lu capacity_fallbacks=%lu "
        "topology_fallbacks=%lu state_fallbacks=%lu "
        "extra_topology_batches=%lu index_fallbacks=%lu "
        "native_fallbacks=%lu topology_checks=%lu "
        "topology_checked_quads=%lu extra_split_batches=%lu "
        "extra_flushes=%lu abandoned_batches=%lu abandoned_quads=%lu "
        "max_batch_quads=%lu marked_batches=%lu marked_quads=%lu "
        "frontend_bytes_saved=0 backend_output_bytes_saved=%lu "
        "native_no_copy_attempted=0 native_copy_enabled=%d "
        "native_copy_owned_same_call=%d client_owned_same_call=1\n",
        kind, phase != nullptr ? phase : "periodic",
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        static_cast<unsigned long>(itemNaturalQuads.passes),
        static_cast<unsigned long>(itemNaturalQuads.canonicalBatches),
        static_cast<unsigned long>(itemNaturalQuads.itemTimeCandidates),
        static_cast<unsigned long>(itemNaturalQuads.visibleItemTime),
        static_cast<unsigned long>(itemNaturalQuads.culledItemTime),
        static_cast<unsigned long>(itemNaturalQuads.triggerBatches),
        static_cast<unsigned long>(itemNaturalQuads.triggerQuads),
        static_cast<unsigned long>(itemNaturalQuads.coalescedQuads),
        static_cast<unsigned long>(itemNaturalQuads.eligibleQuads),
        static_cast<unsigned long>(itemNaturalQuads.submittedBatches),
        static_cast<unsigned long>(itemNaturalQuads.submittedQuads),
        static_cast<unsigned long>(itemNaturalQuads.nativeSubmits),
        static_cast<unsigned long>(itemNaturalQuads.nativeSubmittedQuads),
        static_cast<unsigned long>(itemNaturalQuads.clientFallbackSubmits),
        static_cast<unsigned long>(itemNaturalQuads.clientFallbackQuads),
        static_cast<unsigned long>(itemNaturalQuads.canonicalInputVertices),
        static_cast<unsigned long>(itemNaturalQuads.packedOutputVertices),
        static_cast<unsigned long>(itemNaturalQuads.duplicateVerticesAvoided),
        static_cast<unsigned long>(itemNaturalQuads.fallbackBatches),
        static_cast<unsigned long>(itemNaturalQuads.pointerFallbacks),
        static_cast<unsigned long>(itemNaturalQuads.spanFallbacks),
        static_cast<unsigned long>(itemNaturalQuads.capacityFallbacks),
        static_cast<unsigned long>(itemNaturalQuads.topologyFallbacks),
        static_cast<unsigned long>(itemNaturalQuads.stateFallbacks),
        static_cast<unsigned long>(itemNaturalQuads.extraTopologyBatches),
        static_cast<unsigned long>(itemNaturalQuads.indexFallbacks),
        static_cast<unsigned long>(itemNaturalQuads.nativeFallbacks),
        static_cast<unsigned long>(itemNaturalQuads.topologyChecks),
        static_cast<unsigned long>(itemNaturalQuads.topologyCheckedQuads),
        static_cast<unsigned long>(itemNaturalQuads.extraSplitBatches),
        static_cast<unsigned long>(itemNaturalQuads.extraFlushes),
        static_cast<unsigned long>(itemNaturalQuads.abandonedBatches),
        static_cast<unsigned long>(itemNaturalQuads.abandonedQuads),
        static_cast<unsigned long>(itemNaturalQuads.maxBatchQuads),
        static_cast<unsigned long>(itemNaturalQuads.triggerBatches),
        static_cast<unsigned long>(itemNaturalQuads.triggerQuads),
        static_cast<unsigned long>(itemNaturalQuads.eligibleQuads) * 48UL,
        TH08_PSP_ITEM_NATURAL_NATIVE_COPY_ENABLED,
        TH08_PSP_ITEM_NATURAL_NATIVE_COPY_ENABLED);
#endif

#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED
    // Effect counters are render-thread owned and deliberately have no peek
    // API.  An asynchronous MARK therefore cannot race them or split the
    // fixed 300-frame A/B interval.
    if (renderPerf != nullptr)
    {
        th08::PspEffectIndexedQuadStats effectIndexedQuads{};
        th08::PspQueryEffectIndexedQuadStats(&effectIndexedQuads);
        const u32 successfulQuads =
            effectIndexedQuads.successfulOrdinaryQuads;
        AppendLine(
            "EFFECT_INDEXED_QUADS_TELEMETRY kind=%s phase=%s frame=%lu "
            "stage=%ld stage_frame=%lu valid=1 mode=product "
#if TH08_PSP_STAGE_RELATIVE_PERF_SAMPLING
            "counter_scope=stage_relative_interval "
#else
            "counter_scope=periodic_interval "
#endif
            "cumulative=0 interval_consumed=1 owner=game_frame_thread "
            "marks_peek=0 ordinary_effect_only=1 radial_trail_excluded=1 "
            "topology=6v_to_4v_indexed render_perf_vertices=logical_6v "
            "vertex_stride_bytes=%lu "
            "passes=%lu flushes=%lu batches=%lu "
            "successful_ordinary_quads=%lu canonical_input_vertices=%lu "
            "indexed_output_vertices=%lu vertices_saved=%lu "
            "bytes_saved=%lu fallbacks=%lu fallback_quads=%lu "
            "owner_conflicts=%lu abandoned_passes=%lu abandoned_quads=%lu "
            "max_batch_quads=%lu\n",
            kind, phase != nullptr ? phase : "periodic",
            static_cast<unsigned long>(gGameFrame),
            static_cast<long>(th08::g_GameManager.currentStage),
            static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
            static_cast<unsigned long>(sizeof(th08::VertexTex1DiffuseXyzrhw)),
            static_cast<unsigned long>(effectIndexedQuads.passes),
            static_cast<unsigned long>(effectIndexedQuads.flushes),
            static_cast<unsigned long>(effectIndexedQuads.batches),
            static_cast<unsigned long>(successfulQuads),
            static_cast<unsigned long>(successfulQuads * 6U),
            static_cast<unsigned long>(successfulQuads * 4U),
            static_cast<unsigned long>(effectIndexedQuads.verticesSaved),
            static_cast<unsigned long>(effectIndexedQuads.bytesSaved),
            static_cast<unsigned long>(effectIndexedQuads.fallbacks),
            static_cast<unsigned long>(effectIndexedQuads.fallbackQuads),
            static_cast<unsigned long>(effectIndexedQuads.ownerConflicts),
            static_cast<unsigned long>(effectIndexedQuads.abandonedPasses),
            static_cast<unsigned long>(effectIndexedQuads.abandonedQuads),
            static_cast<unsigned long>(effectIndexedQuads.maxBatchQuads));
    }
#endif

#if defined(TH08_PSP_BULLET_CANCEL_SPATIAL) && \
    TH08_PSP_BULLET_CANCEL_SPATIAL
    if (renderPerf != nullptr)
    {
        const th08::PspBulletCancelSpatialTelemetrySnapshot cancelSpatial =
            th08::PspPeekBulletCancelSpatialTelemetry();
        AppendLine(
            "BULLET_CANCEL_SPATIAL_TELEMETRY kind=%s phase=%s frame=%lu "
            "stage=%ld stage_frame=%lu valid=1 mode=product "
#if TH08_PSP_STAGE_RELATIVE_PERF_SAMPLING
            "counter_scope=stage_relative_interval "
#else
            "counter_scope=periodic_interval "
#endif
            "cumulative=0 interval_consumed=1 owner=game_frame_thread "
            "marks_peek=0 conservative_coverage=1 canonical_slot_order=1 "
            "calls=%llu indexed_queries=%llu rejected_queries=%llu "
            "fallbacks=%llu rebuilds=%llu circles=%llu rects=%llu "
            "full_candidates=%llu indexed_candidates=%llu "
            "fallback_candidates=%llu exact_tests=%llu "
            "false_positives=%llu occupancy_owner_fallbacks=%llu "
            "unsupported_region_fallbacks=%llu nonfinite_fallbacks=%llu "
            "duplicate_pairs=%llu duplicate_replays=%llu "
            "duplicate_exact_tests_saved=%llu duplicate_fallbacks=%llu\n",
            kind, phase != nullptr ? phase : "periodic",
            static_cast<unsigned long>(gGameFrame),
            static_cast<long>(th08::g_GameManager.currentStage),
            static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
            cancelSpatial.calls,
            cancelSpatial.indexedQueries,
            cancelSpatial.rejectedQueries,
            cancelSpatial.fallbacks,
            cancelSpatial.rebuilds,
            cancelSpatial.circles,
            cancelSpatial.rects,
            cancelSpatial.fullCandidates,
            cancelSpatial.indexedCandidates,
            cancelSpatial.fallbackCandidates,
            cancelSpatial.exactTests,
            cancelSpatial.falsePositives,
            cancelSpatial.occupancyOwnerFallbacks,
            cancelSpatial.unsupportedRegionFallbacks,
            cancelSpatial.nonfiniteFallbacks,
            cancelSpatial.duplicatePairs,
            cancelSpatial.duplicateReplays,
            cancelSpatial.duplicateExactTestsSaved,
            cancelSpatial.duplicateFallbacks);
    }
#endif

#if TH08_PSP_X87_TRIG_CACHE_ENABLED
    // Game-frame SAMPLE runs on the only owner of the gameplay trig cache.
    // MARK may run on the asynchronous setup thread, so it must not even peek
    // at the unsynchronised hot counters/entries.
    if (renderPerf != nullptr)
    {
        const th08::psp::X87TrigCacheStats x87TrigCache =
            th08::psp::X87TrigCacheTake();
        const std::uint32_t x87Requests =
            x87TrigCache.sinRequests + x87TrigCache.cosRequests;
        const std::uint32_t x87Hits =
            x87TrigCache.sinHits + x87TrigCache.cosHits;
        AppendLine(
            "X87_TRIG_CACHE_TELEMETRY kind=%s phase=%s frame=%lu stage=%ld "
            "stage_frame=%lu interval_consumed=1 exact_binary64=1 "
            "owner=game_frame_thread marks_peek=0 counter_bits=32 "
            "requests=%lu hits=%lu misses=%lu "
            "sin_requests=%lu sin_hits=%lu cos_requests=%lu cos_hits=%lu "
            "entry_replacements=%lu nonfinite_fallbacks=%lu entries=%lu "
            "storage_bytes=%lu\n",
            kind, phase != nullptr ? phase : "periodic",
            static_cast<unsigned long>(gGameFrame),
            static_cast<long>(th08::g_GameManager.currentStage),
            static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
            static_cast<unsigned long>(x87Requests),
            static_cast<unsigned long>(x87Hits),
            static_cast<unsigned long>(x87Requests - x87Hits),
            static_cast<unsigned long>(x87TrigCache.sinRequests),
            static_cast<unsigned long>(x87TrigCache.sinHits),
            static_cast<unsigned long>(x87TrigCache.cosRequests),
            static_cast<unsigned long>(x87TrigCache.cosHits),
            static_cast<unsigned long>(x87TrigCache.entryReplacements),
            static_cast<unsigned long>(x87TrigCache.nonFiniteFallbacks),
            static_cast<unsigned long>(th08::psp::kX87TrigCacheEntryCount),
            static_cast<unsigned long>(th08::psp::X87TrigCacheStorageBytes()));
    }
#endif

    AppendLine(
        "RADIAL_TRAIL_TELEMETRY kind=%s phase=%s frame=%lu stage=%ld "
        "stage_frame=%lu interval_consumed=%d draws=%llu vertices=%llu "
        "dirty_rebuilds=%llu zero_secondary=%llu ellipse=%llu wavy=%llu "
        "angular_samples=%llu rebuilt_vertices=%llu reusable_pairs=%llu "
        "reused_pairs=%llu trig_evaluations_avoided=%llu "
        "peak_segments=%lu peak_vertices=%lu\n",
        kind, phase != nullptr ? phase : "periodic",
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        renderPerf != nullptr ? 1 : 0,
        static_cast<unsigned long long>(radialTrail.drawCalls),
        static_cast<unsigned long long>(radialTrail.submittedVertices),
        static_cast<unsigned long long>(radialTrail.dirtyRebuilds),
        static_cast<unsigned long long>(radialTrail.zeroSecondaryRebuilds),
        static_cast<unsigned long long>(radialTrail.ellipseRebuilds),
        static_cast<unsigned long long>(radialTrail.wavyRebuilds),
        static_cast<unsigned long long>(radialTrail.angularSamples),
        static_cast<unsigned long long>(radialTrail.rebuiltVertices),
        static_cast<unsigned long long>(radialTrail.reusableTrigPairs),
        static_cast<unsigned long long>(radialTrail.reusedTrigPairs),
        static_cast<unsigned long long>(radialTrail.trigEvaluationsAvoided),
        static_cast<unsigned long>(radialTrail.peakSegments),
        static_cast<unsigned long>(radialTrail.peakSubmittedVertices));

    AppendLine(
        "%s phase=%s frame=%lu stage=%ld stage_frame=%lu replay=%d demo=%d "
        "fps=%llu.%03llu heap_arena=%lu heap_used=%lu heap_used_peak=%lu heap_free=%lu "
        "heap_top=%lu heap_chunks=%lu heap_largest_free=%lu heap_largest_request_nogrow=%lu "
        "heap_geometry_valid=%lu heap_geometry_errors=%lu heap_geometry_error_flags=0x%08lx "
        "kernel_free=%lu kernel_largest=%lu "
        "tracked_live=%lu tracked_peak=%lu tracked_allocs=%lu tracked_failures=%lu "
        "render_arena_capacity=%lu render_arena_live=%lu render_arena_peak=%lu "
        "render_arena_free=%lu render_arena_largest=%lu render_arena_allocs=%lu "
        "render_arena_failures=%lu render_arena_live_allocs=%lu "
        "render_arena_quarantines=%lu render_arena_scope_contentions=%lu "
        "render_arena_poisoned=%lu "
        "ecl_child_block_bytes=%lu ecl_child_live=%lu ecl_child_peak=%lu "
        "ecl_child_live_bytes=%lu ecl_child_peak_bytes=%lu "
        "ecl_child_arena_live=%lu ecl_child_arena_peak=%lu "
        "ecl_child_heap_live=%lu ecl_child_heap_peak=%lu "
        "ecl_child_arena_allocs=%lu ecl_child_arena_misses=%lu "
        "ecl_child_heap_fallbacks=%lu ecl_child_alloc_failures=%lu "
        "ecl_child_free_failures=%lu "
        "anm_scratch_capacity=%lu anm_scratch_active=%lu anm_scratch_busy=%d "
        "anm_scratch_poisoned=%d anm_scratch_generation=%lu anm_scratch_owner=%d "
        "stage_pool_allocated=%d stage_pool_bound=%d stage_pool_guards=%s "
        "stage_pool_generation=%lu stage_pool_base=0x%08lx stage_pool_reserved=%lu stage_pool_payload=%lu "
        "stage_pool_transient_active=%lu stage_pool_transient_peak=%lu "
        "stage_pool_transient_loans=%lu stage_pool_transient_failures=%lu "
        "stage_pool_transient_quarantines=%lu "
        "enemy_reported=%ld/%u enemy_reported_peak=%lu enemy_exact=%lu enemy_exact_sampled_peak=%lu enemy_hi=%ld "
        "bullet_reported=%ld/%u bullet_reported_peak=%lu bullet_exact=%lu bullet_exact_sampled_peak=%lu bullet_hi=%ld "
        "bullet_enum_frames=%llu bullet_enum_slot_probes=%llu "
        "bullet_enum_word_probes=%llu bullet_enum_visited=%llu "
        "bullet_enum_fallback_frames=%llu "
        "laser_exact=%lu/%u laser_exact_sampled_peak=%lu laser_hi=%ld "
        "item_reported=%lu/%u item_reported_peak=%lu item_array_exact=%lu item_list_exact=%lu "
        "item_exact_sampled_peak=%lu item_hi=%ld "
        "render_perf_valid=%d render_frames=%lu "
        "render_draws_total=%llu render_draws_peak=%llu "
        "render_vertices_total=%llu render_vertices_peak=%llu "
        "render_fps_overlay_vertices_total=%llu render_fps_overlay_vertices_peak=%llu "
        "render_game_vertices_total=%llu render_game_vertices_peak=%llu "
        "render_state_requested_total=%llu render_state_requested_peak=%llu "
        "render_state_emitted_total=%llu render_state_emitted_peak=%llu "
        "render_matrix_recompute_total=%llu render_matrix_recompute_peak=%llu "
        "render_vfpu_sincos_total=%llu render_vfpu_sincos_peak=%llu "
        "render_bg_visits_total=%llu render_bg_visits_peak=%llu "
        "render_bg_candidates_total=%llu render_bg_candidates_peak=%llu "
        "render_bg_drawn_total=%llu render_bg_drawn_peak=%llu "
        "render_bg_project_total=%llu render_bg_project_peak=%llu "
        "render_bg_radius_cache_total=%llu render_bg_radius_cache_peak=%llu "
        "render_effect_spawn_total=%llu render_effect_spawn_peak=%llu "
        "render_effect_active_total=%llu render_effect_active_peak=%llu "
        "render_effect_drawn_total=%llu render_effect_drawn_peak=%llu "
        "render_item_spawn_total=%llu render_item_spawn_peak=%llu "
        "render_item_time_spawn_init_candidates_total=%llu render_item_time_spawn_init_candidates_peak=%llu "
        "render_item_time_spawn_init_eligible_total=%llu render_item_time_spawn_init_eligible_peak=%llu "
        "render_item_time_spawn_init_full_vm_mismatch_total=%llu render_item_time_spawn_init_full_vm_mismatch_peak=%llu "
        "render_item_time_spawn_init_field_mismatch_total=%llu render_item_time_spawn_init_field_mismatch_peak=%llu "
        "render_item_time_spawn_init_fallback_total=%llu render_item_time_spawn_init_fallback_peak=%llu "
        "render_item_drawn_total=%llu render_item_drawn_peak=%llu "
        "render_popup_active_total=%llu render_popup_active_peak=%llu "
        "render_popup_digits_total=%llu render_popup_digits_peak=%llu "
        "render_ascii_popup_batch_calls_total=%llu render_ascii_popup_batch_calls_peak=%llu "
        "render_ascii_popup_batch_digits_total=%llu render_ascii_popup_batch_digits_peak=%llu "
        "render_ascii_popup_batch_sprites_total=%llu render_ascii_popup_batch_sprites_peak=%llu "
        "render_ascii_popup_batch_vertices_saved_total=%llu render_ascii_popup_batch_vertices_saved_peak=%llu "
        "render_ascii_popup_batch_bytes_saved_total=%llu render_ascii_popup_batch_bytes_saved_peak=%llu "
        "render_ascii_popup_batch_fallbacks_total=%llu render_ascii_popup_batch_fallbacks_peak=%llu "
        "render_ascii_popup_direct_active_popups_total=%llu render_ascii_popup_direct_active_popups_peak=%llu "
        "render_ascii_popup_direct_validation_quads_avoided_total=%llu render_ascii_popup_direct_validation_quads_avoided_peak=%llu "
        "render_ascii_popup_direct_validation_culls_avoided_total=%llu render_ascii_popup_direct_validation_culls_avoided_peak=%llu "
        "render_ascii_popup_direct_nearbyint_avoided_total=%llu render_ascii_popup_direct_nearbyint_avoided_peak=%llu "
        "render_upload_attempt_total=%llu render_upload_attempt_peak=%llu "
        "render_actual_upload_total=%llu render_actual_upload_peak=%llu "
        "render_upload_bytes_total=%llu render_upload_bytes_peak=%llu "
        "render_text_bytes_total=%llu render_text_bytes_peak=%llu "
        "audio_callbacks=%lu audio_frames=%lu audio_nonzero_samples=%lu audio_peak=%lu "
        "audio_voices=%lu audio_voices_peak=%lu audio_play_submits=%lu "
        "audio_bgm_notify=%lu audio_bgm_refill=%lu audio_bgm_bytes=%lu "
        "audio_bgm_skip=%lu audio_bgm_fail=%lu "
        "audio_underrun_exact_available=%lu audio_underrun_exact=NA\n",
        kind,
        phase != nullptr ? phase : "periodic",
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        th08::g_GameManager.flags.isReplay ? 1 : 0,
        th08::g_GameManager.flags.isDemoMode ? 1 : 0,
        static_cast<unsigned long long>(fpsMilli / 1000ULL),
        static_cast<unsigned long long>(fpsMilli % 1000ULL),
        static_cast<unsigned long>(heap.arenaBytes),
        static_cast<unsigned long>(heap.usedBytes),
        static_cast<unsigned long>(gPeakMallinfoUsed),
        static_cast<unsigned long>(heap.freeBytes),
        static_cast<unsigned long>(heap.topChunkBytes),
        static_cast<unsigned long>(heap.freeChunkCount),
        static_cast<unsigned long>(heap.largestFreeChunkBytes),
        static_cast<unsigned long>(heap.largestNoGrowRequestBytes),
        static_cast<unsigned long>(heap.scanValid),
        static_cast<unsigned long>(heap.scanErrorCount),
        static_cast<unsigned long>(heap.scanErrorFlags),
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()),
        static_cast<unsigned long>(gTrackedLiveBytes),
        static_cast<unsigned long>(gTrackedPeakBytes),
        static_cast<unsigned long>(gTrackedAllocationCount),
        static_cast<unsigned long>(gTrackedFailureCount),
        static_cast<unsigned long>(renderArena.capacityBytes),
        static_cast<unsigned long>(renderArena.liveBytes),
        static_cast<unsigned long>(renderArena.peakBytes),
        static_cast<unsigned long>(renderArena.freeBytes),
        static_cast<unsigned long>(renderArena.largestFreeBytes),
        static_cast<unsigned long>(renderArena.allocationCount),
        static_cast<unsigned long>(renderArena.failureCount),
        static_cast<unsigned long>(renderArena.liveAllocations),
        static_cast<unsigned long>(renderArena.quarantineCount),
        static_cast<unsigned long>(renderArena.scopeContentionCount),
        static_cast<unsigned long>(renderArena.poisoned),
        static_cast<unsigned long>(eclChild.blockBytes),
        static_cast<unsigned long>(eclChild.liveBlocks),
        static_cast<unsigned long>(eclChild.peakBlocks),
        static_cast<unsigned long>(eclChild.liveBytes),
        static_cast<unsigned long>(eclChild.peakBytes),
        static_cast<unsigned long>(eclChild.arenaLiveBlocks),
        static_cast<unsigned long>(eclChild.arenaPeakBlocks),
        static_cast<unsigned long>(eclChild.heapLiveBlocks),
        static_cast<unsigned long>(eclChild.heapPeakBlocks),
        static_cast<unsigned long>(eclChild.arenaAllocations),
        static_cast<unsigned long>(eclChild.arenaMisses),
        static_cast<unsigned long>(eclChild.heapFallbacks),
        static_cast<unsigned long>(eclChild.allocationFailures),
        static_cast<unsigned long>(eclChild.freeFailures),
        static_cast<unsigned long>(th08::psp::AnmScratchCapacity()),
        static_cast<unsigned long>(th08::psp::AnmScratchActiveBytes()),
        th08::psp::AnmScratchBusy() ? 1 : 0,
        th08::psp::AnmScratchPoisoned() ? 1 : 0,
        static_cast<unsigned long>(th08::psp::AnmScratchGeneration()),
        th08::psp::AnmScratchOwnerIndex(),
        th08::psp::StagePoolArenaIsAllocated() ? 1 : 0,
        stagePoolBound ? 1 : 0,
        th08::psp::StagePoolArenaGuardsIntact() ? "OK" : "CORRUPT",
        static_cast<unsigned long>(th08::psp::StagePoolArenaGeneration()),
        static_cast<unsigned long>(th08::psp::StagePoolArenaBase()),
        static_cast<unsigned long>(th08::psp::StagePoolArenaReservedBytes()),
        static_cast<unsigned long>(th08::psp::StagePoolArenaPayloadBytes()),
        static_cast<unsigned long>(th08::psp::StagePoolArenaTransientActiveBytes()),
        static_cast<unsigned long>(th08::psp::StagePoolArenaTransientPeakBytes()),
        static_cast<unsigned long>(th08::psp::StagePoolArenaTransientLoanCount()),
        static_cast<unsigned long>(th08::psp::StagePoolArenaTransientFailureCount()),
        static_cast<unsigned long>(th08::psp::StagePoolArenaTransientQuarantineCount()),
        static_cast<long>(enemyReported), 480U,
        static_cast<unsigned long>(gPeakEnemyCount),
        static_cast<unsigned long>(exact.enemies),
        static_cast<unsigned long>(gSampledPeakEnemyCount),
        exact.enemyHighestSlot == 0xffffffffU ? -1L : static_cast<long>(exact.enemyHighestSlot),
        static_cast<long>(bulletReported), 0x600U,
        static_cast<unsigned long>(gPeakBulletCount),
        static_cast<unsigned long>(exact.bullets),
        static_cast<unsigned long>(gSampledPeakBulletCount),
        exact.bulletHighestSlot == 0xffffffffU ? -1L : static_cast<long>(exact.bulletHighestSlot),
        static_cast<unsigned long long>(bulletLiveEnum.frames),
        static_cast<unsigned long long>(bulletLiveEnum.slotProbes),
        static_cast<unsigned long long>(bulletLiveEnum.wordProbes),
        static_cast<unsigned long long>(bulletLiveEnum.visited),
        static_cast<unsigned long long>(bulletLiveEnum.fallbackFrames),
        static_cast<unsigned long>(exact.lasers), 0x100U,
        static_cast<unsigned long>(gSampledPeakLaserCount),
        exact.laserHighestSlot == 0xffffffffU ? -1L : static_cast<long>(exact.laserHighestSlot),
        static_cast<unsigned long>(itemReported),
        static_cast<unsigned int>(MAX_ITEMS),
        static_cast<unsigned long>(gPeakItemCount),
        static_cast<unsigned long>(exact.itemArray),
        static_cast<unsigned long>(exact.itemList),
        static_cast<unsigned long>(gSampledPeakItemCount),
        exact.itemHighestSlot == 0xffffffffU ? -1L : static_cast<long>(exact.itemHighestSlot),
        renderPerf != nullptr ? 1 : 0,
        static_cast<unsigned long>(render.frames),
        static_cast<unsigned long long>(render.total.draws),
        static_cast<unsigned long long>(render.peak.draws),
        static_cast<unsigned long long>(render.total.vertices),
        static_cast<unsigned long long>(render.peak.vertices),
        static_cast<unsigned long long>(render.total.fpsOverlayVertices),
        static_cast<unsigned long long>(render.peak.fpsOverlayVertices),
        static_cast<unsigned long long>(render.total.gameVertices),
        static_cast<unsigned long long>(render.peak.gameVertices),
        static_cast<unsigned long long>(render.total.stateRequested),
        static_cast<unsigned long long>(render.peak.stateRequested),
        static_cast<unsigned long long>(render.total.stateEmitted),
        static_cast<unsigned long long>(render.peak.stateEmitted),
        static_cast<unsigned long long>(render.total.matrixRecomputes),
        static_cast<unsigned long long>(render.peak.matrixRecomputes),
        static_cast<unsigned long long>(render.total.vfpuSinCos),
        static_cast<unsigned long long>(render.peak.vfpuSinCos),
        static_cast<unsigned long long>(render.total.backgroundInstanceVisits),
        static_cast<unsigned long long>(render.peak.backgroundInstanceVisits),
        static_cast<unsigned long long>(render.total.backgroundCandidates),
        static_cast<unsigned long long>(render.peak.backgroundCandidates),
        static_cast<unsigned long long>(render.total.backgroundInstancesDrawn),
        static_cast<unsigned long long>(render.peak.backgroundInstancesDrawn),
        static_cast<unsigned long long>(render.total.backgroundProjectCalls),
        static_cast<unsigned long long>(render.peak.backgroundProjectCalls),
        static_cast<unsigned long long>(render.total.backgroundRadiusCacheHits),
        static_cast<unsigned long long>(render.peak.backgroundRadiusCacheHits),
        static_cast<unsigned long long>(render.total.effectSpawnRequests),
        static_cast<unsigned long long>(render.peak.effectSpawnRequests),
        static_cast<unsigned long long>(render.total.effectsActive),
        static_cast<unsigned long long>(render.peak.effectsActive),
        static_cast<unsigned long long>(render.total.effectsDrawn),
        static_cast<unsigned long long>(render.peak.effectsDrawn),
        static_cast<unsigned long long>(render.total.itemSpawnRequests),
        static_cast<unsigned long long>(render.peak.itemSpawnRequests),
        static_cast<unsigned long long>(
            render.total.itemTimeSpawnInitCandidates),
        static_cast<unsigned long long>(
            render.peak.itemTimeSpawnInitCandidates),
        static_cast<unsigned long long>(render.total.itemTimeSpawnInitEligible),
        static_cast<unsigned long long>(render.peak.itemTimeSpawnInitEligible),
        static_cast<unsigned long long>(
            render.total.itemTimeSpawnInitFullVmMismatches),
        static_cast<unsigned long long>(
            render.peak.itemTimeSpawnInitFullVmMismatches),
        static_cast<unsigned long long>(
            render.total.itemTimeSpawnInitFieldMismatches),
        static_cast<unsigned long long>(
            render.peak.itemTimeSpawnInitFieldMismatches),
        static_cast<unsigned long long>(render.total.itemTimeSpawnInitFallbacks),
        static_cast<unsigned long long>(render.peak.itemTimeSpawnInitFallbacks),
        static_cast<unsigned long long>(render.total.itemsDrawn),
        static_cast<unsigned long long>(render.peak.itemsDrawn),
        static_cast<unsigned long long>(render.total.popupsActive),
        static_cast<unsigned long long>(render.peak.popupsActive),
        static_cast<unsigned long long>(render.total.popupDigits),
        static_cast<unsigned long long>(render.peak.popupDigits),
        static_cast<unsigned long long>(render.total.asciiPopupBatchCalls),
        static_cast<unsigned long long>(render.peak.asciiPopupBatchCalls),
        static_cast<unsigned long long>(render.total.asciiPopupBatchDigits),
        static_cast<unsigned long long>(render.peak.asciiPopupBatchDigits),
        static_cast<unsigned long long>(render.total.asciiPopupBatchSprites),
        static_cast<unsigned long long>(render.peak.asciiPopupBatchSprites),
        static_cast<unsigned long long>(
            render.total.asciiPopupBatchVerticesSaved),
        static_cast<unsigned long long>(
            render.peak.asciiPopupBatchVerticesSaved),
        static_cast<unsigned long long>(render.total.asciiPopupBatchBytesSaved),
        static_cast<unsigned long long>(render.peak.asciiPopupBatchBytesSaved),
        static_cast<unsigned long long>(render.total.asciiPopupBatchFallbacks),
        static_cast<unsigned long long>(render.peak.asciiPopupBatchFallbacks),
        static_cast<unsigned long long>(
            render.total.asciiPopupDirectActivePopups),
        static_cast<unsigned long long>(
            render.peak.asciiPopupDirectActivePopups),
        static_cast<unsigned long long>(
            render.total.asciiPopupDirectValidationQuadsAvoided),
        static_cast<unsigned long long>(
            render.peak.asciiPopupDirectValidationQuadsAvoided),
        static_cast<unsigned long long>(
            render.total.asciiPopupDirectValidationCullTestsAvoided),
        static_cast<unsigned long long>(
            render.peak.asciiPopupDirectValidationCullTestsAvoided),
        static_cast<unsigned long long>(
            render.total.asciiPopupDirectNearbyintCallsAvoided),
        static_cast<unsigned long long>(
            render.peak.asciiPopupDirectNearbyintCallsAvoided),
        static_cast<unsigned long long>(render.total.uploadAttempts),
        static_cast<unsigned long long>(render.peak.uploadAttempts),
        static_cast<unsigned long long>(render.total.actualUploads),
        static_cast<unsigned long long>(render.peak.actualUploads),
        static_cast<unsigned long long>(render.total.uploadBytes),
        static_cast<unsigned long long>(render.peak.uploadBytes),
        static_cast<unsigned long long>(render.total.textBytes),
        static_cast<unsigned long long>(render.peak.textBytes),
        static_cast<unsigned long>(audio.callbackCount),
        static_cast<unsigned long>(audio.callbackFrames),
        static_cast<unsigned long>(audio.nonzeroSamples),
        static_cast<unsigned long>(audio.peakAmplitude),
        static_cast<unsigned long>(audio.activeVoicesCurrent),
        static_cast<unsigned long>(audio.activeVoicesPeak),
        static_cast<unsigned long>(audio.playSubmits),
        static_cast<unsigned long>(audio.bgmNotifySignals),
        static_cast<unsigned long>(audio.bgmRefills),
        static_cast<unsigned long>(audio.bgmRefillBytes),
        static_cast<unsigned long>(audio.bgmRefillSkips),
        static_cast<unsigned long>(audio.bgmRefillFailures),
        static_cast<unsigned long>(audio.exactUnderrunAvailable));

#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT) && \
    TH08_PSP_BULLET_COLLISION_GATE_AUDIT
    const unsigned long long canonicalResultSum =
        bulletCollisionGateAudit.canonicalZero +
        bulletCollisionGateAudit.canonicalOne +
        bulletCollisionGateAudit.canonicalTwo;
    const unsigned long long decisionBucketSum =
        bulletCollisionGateAudit.clearGrazeSuppressed +
        bulletCollisionGateAudit.clearGrazeSeparate +
        bulletCollisionGateAudit.clearLethalSeparate +
        bulletCollisionGateAudit.cancelUnknownFallbacks +
        bulletCollisionGateAudit.invalidSnapshotFallbacks +
        bulletCollisionGateAudit.invalidBulletFallbacks +
        bulletCollisionGateAudit.touchOrOverlapFallbacks;
    const bool acceptanceWitnessesZero =
        bulletCollisionGateAudit.cancelFalseEmptyWitnesses == 0ULL &&
        bulletCollisionGateAudit.snapshotMutationWitnesses == 0ULL &&
        bulletCollisionGateAudit.falseClearWitnesses == 0ULL &&
        bulletCollisionGateAudit.itemTypeWitnesses == 0ULL;
    AppendLine(
        "BULLET_COLLISION_GATE_AUDIT kind=%s phase=%s frame=%lu stage=%ld "
        "stage_frame=%lu frames=%llu sidecar_owner_invalid_frames=%llu "
        "sidecar_claims_empty_frames=%llu authoritative_empty_frames=%llu "
        "known_empty_frames=%llu cancel_false_empty_witnesses=%llu "
        "cancel_stale_positive_frames=%llu collision_eligible=%llu "
        "graze_path=%llu lethal_path=%llu "
        "clear_graze_suppressed=%llu clear_graze_separate=%llu "
        "clear_lethal_separate=%llu cancel_unknown_fallbacks=%llu "
        "invalid_snapshot_fallbacks=%llu invalid_bullet_fallbacks=%llu "
        "touch_or_overlap_fallbacks=%llu snapshot_mutation_witnesses=%llu "
        "canonical_zero=%llu canonical_one=%llu canonical_two=%llu "
        "false_clear_witnesses=%llu item_type_witnesses=%llu "
        "canonical_result_sum=%llu canonical_partition_ok=%d "
        "decision_bucket_sum=%llu decision_partition_ok=%d "
        "acceptance_witnesses_zero=%d\n",
        kind, phase != nullptr ? phase : "periodic",
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        bulletCollisionGateAudit.frames,
        bulletCollisionGateAudit.sidecarOwnerInvalidFrames,
        bulletCollisionGateAudit.sidecarClaimsEmptyFrames,
        bulletCollisionGateAudit.authoritativeEmptyFrames,
        bulletCollisionGateAudit.knownEmptyFrames,
        bulletCollisionGateAudit.cancelFalseEmptyWitnesses,
        bulletCollisionGateAudit.cancelStalePositiveFrames,
        bulletCollisionGateAudit.collisionEligibleBullets,
        bulletCollisionGateAudit.grazePathBullets,
        bulletCollisionGateAudit.lethalPathBullets,
        bulletCollisionGateAudit.clearGrazeSuppressed,
        bulletCollisionGateAudit.clearGrazeSeparate,
        bulletCollisionGateAudit.clearLethalSeparate,
        bulletCollisionGateAudit.cancelUnknownFallbacks,
        bulletCollisionGateAudit.invalidSnapshotFallbacks,
        bulletCollisionGateAudit.invalidBulletFallbacks,
        bulletCollisionGateAudit.touchOrOverlapFallbacks,
        bulletCollisionGateAudit.snapshotMutationWitnesses,
        bulletCollisionGateAudit.canonicalZero,
        bulletCollisionGateAudit.canonicalOne,
        bulletCollisionGateAudit.canonicalTwo,
        bulletCollisionGateAudit.falseClearWitnesses,
        bulletCollisionGateAudit.itemTypeWitnesses,
        canonicalResultSum,
        canonicalResultSum == bulletCollisionGateAudit.collisionEligibleBullets
            ? 1
            : 0,
        decisionBucketSum,
        decisionBucketSum == bulletCollisionGateAudit.collisionEligibleBullets
            ? 1
            : 0,
        acceptanceWitnessesZero ? 1 : 0);
#endif

#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT) && \
    TH08_PSP_BULLET_TRANSFORM_AUDIT
    // Keep this audit out of the already-large general snapshot record.  It
    // is emitted only in explicit audit builds and performs no hot-path I/O.
    AppendLine(
        "BULLET_TRANSFORM_AUDIT kind=%s phase=%s frame=%lu stage=%ld "
        "stage_frame=%lu advance_calls=%llu fired_update_calls=%llu "
        "terminal_index_returns=%llu terminal_none_returns=%llu "
        "active_blocked_returns=%llu disabled_records_skipped=%llu "
        "records_dispatched=%llu active_transform_starts=%llu "
        "child_spawn_records=%llu spawn_program_writes=%llu "
        "whole_slot_resets=%llu\n",
        kind, phase != nullptr ? phase : "periodic",
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        static_cast<unsigned long long>(bulletTransformAudit.advanceCalls),
        static_cast<unsigned long long>(bulletTransformAudit.firedUpdateCalls),
        static_cast<unsigned long long>(
            bulletTransformAudit.terminalIndexReturns),
        static_cast<unsigned long long>(
            bulletTransformAudit.terminalNoneReturns),
        static_cast<unsigned long long>(
            bulletTransformAudit.activeBlockedReturns),
        static_cast<unsigned long long>(
            bulletTransformAudit.disabledRecordsSkipped),
        static_cast<unsigned long long>(bulletTransformAudit.recordsDispatched),
        static_cast<unsigned long long>(
            bulletTransformAudit.activeTransformStarts),
        static_cast<unsigned long long>(bulletTransformAudit.childSpawnRecords),
        static_cast<unsigned long long>(bulletTransformAudit.spawnProgramWrites),
        static_cast<unsigned long long>(bulletTransformAudit.wholeSlotResets));
#endif

#if defined(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH) && \
    TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH
    AppendLine(
        "BULLET_TRANSFORM_TERMINAL kind=%s phase=%s frame=%lu stage=%ld "
        "stage_frame=%lu fired_calls=%llu hits=%llu misses=%llu "
        "fallbacks=%llu invariant_witnesses=%llu repairs=%llu "
        "terminal_index_marks=%llu terminal_none_marks=%llu "
        "mark_fallbacks=%llu spawn_program_clears=%llu "
        "whole_slot_clears=%llu\n",
        kind, phase != nullptr ? phase : "periodic",
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        static_cast<unsigned long long>(bulletTransformTerminal.firedCalls),
        static_cast<unsigned long long>(bulletTransformTerminal.hits),
        static_cast<unsigned long long>(bulletTransformTerminal.misses),
        static_cast<unsigned long long>(bulletTransformTerminal.fallbacks),
        static_cast<unsigned long long>(
            bulletTransformTerminal.invariantWitnesses),
        static_cast<unsigned long long>(bulletTransformTerminal.repairs),
        static_cast<unsigned long long>(
            bulletTransformTerminal.terminalIndexMarks),
        static_cast<unsigned long long>(
            bulletTransformTerminal.terminalNoneMarks),
        static_cast<unsigned long long>(bulletTransformTerminal.markFallbacks),
        static_cast<unsigned long long>(
            bulletTransformTerminal.spawnProgramClears),
        static_cast<unsigned long long>(
            bulletTransformTerminal.wholeSlotClears));
#endif

#if defined(TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT) && \
    TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT
    const th08::ItemAutocollectCacheAuditStats &itemAutocollect =
        th08::GetItemAutocollectCacheAuditStats();
    AppendLine(
        "ITEM_AUTOCOLLECT_AUDIT kind=%s phase=%s frame=%lu stage=%ld "
        "stage_frame=%lu canonical_calls=%lu exact_input_repeats=%lu "
        "exact_output_matches=%lu exact_output_mismatches=%lu "
        "invalid_slot_observations=%lu\n",
        kind, phase != nullptr ? phase : "periodic",
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        static_cast<unsigned long>(itemAutocollect.canonicalCalls),
        static_cast<unsigned long>(itemAutocollect.exactInputRepeats),
        static_cast<unsigned long>(itemAutocollect.exactOutputMatches),
        static_cast<unsigned long>(itemAutocollect.exactOutputMismatches),
        static_cast<unsigned long>(itemAutocollect.invalidSlotObservations));
#endif

#if defined(TH08_PSP_ITEM_AUTOCOLLECT_CACHE) && \
    TH08_PSP_ITEM_AUTOCOLLECT_CACHE && \
    defined(TH08_REPLAY_SYNC_AUDIT) && TH08_REPLAY_SYNC_AUDIT
    const th08::ItemAutocollectCacheStats &itemAutocollectCache =
        th08::GetItemAutocollectCacheStats();
    AppendLine(
        "ITEM_AUTOCOLLECT_CACHE kind=%s phase=%s frame=%lu stage=%ld "
        "stage_frame=%lu lookups=%lu hits=%lu misses=%lu conflicts=%lu "
        "invalid_slot_lookups=%lu capacity=1024 entry_bytes=16 "
        "tag_bytes=1024 total_bytes=17408\n",
        kind, phase != nullptr ? phase : "periodic",
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        static_cast<unsigned long>(itemAutocollectCache.lookups),
        static_cast<unsigned long>(itemAutocollectCache.hits),
        static_cast<unsigned long>(itemAutocollectCache.misses),
        static_cast<unsigned long>(itemAutocollectCache.conflicts),
        static_cast<unsigned long>(itemAutocollectCache.invalidSlotLookups));
#endif

#if defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT) && \
    TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT
    const th08::ItemTimeSpawnInitAuditStats &itemTimeSpawnInit =
        th08::GetItemTimeSpawnInitAuditStats();
    AppendLine(
        "ITEM_TIME_SPAWN_INIT_AUDIT kind=%s phase=%s frame=%lu stage=%ld "
        "stage_frame=%lu candidates=%lu eligible=%lu canonical_fallbacks=%lu "
        "full_vm_matches=%lu full_vm_mismatches=%lu field_matches=%lu "
        "field_mismatches=%lu candidate_return_contract_matches=%lu "
        "candidate_return_contract_mismatches=%lu "
        "pointer_matches=%lu pointer_mismatches=%lu counter_matches=%lu "
        "counter_mismatches=%lu sprite_matches=%lu sprite_mismatches=%lu "
        "load_generation_matches=%lu load_generation_mismatches=%lu "
        "script_fingerprint_fallbacks=%lu compact_range_fallbacks=%lu "
        "load_state_fallbacks=%lu "
        "canonical_state_fallbacks=%lu peak_candidates_per_frame=%lu "
        "peak_candidates_stage_frame=%ld peak_eligible_per_frame=%lu "
        "peak_eligible_stage_frame=%ld vm_bytes=%u base_clear_bytes=%u "
        "authoritative=canonical candidate=shadow_only "
        "canonical_return_observed=0\n",
        kind, phase != nullptr ? phase : "periodic",
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        static_cast<unsigned long>(itemTimeSpawnInit.candidates),
        static_cast<unsigned long>(itemTimeSpawnInit.eligible),
        static_cast<unsigned long>(itemTimeSpawnInit.canonicalFallbacks),
        static_cast<unsigned long>(itemTimeSpawnInit.fullVmMatches),
        static_cast<unsigned long>(itemTimeSpawnInit.fullVmMismatches),
        static_cast<unsigned long>(itemTimeSpawnInit.fieldMatches),
        static_cast<unsigned long>(itemTimeSpawnInit.fieldMismatches),
        static_cast<unsigned long>(
            itemTimeSpawnInit.candidateReturnContractMatches),
        static_cast<unsigned long>(
            itemTimeSpawnInit.candidateReturnContractMismatches),
        static_cast<unsigned long>(itemTimeSpawnInit.pointerMatches),
        static_cast<unsigned long>(itemTimeSpawnInit.pointerMismatches),
        static_cast<unsigned long>(itemTimeSpawnInit.counterMatches),
        static_cast<unsigned long>(itemTimeSpawnInit.counterMismatches),
        static_cast<unsigned long>(itemTimeSpawnInit.spriteMatches),
        static_cast<unsigned long>(itemTimeSpawnInit.spriteMismatches),
        static_cast<unsigned long>(itemTimeSpawnInit.loadGenerationMatches),
        static_cast<unsigned long>(itemTimeSpawnInit.loadGenerationMismatches),
        static_cast<unsigned long>(
            itemTimeSpawnInit.scriptFingerprintFallbacks),
        static_cast<unsigned long>(itemTimeSpawnInit.compactRangeFallbacks),
        static_cast<unsigned long>(itemTimeSpawnInit.loadStateFallbacks),
        static_cast<unsigned long>(itemTimeSpawnInit.canonicalStateFallbacks),
        static_cast<unsigned long>(itemTimeSpawnInit.peakCandidatesPerFrame),
        itemTimeSpawnInit.peakCandidatesStageFrame == 0xffffffffU
            ? -1L
            : static_cast<long>(itemTimeSpawnInit.peakCandidatesStageFrame),
        static_cast<unsigned long>(itemTimeSpawnInit.peakEligiblePerFrame),
        itemTimeSpawnInit.peakEligibleStageFrame == 0xffffffffU
            ? -1L
            : static_cast<long>(itemTimeSpawnInit.peakEligibleStageFrame),
        static_cast<unsigned int>(sizeof(th08::AnmVm)),
        static_cast<unsigned int>(sizeof(th08::AnmVmBase)));
#endif

#if defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH) && \
    TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH
    const th08::ItemTimeSpawnInitFastpathStats &itemTimeSpawnInitFastpath =
        th08::GetItemTimeSpawnInitFastpathStats();
    AppendLine(
        "ITEM_TIME_SPAWN_INIT_FASTPATH kind=%s phase=%s frame=%lu stage=%ld "
        "stage_frame=%lu calls=%lu hits=%lu canonical_fallbacks=%lu "
        "owner_fallbacks=%lu generation_fallbacks=%lu "
        "readiness_fallbacks=%lu script_fallbacks=%lu range_fallbacks=%lu "
        "fingerprint_fallbacks=%lu cache_hits=%lu cache_revalidations=%lu "
        "cache_generation_changes=%lu cache_validation_failures=%lu "
        "cache_resets=%lu fingerprint_scope=script68_4_instructions "
        "vm_bytes=%u base_clear_bytes=%u\n",
        kind, phase != nullptr ? phase : "periodic",
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        static_cast<unsigned long>(itemTimeSpawnInitFastpath.calls),
        static_cast<unsigned long>(itemTimeSpawnInitFastpath.hits),
        static_cast<unsigned long>(
            itemTimeSpawnInitFastpath.canonicalFallbacks),
        static_cast<unsigned long>(itemTimeSpawnInitFastpath.ownerFallbacks),
        static_cast<unsigned long>(
            itemTimeSpawnInitFastpath.generationFallbacks),
        static_cast<unsigned long>(
            itemTimeSpawnInitFastpath.readinessFallbacks),
        static_cast<unsigned long>(itemTimeSpawnInitFastpath.scriptFallbacks),
        static_cast<unsigned long>(itemTimeSpawnInitFastpath.rangeFallbacks),
        static_cast<unsigned long>(
            itemTimeSpawnInitFastpath.fingerprintFallbacks),
        static_cast<unsigned long>(itemTimeSpawnInitFastpath.cacheHits),
        static_cast<unsigned long>(
            itemTimeSpawnInitFastpath.cacheRevalidations),
        static_cast<unsigned long>(
            itemTimeSpawnInitFastpath.cacheGenerationChanges),
        static_cast<unsigned long>(
            itemTimeSpawnInitFastpath.cacheValidationFailures),
        static_cast<unsigned long>(itemTimeSpawnInitFastpath.cacheResets),
        static_cast<unsigned int>(sizeof(th08::AnmVm)),
        static_cast<unsigned int>(sizeof(th08::AnmVmBase)));
#endif

#if defined(TH08_PSP_ITEM_TIME_ANM_IDLE_AUDIT) && \
    TH08_PSP_ITEM_TIME_ANM_IDLE_AUDIT
    const th08::ItemTimeAnmIdleAuditStats &itemTimeAnmIdle =
        th08::GetItemTimeAnmIdleAuditStats();
    AppendLine(
        "ITEM_TIME_ANM_IDLE_AUDIT kind=%s phase=%s frame=%lu stage=%ld "
        "stage_frame=%lu canonical_calls=%lu script68_candidates=%lu "
        "eligible_idle_calls=%lu rejected_window_calls=%lu "
        "rejected_dynamic_state_calls=%lu full_vm_matches=%lu "
        "full_vm_mismatches=%lu return_matches=%lu return_mismatches=%lu "
        "counter_matches=%lu counter_mismatches=%lu\n",
        kind, phase != nullptr ? phase : "periodic",
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        static_cast<unsigned long>(itemTimeAnmIdle.canonicalCalls),
        static_cast<unsigned long>(itemTimeAnmIdle.script68Candidates),
        static_cast<unsigned long>(itemTimeAnmIdle.eligibleIdleCalls),
        static_cast<unsigned long>(itemTimeAnmIdle.rejectedWindowCalls),
        static_cast<unsigned long>(itemTimeAnmIdle.rejectedDynamicStateCalls),
        static_cast<unsigned long>(itemTimeAnmIdle.fullVmMatches),
        static_cast<unsigned long>(itemTimeAnmIdle.fullVmMismatches),
        static_cast<unsigned long>(itemTimeAnmIdle.returnMatches),
        static_cast<unsigned long>(itemTimeAnmIdle.returnMismatches),
        static_cast<unsigned long>(itemTimeAnmIdle.counterMatches),
        static_cast<unsigned long>(itemTimeAnmIdle.counterMismatches));
#endif

#if defined(TH08_PSP_ITEM_TIME_ANM_IDLE_FASTPATH) && \
    TH08_PSP_ITEM_TIME_ANM_IDLE_FASTPATH && \
    defined(TH08_REPLAY_SYNC_AUDIT) && TH08_REPLAY_SYNC_AUDIT
    const th08::ItemTimeAnmIdleFastpathStats &itemTimeAnmIdleFastpath =
        th08::GetItemTimeAnmIdleFastpathStats();
    AppendLine(
        "ITEM_TIME_ANM_IDLE_FASTPATH kind=%s phase=%s frame=%lu stage=%ld "
        "stage_frame=%lu calls=%lu hits=%lu canonical_fallbacks=%lu "
        "identity_fallbacks=%lu window_fallbacks=%lu dynamic_fallbacks=%lu "
        "cache_pointer_changes=%lu cache_revalidations=%lu "
        "cache_validation_failures=%lu\n",
        kind, phase != nullptr ? phase : "periodic",
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        static_cast<unsigned long>(itemTimeAnmIdleFastpath.calls),
        static_cast<unsigned long>(itemTimeAnmIdleFastpath.hits),
        static_cast<unsigned long>(itemTimeAnmIdleFastpath.canonicalFallbacks),
        static_cast<unsigned long>(itemTimeAnmIdleFastpath.identityFallbacks),
        static_cast<unsigned long>(itemTimeAnmIdleFastpath.windowFallbacks),
        static_cast<unsigned long>(itemTimeAnmIdleFastpath.dynamicFallbacks),
        static_cast<unsigned long>(itemTimeAnmIdleFastpath.cachePointerChanges),
        static_cast<unsigned long>(itemTimeAnmIdleFastpath.cacheRevalidations),
        static_cast<unsigned long>(itemTimeAnmIdleFastpath.cacheValidationFailures));
#endif

#if TH08_PSP_ITEM_TIME_DRAW_PAIR_ENABLED
    const th08::ItemTimeDrawPairStats &itemTimeDrawPair =
        th08::GetItemTimeDrawPairStats();
#if defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT) && \
    TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT
    const char *const itemTimeDrawPairMode = "audit";
#else
    const char *const itemTimeDrawPairMode = "product";
#endif
    AppendLine(
        "ITEM_TIME_DRAW_PAIR kind=%s phase=%s mode=%s frame=%lu stage=%ld "
        "stage_frame=%lu passes=%lu candidates=%lu canonical_draws=%lu "
        "visible=%lu culled=%lu eligible_pairs=%lu compatible_runs=%lu "
        "submitted_runs=%lu submitted_pairs=%lu endpoint_matches=%lu "
        "endpoint_mismatches=%lu canonical_fallbacks=%lu "
        "owner_fallbacks=%lu load_fallbacks=%lu script_fallbacks=%lu "
        "sprite_fallbacks=%lu visibility_fallbacks=%lu "
        "rotation_fallbacks=%lu scale_fallbacks=%lu "
        "nonfinite_fallbacks=%lu texture_fallbacks=%lu "
        "state_fallbacks=%lu axis_fallbacks=%lu endpoint_fallbacks=%lu "
        "capacity_fallbacks=%lu backend_fallbacks=%lu "
        "vertices_saved=%lu frontend_bytes_saved=%lu "
        "backend_bytes_saved=%lu max_run=%lu peak_candidates=%lu "
        "peak_visible=%lu peak_eligible=%lu peak_runs=%lu "
        "peak_stage_frame=%ld cache_hits=%lu cache_revalidations=%lu "
        "cache_generation_changes=%lu cache_validation_failures=%lu "
        "semantic_hash=%08lx\n",
        kind, phase != nullptr ? phase : "periodic",
        itemTimeDrawPairMode, static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        static_cast<unsigned long>(itemTimeDrawPair.passes),
        static_cast<unsigned long>(itemTimeDrawPair.candidates),
        static_cast<unsigned long>(itemTimeDrawPair.canonicalDraws),
        static_cast<unsigned long>(itemTimeDrawPair.visibleCandidates),
        static_cast<unsigned long>(itemTimeDrawPair.culledCandidates),
        static_cast<unsigned long>(itemTimeDrawPair.eligiblePairs),
        static_cast<unsigned long>(itemTimeDrawPair.compatibleRuns),
        static_cast<unsigned long>(itemTimeDrawPair.submittedRuns),
        static_cast<unsigned long>(itemTimeDrawPair.submittedPairs),
        static_cast<unsigned long>(itemTimeDrawPair.endpointMatches),
        static_cast<unsigned long>(itemTimeDrawPair.endpointMismatches),
        static_cast<unsigned long>(itemTimeDrawPair.canonicalFallbacks),
        static_cast<unsigned long>(itemTimeDrawPair.ownerFallbacks),
        static_cast<unsigned long>(itemTimeDrawPair.loadFallbacks),
        static_cast<unsigned long>(itemTimeDrawPair.scriptFallbacks),
        static_cast<unsigned long>(itemTimeDrawPair.spriteFallbacks),
        static_cast<unsigned long>(itemTimeDrawPair.visibilityFallbacks),
        static_cast<unsigned long>(itemTimeDrawPair.rotationFallbacks),
        static_cast<unsigned long>(itemTimeDrawPair.scaleFallbacks),
        static_cast<unsigned long>(itemTimeDrawPair.nonfiniteFallbacks),
        static_cast<unsigned long>(itemTimeDrawPair.textureFallbacks),
        static_cast<unsigned long>(itemTimeDrawPair.stateFallbacks),
        static_cast<unsigned long>(itemTimeDrawPair.axisFallbacks),
        static_cast<unsigned long>(itemTimeDrawPair.endpointFallbacks),
        static_cast<unsigned long>(itemTimeDrawPair.capacityFallbacks),
        static_cast<unsigned long>(itemTimeDrawPair.backendFallbacks),
        static_cast<unsigned long>(itemTimeDrawPair.verticesSaved),
        static_cast<unsigned long>(itemTimeDrawPair.frontendBytesSaved),
        static_cast<unsigned long>(itemTimeDrawPair.backendBytesSaved),
        static_cast<unsigned long>(itemTimeDrawPair.maxRunLength),
        static_cast<unsigned long>(itemTimeDrawPair.peakCandidatesPerPass),
        static_cast<unsigned long>(itemTimeDrawPair.peakVisiblePerPass),
        static_cast<unsigned long>(itemTimeDrawPair.peakEligiblePerPass),
        static_cast<unsigned long>(itemTimeDrawPair.peakRunsPerPass),
        itemTimeDrawPair.peakStageFrame == 0xffffffffU
            ? -1L
            : static_cast<long>(itemTimeDrawPair.peakStageFrame),
        static_cast<unsigned long>(itemTimeDrawPair.cacheHits),
        static_cast<unsigned long>(itemTimeDrawPair.cacheRevalidations),
        static_cast<unsigned long>(itemTimeDrawPair.cacheGenerationChanges),
        static_cast<unsigned long>(itemTimeDrawPair.cacheValidationFailures),
        static_cast<unsigned long>(itemTimeDrawPair.semanticHash));
#endif

#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
    const th08::BulletMixedQuadStats &bulletMixedQuads =
        th08::GetBulletMixedQuadStats();
#if defined(TH08_PSP_BULLET_MIXED_QUADS_AUDIT) && \
    TH08_PSP_BULLET_MIXED_QUADS_AUDIT
    const char *const bulletMixedQuadsMode = "audit";
#else
    const char *const bulletMixedQuadsMode = "product";
#endif
    AppendLine(
        "BULLET_MIXED_QUADS kind=%s phase=%s mode=%s frame=%lu stage=%ld "
        "stage_frame=%lu passes=%lu owner_conflict_passes=%lu "
        "state_runs=%lu batches=%lu "
        "candidates=%lu eligible_prefix_quads=%lu general_quads=%lu "
        "sticky_general_quads=%lu nonfinite_fallbacks=%lu "
        "axis_fallbacks=%lu area_or_mirror_fallbacks=%lu "
        "z_or_w_fallbacks=%lu uv_fallbacks=%lu diffuse_fallbacks=%lu "
        "submitted_batches=%lu submitted_pair_quads=%lu "
        "submitted_general_quads=%lu backend_fallback_batches=%lu "
        "fail_closed_batches=%lu missing_run_batches=%lu "
        "invalid_range_batches=%lu canonical_recovery_draw_failures=%lu "
        "canonical_recovery_quads=%lu potential_frontend_vertices_saved=%lu "
        "potential_ge_vertices_saved=%lu "
        "submitted_frontend_vertices_saved=%llu "
        "submitted_ge_vertices_saved=%llu max_pair_prefix=%lu "
        "max_general_suffix=%lu\n",
        kind, phase != nullptr ? phase : "periodic", bulletMixedQuadsMode,
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        static_cast<unsigned long>(bulletMixedQuads.passes),
        static_cast<unsigned long>(bulletMixedQuads.ownerConflictPasses),
        static_cast<unsigned long>(bulletMixedQuads.stateRuns),
        static_cast<unsigned long>(bulletMixedQuads.batches),
        static_cast<unsigned long>(bulletMixedQuads.candidates),
        static_cast<unsigned long>(bulletMixedQuads.eligiblePrefixQuads),
        static_cast<unsigned long>(bulletMixedQuads.generalQuads),
        static_cast<unsigned long>(bulletMixedQuads.stickyGeneralQuads),
        static_cast<unsigned long>(bulletMixedQuads.nonfiniteFallbacks),
        static_cast<unsigned long>(bulletMixedQuads.axisFallbacks),
        static_cast<unsigned long>(bulletMixedQuads.areaOrMirrorFallbacks),
        static_cast<unsigned long>(bulletMixedQuads.zOrWFallbacks),
        static_cast<unsigned long>(bulletMixedQuads.uvFallbacks),
        static_cast<unsigned long>(bulletMixedQuads.diffuseFallbacks),
        static_cast<unsigned long>(bulletMixedQuads.submittedBatches),
        static_cast<unsigned long>(bulletMixedQuads.submittedPairQuads),
        static_cast<unsigned long>(bulletMixedQuads.submittedGeneralQuads),
        static_cast<unsigned long>(bulletMixedQuads.backendFallbackBatches),
        static_cast<unsigned long>(bulletMixedQuads.failClosedBatches),
        static_cast<unsigned long>(bulletMixedQuads.missingRunBatches),
        static_cast<unsigned long>(bulletMixedQuads.invalidRangeBatches),
        static_cast<unsigned long>(
            bulletMixedQuads.canonicalRecoveryDrawFailures),
        static_cast<unsigned long>(bulletMixedQuads.canonicalRecoveryQuads),
        static_cast<unsigned long>(bulletMixedQuads.frontendVerticesSaved),
        static_cast<unsigned long>(bulletMixedQuads.geVerticesSaved),
        static_cast<unsigned long long>(bulletMixedQuads.submittedPairQuads) *
            2ULL,
        static_cast<unsigned long long>(bulletMixedQuads.submittedPairQuads) *
            4ULL,
        static_cast<unsigned long>(bulletMixedQuads.maxPairPrefix),
        static_cast<unsigned long>(bulletMixedQuads.maxGeneralSuffix));
#endif

#if TH08_PSP_ITEM_MIXED_QUADS_ENABLED
    const th08::ItemMixedQuadStats &itemMixedQuads =
        th08::GetItemMixedQuadStats();
#if defined(TH08_PSP_ITEM_MIXED_QUADS_AUDIT) && \
    TH08_PSP_ITEM_MIXED_QUADS_AUDIT
    const char *const itemMixedQuadsMode = "audit";
#else
    const char *const itemMixedQuadsMode = "product";
#endif
    AppendLine(
        "ITEM_MIXED_QUADS kind=%s phase=%s mode=%s frame=%lu stage=%ld "
        "stage_frame=%lu passes=%lu owner_conflict_passes=%lu "
        "state_runs=%lu batches=%lu candidates=%lu "
        "eligible_prefix_quads=%lu general_quads=%lu "
        "sticky_general_quads=%lu nonfinite_fallbacks=%lu "
        "axis_fallbacks=%lu area_or_mirror_fallbacks=%lu "
        "z_or_w_fallbacks=%lu uv_fallbacks=%lu diffuse_fallbacks=%lu "
        "submitted_batches=%lu submitted_pair_quads=%lu "
        "submitted_general_quads=%lu backend_fallback_batches=%lu "
        "fail_closed_batches=%lu missing_run_batches=%lu "
        "invalid_range_batches=%lu canonical_recovery_draw_failures=%lu "
        "canonical_recovery_quads=%lu potential_frontend_vertices_saved=%lu "
        "potential_ge_vertices_saved=%lu "
        "submitted_frontend_vertices_saved=%llu "
        "submitted_ge_vertices_saved=%llu max_pair_prefix=%lu "
        "max_general_suffix=%lu\n",
        kind, phase != nullptr ? phase : "periodic", itemMixedQuadsMode,
        static_cast<unsigned long>(gGameFrame),
        static_cast<long>(th08::g_GameManager.currentStage),
        static_cast<unsigned long>(th08::g_GameManager.stageActiveFrames),
        static_cast<unsigned long>(itemMixedQuads.passes),
        static_cast<unsigned long>(itemMixedQuads.ownerConflictPasses),
        static_cast<unsigned long>(itemMixedQuads.stateRuns),
        static_cast<unsigned long>(itemMixedQuads.batches),
        static_cast<unsigned long>(itemMixedQuads.candidates),
        static_cast<unsigned long>(itemMixedQuads.eligiblePrefixQuads),
        static_cast<unsigned long>(itemMixedQuads.generalQuads),
        static_cast<unsigned long>(itemMixedQuads.stickyGeneralQuads),
        static_cast<unsigned long>(itemMixedQuads.nonfiniteFallbacks),
        static_cast<unsigned long>(itemMixedQuads.axisFallbacks),
        static_cast<unsigned long>(itemMixedQuads.areaOrMirrorFallbacks),
        static_cast<unsigned long>(itemMixedQuads.zOrWFallbacks),
        static_cast<unsigned long>(itemMixedQuads.uvFallbacks),
        static_cast<unsigned long>(itemMixedQuads.diffuseFallbacks),
        static_cast<unsigned long>(itemMixedQuads.submittedBatches),
        static_cast<unsigned long>(itemMixedQuads.submittedPairQuads),
        static_cast<unsigned long>(itemMixedQuads.submittedGeneralQuads),
        static_cast<unsigned long>(itemMixedQuads.backendFallbackBatches),
        static_cast<unsigned long>(itemMixedQuads.failClosedBatches),
        static_cast<unsigned long>(itemMixedQuads.missingRunBatches),
        static_cast<unsigned long>(itemMixedQuads.invalidRangeBatches),
        static_cast<unsigned long>(
            itemMixedQuads.canonicalRecoveryDrawFailures),
        static_cast<unsigned long>(itemMixedQuads.canonicalRecoveryQuads),
        static_cast<unsigned long>(itemMixedQuads.frontendVerticesSaved),
        static_cast<unsigned long>(itemMixedQuads.geVerticesSaved),
        static_cast<unsigned long long>(itemMixedQuads.submittedPairQuads) *
                4ULL +
            static_cast<unsigned long long>(
                itemMixedQuads.submittedGeneralQuads) * 2ULL,
        static_cast<unsigned long long>(itemMixedQuads.submittedPairQuads) *
            4ULL,
        static_cast<unsigned long>(itemMixedQuads.maxPairPrefix),
        static_cast<unsigned long>(itemMixedQuads.maxGeneralSuffix));
#endif
}
} // namespace

extern "C" void *th08_psp_tracked_malloc(std::size_t size, const char *owner)
{
    if (size == 0)
        size = 1;
    void *memory = std::malloc(size);
    if (memory == nullptr)
    {
        __sync_fetch_and_add(&gTrackedFailureCount, 1);
        RecordAllocationEvent(AllocationEventKind::Failure, nullptr, size, 0, owner);
        return nullptr;
    }

    const std::size_t usable = malloc_usable_size(memory);
    const std::uint32_t live = __sync_add_and_fetch(
        &gTrackedLiveBytes, static_cast<std::uint32_t>(usable));
    UpdatePeak(&gTrackedPeakBytes, live);
    __sync_fetch_and_add(&gTrackedAllocationCount, 1);
    if (size >= kLargeAllocationThreshold)
        RecordAllocationEvent(AllocationEventKind::Allocate, memory, size, usable, owner);
    return memory;
}

extern "C" void *th08_psp_tracked_realloc(void *memory, std::size_t size, const char *owner)
{
    if (memory == nullptr)
        return th08_psp_tracked_malloc(size, owner);
    if (th08::psp::RenderResourceArenaContains(memory))
    {
        __sync_fetch_and_add(&gTrackedFailureCount, 1);
        RecordAllocationEvent(AllocationEventKind::Failure, memory, size, 0,
                              "render arena generic realloc rejected");
        return nullptr;
    }
    if (size == 0)
        size = 1;

    const std::size_t oldUsable = malloc_usable_size(memory);
    void *resized = std::realloc(memory, size);
    if (resized == nullptr)
    {
        __sync_fetch_and_add(&gTrackedFailureCount, 1);
        RecordAllocationEvent(AllocationEventKind::Failure, nullptr, size, 0, owner);
        return nullptr;
    }

    const std::size_t newUsable = malloc_usable_size(resized);
    if (newUsable >= oldUsable)
    {
        const std::uint32_t live = __sync_add_and_fetch(
            &gTrackedLiveBytes, static_cast<std::uint32_t>(newUsable - oldUsable));
        UpdatePeak(&gTrackedPeakBytes, live);
    }
    else
    {
        __sync_fetch_and_sub(&gTrackedLiveBytes,
                             static_cast<std::uint32_t>(oldUsable - newUsable));
    }
    if (oldUsable >= kLargeAllocationThreshold || newUsable >= kLargeAllocationThreshold)
        RecordAllocationEvent(AllocationEventKind::Resize, resized, size, newUsable, owner);
    return resized;
}

extern "C" void th08_psp_tracked_free(void *memory)
{
    if (memory == nullptr)
        return;
    if (th08::psp::StagePoolArenaFreeIdleTransient(memory))
        return;
    if (th08::psp::AnmScratchContains(memory))
    {
        // The retained arena has an exact manager-owned idx+generation
        // lease. A generic free can only be stale or an ownership bug.
        th08::psp::AnmScratchRejectGenericFree(memory);
        return;
    }
    if (th08::psp::RenderResourceArenaFree(memory))
        return;
    const std::size_t usable = malloc_usable_size(memory);
    if (usable >= kLargeAllocationThreshold)
        RecordAllocationEvent(AllocationEventKind::Free, memory, usable, usable, "tracked");
    __sync_fetch_and_sub(&gTrackedLiveBytes, static_cast<std::uint32_t>(usable));
    std::free(memory);
}

namespace th08::psp
{
void MemoryTelemetryInitialize(const char *gameDirectory)
{
    if (gInitialized)
        return;
    if (gameDirectory != nullptr && gameDirectory[0] != '\0')
    {
        std::snprintf(gTelemetryPath, sizeof(gTelemetryPath), "%s/%s", gameDirectory,
                      "TH08PSP_MEMORY.LOG");
    }
    std::FILE *file = std::fopen(gTelemetryPath, "wb");
    if (file != nullptr)
        std::fclose(file);
    AudioTelemetryReset();
    RadialTrailTelemetryReset();
#if TH08_PSP_X87_TRIG_CACHE_ENABLED
    X87TrigCacheReset();
#endif
    th08::PspResetBulletLiveEnumTelemetry();
#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT) && \
    TH08_PSP_BULLET_TRANSFORM_AUDIT
    th08::PspResetBulletTransformAuditTelemetry();
#endif
#if defined(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH) && \
    TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH
    th08::PspResetBulletTransformTerminalTelemetry();
#endif
#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT) && \
    TH08_PSP_BULLET_COLLISION_GATE_AUDIT
    th08::PspResetBulletCollisionGateAuditTelemetry();
#endif
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
    ResetBulletMixedQuadIntervalIfReady();
#endif
#if TH08_PSP_ITEM_MIXED_QUADS_ENABLED
    ResetItemMixedQuadIntervalIfReady();
#endif
#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
    ResetItemNaturalQuadInterval();
#endif
#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED
    ResetEffectIndexedQuadInterval();
#endif
#if defined(TH08_PSP_BULLET_CANCEL_SPATIAL) && \
    TH08_PSP_BULLET_CANCEL_SPATIAL
    ResetBulletCancelSpatialInterval();
#endif
    gInitialized = true;
    RenderPerfTelemetryReset();
    gLastSampleTimeUs = sceKernelGetSystemTimeWide();
    gLastSampleFrame = gGameFrame;
#if TH08_PSP_STAGE_RELATIVE_PERF_SAMPLING
    gStageRelativePerfSampling = StageRelativePerfSamplingState{};
#endif

    AppendLine("TH08 PSP MEMORY TELEMETRY v1 SC_ONLY=1 ME=0 quality_reduction=0\n");
    AppendLine(
        "ITEM_TIME_DRAW_PAIR_STORAGE sidecar_bytes=512 "
        "variants=off_audit_product_equal staging=AnmManager_vertexBuffer "
        "backend_pack=in_place_28_to_24 heap_arena_bytes=0 "
        "owner_tokens=ItemTime_Bullet_Extensible\n");
#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
    AppendLine(
        "ITEM_NATURAL_QUADS_POLICY mode=product default_off=1 "
        "counter_scope=stage_relative_interval cumulative=0 "
        "existing_flush=1 begin_end_added=0 topology=6v_to_4v_indexed "
        "frontend_bytes_saved=0 backend_bytes_saved_per_quad=48 "
        "source_corners=0,1,2,5 duplicate_topology=1eq3,2eq4 "
        "native_no_copy_forbidden=1 native_copy_enabled=%d "
        "native_copy_owned_same_call=%d client_owned_same_call=1 "
        "canonical_fallback=untouched_6v_one_draw no_extra_prepare_state=1 "
        "gameplay_rng_vm_replay_unchanged=1 me=0\n",
        TH08_PSP_ITEM_NATURAL_NATIVE_COPY_ENABLED,
        TH08_PSP_ITEM_NATURAL_NATIVE_COPY_ENABLED);
#endif
#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED
    AppendLine(
        "EFFECT_INDEXED_QUADS_POLICY mode=product default_off=1 "
        "counter_scope=sample_interval cumulative=0 interval_frames=300 "
        "owner=game_frame_thread marks_peek=0 ordinary_effect_only=1 "
        "radial_trail_excluded=1 topology=6v_to_4v_indexed "
        "vertex_stride_bytes=%lu begin_end_boundaries=1 "
        "success_savings_per_quad_vertices=2 "
        "gameplay_rng_vm_replay_unchanged=1 me=0\n",
        static_cast<unsigned long>(sizeof(th08::VertexTex1DiffuseXyzrhw)));
#endif
#if defined(TH08_PSP_BULLET_CANCEL_SPATIAL) && \
    TH08_PSP_BULLET_CANCEL_SPATIAL
    AppendLine(
        "BULLET_CANCEL_SPATIAL_POLICY mode=product default_off=1 "
        "counter_scope=sample_interval cumulative=0 interval_frames=300 "
        "owner=game_frame_thread marks_peek=0 grid=12x12 cell_size=64 "
        "fixed_bss_off_on=1 heap_arena_bytes=0 "
        "source=authoritative_cancel_regions query_owner=bullet_runtime_bits "
        "uncovered=return_zero covered=canonical_ascending_exact "
        "uncertain=canonical_full_scan duplicate_second_call=side_effect_replay "
        "gameplay_rng_update_order_replay_unchanged=1 me=0\n");
#endif
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
    AppendLine(
        "BULLET_MIXED_QUADS_POLICY counter_scope=sample_window "
        "mark_is_non_destructive=1 stage_relative_baseline_resets=1 "
        "shared_sidecar_bytes=512 fixed_bss=1 heap_arena_bytes=0 "
        "item_manager_reset_may_precede_first_stage_baseline=1 "
        "telemetry_reset_scope=bullet_stats_only "
        "accept_requires_fail_closed_missing_run_invalid_range_"
        "recovery_draw_failures_all_zero=1\n");
#endif
#if TH08_PSP_ITEM_MIXED_QUADS_ENABLED
    AppendLine(
        "ITEM_MIXED_QUADS_POLICY counter_scope=sample_window "
        "mark_is_non_destructive=1 stage_relative_baseline_resets=1 "
        "shared_sidecar_bytes=512 fixed_bss=1 heap_arena_bytes=0 "
        "owner=Item order=linked_list canonical_audit_vertices=6 "
        "product_prefix_vertices=2 product_suffix_vertices=4 "
        "potential_frontend_saved=pair_x4_plus_general_x2 "
        "potential_ge_saved=pair_x4 "
        "accept_requires_fail_closed_missing_run_invalid_range_"
        "recovery_draw_failures_all_zero=1\n");
#endif
#if (defined(TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) && \
     TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) || \
    (defined(TH08_PSP_ITEM_MIXED_QUADS_FASTPATH) && \
     TH08_PSP_ITEM_MIXED_QUADS_FASTPATH)
    AppendLine(
        "MIXED_GE_BACKEND_TELEMETRY_POLICY counter_scope=device_lifetime "
        "cumulative=1 owner_counters=separate shared_arena_cursor=1 "
        "present_fenced=1 mark_is_non_destructive=1 query_is_read_only=1 "
        "accept_requires_owner_fallbacks_and_arena_exhaustions_zero=1\n");
#endif
    AppendLine("COUNTER_POLICY reported_peak=per_frame_manager_counter exact=phase_and_300_frame_scan "
               "exact_sampled_peak_is_lower_bound=1\n");
    AppendLine("RENDER_COUNTER_POLICY frame_boundary=present interval=300_game_frames "
               "totals_and_per_present_peaks=1 hot_path_io=0 hot_path_allocation=0 "
               "state_requested=d3d_setter_invocations "
               "state_emitted=prepare_and_client_array_gl_state_calls "
               "immediate_vertex_attributes_excluded=1 "
               "upload_scope=d3d8_all_psp_texture_transfer_paths "
               "setup_thread_transfer_fields_atomic=1 "
               "text_bytes=text_raster_source_rect_bytes "
               "ascii_popup_batch_saved=visible_sprites_x4_vertices_x28_bytes\n");
    AppendLine("BULLET_ENUM_COUNTER_POLICY interval=sample_window "
               "mark_is_non_destructive=1 "
               "stage_relative_baseline_resets=1\n");
#if defined(TH08_PSP_BULLET_TRANSFORM_AUDIT) && \
    TH08_PSP_BULLET_TRANSFORM_AUDIT
    AppendLine("BULLET_TRANSFORM_AUDIT_POLICY interval=sample_window "
               "mark_is_non_destructive=1 "
               "stage_relative_baseline_resets=1 "
               "canonical_path_always_runs=1\n");
#endif
#if defined(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH) && \
    TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH
    AppendLine("BULLET_TRANSFORM_TERMINAL_POLICY interval=sample_window "
               "mark_is_non_destructive=1 "
               "stage_relative_baseline_resets=1 sidecar_bytes=192 "
               "initial_spawn_advance=canonical "
               "skip_scope=fired_terminal_bit_only "
               "replay_audit_repairs_false_positive=1\n");
#endif
#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT) && \
    TH08_PSP_BULLET_COLLISION_GATE_AUDIT
    AppendLine("BULLET_COLLISION_GATE_AUDIT_POLICY interval=sample_window "
               "mark_is_non_destructive=1 "
               "stage_relative_baseline_resets=1 canonical_always_runs=1 "
               "sidecar_required=1 product_fastpath=0 fixed_bss=1\n");
#endif
#if defined(TH08_PSP_SCORE_POPUP_NATIVE_GE) && \
    TH08_PSP_SCORE_POPUP_NATIVE_GE
    AppendLine(
        "SCORE_POPUP_NATIVE_GE_TELEMETRY_POLICY "
        "counter_scope=device_lifetime cumulative=1 "
        "mark_is_non_destructive=1 query_is_read_only=1 "
        "record_precedes_general_snapshot=1 hot_path_io=0 allocation=0\n");
#endif
#if defined(TH08_PSP_BULLET_PACKED_VERTEX_AUDIT) && \
    TH08_PSP_BULLET_PACKED_VERTEX_AUDIT
    AppendLine(
        "BULLET_PACKED_VERTEX_AUDIT_TELEMETRY_POLICY "
        "counter_scope=device_lifetime cumulative=1 mark_is_non_destructive=1 "
        "query_is_read_only=1 canonical_authority=direct_ge_4v_indexed "
        "candidate_storage=stack_96B candidate_submits=0 product_enabled=0 "
        "gameplay_rng_vm_order_unchanged=1 me=0 topology=4v "
        "algebra_attempts=eligible_batches_plus_owner_state_index_capacity "
        "algebra_eligible_batches=canonical_native_submits_plus_submit_fallbacks "
        "algebra_eligible_quads=matched_quads_plus_mismatch_quads "
        "algebra_diffuse_quads=uniform_diffuse_plus_per_vertex_diffuse "
        "algebra_canonical_fallbacks=sum_owner_state_index_capacity_submit "
        "accept_requires_mismatch_fallbacks_per_vertex_diffuse_zero=1\n");
#endif
#if defined(TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH) && \
    TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH
    AppendLine(
        "BULLET_PACKED_VERTEX_FASTPATH_TELEMETRY_POLICY "
        "counter_scope=device_lifetime cumulative=1 mark_is_non_destructive=1 "
        "query_is_read_only=1 writer=frontend_final_quad "
        "storage=direct_ge_present_fenced source_stride=28 packed_stride=24 "
        "canonical_flush_arg2_diffuse_modeled_purely=1 "
        "native_fallback=same_24B_client_array "
        "gameplay_rng_vm_order_unchanged=1 me=0 topology=4v_indexed "
        "algebra_begin_attempts=accepted_plus_canonical_fallback "
        "accept_requires_append_attempts_equal_appended_quads=1 "
        "algebra_packed_vertices=4x_appended_quads "
        "algebra_appended_quads=submitted_quads_plus_abandoned_quads "
        "algebra_submitted_runs=native_plus_client_only_when_split_zero "
        "algebra_submitted_quads=native_quads_plus_client_fallback_quads "
        "algebra_diffuse_quads=uniform_plus_per_vertex "
        "full_arena_preflight_quads=1536 normal_extra_flush=0 "
        "unexpected_recovery=submit_prefix_then_canonical_split "
        "accept_requires_all_fallback_abandoned_split_per_vertex_zero=1\n");
#endif
#if defined(TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH) && \
    TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH
    AppendLine(
        "ITEM_TIME_SPAWN_INIT_FASTPATH_POLICY counter_scope=manager_lifetime "
        "cumulative=1 reset_scope=ItemManager::Initialize "
        "cache_scope=immutable_ready_compact_anm_load_generation "
        "fallback=canonical_on_any_identity_or_validation_mismatch "
        "replay_hash_proves_gameplay_state_only=1 "
        "draw_fields_require_same_frame_pixel_gate=1 pixel_gate=pending\n");
#endif
#if TH08_PSP_STAGE_RELATIVE_PERF_SAMPLING
    AppendLine("STAGE_RELATIVE_PERF_POLICY enabled=1 period_stage_frames=%lu "
               "first_positive_frame_is_baseline=1 sample_at=baseline_plus_period "
               "boundary=after_present baseline_present=discarded "
               "interval=next_300_complete_presents "
               "rearm=stage_change_or_rewind_or_zero duplicate_stage_frame=ignore\n",
               static_cast<unsigned long>(kSamplePeriodFrames));
#endif
    AppendLine("AUDIO_COUNTER_POLICY scope=PSP_SC_SDL_MIXER cumulative=1 counter_bits=32_wrap "
               "callback_frames=stereo_sample_frames nonzero_samples=interleaved_channel_samples "
               "active_voices=current_at_callback_entry bgm_notify=notification_offsets_crossed "
               "bgm_refill_bytes=successful_nominal_notify_chunk_bytes "
               "bgm_skip=unsafe_write_or_inactive_wakeup exact_device_underrun=NA\n");
    AppendLine("POOL_STATIC enemy_shell=%lu enemy_bytes=%lu enemy_slots=481 "
               "bullet_shell=%lu bullet_bytes=%lu bullet_slots=%u laser_bytes=%lu laser_slots=%u "
               "item_shell=%lu item_bytes=%lu item_slots=%u arena_payload=%lu "
               "bss_pool_policy=PSP_STAGE_ARENA\n",
               static_cast<unsigned long>(sizeof(th08::EnemyManager)),
               static_cast<unsigned long>(sizeof(th08::Enemy) * th08::psp::kEnemyPoolStorageCount),
               static_cast<unsigned long>(sizeof(th08::BulletManager)),
               static_cast<unsigned long>(sizeof(th08::Bullet) * th08::psp::kBulletPoolStorageCount),
               0x600U,
               static_cast<unsigned long>(sizeof(th08::Laser) * th08::psp::kLaserPoolStorageCount),
               0x100U,
               static_cast<unsigned long>(sizeof(th08::ItemManager)),
               static_cast<unsigned long>(sizeof(th08::Item) * th08::psp::kItemPoolStorageCount),
               static_cast<unsigned int>(MAX_ITEMS),
               static_cast<unsigned long>(
                   sizeof(th08::Enemy) * th08::psp::kEnemyPoolStorageCount +
                   sizeof(th08::Bullet) * th08::psp::kBulletPoolStorageCount +
                   sizeof(th08::Laser) * th08::psp::kLaserPoolStorageCount +
                   sizeof(th08::Item) * th08::psp::kItemPoolStorageCount));
    FlushAllocationEvents();
    LogSnapshot("MARK", "telemetry_init", 0, 0, nullptr);
}

void MemoryTelemetryMarkPhase(const char *phase)
{
    if (!gInitialized)
        return;
    UpdatePoolHighWater();
    FlushAllocationEvents();
    const std::uint64_t now = sceKernelGetSystemTimeWide();
    // MARK records are observational.  In particular they neither consume the
    // render interval nor move the stage-relative timer/sampling baseline.
    LogSnapshot("MARK", phase, gGameFrame - gLastSampleFrame, now - gLastSampleTimeUs,
                nullptr);
}

void MemoryTelemetrySampleGameFrame()
{
    if (!gInitialized)
        return;
    __sync_fetch_and_add(&gGameFrame, 1);
    UpdatePoolHighWater();
// The stage-relative path is deliberately scheduled by
// MemoryTelemetryAfterPresent.  Keep this pre-draw callback limited to the
// game-frame/high-water observations shared with production telemetry.
#if !TH08_PSP_STAGE_RELATIVE_PERF_SAMPLING
    if (gGameFrame - gLastSampleFrame < kSamplePeriodFrames)
        return;

    const std::uint64_t now = sceKernelGetSystemTimeWide();
    const std::uint32_t elapsedFrames = gGameFrame - gLastSampleFrame;
    const std::uint64_t elapsedUs = now - gLastSampleTimeUs;
    const RenderPerfIntervalSnapshot renderPerf = RenderPerfTelemetryTakeInterval();
    FlushAllocationEvents();
    LogSnapshot("SAMPLE", "periodic", elapsedFrames, elapsedUs, &renderPerf);
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
    ResetBulletMixedQuadIntervalIfReady();
#endif
#if TH08_PSP_ITEM_MIXED_QUADS_ENABLED
    ResetItemMixedQuadIntervalIfReady();
#endif
#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
    ResetItemNaturalQuadInterval();
#endif
#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED
    ResetEffectIndexedQuadInterval();
#endif
#if defined(TH08_PSP_BULLET_CANCEL_SPATIAL) && \
    TH08_PSP_BULLET_CANCEL_SPATIAL
    ResetBulletCancelSpatialInterval();
#endif
    gLastSampleFrame = gGameFrame;
    gLastSampleTimeUs = now;
#endif
}

void MemoryTelemetryAfterPresent()
{
#if TH08_PSP_STAGE_RELATIVE_PERF_SAMPLING
    if (!gInitialized)
        return;

    const std::int32_t stage = static_cast<std::int32_t>(g_GameManager.currentStage);
    const std::uint32_t stageFrame = g_GameManager.stageActiveFrames;
    const bool stageChanged = !gStageRelativePerfSampling.stageObserved ||
                              stage != gStageRelativePerfSampling.stage;
    const bool stageRewound = gStageRelativePerfSampling.stageObserved &&
                              !stageChanged &&
                              stageFrame < gStageRelativePerfSampling.lastObservedStageFrame;

    if (stageChanged || stageRewound || stageFrame == 0U)
        RearmStageRelativePerfSampling(stage, stageFrame);
    if (stageFrame == 0U)
        return;

    if (!gStageRelativePerfSampling.baselineReady)
    {
        BeginStageRelativePerfBaseline(stage, stageFrame,
                                       stageRewound ? "rewind" : "first_positive");
        return;
    }

    // Dialogue, pause and terminal states can present repeatedly without
    // advancing stageActiveFrames.  Never emit a second sample for one stage
    // frame and never let those repeats move the stage-frame target.
    if (stageFrame == gStageRelativePerfSampling.lastObservedStageFrame)
        return;
    gStageRelativePerfSampling.lastObservedStageFrame = stageFrame;

    const std::uint32_t target = gStageRelativePerfSampling.nextSampleStageFrame;
    if (target == 0U || stageFrame < target)
        return;
    if (stageFrame > target)
    {
        // stageActiveFrames normally advances one at a time.  If an unusual
        // caller skips the exact boundary, discard the partial interval rather
        // than label a non-identical A/B window as comparable.
        BeginStageRelativePerfBaseline(stage, stageFrame, "missed_target");
        return;
    }

    const std::uint64_t now = sceKernelGetSystemTimeWide();
    const std::uint32_t elapsedFrames = gGameFrame - gLastSampleFrame;
    const std::uint64_t elapsedUs = now - gLastSampleTimeUs;
    const RenderPerfIntervalSnapshot renderPerf = RenderPerfTelemetryTakeInterval();
    const std::uint32_t baselineStageFrame =
        gStageRelativePerfSampling.baselineStageFrame;
    FlushAllocationEvents();
    LogSnapshot("SAMPLE", "stage_relative_periodic", elapsedFrames, elapsedUs,
                &renderPerf);
    AppendLine(
        "STAGE_RELATIVE_PERF_SAMPLE stage=%ld baseline_stage_frame=%lu "
        "sample_stage_frame=%lu elapsed_stage_frames=%lu elapsed_game_frames=%lu "
        "render_frames=%lu\n",
        static_cast<long>(stage),
        static_cast<unsigned long>(baselineStageFrame),
        static_cast<unsigned long>(stageFrame),
        static_cast<unsigned long>(stageFrame - baselineStageFrame),
        static_cast<unsigned long>(elapsedFrames),
        static_cast<unsigned long>(renderPerf.frames));
#if TH08_PSP_BULLET_MIXED_QUADS_ENABLED
    // The just-emitted line owns the completed interval.  Reset only Bullet's
    // counters before moving the baseline; MARK records remain pure peeks.
    ResetBulletMixedQuadIntervalIfReady();
#endif
#if TH08_PSP_ITEM_MIXED_QUADS_ENABLED
    ResetItemMixedQuadIntervalIfReady();
#endif
#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
    ResetItemNaturalQuadInterval();
#endif
#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED
    ResetEffectIndexedQuadInterval();
#endif
#if defined(TH08_PSP_BULLET_CANCEL_SPATIAL) && \
    TH08_PSP_BULLET_CANCEL_SPATIAL
    ResetBulletCancelSpatialInterval();
#endif
    // EndFrame has already closed the sample Present.  Start the following
    // timer after logger I/O so it does not tax the next comparable window.
    gLastSampleFrame = gGameFrame;
    gLastSampleTimeUs = sceKernelGetSystemTimeWide();
    gStageRelativePerfSampling.baselineStageFrame = stageFrame;
    if (stageFrame <= kMaximumStageFrame - kSamplePeriodFrames)
        gStageRelativePerfSampling.nextSampleStageFrame =
            stageFrame + kSamplePeriodFrames;
    else
        gStageRelativePerfSampling.nextSampleStageFrame = 0U;
#endif
}

void MemoryTelemetryShutdown()
{
    if (!gInitialized)
        return;
    MemoryTelemetryMarkPhase("shutdown");
    gInitialized = false;
}
} // namespace th08::psp
