// See me_bg_adopt.hpp.  The kernel mirrors Background::RenderObjects (type 0
// billboards: PspProjectGeneral x2, projected width -> scale, sky fog colour
// mix, DrawNoRotationNoRound + DrawInner(vm, 0); type 1 strips: the two
// projections, half widths, fog colours, direction and QueueSpriteQuad
// vertices) expression by expression, then packs GE client vertices.
#include "me_bg_adopt.hpp"

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
inline unsigned int MeBgMix(unsigned int c1, unsigned int c2)
{
    unsigned int color = (c1 * c2) / 128U;
    if (color >= 256U)
        color = 255U;
    return color;
}

// D3DXVec3Project with world = identity + translation (wx, wy, wz) and
// source (sx, sy, sz): identical to psp/bg_fast_project.hpp PspProjectGeneral.
inline void MeBgProject(const PspMeBgJob *job, const float *vpRows, float wx, float wy, float wz, float sx,
                        float sy, float sz, float *ox, float *oy, float *oz)
{
    // world * view: rows 0..2 of view are the world rows, translation row =
    // (wx,wy,wz,1) * view.
    const float *v = job->view;
    const float tx = wx * v[0] + wy * v[4] + wz * v[8] + v[12];
    const float ty = wx * v[1] + wy * v[5] + wz * v[9] + v[13];
    const float tz = wx * v[2] + wy * v[6] + wz * v[10] + v[14];
    const float tw = wx * v[3] + wy * v[7] + wz * v[11] + v[15];
    const float *p = job->proj;
    // translation row of (world*view)*proj
    const float r30 = tx * p[0] + ty * p[4] + tz * p[8] + tw * p[12];
    const float r31 = tx * p[1] + ty * p[5] + tz * p[9] + tw * p[13];
    const float r32 = tx * p[2] + ty * p[6] + tz * p[10] + tw * p[14];
    const float r33 = tx * p[3] + ty * p[7] + tz * p[11] + tw * p[15];
    float cx = sx * vpRows[0] + sy * vpRows[4] + sz * vpRows[8] + r30;
    float cy = sx * vpRows[1] + sy * vpRows[5] + sz * vpRows[9] + r31;
    float cz = sx * vpRows[2] + sy * vpRows[6] + sz * vpRows[10] + r32;
    const float cw = sx * vpRows[3] + sy * vpRows[7] + sz * vpRows[11] + r33;
    cx /= cw;
    cy /= cw;
    cz /= cw;
    *ox = job->vpX + (1.0f + cx) * job->vpW / 2.0f;
    *oy = job->vpY + (1.0f - cy) * job->vpH / 2.0f;
    *oz = cz; // MinZ 0, MaxZ 1
}

inline float MeBgLength(float x, float y, float z) { return sqrtf(x * x + y * y + z * z); }
} // namespace

extern "C" void th08_me_bg_kernel(PspMeBgJob *job, const PspMeBgRecord *records, PspMeClientVertex *out,
                                  unsigned short *status)
{
    float vpRows[12];
    for (int row = 0; row < 3; ++row)
        for (int cc = 0; cc < 4; ++cc)
            vpRows[row * 4 + cc] = job->view[row * 4] * job->proj[cc] + job->view[row * 4 + 1] * job->proj[4 + cc] +
                                   job->view[row * 4 + 2] * job->proj[8 + cc] + job->view[row * 4 + 3] * job->proj[12 + cc];
    unsigned int drawn = 0U;
    const unsigned int fogB = job->fogColor & 255U, fogG = (job->fogColor >> 8) & 255U, fogR = (job->fogColor >> 16) & 255U;
    // RenderObjects' projectSrc: starts at the origin for each mode and is
    // overwritten by every type-1 strip (the direction between its projected
    // ends), which later projections then use as their source point.
    float srcX = 0.0f, srcY = 0.0f, srcZ = 0.0f;
    unsigned int curMode = 0xFFU;
    for (unsigned int i = 0U; i < job->count; ++i)
    {
        const PspMeBgRecord r = records[i];
        if (r.mode != curMode)
        {
            curMode = r.mode;
            srcX = srcY = srcZ = 0.0f;
        }
        if (r.kind != 5U && r.kind != 7U && !(r.kind == 0xFFU && r.pad2 == 7U))
        {
            status[i] = TH08_ME_BG_STATUS_SKIP;
            continue;
        }
        float px[4], py[4], pz[4];
        unsigned int colors[4];
        float u0, u1, v0, v1;
        if (r.kind == 5U)
        {
            float qx, qy, qz, dx, dy, dz;
            MeBgProject(job, vpRows, r.posX, r.posY, r.posZ, srcX, srcY, srcZ, &qx, &qy, &qz);
            const float quadWidth = r.quadWidth;
            MeBgProject(job, vpRows, job->camVecX * quadWidth * r.scaleX0 + r.posX,
                        job->camVecY * quadWidth * r.scaleX0 + r.posY, job->camVecZ * quadWidth * r.scaleX0 + r.posZ,
                        srcX, srcY, srcZ, &dx, &dy, &dz);
            dx -= qx; dy -= qy; dz -= qz;
            float scaleX = MeBgLength(dx, dy, dz) / quadWidth;
            float scaleY = scaleX;
            if (quadWidth < 0.0f)
                scaleY = -scaleY;
            const float ex = r.posX - job->camX, ey = r.posY - job->camY, ez = r.posZ - job->camZ;
            float dist = MeBgLength(ex, ey, ez);
            unsigned int color1 = r.color1;
            if (job->fogNear < dist)
            {
                dist = (job->fogNear - dist) / (job->fogNear - job->fogFar);
                if (dist >= 1.0f)
                {
                    status[i] = TH08_ME_BG_STATUS_CULLED;
                    continue;
                }
                const unsigned int cb = color1 & 255U, cg = (color1 >> 8) & 255U, cr = (color1 >> 16) & 255U,
                                   ca = color1 >> 24;
                const unsigned char nb = static_cast<unsigned char>(cb - static_cast<unsigned char>((static_cast<int>(cb) - static_cast<int>(fogB)) * dist));
                const unsigned char ng = static_cast<unsigned char>(cg - static_cast<unsigned char>((static_cast<int>(cg) - static_cast<int>(fogG)) * dist));
                const unsigned char nr = static_cast<unsigned char>(cr - static_cast<unsigned char>((static_cast<int>(cr) - static_cast<int>(fogR)) * dist));
                const unsigned char na = static_cast<unsigned char>(ca * (1.0f - dist));
                color1 = (static_cast<unsigned int>(na) << 24) | (static_cast<unsigned int>(nr) << 16) |
                         (static_cast<unsigned int>(ng) << 8) | nb;
            }
            if (qz < 0.0f || qz > 1.0f || (color1 >> 24) == 0U)
            {
                status[i] = TH08_ME_BG_STATUS_CULLED;
                continue;
            }
            // DrawNoRotationNoRound: axis-aligned quad, anchor, then DrawInner(vm, 0).
            const float halfW = (r.sizeX * scaleX) / 2.0f;
            const float halfH = (r.sizeY * scaleY) / 2.0f;
            if ((r.anchor & 1U) == 0U)
            {
                px[0] = px[2] = qx - halfW;
                px[1] = px[3] = halfW + qx;
            }
            else
            {
                px[0] = px[2] = qx;
                px[1] = px[3] = halfW + qx + halfW;
            }
            if ((r.anchor & 2U) == 0U)
            {
                py[0] = py[1] = qy - halfH;
                py[2] = py[3] = halfH + qy;
            }
            else
            {
                py[0] = py[1] = qy;
                py[2] = py[3] = halfH + qy + halfH;
            }
            for (int k = 0; k < 4; ++k)
            {
                pz[k] = qz;
                px[k] += job->shakeX;
                py[k] += job->shakeY;
            }
            u0 = r.uvStartX + r.scrollX; u1 = r.uvEndX + r.scrollX;
            v0 = r.uvStartY + r.scrollY; v1 = r.uvEndY + r.scrollY;
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
            if (maxX < job->vpX || maxY < job->vpY || minX > (job->vpX + job->vpW) || minY > (job->vpY + job->vpH))
            {
                status[i] = TH08_ME_BG_STATUS_CULLED;
                continue;
            }
            unsigned int color = r.flag17 ? r.color2 : color1;
            if (job->useMixColor)
            {
                const unsigned int m = job->mixColor;
                const unsigned int b = MeBgMix(color & 255U, m & 255U);
                const unsigned int g = MeBgMix((color >> 8) & 255U, (m >> 8) & 255U);
                const unsigned int rr = MeBgMix((color >> 16) & 255U, (m >> 16) & 255U);
                const unsigned int a = MeBgMix((color >> 24) & 255U, (m >> 24) & 255U);
                color = (a << 24) | (rr << 16) | (g << 8) | b;
            }
            colors[0] = colors[1] = colors[2] = colors[3] = color;
        }
        else
        {
            // type-1 strip
            float qx, qy, qz, sx, sy, sz, dx, dy, dz;
            MeBgProject(job, vpRows, r.posX, r.posY, r.posZ, srcX, srcY, srcZ, &qx, &qy, &qz);
            MeBgProject(job, vpRows, job->camVecX * r.quadWidth + r.posX, job->camVecY * r.quadWidth + r.posY,
                        job->camVecZ * r.quadWidth + r.posZ, srcX, srcY, srcZ, &dx, &dy, &dz);
            dx -= qx; dy -= qy; dz -= qz;
            const float halfWidthFirst = MeBgLength(dx, dy, dz) / 2.0f;
            float dist = MeBgLength(r.posX - job->camX, r.posY - job->camY, r.posZ - job->camZ);
            unsigned int colorA;
            {
                const unsigned int c = r.color1;
                if (job->fogNear < dist)
                {
                    dist = (job->fogNear - dist) / (job->fogNear - job->fogFar);
                    if (dist < 1.0f)
                    {
                        const unsigned int cb = c & 255U, cg = (c >> 8) & 255U, cr = (c >> 16) & 255U, ca = c >> 24;
                        const unsigned char nb = static_cast<unsigned char>(cb - static_cast<unsigned char>((static_cast<int>(cb) - static_cast<int>(fogB)) * dist));
                        const unsigned char ng = static_cast<unsigned char>(cg - static_cast<unsigned char>((static_cast<int>(cg) - static_cast<int>(fogG)) * dist));
                        const unsigned char nr = static_cast<unsigned char>(cr - static_cast<unsigned char>((static_cast<int>(cr) - static_cast<int>(fogR)) * dist));
                        const unsigned char na = static_cast<unsigned char>(ca * (1.0f - dist));
                        colorA = (static_cast<unsigned int>(na) << 24) | (static_cast<unsigned int>(nr) << 16) |
                                 (static_cast<unsigned int>(ng) << 8) | nb;
                    }
                    else
                        colorA = c & 0x00ffffffU; // alpha 0, rgb untouched (vertices were uninitialised in the original: rgb from color1)
                }
                else
                    colorA = c;
            }
            MeBgProject(job, vpRows, r.pos2X, r.pos2Y, r.pos2Z, srcX, srcY, srcZ, &sx, &sy, &sz);
            MeBgProject(job, vpRows, job->camVecX * r.quadWidth + r.pos2X, job->camVecY * r.quadWidth + r.pos2Y,
                        job->camVecZ * r.quadWidth + r.pos2Z, srcX, srcY, srcZ, &dx, &dy, &dz);
            dx -= sx; dy -= sy; dz -= sz;
            const float halfWidthSecond = MeBgLength(dx, dy, dz) / 2.0f;
            dist = MeBgLength(r.pos2X - job->camX, r.pos2Y - job->camY, r.pos2Z - job->camZ);
            unsigned int colorB;
            {
                const unsigned int c = r.color1;
                if (job->fogNear < dist)
                {
                    dist = (job->fogNear - dist) / (job->fogNear - job->fogFar);
                    if (dist < 1.0f)
                    {
                        const unsigned int cb = c & 255U, cg = (c >> 8) & 255U, cr = (c >> 16) & 255U, ca = c >> 24;
                        const unsigned char nb = static_cast<unsigned char>(cb - static_cast<unsigned char>((static_cast<int>(cb) - static_cast<int>(fogB)) * dist));
                        const unsigned char ng = static_cast<unsigned char>(cg - static_cast<unsigned char>((static_cast<int>(cg) - static_cast<int>(fogG)) * dist));
                        const unsigned char nr = static_cast<unsigned char>(cr - static_cast<unsigned char>((static_cast<int>(cr) - static_cast<int>(fogR)) * dist));
                        const unsigned char na = static_cast<unsigned char>(ca * (1.0f - dist));
                        colorB = (static_cast<unsigned int>(na) << 24) | (static_cast<unsigned int>(nr) << 16) |
                                 (static_cast<unsigned int>(ng) << 8) | nb;
                    }
                    else
                        colorB = c & 0x00ffffffU;
                }
                else
                    colorB = c;
            }
            // projectSrc = projectedSecond - quadPos; normalised by the 2D length
            // (all three components) and left in place for later quads.
            srcX = sx - qx;
            srcY = sy - qy;
            srcZ = sz - qz;
            const float len = sqrtf(srcX * srcX + srcY * srcY);
            if (len < 0.00001f)
            {
                status[i] = TH08_ME_BG_STATUS_CULLED;
                continue;
            }
            srcX /= len;
            srcY /= len;
            srcZ /= len;
            if (qz < 0.0f || qz > 1.0f || sz < 0.0f || sz > 1.0f || r.kind == 0xFFU)
            {
                status[i] = r.kind == 0xFFU ? TH08_ME_BG_STATUS_SKIP : TH08_ME_BG_STATUS_CULLED;
                continue;
            }
            const float ddx = srcX, ddy = srcY;
            px[0] = ddy * halfWidthFirst + qx;  py[0] = qy - ddx * halfWidthFirst;  pz[0] = qz;
            px[1] = qx - ddy * halfWidthFirst;  py[1] = ddx * halfWidthFirst + qy;  pz[1] = qz;
            px[2] = ddy * halfWidthSecond + sx; py[2] = sy - ddx * halfWidthSecond; pz[2] = sz;
            px[3] = sx - ddy * halfWidthSecond; py[3] = ddx * halfWidthSecond + sy; pz[3] = sz;
            u0 = r.uvStartX + r.scrollX; u1 = r.uvEndX + r.scrollX;
            v0 = r.uvStartY + r.scrollY; v1 = r.uvEndY + r.scrollY;
            colors[0] = colors[1] = colorA;
            colors[2] = colors[3] = colorB;
        }
        if (drawn >= job->outCapacity)
        {
            status[i] = TH08_ME_BG_STATUS_CULLED;
            continue;
        }
        PspMeClientVertex *v = out + drawn * 4U;
        for (int k = 0; k < 4; ++k)
        {
            v[k].u = (k & 1) ? u1 : u0;
            v[k].v = (k & 2) ? v1 : v0;
            v[k].r = static_cast<unsigned char>((colors[k] >> 16) & 255U);
            v[k].g = static_cast<unsigned char>((colors[k] >> 8) & 255U);
            v[k].b = static_cast<unsigned char>(colors[k] & 255U);
            v[k].a = static_cast<unsigned char>((colors[k] >> 24) & 255U);
            v[k].x = px[k] + 0.5f;
            v[k].y = py[k] + 0.5f;
            v[k].z = 1.0f - 2.0f * pz[k];
        }
        status[i] = static_cast<unsigned short>(drawn);
        ++drawn;
    }
    job->drawn = drawn;
}

#if TH08_PSP_ME_BG_ANY_ENABLED
namespace
{
PspMeBgRecord gRecords[TH08_ME_BG_MAX_RECORDS] __attribute__((aligned(64)));
unsigned short gStatus[TH08_ME_BG_MAX_RECORDS] __attribute__((aligned(64)));
const void *gTag[TH08_ME_BG_MAX_RECORDS];
PspMeBgJob gJob __attribute__((aligned(64)));
unsigned gCount = 0U;
bool gCapturing = false, gLive = false, gOnMe = false, gAcquired = false;
PspMeClientVertex *gOut = nullptr;
unsigned long gStats[TH08_ME_BG_STAT_COUNT];
template <typename T> inline volatile T *Uncached(T *p)
{
    return reinterpret_cast<volatile T *>(0x40000000u | reinterpret_cast<std::uintptr_t>(p));
}
inline unsigned RoundLines(unsigned bytes) { return (bytes + 63U) & ~63U; }
} // namespace

extern "C" void th08_me_bg_adopt_note(unsigned int what)
{
    if (what < TH08_ME_BG_STAT_COUNT)
        ++gStats[what];
}

extern "C" int th08_me_bg_adopt_begin(const PspMeBgJob *header)
{
    ++gStats[TH08_ME_BG_STAT_FRAMES];
    if ((gStats[TH08_ME_BG_STAT_FRAMES] % 600UL) == 0UL)
    {
        th08::psp::BootLog("ME_BG stats frames=%lu captured=%lu submitted=%lu on_me=%lu adopted=%lu late=%lu "
                           "fb_state=%lu fb_order=%lu audit_cull=%lu audit_quad=%lu records=%lu drawn=%lu groups=%lu "
                           "overflow=%lu busy=%lu no_arena=%lu audit_ulp=%lu\n",
                           gStats[0], gStats[1], gStats[2], gStats[3], gStats[4], gStats[5], gStats[6], gStats[7],
                           gStats[8], gStats[9], gStats[10], gStats[11], gStats[12], gStats[13], gStats[14],
                           gStats[15], gStats[16]);
        th08::psp::FlushBootLog();
    }
    if (gAcquired)
        return 0;
    if (gLive)
    {
        if (gOnMe && th08_me_core_job_done(&gJob) == 0)
        {
            ++gStats[TH08_ME_BG_STAT_BUSY];
            return 0;
        }
        gLive = false;
    }
    gCount = 0U;
    gJob = *header;
    gJob.count = 0U;
    gJob.drawn = 0U;
    gCapturing = true;
    ++gStats[TH08_ME_BG_STAT_CAPTURED];
    return 1;
}

extern "C" int th08_me_bg_adopt_add(const void *tag, const PspMeBgRecord *record)
{
    if (!gCapturing || gCount >= TH08_ME_BG_MAX_RECORDS)
        return 0;
    gRecords[gCount] = *record;
    gTag[gCount] = tag;
    ++gCount;
    return 1;
}

extern "C" void th08_me_bg_adopt_abort(void)
{
    if (!gCapturing)
        return;
    gCapturing = false;
    ++gStats[TH08_ME_BG_STAT_OVERFLOW];
}

extern "C" void th08_me_bg_adopt_submit(void *outVertices, unsigned int outCapacityQuads)
{
    if (!gCapturing)
        return;
    gCapturing = false;
    gStats[TH08_ME_BG_STAT_RECORDS] += gCount;
    if (gCount == 0U)
        return;
    if (outVertices == nullptr || outCapacityQuads == 0U)
    {
        ++gStats[TH08_ME_BG_STAT_NO_ARENA];
        return;
    }
    gOut = static_cast<PspMeClientVertex *>(outVertices);
    gJob.count = gCount;
    gJob.outCapacity = outCapacityQuads;
    gJob.recordsAddr = reinterpret_cast<std::uintptr_t>(gRecords);
    gJob.outAddr = reinterpret_cast<std::uintptr_t>(gOut);
    gJob.statusAddr = reinterpret_cast<std::uintptr_t>(gStatus);
    sceKernelDcacheWritebackRange(gRecords, RoundLines(sizeof(PspMeBgRecord) * gCount));
    sceKernelDcacheWritebackRange(&gJob, sizeof(gJob));
    bool onMe = false;
    if (th08_me_core_ready())
        onMe = th08_me_core_submit_job_kind(&gJob, 4U) != 0;
    if (!onMe)
        th08_me_bg_kernel(const_cast<PspMeBgJob *>(reinterpret_cast<volatile PspMeBgJob *>(Uncached(&gJob))), gRecords,
                          const_cast<PspMeClientVertex *>(reinterpret_cast<volatile PspMeClientVertex *>(Uncached(gOut))),
                          const_cast<unsigned short *>(reinterpret_cast<volatile unsigned short *>(Uncached(gStatus))));
    else
        ++gStats[TH08_ME_BG_STAT_ON_ME];
    gOnMe = onMe;
    gLive = true;
    ++gStats[TH08_ME_BG_STAT_SUBMITTED];
}

extern "C" int th08_me_bg_adopt_state_matches(float shakeX, float shakeY, float vpX, float vpY, float vpW, float vpH,
                                              unsigned int useMixColor, unsigned int mixColor, const float *view16,
                                              const float *proj16)
{
    if (!gLive)
        return 2;
    if (gJob.shakeX != shakeX || gJob.shakeY != shakeY || gJob.vpX != vpX || gJob.vpY != vpY || gJob.vpW != vpW ||
        gJob.vpH != vpH || gJob.useMixColor != useMixColor || gJob.mixColor != mixColor)
        return 0;
    if (std::memcmp(gJob.view, view16, sizeof(gJob.view)) != 0 || std::memcmp(gJob.proj, proj16, sizeof(gJob.proj)) != 0)
        return 0;
    return 1;
}

extern "C" int th08_me_bg_adopt_acquire(unsigned int waitUs, unsigned int *recordCount, unsigned int *drawnQuads)
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
                ++gStats[TH08_ME_BG_STAT_LATE];
                return 0;
            }
            sceKernelDelayThread(20);
            waited += 20U;
        }
    }
    sceKernelDcacheInvalidateRange(gStatus, RoundLines(sizeof(unsigned short) * gCount));
    sceKernelDcacheInvalidateRange(&gJob, sizeof(gJob));
    gAcquired = true;
    *recordCount = gCount;
    *drawnQuads = gJob.drawn;
    ++gStats[TH08_ME_BG_STAT_ADOPTED];
    gStats[TH08_ME_BG_STAT_DRAWN] += gJob.drawn;
    return 1;
}

extern "C" const void *th08_me_bg_adopt_tag(unsigned int i) { return gTag[i]; }
extern "C" unsigned int th08_me_bg_adopt_status(unsigned int i) { return gStatus[i]; }
extern "C" const PspMeClientVertex *th08_me_bg_adopt_vertices(void) { return gOut; }
extern "C" void th08_me_bg_adopt_release(void)
{
    if (!gAcquired)
        return;
    gAcquired = false;
    gLive = false;
}
#endif
