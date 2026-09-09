// See me_bullet_adopt.hpp.  The kernel mirrors AnmManager::DrawNoRotation /
// rotated Draw2D (BuildBulletOnePassRotatedQuad4V) + DrawInner + the
// d3d8_compat pre-transformed vertex conversion, expression by expression.
#include "me_bullet_adopt.hpp"

#include "fileio.hpp"
#include "me_core.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#if defined(PSP)
#include <pspkernel.h>
#endif

namespace
{
inline unsigned int MeBulletMixColors(unsigned int c1, unsigned int c2)
{
    unsigned int color = (c1 * c2) / 128U;
    if (color >= 256U)
        color = 255U;
    return color;
}
inline std::uint32_t MeBulletFiniteCarry(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7fffffffU) + 0x00800000U;
}
} // namespace

extern "C" void th08_me_bullet_kernel(PspMeBulletJob *job, const PspMeBulletRecord *records, PspMeClientVertex *out,
                                      unsigned short *status)
{
    const float zeroPointFive = 0.5f;
    unsigned int drawn = 0U;
    for (unsigned int i = 0U; i < job->count; ++i)
    {
        const PspMeBulletRecord r = records[i];
        if (r.kind == 0xFFU)
        {
            status[i] = TH08_ME_BULLET_STATUS_SKIP;
            continue;
        }
        float px[4], py[4];
        const float x = (r.sizeX * r.scaleX) / 2.0f;
        const float y = (r.sizeY * r.scaleY) / 2.0f;
        if (r.kind == 1U)
        {
            // TranslateRotation for the four corners, then anchor.
            const float xOffset = r.posX, yOffset = r.posY, sine = r.sine, cosine = r.cosine;
            { const float ax = -x, ay = -y; px[0] = ax * cosine - ay * sine + xOffset; py[0] = ax * sine + ay * cosine + yOffset; }
            { const float ax = x, ay = -y;  px[1] = ax * cosine - ay * sine + xOffset; py[1] = ax * sine + ay * cosine + yOffset; }
            { const float ax = -x, ay = y;  px[2] = ax * cosine - ay * sine + xOffset; py[2] = ax * sine + ay * cosine + yOffset; }
            { const float ax = x, ay = y;   px[3] = ax * cosine - ay * sine + xOffset; py[3] = ax * sine + ay * cosine + yOffset; }
            if (r.anchor & 1U)
                for (int k = 0; k < 4; ++k)
                    px[k] += x;
            if (r.anchor & 2U)
                for (int k = 0; k < 4; ++k)
                    py[k] += y;
        }
        else
        {
            if ((r.anchor & 1U) == 0U)
            {
                px[0] = px[2] = r.posX - x;
                px[1] = px[3] = x + r.posX;
            }
            else
            {
                px[0] = px[2] = r.posX;
                px[1] = px[3] = x + r.posX + x;
            }
            if ((r.anchor & 2U) == 0U)
            {
                py[0] = py[1] = r.posY - y;
                py[2] = py[3] = y + r.posY;
            }
            else
            {
                py[0] = py[1] = r.posY;
                py[2] = py[3] = y + r.posY + y;
            }
        }
        // DrawInner: shake, rounding (axis-aligned only), UV, cull.
        for (int k = 0; k < 4; ++k)
        {
            px[k] += job->shakeX;
            py[k] += job->shakeY;
        }
        if (r.kind == 0U)
        {
            const float triangleX1 = nearbyintf(px[0]) - zeroPointFive;
            const float triangleX2 = nearbyintf(px[1]) - zeroPointFive;
            const float triangleY1 = nearbyintf(py[0]) - zeroPointFive;
            const float triangleY2 = nearbyintf(py[2]) - zeroPointFive;
            py[2] = py[3] = triangleY2;
            py[0] = py[1] = triangleY1;
            px[1] = px[3] = triangleX2;
            px[0] = px[2] = triangleX1;
        }
        const float u0 = r.uvStartX + r.scrollX, u1 = r.uvEndX + r.scrollX;
        const float v0 = r.uvStartY + r.scrollY, v1 = r.uvEndY + r.scrollY;
        std::uint32_t carry = 0U;
        for (int k = 0; k < 4; ++k)
            carry |= MeBulletFiniteCarry(px[k]) | MeBulletFiniteCarry(py[k]);
        carry |= MeBulletFiniteCarry(r.posZ) | MeBulletFiniteCarry(u0) | MeBulletFiniteCarry(u1) |
                 MeBulletFiniteCarry(v0) | MeBulletFiniteCarry(v1);
        float maxX = px[0] > px[1] ? px[0] : px[1];
        maxX = px[2] > maxX ? px[2] : maxX;
        maxX = px[3] > maxX ? px[3] : maxX;
        float maxY = py[0] > py[1] ? py[0] : py[1];
        maxY = py[2] > maxY ? py[2] : maxY;
        maxY = py[3] > maxY ? py[3] : maxY;
        float minX = px[0] < px[1] ? px[0] : px[1];
        minX = px[2] < minX ? px[2] : minX;
        minX = px[3] < minX ? px[3] : minX;
        float minY = py[0] < py[1] ? py[0] : py[1];
        minY = py[2] < minY ? py[2] : minY;
        minY = py[3] < minY ? py[3] : minY;
        const bool culled = (carry & 0x80000000U) != 0U || maxX < job->vpX || maxY < job->vpY ||
                            minX > (job->vpX + job->vpW) || minY > (job->vpY + job->vpH);
        if (culled || drawn >= job->outCapacity)
        {
            status[i] = TH08_ME_BULLET_STATUS_CULLED;
            continue;
        }
        unsigned int color = r.flag17 ? r.color2 : r.color1;
        if (job->useMixColor)
        {
            const unsigned int m = job->mixColor;
            const unsigned int b = MeBulletMixColors(color & 255U, m & 255U);
            const unsigned int g = MeBulletMixColors((color >> 8) & 255U, (m >> 8) & 255U);
            const unsigned int rr = MeBulletMixColors((color >> 16) & 255U, (m >> 16) & 255U);
            const unsigned int a = MeBulletMixColors((color >> 24) & 255U, (m >> 24) & 255U);
            color = (a << 24) | (rr << 16) | (g << 8) | b;
        }
        PspMeClientVertex *v = out + drawn * 4U;
        const float zOut = 1.0f - 2.0f * r.posZ;
        for (int k = 0; k < 4; ++k)
        {
            v[k].u = (k & 1) ? u1 : u0;
            v[k].v = (k & 2) ? v1 : v0;
            v[k].r = static_cast<unsigned char>((color >> 16) & 255U);
            v[k].g = static_cast<unsigned char>((color >> 8) & 255U);
            v[k].b = static_cast<unsigned char>(color & 255U);
            v[k].a = static_cast<unsigned char>((color >> 24) & 255U);
            v[k].x = px[k] + 0.5f;
            v[k].y = py[k] + 0.5f;
            v[k].z = zOut;
        }
        status[i] = static_cast<unsigned short>(drawn);
        ++drawn;
    }
    job->drawn = drawn;
}

#if TH08_PSP_ME_BULLET_ANY_ENABLED
namespace
{
PspMeBulletRecord gRecords[TH08_ME_BULLET_MAX_RECORDS] __attribute__((aligned(64)));
unsigned short gStatus[TH08_ME_BULLET_MAX_RECORDS] __attribute__((aligned(64)));
const void *gTag[TH08_ME_BULLET_MAX_RECORDS];
PspMeBulletJob gJob __attribute__((aligned(64)));
unsigned gCount = 0U;
bool gCapturing = false, gLive = false, gOnMe = false, gAcquired = false;
PspMeClientVertex *gOut = nullptr;
unsigned long gStats[TH08_ME_BULLET_STAT_COUNT];

template <typename T> inline volatile T *Uncached(T *p)
{
    return reinterpret_cast<volatile T *>(0x40000000u | reinterpret_cast<std::uintptr_t>(p));
}
inline unsigned RoundLines(unsigned bytes) { return (bytes + 63U) & ~63U; }
} // namespace

extern "C" void th08_me_bullet_adopt_note(unsigned int what)
{
    if (what < TH08_ME_BULLET_STAT_COUNT)
        ++gStats[what];
}

extern "C" int th08_me_bullet_adopt_begin(float shakeX, float shakeY, float vpX, float vpY, float vpW, float vpH,
                                          unsigned int useMixColor, unsigned int mixColor)
{
    ++gStats[TH08_ME_BULLET_STAT_FRAMES];
    if ((gStats[TH08_ME_BULLET_STAT_FRAMES] % 600UL) == 0UL)
    {
        th08::psp::BootLog("ME_BULLET stats frames=%lu captured=%lu submitted=%lu on_me=%lu adopted=%lu late=%lu "
                           "fb_state=%lu fb_order=%lu fb_color=%lu audit_cull=%lu audit_quad=%lu records=%lu "
                           "drawn=%lu groups=%lu overflow=%lu busy=%lu no_arena=%lu\n",
                           gStats[0], gStats[1], gStats[2], gStats[3], gStats[4], gStats[5], gStats[6], gStats[7],
                           gStats[8], gStats[9], gStats[10], gStats[11], gStats[12], gStats[13], gStats[14],
                           gStats[15], gStats[16]);
        th08::psp::FlushBootLog();
    }
    if (gAcquired)
        return 0;
    if (gLive)
    {
        // Superseded job (draw skipped a frame): the ME must be done with the buffers.
        if (gOnMe && th08_me_core_job_done(&gJob) == 0)
        {
            ++gStats[TH08_ME_BULLET_STAT_BUSY];
            return 0;
        }
        gLive = false;
    }
    gCount = 0U;
    std::memset(&gJob, 0, sizeof(gJob));
    gJob.shakeX = shakeX; gJob.shakeY = shakeY;
    gJob.vpX = vpX; gJob.vpY = vpY; gJob.vpW = vpW; gJob.vpH = vpH;
    gJob.useMixColor = useMixColor; gJob.mixColor = mixColor;
    gCapturing = true;
    ++gStats[TH08_ME_BULLET_STAT_CAPTURED];
    return 1;
}

extern "C" int th08_me_bullet_adopt_add(const void *tag, const PspMeBulletRecord *record)
{
    if (!gCapturing)
        return 0;
    if (gCount >= TH08_ME_BULLET_MAX_RECORDS)
        return 0;
    gRecords[gCount] = *record;
    gTag[gCount] = tag;
    ++gCount;
    return 1;
}

extern "C" void th08_me_bullet_adopt_abort(void)
{
    if (!gCapturing)
        return;
    gCapturing = false;
    ++gStats[TH08_ME_BULLET_STAT_OVERFLOW];
}

extern "C" void th08_me_bullet_adopt_submit(void *outVertices, unsigned int outCapacityQuads)
{
    if (!gCapturing)
        return;
    gCapturing = false;
    gStats[TH08_ME_BULLET_STAT_RECORDS] += gCount;
    if (gCount == 0U)
        return;
    if (outVertices == nullptr || outCapacityQuads == 0U)
    {
        ++gStats[TH08_ME_BULLET_STAT_NO_ARENA];
        return;
    }
    gOut = static_cast<PspMeClientVertex *>(outVertices);
    gJob.count = gCount;
    gJob.outCapacity = outCapacityQuads;
    gJob.recordsAddr = reinterpret_cast<std::uintptr_t>(gRecords);
    gJob.outAddr = reinterpret_cast<std::uintptr_t>(gOut);
    gJob.statusAddr = reinterpret_cast<std::uintptr_t>(gStatus);
    sceKernelDcacheWritebackRange(gRecords, RoundLines(sizeof(PspMeBulletRecord) * gCount));
    sceKernelDcacheWritebackRange(&gJob, sizeof(gJob));
    bool onMe = false;
    if (th08_me_core_ready())
        onMe = th08_me_core_submit_job_kind(&gJob, 3U) != 0;
    if (!onMe)
    {
        th08_me_bullet_kernel(const_cast<PspMeBulletJob *>(reinterpret_cast<volatile PspMeBulletJob *>(Uncached(&gJob))),
                              gRecords,
                              const_cast<PspMeClientVertex *>(reinterpret_cast<volatile PspMeClientVertex *>(Uncached(gOut))),
                              const_cast<unsigned short *>(reinterpret_cast<volatile unsigned short *>(Uncached(gStatus))));
    }
    else
        ++gStats[TH08_ME_BULLET_STAT_ON_ME];
    gOnMe = onMe;
    gLive = true;
    ++gStats[TH08_ME_BULLET_STAT_SUBMITTED];
}

extern "C" int th08_me_bullet_adopt_state_matches(float shakeX, float shakeY, float vpX, float vpY, float vpW,
                                                  float vpH, unsigned int useMixColor, unsigned int mixColor)
{
    if (!gLive)
        return 2; // nothing submitted this frame (no bullets): not a fallback
    return gJob.shakeX == shakeX && gJob.shakeY == shakeY && gJob.vpX == vpX && gJob.vpY == vpY && gJob.vpW == vpW &&
                   gJob.vpH == vpH && gJob.useMixColor == useMixColor && gJob.mixColor == mixColor
               ? 1
               : 0;
}

extern "C" int th08_me_bullet_adopt_acquire(unsigned int waitUs, unsigned int *recordCount, unsigned int *drawnQuads)
{
    if (!gLive || gAcquired)
        return 0;
    if (gOnMe)
    {
        unsigned waited = 0U;
        while (th08_me_core_job_done(&gJob) == 0)
        {
            if (waited >= waitUs)
            {
                ++gStats[TH08_ME_BULLET_STAT_LATE];
                return 0;
            }
            sceKernelDelayThread(20);
            waited += 20U;
        }
    }
    // The producer wrote status/job through RAM (ME writeback or SC uncached):
    // drop stale lines before cached reads.  The vertex arena is only read by the GE.
    sceKernelDcacheInvalidateRange(gStatus, RoundLines(sizeof(unsigned short) * gCount));
    sceKernelDcacheInvalidateRange(&gJob, sizeof(gJob));
    gAcquired = true;
    *recordCount = gCount;
    *drawnQuads = gJob.drawn;
    ++gStats[TH08_ME_BULLET_STAT_ADOPTED];
    gStats[TH08_ME_BULLET_STAT_DRAWN] += gJob.drawn;
    return 1;
}

extern "C" const void *th08_me_bullet_adopt_tag(unsigned int i) { return gTag[i]; }
extern "C" unsigned int th08_me_bullet_adopt_status(unsigned int i) { return gStatus[i]; }
extern "C" const PspMeClientVertex *th08_me_bullet_adopt_vertices(void) { return gOut; }
extern "C" void th08_me_bullet_adopt_release(void)
{
    if (!gAcquired)
        return;
    gAcquired = false;
    gLive = false;
}
#endif
