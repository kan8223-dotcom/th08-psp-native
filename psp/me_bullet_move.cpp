#include "me_bullet_move.hpp"

#if defined(PSP) && defined(TH08_PSP_ME_BULLET_MOVE) && TH08_PSP_ME_BULLET_MOVE

#include "AnmManager.hpp"
#include "BulletManager.hpp"
#include "GameManager.hpp"
#include <cstdint>
#include "fileio.hpp"
#include "me_core.hpp"

#include <cstring>
#include <cstddef>

extern "C" void sceKernelDcacheWritebackAll(void);
// Media Engine cache maintenance (me-core); only called when job->onMe.
extern "C" void meLibDcacheInvalidateRange(unsigned int addr, unsigned int size);
extern "C" void meLibDcacheWritebackRange(unsigned int addr, unsigned int size);
extern "C" void sceKernelDcacheWritebackInvalidateAll(void);
extern "C" int sceKernelDelayThread(unsigned int us);
extern "C" unsigned int sceKernelGetSystemTimeLow(void);

#ifndef TH08_PSP_ME_BULLET_MOVE_SHARE
#define TH08_PSP_ME_BULLET_MOVE_SHARE 100
#endif

using namespace th08;

// Runs on the ME too: no game function calls, no globals, every RAM pointer
// (bullets, flags, sprite) goes through job->addrMask (kseg0 alias on the ME;
// the ME cannot dereference the SC's user-segment addresses).  On the ME only
// the cache lines that hold the fields used are invalidated / written back.
namespace
{
inline void MeInvalidate(bool onMe, std::uint32_t addr, std::uint32_t bytes)
{
    if (onMe)
        meLibDcacheInvalidateRange(addr & ~63U, ((addr & 63U) + bytes + 63U) & ~63U);
}
inline void MeWriteback(bool onMe, std::uint32_t addr, std::uint32_t bytes)
{
    if (onMe)
        meLibDcacheWritebackRange(addr & ~63U, ((addr & 63U) + bytes + 63U) & ~63U);
}
} // namespace

extern "C" void th08_me_bullet_move_kernel(PspMeBulletMoveJob *job)
{
    const std::uint32_t mask = job->addrMask;
    const bool onMe = job->onMe != 0U;
    unsigned char *flags = reinterpret_cast<unsigned char *>(job->flagsAddr | mask);
    const std::uint32_t stride = job->strideBytes;
    const std::uint32_t base = job->bulletsAddr | mask;
    const std::uint32_t *bits =
        job->activeBitsAddr != 0U ? reinterpret_cast<const std::uint32_t *>(job->activeBitsAddr | mask) : NULL;
    if (bits != NULL)
        MeInvalidate(onMe, job->activeBitsAddr | mask, 48U * 4U);
    std::uint32_t processed = 0U, moved = 0U, deactivated = 0U, anmDone = 0U;
    for (std::uint32_t i = job->first; i < job->last; ++i)
    {
        flags[i] = 0U;
        if (bits != NULL && ((bits[i >> 5U] >> (i & 31U)) & 1U) == 0U)
            continue;
        const std::uint32_t baddr = base + i * stride;
        Bullet *const b = reinterpret_cast<Bullet *>(baddr);
        MeInvalidate(onMe, baddr + job->offA, job->lenA);
        if (b->state != BULLET_STATE_FIRED)
            continue;
        ++processed;
        if (b->activeTransformFlags != 0U)
            continue;
        // AdvanceTransformProgram would return at once (terminal program).
        MeInvalidate(onMe, baddr + job->offTransformIndex, 4U);
        const i32 tIndex = b->transformIndex;
        if (tIndex < 18)
        {
            MeInvalidate(onMe, baddr + job->offTransforms + tIndex * job->transformRecordBytes + job->offKindInRecord, 4U);
            if (b->transforms[tIndex].kind != BULLET_TRANSFORM_NONE)
                continue;
        }
        unsigned char f = 1U;
        if (b->offscreenCullDelayFrames != 0)
            --b->offscreenCullDelayFrames;
        if (!job->freeze)
            b->position += b->velocity;
        if (b->offscreenCullDelayFrames == 0)
        {
            MeInvalidate(onMe, baddr + job->offLoadedSprite, 4U);
            const std::uint32_t spriteAddr = static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(b->sprites.bulletVm.loadedSprite) | mask);
            MeInvalidate(onMe, spriteAddr, 64U);
            const AnmLoadedSprite *const sprite = reinterpret_cast<const AnmLoadedSprite *>(spriteAddr);
            // GameManager::IsWithinPlayfield(x, y, widthPx, heightPx)
            const f32 x = b->position.x, y = b->position.y;
            const f32 width = sprite->widthPx, height = sprite->heightPx;
            const bool inside = !(width / 2.0f + x < 0.0f) && !(x - width / 2.0f > 384.0f) &&
                                !(height / 2.0f + y < 0.0f) && !(y - height / 2.0f > 448.0f);
            if (!inside)
            {
                if (b->offscreenFrames == 0)
                {
                    flags[i] = 1U | 2U;
                    ++moved;
                    ++deactivated;
                    MeWriteback(onMe, baddr + job->offA, job->lenA);
                    continue;
                }
                --b->offscreenFrames;
            }
            else
                b->offscreenFrames = 0;
        }
        ++moved;
        // The ANM step stays on the SC (bullet scripts normally have no
        // instruction left to run: then the SC skips it as well).
        MeInvalidate(onMe, baddr + job->offCurrentInstruction, 4U);
        if (b->sprites.bulletVm.currentInstruction == NULL)
        {
            f |= 4U;
            ++anmDone;
        }
        flags[i] = f;
        MeWriteback(onMe, baddr + job->offA, job->lenA);
    }
    MeWriteback(onMe, job->flagsAddr | mask, job->last);
    job->processed = processed;
    job->moved = moved;
    job->deactivated = deactivated;
    job->anmDone = anmDone;
    job->anmAborted = 0U;
}

extern "C" const unsigned int *th08_psp_bullet_active_bits(void);
namespace
{
PspMeBulletMoveJob gJob __attribute__((aligned(64)));
unsigned char gFlags[0x600] __attribute__((aligned(64)));
std::uint32_t gActiveBits[48] __attribute__((aligned(64)));
bool gPending = false, gValid = false, gOnMe = false, gMeDead = false;
unsigned long gKicks = 0UL, gOnMeCount = 0UL, gScCount = 0UL, gWaits = 0UL, gWaitUs = 0UL, gLate = 0UL,
              gProcessed = 0UL, gMoved = 0UL, gDeactivated = 0UL, gAnmDone = 0UL, gAnmAborted = 0UL, gSkippedFrames = 0UL;
} // namespace

extern "C" void th08_psp_me_bullet_move_kick(void)
{
    gValid = false;
    if (gPending)
        return; // the previous job was never consumed (no bullet update ran): keep it
    // BulletManager::OnUpdate returns at once when this flag is set.
    if (((*reinterpret_cast<u32 *>(&g_GameManager.flags) >> 10) & 1) != 0 || g_BulletManager.bullets == NULL)
    {
        ++gSkippedFrames;
        return;
    }
    std::memset(&gJob, 0, sizeof(gJob));
    gJob.first = 0U;
    gJob.last = static_cast<std::uint32_t>((0x600UL * TH08_PSP_ME_BULLET_MOVE_SHARE) / 100UL);
    gJob.bulletsAddr = reinterpret_cast<std::uintptr_t>(g_BulletManager.bullets);
    gJob.strideBytes = sizeof(Bullet);
    gJob.flagsAddr = reinterpret_cast<std::uintptr_t>(gFlags);
    gJob.freeze = g_GameManager.scriptedUpdateFreeze ? 1U : 0U;
    gJob.addrMask = 0U; // the SC path dereferences user addresses directly
    gJob.onMe = 0U;
    // Occupancy bitmap (bit i = slot i) copied next to the job so the ME reads a
    // .bss address; the SC's scan order does not matter for the ME.
    gJob.activeBitsAddr = 0U;
    if (const unsigned int *const liveBits = th08_psp_bullet_active_bits())
    {
        std::memcpy(gActiveBits, liveBits, sizeof(gActiveBits));
        gJob.activeBitsAddr = reinterpret_cast<std::uintptr_t>(gActiveBits);
    }
    gJob.probe = ((gKicks % 300UL) == 0UL || gKicks < 3UL) ? 1U : 0U;
    gJob.scFired = 0U;
    if (gJob.probe)
        for (std::uint32_t i = 0U; i < 0x600U; ++i)
            if (g_BulletManager.bullets[i].state == BULLET_STATE_FIRED)
                ++gJob.scFired;
    {
        const std::uint32_t o[] = {static_cast<std::uint32_t>(offsetof(Bullet, position)),
                                   static_cast<std::uint32_t>(offsetof(Bullet, velocity)),
                                   static_cast<std::uint32_t>(offsetof(Bullet, offscreenCullDelayFrames)),
                                   static_cast<std::uint32_t>(offsetof(Bullet, activeTransformFlags)),
                                   static_cast<std::uint32_t>(offsetof(Bullet, state)),
                                   static_cast<std::uint32_t>(offsetof(Bullet, offscreenFrames))};
        std::uint32_t lo = o[0], hi = o[0];
        for (int k = 0; k < 6; ++k)
        {
            if (o[k] < lo) lo = o[k];
            if (o[k] > hi) hi = o[k];
        }
        gJob.offA = lo;
        gJob.lenA = hi + 12U - lo; // the widest member is a Float3
    }
    gJob.offTransformIndex = static_cast<std::uint32_t>(offsetof(Bullet, transformIndex));
    gJob.offTransforms = static_cast<std::uint32_t>(offsetof(Bullet, transforms));
    gJob.transformRecordBytes = static_cast<std::uint32_t>(sizeof(BulletTransformRecord));
    gJob.offKindInRecord = static_cast<std::uint32_t>(offsetof(BulletTransformRecord, kind));
    gJob.offLoadedSprite = static_cast<std::uint32_t>(offsetof(Bullet, sprites.bulletVm.loadedSprite));
    gJob.offState = static_cast<std::uint32_t>(offsetof(Bullet, state));
    gJob.offCurrentInstruction = static_cast<std::uint32_t>(offsetof(Bullet, sprites.bulletVm.currentInstruction));
    ++gKicks;
    // Everything the ME reads (bullets spawned this frame, ANM tables, globals)
    // must be in RAM.
    sceKernelDcacheWritebackAll();
    if (!gMeDead && th08_me_core_ready() && th08_me_core_submit_job_kind(&gJob, 5U))
    {
        gOnMe = true;
        ++gOnMeCount;
    }
    else
    {
        th08_me_bullet_move_kernel(&gJob);
        gOnMe = false;
        ++gScCount;
    }
    gPending = true;
}

extern "C" void th08_psp_me_bullet_move_wait(void)
{
    if (!gPending)
    {
        gValid = false;
        return;
    }
    gPending = false;
    if (gOnMe)
    {
        const unsigned int t0 = sceKernelGetSystemTimeLow();
        unsigned waited = 0U;
        bool done = th08_me_core_job_done(&gJob) != 0;
        while (!done)
        {
            if (waited >= 200000U)
                break;
            sceKernelDelayThread(20);
            waited += 20U;
            done = th08_me_core_job_done(&gJob) != 0;
        }
        const unsigned int dt = sceKernelGetSystemTimeLow() - t0;
        if (dt > 30U)
        {
            ++gWaits;
            gWaitUs += dt;
        }
        if (!done)
        {
            // The ME never finished: stop using it and run the canonical loop.
            ++gLate;
            gMeDead = true;
            gValid = false;
            th08::psp::BootLog("ME_BMOVE timeout: ME disabled for bullet movement\n");
            sceKernelDcacheWritebackInvalidateAll();
            return;
        }
        // Drop every cached line: the ME rewrote bullets and the flags.
        sceKernelDcacheWritebackInvalidateAll();
    }
    gValid = true;
    if (gOnMe)
    {
        static unsigned long unreadable = 0UL, reported = 0UL;
        if (gJob.probe && gJob.scFired != 0U)
        {
            if (gJob.meFired != gJob.scFired)
            {
                ++unreadable;
                gValid = false;
                if (unreadable >= 3UL && !gMeDead)
                {
                    gMeDead = true;
                    th08::psp::BootLog("ME_BMOVE unreadable: sc_fired=%lu me_fired=%lu me_last=%lu bullets=%08lx -> ME disabled\n",
                                       static_cast<unsigned long>(gJob.scFired), static_cast<unsigned long>(gJob.meFired),
                                       static_cast<unsigned long>(gJob.meLast), static_cast<unsigned long>(gJob.bulletsAddr));
                }
                return;
            }
            if (reported < 3UL)
            {
                ++reported;
                th08::psp::BootLog("ME_BMOVE probe ok sc_fired=%lu me_fired=%lu processed=%lu moved=%lu bitmap=%d\n",
                                   static_cast<unsigned long>(gJob.scFired), static_cast<unsigned long>(gJob.meFired),
                                   static_cast<unsigned long>(gJob.processed), static_cast<unsigned long>(gJob.moved),
                                   gJob.activeBitsAddr != 0U ? 1 : 0);
            }
        }
    }
    gProcessed += gJob.processed;
    gMoved += gJob.moved;
    gDeactivated += gJob.deactivated;
    gAnmDone += gJob.anmDone;
    gAnmAborted += gJob.anmAborted;
    if ((gKicks % 600UL) == 0UL)
        th08::psp::BootLog("ME_BMOVE stats kicks=%lu on_me=%lu sc=%lu waits=%lu wait_us=%lu late=%lu skipped=%lu "
                           "fired=%lu moved=%lu deact=%lu anm=%lu anm_abort=%lu share=%d\n",
                           gKicks, gOnMeCount, gScCount, gWaits, gWaitUs, gLate, gSkippedFrames, gProcessed, gMoved,
                           gDeactivated, gAnmDone, gAnmAborted, TH08_PSP_ME_BULLET_MOVE_SHARE);
}

extern "C" const unsigned char *th08_psp_me_bullet_move_flags(void)
{
    return gValid ? gFlags : NULL;
}

#else
extern "C" void th08_me_bullet_move_kernel(PspMeBulletMoveJob *) {}
extern "C" void th08_psp_me_bullet_move_kick(void) {}
extern "C" void th08_psp_me_bullet_move_wait(void) {}
extern "C" const unsigned char *th08_psp_me_bullet_move_flags(void) { return 0; }
#endif
