#include "me_core.hpp"

#include "fileio.hpp"
#include "usage_meter.hpp"
#include "me_effect_shadow.hpp"
#include "me_bullet_adopt.hpp"
#include "me_bg_adopt.hpp"
#include "me_bullet_move.hpp"
#include "me_popup.hpp"

#if TH08_PSP_ME_CORE_ENABLED
extern "C"
{
#include "me-core.h"
}
#include <kubridge.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psppower.h>
#include <cstring>

namespace
{
enum : std::uint32_t
{
    kCmdNone = 0U,
    kCmdHello = 1U,
    kCmdEffect = 2U,
    kCmdBullet = 3U,
    kCmdBg = 4U,
    kCmdBulletMove = 5U,
    kCmdPopup = 6U,
    kSlots = 8U,
    kStateBooting = 0U,
    kStateReady = 1U,
    kStateSuspended = 2U,
    kHelloWords = 1024U,
};

// One cache line, always accessed through the uncached alias on both CPUs.
// Two independent job slots so an effect job and a bullet job can be queued
// in the same frame; the ME serves slot 0 first.
struct __attribute__((aligned(64))) MeMailbox
{
    volatile std::uint32_t command[kSlots];
    volatile std::uint32_t state;
    volatile std::uint32_t jobId[kSlots];
    volatile std::uint32_t jobDone[kSlots];
    volatile std::uint32_t arg0[kSlots];
    volatile std::uint32_t result;
    volatile std::uint32_t cycles[kSlots];
    volatile std::uint32_t suspendRequested;
    volatile std::uint32_t heartbeat;
    volatile std::uint32_t pad[4];
};
static_assert(sizeof(MeMailbox) == 192U, "mailbox must stay whole cache lines");

MeMailbox gMailbox __attribute__((aligned(64)));
std::uint32_t gHelloBuffer[kHelloWords] __attribute__((aligned(64)));

inline volatile MeMailbox *Box()
{
    return reinterpret_cast<volatile MeMailbox *>(0x40000000u | reinterpret_cast<std::uintptr_t>(&gMailbox));
}

inline std::uint32_t ReadCount()
{
    std::uint32_t value;
    __asm__ volatile("mfc0 %0, $9" : "=r"(value));
    return value;
}

// SC-side state.
bool gInitAttempted = false;
bool gReady = false;
bool gPowerLocked = false;
const char *gFeature = "DISABLED";
std::uint32_t gNextJob = 0U;
bool gJobPending = false;
std::uint32_t gPendingFrames = 0U;
unsigned long gJobsSubmitted = 0UL, gJobsDone = 0UL, gTimeouts = 0UL, gLastCycles = 0UL, gFrames = 0UL;
unsigned long gCyclesAccum = 0UL;
unsigned long gLastEffectRequestFrame = 0UL;
bool gEffectEverRequested = false;
unsigned long gEffectRefused = 0UL;
std::uint32_t gExpectedSum = 0U;
unsigned long gMismatch = 0UL;

bool RunningUnderPpsspp()
{
    SceIoStat stat;
    return sceIoGetstat("ms0:/PSP/SYSTEM/ppsspp.ini", &stat) >= 0;
}
} // namespace

// ---------------------------------------------------------------- ME side --
// Executed on the Media Engine.  Only touches uncached aliases and CP0.
extern "C" void meLibOnProcess(void)
{
    volatile MeMailbox *box = Box();
    // The ME's CP0 Count does not advance after reset until written once.
    __asm__ volatile("mtc0 $0, $9\n\tsync\n\tnop\n\tnop" : : : "memory");
    meLibDcacheWritebackInvalidateAll();
    meLibIcacheInvalidateAll();
    for (std::uint32_t slot = 0U; slot < kSlots; ++slot)
        box->command[slot] = kCmdNone;
    __asm__ volatile("sync");
    box->state = kStateReady;
    __asm__ volatile("sync");
    for (;;)
    {
        while (box->command[0] == kCmdNone && box->command[1] == kCmdNone && box->command[2] == kCmdNone &&
               box->command[3] == kCmdNone && box->command[4] == kCmdNone && box->command[5] == kCmdNone &&
               box->command[6] == kCmdNone && box->command[7] == kCmdNone && !box->suspendRequested)
        {
            box->heartbeat = box->heartbeat + 1U;
            __asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop;");
        }
        if (box->suspendRequested)
        {
            box->state = kStateSuspended;
            __asm__ volatile("sync");
            while (box->suspendRequested)
                __asm__ volatile("nop; nop; nop; nop;");
            box->state = kStateReady;
            __asm__ volatile("sync");
            continue;
        }
        for (std::uint32_t slot = 0U; slot < kSlots; ++slot)
        {
            const std::uint32_t command = box->command[slot];
            if (command == kCmdNone)
                continue;
            const std::uint32_t start = ReadCount();
            const std::uint32_t jobAddr = box->arg0[slot];
            if (command == kCmdEffect)
            {
                // Header via the uncached alias; bulk records/outputs through the
                // cached kseg0 alias with explicit invalidate/writeback (TH07 MERW rule).
                const volatile PspMeEffectJob *ujob = reinterpret_cast<const volatile PspMeEffectJob *>(0x40000000u | jobAddr);
                const std::uint32_t count = ujob->count;
                const std::uint32_t recAddr = ujob->recordsAddr, outAddr = ujob->outAddr;
                meLibDcacheInvalidateRange(0x80000000u | (jobAddr & ~63u), (sizeof(PspMeEffectJob) + 127u) & ~63u);
                meLibDcacheInvalidateRange(0x80000000u | (recAddr & ~63u), (count * sizeof(PspMeEffectRecord) + 127u) & ~63u);
                const PspMeEffectJob *job = reinterpret_cast<const PspMeEffectJob *>(0x80000000u | jobAddr);
                const PspMeEffectRecord *records = reinterpret_cast<const PspMeEffectRecord *>(0x80000000u | recAddr);
                PspMeEffectQuad *out = reinterpret_cast<PspMeEffectQuad *>(0x80000000u | outAddr);
                th08_me_effect_kernel(job, records, out);
                meLibDcacheWritebackRange(0x80000000u | (outAddr & ~63u), (count * sizeof(PspMeEffectQuad) + 127u) & ~63u);
            }
            else if (command == kCmdBullet)
            {
                const volatile PspMeBulletJob *ujob = reinterpret_cast<const volatile PspMeBulletJob *>(0x40000000u | jobAddr);
                const std::uint32_t count = ujob->count;
                const std::uint32_t recAddr = ujob->recordsAddr, outAddr = ujob->outAddr, statusAddr = ujob->statusAddr;
                const std::uint32_t outCapacity = ujob->outCapacity;
                meLibDcacheInvalidateRange(0x80000000u | (jobAddr & ~63u), (sizeof(PspMeBulletJob) + 127u) & ~63u);
                meLibDcacheInvalidateRange(0x80000000u | (recAddr & ~63u), (count * sizeof(PspMeBulletRecord) + 127u) & ~63u);
                PspMeBulletJob *job = reinterpret_cast<PspMeBulletJob *>(0x80000000u | jobAddr);
                const PspMeBulletRecord *records = reinterpret_cast<const PspMeBulletRecord *>(0x80000000u | recAddr);
                PspMeClientVertex *out = reinterpret_cast<PspMeClientVertex *>(0x80000000u | outAddr);
                unsigned short *status = reinterpret_cast<unsigned short *>(0x80000000u | statusAddr);
                th08_me_bullet_kernel(job, records, out, status);
                // The vertex arena is GE-read only: write back what was produced,
                // the status words and the job header (drawn count).
                meLibDcacheWritebackRange(0x80000000u | (outAddr & ~63u),
                                          (outCapacity * 4u * sizeof(PspMeClientVertex) + 127u) & ~63u);
                meLibDcacheWritebackRange(0x80000000u | (statusAddr & ~63u), (count * sizeof(unsigned short) + 127u) & ~63u);
                meLibDcacheWritebackRange(0x80000000u | (jobAddr & ~63u), (sizeof(PspMeBulletJob) + 127u) & ~63u);
            }
            else if (command == kCmdBulletMove)
            {
                // Header through the uncached alias (like the other jobs); the
                // kernel invalidates/writes back only the lines it touches.
                volatile PspMeBulletMoveJob *const ujob =
                    reinterpret_cast<volatile PspMeBulletMoveJob *>(0x40000000u | jobAddr);
                PspMeBulletMoveJob local;
                {
                    const volatile std::uint32_t *src = reinterpret_cast<const volatile std::uint32_t *>(ujob);
                    std::uint32_t *dst = reinterpret_cast<std::uint32_t *>(&local);
                    for (std::uint32_t k = 0U; k < sizeof(local) / 4U; ++k)
                        dst[k] = src[k];
                }
                local.processed = local.moved = local.deactivated = local.anmDone = local.anmAborted = 0U;
                local.meLast = local.last;
                local.meFired = 0U;
                bool run = true;
                if (local.probe)
                {
                    // Reachability probe through the uncached alias.
                    const std::uint32_t b0 = local.bulletsAddr, stride = local.strideBytes;
                    const std::uint32_t off = local.offState;
                    std::uint32_t seen = 0U;
                    for (std::uint32_t i = local.first; i < local.last; ++i)
                        if (*reinterpret_cast<volatile std::uint16_t *>(0x40000000u | (b0 + i * stride + off)) == 1U)
                            ++seen;
                    local.meFired = seen;
                    run = seen == local.scFired;
                }
                local.addrMask = 0x80000000u;
                local.onMe = 1U;
                if (run)
                    th08_me_bullet_move_kernel(&local);
                ujob->processed = local.processed;
                ujob->moved = local.moved;
                ujob->deactivated = local.deactivated;
                ujob->anmDone = local.anmDone;
                ujob->anmAborted = run ? 0x80000000u : 0U;
                ujob->meFired = local.meFired;
                ujob->meLast = local.meLast;
                __asm__ volatile("sync");
            }
            else if (command == kCmdPopup)
            {
                volatile PspMePopupJob *const ujob = reinterpret_cast<volatile PspMePopupJob *>(0x40000000u | jobAddr);
                PspMePopupJob local;
                {
                    const volatile std::uint32_t *src = reinterpret_cast<const volatile std::uint32_t *>(ujob);
                    std::uint32_t *dst = reinterpret_cast<std::uint32_t *>(&local);
                    for (std::uint32_t k = 0U; k < sizeof(local) / 4U; ++k)
                        dst[k] = src[k];
                }
                local.addrMask = 0x80000000u;
                local.onMe = 1U;
                th08_me_popup_kernel(&local);
                ujob->outCount = local.outCount;
                ujob->digits = local.digits;
                __asm__ volatile("sync");
            }
            else if (command == kCmdBg)
            {
                const volatile PspMeBgJob *ujob = reinterpret_cast<const volatile PspMeBgJob *>(0x40000000u | jobAddr);
                const std::uint32_t count = ujob->count;
                const std::uint32_t recAddr = ujob->recordsAddr, outAddr = ujob->outAddr, statusAddr = ujob->statusAddr;
                const std::uint32_t outCapacity = ujob->outCapacity;
                meLibDcacheInvalidateRange(0x80000000u | (jobAddr & ~63u), (sizeof(PspMeBgJob) + 127u) & ~63u);
                meLibDcacheInvalidateRange(0x80000000u | (recAddr & ~63u), (count * sizeof(PspMeBgRecord) + 127u) & ~63u);
                PspMeBgJob *job = reinterpret_cast<PspMeBgJob *>(0x80000000u | jobAddr);
                const PspMeBgRecord *records = reinterpret_cast<const PspMeBgRecord *>(0x80000000u | recAddr);
                PspMeClientVertex *out = reinterpret_cast<PspMeClientVertex *>(0x80000000u | outAddr);
                unsigned short *status = reinterpret_cast<unsigned short *>(0x80000000u | statusAddr);
                th08_me_bg_kernel(job, records, out, status);
                meLibDcacheWritebackRange(0x80000000u | (outAddr & ~63u),
                                          (outCapacity * 4u * sizeof(PspMeClientVertex) + 127u) & ~63u);
                meLibDcacheWritebackRange(0x80000000u | (statusAddr & ~63u), (count * sizeof(unsigned short) + 127u) & ~63u);
                meLibDcacheWritebackRange(0x80000000u | (jobAddr & ~63u), (sizeof(PspMeBgJob) + 127u) & ~63u);
            }
            else if (command == kCmdHello)
            {
                const volatile std::uint32_t *source =
                    reinterpret_cast<const volatile std::uint32_t *>(0x40000000u | jobAddr);
                std::uint32_t sum = 0U;
                for (std::uint32_t k = 0U; k < kHelloWords; ++k)
                    sum += source[k];
                box->result = sum;
            }
            box->cycles[slot] = ReadCount() - start;
            box->jobDone[slot] = box->jobId[slot];
            __asm__ volatile("sync");
            box->command[slot] = kCmdNone;
            __asm__ volatile("sync");
        }
    }
}

extern "C" void meLibOnSleep(void)
{
    Box()->suspendRequested = 1U;
    __asm__ volatile("sync");
}

extern "C" void th08_me_core_request_stop(void)
{
    // The static uncached mailbox exists even before ME initialization. This
    // is advisory only: OS exit must not wait for the current job to finish.
    meLibOnSleep();
}

extern "C" __attribute__((noinline, aligned(4))) void meLibOnWake(void)
{
}

// ---------------------------------------------------------------- SC side --
namespace
{
// In-flight jobs per mailbox slot (nullptr = free).  The hello job is tagged
// with gHelloBuffer.
const void *gSlotJob[kSlots];
std::uint32_t gSlotJobId[kSlots];
std::uint32_t gSlotFrames[kSlots];
unsigned long gEffectJobs = 0UL, gEffectJobsDone = 0UL, gEffectCycles = 0UL;
unsigned long gBulletJobs = 0UL, gBulletJobsDone = 0UL, gBulletCycles = 0UL;
const void *gBulletJobPtr = nullptr;

// Collect every finished job.
void PollSlots()
{
    volatile MeMailbox *box = Box();
    for (std::uint32_t slot = 0U; slot < kSlots; ++slot)
    {
        const void *job = gSlotJob[slot];
        if (job == nullptr)
            continue;
        if (box->jobDone[slot] != gSlotJobId[slot] || box->command[slot] != kCmdNone)
            continue;
        const std::uint32_t cycles = box->cycles[slot];
        gSlotJob[slot] = nullptr;
        th08::psp::UsageMeterAddMeCycles(cycles);
        if (job == gHelloBuffer)
        {
            gJobPending = false;
            ++gJobsDone;
            gLastCycles = cycles;
            gCyclesAccum += cycles;
            if (box->result != gExpectedSum)
                ++gMismatch;
        }
        else if (job == gBulletJobPtr)
        {
            ++gBulletJobsDone;
            gBulletCycles += cycles;
        }
        else
        {
            ++gEffectJobsDone;
            gEffectCycles += cycles;
        }
    }
}

bool AnyJobInFlight()
{
    for (std::uint32_t slot = 0U; slot < kSlots; ++slot)
        if (gSlotJob[slot] != nullptr)
            return true;
    return false;
}
}

extern "C" int th08_me_core_init(void)
{
    if (gInitAttempted)
        return gReady ? 1 : 0;
    gInitAttempted = true;
    if (RunningUnderPpsspp())
    {
        gFeature = "SKIPPED_PPSSPP";
        th08::psp::BootLog("ME_CORE skip=ppsspp\n");
        return 0;
    }
    const int model = kuKernelGetModel();
    if (model < 1)
    {
        gFeature = "SKIPPED_MODEL";
        th08::psp::BootLog("ME_CORE skip=model model=%d (Slim+ only)\n", model);
        return 0;
    }
    // Keep the PSP from suspending while the ME owns its core.
    const int lock = scePowerLock(0);
    gPowerLocked = lock == 0;
    th08::psp::BootLog("ME_CORE takeover=begin model=%d power_lock=%d\n", model, lock);
    th08::psp::FlushBootLogHard();
    volatile MeMailbox *box = Box();
    std::memset(&gMailbox, 0, sizeof(gMailbox));
    for (std::uint32_t i = 0U; i < kHelloWords; ++i)
        gHelloBuffer[i] = i * 2654435761U;
    sceKernelDcacheWritebackInvalidateAll();
    const std::uint32_t startUs = static_cast<std::uint32_t>(sceKernelGetSystemTimeLow());
    const int mapper = meLibDefaultInit();
    const std::uint32_t takeoverUs = static_cast<std::uint32_t>(sceKernelGetSystemTimeLow()) - startUs;
    if (mapper < 0)
    {
        gFeature = "TAKEOVER_FAILED";
        th08::psp::BootLog("ME_CORE takeover=FAILED mapper=%d us=%lu\n", mapper,
                           static_cast<unsigned long>(takeoverUs));
        th08::psp::FlushBootLogHard();
        return 0;
    }
    // Wait for the worker to report READY (it starts on the ME asynchronously).
    std::uint32_t waited = 0U;
    while (box->state != kStateReady && waited < 2000U)
    {
        sceKernelDelayThread(1000);
        ++waited;
    }
    gReady = box->state == kStateReady;
    gFeature = gReady ? "CORE" : "NO_READY";
    th08::psp::BootLog("ME_CORE takeover=%s mapper=%d us=%lu ready_wait_ms=%lu heartbeat=%lu\n",
                       gReady ? "OK" : "NO_READY", mapper, static_cast<unsigned long>(takeoverUs),
                       static_cast<unsigned long>(waited), static_cast<unsigned long>(box->heartbeat));
    th08::psp::FlushBootLogHard();
    return gReady ? 1 : 0;
}

extern "C" void th08_me_core_frame(void)
{
    if (!gReady)
        return;
    volatile MeMailbox *box = Box();
    ++gFrames;
    PollSlots();
    for (std::uint32_t slot = 0U; slot < kSlots; ++slot)
    {
        if (gSlotJob[slot] == nullptr)
        {
            gSlotFrames[slot] = 0U;
            continue;
        }
        if (++gSlotFrames[slot] > 120U)
        {
            ++gTimeouts;
            gReady = false;
            gFeature = "TIMEOUT";
            th08::psp::BootLog("ME_CORE timeout slot=%lu job=%lu state=%lu heartbeat=%lu\n",
                               static_cast<unsigned long>(slot), static_cast<unsigned long>(gSlotJobId[slot]),
                               static_cast<unsigned long>(box->state), static_cast<unsigned long>(box->heartbeat));
            th08::psp::FlushBootLogHard();
            return;
        }
    }
    // Hello heartbeat only while the game is not feeding jobs (menus,
    // dialogue): a recent job request keeps the slots free for the game.
    const bool effectActive = gEffectEverRequested && (gFrames - gLastEffectRequestFrame) < 60UL;
    if (!AnyJobInFlight() && !effectActive)
    {
        // Rotate the input so the checksum changes and the ME really reads it.
        gHelloBuffer[gFrames % kHelloWords] += 1U;
        sceKernelDcacheWritebackRange(gHelloBuffer, sizeof(gHelloBuffer));
        std::uint32_t sum = 0U;
        for (std::uint32_t i = 0U; i < kHelloWords; ++i)
            sum += gHelloBuffer[i];
        gExpectedSum = sum;
        box->arg0[0] = reinterpret_cast<std::uintptr_t>(gHelloBuffer);
        box->jobId[0] = ++gNextJob;
        gSlotJob[0] = gHelloBuffer;
        gSlotJobId[0] = gNextJob;
        gSlotFrames[0] = 0U;
        __asm__ volatile("sync");
        box->command[0] = kCmdHello;
        __asm__ volatile("sync");
        gJobPending = true;
        ++gJobsSubmitted;
    }
    if ((gFrames % 600UL) == 0UL)
    {
        th08::psp::BootLog("ME_CORE stats frames=%lu jobs=%lu done=%lu timeouts=%lu mismatch=%lu last_cycles=%lu "
                           "avg_cycles=%lu heartbeat=%lu effect_jobs=%lu effect_done=%lu effect_cycles_avg=%lu "
                           "effect_refused=%lu bullet_jobs=%lu bullet_done=%lu bullet_cycles_avg=%lu\n",
                           gFrames, gJobsSubmitted, gJobsDone, gTimeouts, gMismatch, gLastCycles,
                           gJobsDone != 0UL ? gCyclesAccum / gJobsDone : 0UL,
                           static_cast<unsigned long>(box->heartbeat), gEffectJobs, gEffectJobsDone,
                           gEffectJobsDone != 0UL ? gEffectCycles / gEffectJobsDone : 0UL, gEffectRefused, gBulletJobs,
                           gBulletJobsDone, gBulletJobsDone != 0UL ? gBulletCycles / gBulletJobsDone : 0UL);
        th08::psp::FlushBootLogHard();
    }
}

// Every RAM range a job hands to the ME must lie in user memory: a zero or
// stray address would make the ME write through the uncached alias into the
// kernel's low memory (physical 0).  Refuse such jobs (the SC path then runs
// with its own static buffers) and log the first ones.
namespace
{
unsigned long gBadAddr = 0UL;
bool RangeOk(std::uint32_t addr, std::uint32_t bytes, bool allowZero)
{
    if (addr == 0U)
        return allowZero;
    const std::uint32_t lo = 0x08400000U, hi = 0x0C000000U;
    return addr >= lo && addr < hi && bytes < hi - addr;
}
bool JobRangesOk(const void *job, unsigned int kind, const char **field, std::uint32_t *addr, std::uint32_t *bytes)
{
    struct R { const char *name; std::uint32_t addr, bytes; bool zeroOk; };
    R r[4]; unsigned n = 0U;
    if (kind == 2U)
    {
        const PspMeEffectJob *j = static_cast<const PspMeEffectJob *>(job);
        r[n++] = {"records", j->recordsAddr, j->count * static_cast<std::uint32_t>(sizeof(PspMeEffectRecord)), false};
        r[n++] = {"out", j->outAddr, j->count * static_cast<std::uint32_t>(sizeof(PspMeEffectQuad)), false};
    }
    else if (kind == 3U)
    {
        const PspMeBulletJob *j = static_cast<const PspMeBulletJob *>(job);
        r[n++] = {"records", j->recordsAddr, j->count * static_cast<std::uint32_t>(sizeof(PspMeBulletRecord)), false};
        r[n++] = {"out", j->outAddr, j->outCapacity * 4U * static_cast<std::uint32_t>(sizeof(PspMeClientVertex)), false};
        r[n++] = {"status", j->statusAddr, j->count * 2U, false};
    }
    else if (kind == 4U)
    {
        const PspMeBgJob *j = static_cast<const PspMeBgJob *>(job);
        r[n++] = {"records", j->recordsAddr, j->count * static_cast<std::uint32_t>(sizeof(PspMeBgRecord)), false};
        r[n++] = {"out", j->outAddr, j->outCapacity * 4U * static_cast<std::uint32_t>(sizeof(PspMeClientVertex)), false};
        r[n++] = {"status", j->statusAddr, j->count * 2U, false};
    }
    else if (kind == 5U)
    {
        const PspMeBulletMoveJob *j = static_cast<const PspMeBulletMoveJob *>(job);
        r[n++] = {"bullets", j->bulletsAddr, j->last * j->strideBytes, false};
        r[n++] = {"flags", j->flagsAddr, 0x600U, false};
        r[n++] = {"activeBits", j->activeBitsAddr, 48U * 4U, true};
    }
    else if (kind == 6U)
    {
        const PspMePopupJob *j = static_cast<const PspMePopupJob *>(job);
        r[n++] = {"records", j->recordsAddr, j->count * static_cast<std::uint32_t>(sizeof(PspMePopupRecord)), false};
        r[n++] = {"sprites", j->spritesAddr, TH08_ME_POPUP_SPRITES * static_cast<std::uint32_t>(sizeof(PspMePopupSprite)), false};
        r[n++] = {"out", j->outAddr, j->outCapacity * 4U * static_cast<std::uint32_t>(sizeof(PspMePopupVertex)), false};
    }
    for (unsigned i = 0U; i < n; ++i)
    {
        if (!RangeOk(r[i].addr, r[i].bytes, r[i].zeroOk))
        {
            *field = r[i].name; *addr = r[i].addr; *bytes = r[i].bytes;
            return false;
        }
    }
    return true;
}
} // namespace

extern "C" int th08_me_core_submit_job_kind(const void *job, unsigned int kind)
{
    if (!gReady || job == NULL || (kind != 2U && kind != 3U && kind != 4U && kind != 5U && kind != 6U))
        return 0;
    {
        const char *field = ""; std::uint32_t addr = 0U, bytes = 0U;
        if (!JobRangesOk(job, kind, &field, &addr, &bytes))
        {
            ++gBadAddr;
            if (gBadAddr <= 16UL)
            {
                th08::psp::BootLog("ME_BADADDR kind=%u field=%s addr=0x%08lx bytes=%lu frame=%lu\n", kind, field,
                                   static_cast<unsigned long>(addr), static_cast<unsigned long>(bytes), gFrames);
                th08::psp::FlushBootLogHard();
            }
            return 0;
        }
    }
    gLastEffectRequestFrame = gFrames;
    gEffectEverRequested = true;
    PollSlots();
    std::uint32_t slot = kSlots;
    for (std::uint32_t s = 0U; s < kSlots; ++s)
    {
        if (gSlotJob[s] == nullptr)
        {
            slot = s;
            break;
        }
    }
    if (slot == kSlots)
    {
        ++gEffectRefused;
        return 0;
    }
    volatile MeMailbox *box = Box();
    box->arg0[slot] = reinterpret_cast<std::uintptr_t>(job);
    box->jobId[slot] = ++gNextJob;
    gSlotJob[slot] = job;
    gSlotJobId[slot] = gNextJob;
    gSlotFrames[slot] = 0U;
    if (kind == 3U)
    {
        gBulletJobPtr = job;
        ++gBulletJobs;
    }
    else
        ++gEffectJobs;
    __asm__ volatile("sync");
    box->command[slot] = kind == 3U ? kCmdBullet
                         : (kind == 4U ? kCmdBg : (kind == 5U ? kCmdBulletMove : (kind == 6U ? kCmdPopup : kCmdEffect)));
    __asm__ volatile("sync");
    return 1;
}

extern "C" int th08_me_core_submit_job(const void *job) { return th08_me_core_submit_job_kind(job, 2U); }

extern "C" int th08_me_core_job_done(const void *job)
{
    PollSlots();
    if (job == NULL)
        return AnyJobInFlight() ? 0 : 1;
    for (std::uint32_t slot = 0U; slot < kSlots; ++slot)
        if (gSlotJob[slot] == job)
            return 0;
    return 1;
}

extern "C" void th08_me_core_shutdown(void)
{
    static bool done = false;
    if (!gInitAttempted || done)
        return;
    done = true;
    if (gReady)
    {
        Box()->suspendRequested = 1U;
        __asm__ volatile("sync");
        sceKernelDelayThread(2000);
    }
    th08::psp::BootLog("ME_CORE shutdown state=%lu jobs=%lu done=%lu timeouts=%lu\n",
                       static_cast<unsigned long>(Box()->state), gJobsSubmitted, gJobsDone, gTimeouts);
    if (gPowerLocked)
    {
        scePowerUnlock(0);
        gPowerLocked = false;
    }
}

extern "C" const char *th08_me_core_feature_string(void) { return gFeature; }
extern "C" int th08_me_core_ready(void) { return gReady ? 1 : 0; }
#else
extern "C" int th08_me_core_init(void) { return 0; }
extern "C" void th08_me_core_frame(void) {}
extern "C" void th08_me_core_shutdown(void) {}
extern "C" void th08_me_core_request_stop(void) {}
extern "C" const char *th08_me_core_feature_string(void) { return "DISABLED"; }
extern "C" int th08_me_core_submit_job_kind(const void *, unsigned int) { return 0; }
extern "C" int th08_me_core_ready(void) { return 0; }
#endif
