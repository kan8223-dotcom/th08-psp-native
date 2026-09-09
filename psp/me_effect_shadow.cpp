#include "me_effect_shadow.hpp"

#include "fileio.hpp"
#include "me_core.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#if defined(PSP)
#include <pspkernel.h>
#endif

// ------------------------------------------------------------ kernel ------
// Mirrors AnmManager::Draw2D / DrawNoRotation / TranslateRotation / DrawInner
// expression by expression so the results are bit-identical.
namespace
{
inline unsigned int MeMixColors(unsigned int c1, unsigned int c2)
{
    unsigned int color = (c1 * c2) / 128U;
    if (color >= 256U)
        color = 255U;
    return color;
}
} // namespace

namespace
{
// D3DXVec3TransformCoord + viewport map on an already-multiplied matrix M.
inline void MeTransformCoordViewport(const float *m, float sx, float sy, float sz, const PspMeEffectJob *job, float *ox,
                                     float *oy, float *oz)
{
    float x = sx * m[0] + sy * m[4] + sz * m[8] + m[12];
    float y = sx * m[1] + sy * m[5] + sz * m[9] + m[13];
    float z = sx * m[2] + sy * m[6] + sz * m[10] + m[14];
    const float w = sx * m[3] + sy * m[7] + sz * m[11] + m[15];
    if (fabsf(w) > 1.0e-8f)
    {
        x /= w;
        y /= w;
        z /= w;
    }
    *ox = job->vpX + (x + 1.0f) * job->vpW * 0.5f;
    *oy = job->vpY + (1.0f - y) * job->vpH * 0.5f;
    *oz = 0.0f + z * (1.0f - 0.0f);
}
// D3DXMatrixMultiply, same summation order.
inline void MeMatrixMultiply(float *r, const float *a, const float *b)
{
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            r[row * 4 + column] = a[row * 4] * b[column] + a[row * 4 + 1] * b[4 + column] +
                                  a[row * 4 + 2] * b[8 + column] + a[row * 4 + 3] * b[12 + column];
}
// D3DXVec3Project(out, src, vp, proj, view, world) with world = identity + translation t:
// (world*view)*proj == rows 0..2 of view*proj plus row 3 = ((t,1)*view)*proj.
inline void MeProjectTranslation(const PspMeEffectJob *job, const float *vpRows, float tx, float ty, float tz, float sx,
                                 float sy, float sz, float *ox, float *oy, float *oz)
{
    const float *v = job->view;
    const float *p = job->proj;
    float t[4], m3[4], m[16];
    for (int cc = 0; cc < 4; ++cc)
        t[cc] = tx * v[cc] + ty * v[4 + cc] + tz * v[8 + cc] + 1.0f * v[12 + cc];
    for (int cc = 0; cc < 4; ++cc)
        m3[cc] = t[0] * p[cc] + t[1] * p[4 + cc] + t[2] * p[8 + cc] + t[3] * p[12 + cc];
    for (int k = 0; k < 12; ++k)
        m[k] = vpRows[k];
    for (int k = 0; k < 4; ++k)
        m[12 + k] = m3[k];
    MeTransformCoordViewport(m, sx, sy, sz, job, ox, oy, oz);
}
} // namespace

extern "C" void th08_me_effect_kernel(const PspMeEffectJob *job, const PspMeEffectRecord *records,
                                      PspMeEffectQuad *out)
{
    const float zeroPointFive = 0.5f;
    float vpRows[12];
    for (int row = 0; row < 3; ++row)
        for (int cc = 0; cc < 4; ++cc)
            vpRows[row * 4 + cc] = job->view[row * 4] * job->proj[cc] + job->view[row * 4 + 1] * job->proj[4 + cc] +
                                   job->view[row * 4 + 2] * job->proj[8 + cc] + job->view[row * 4 + 3] * job->proj[12 + cc];
    for (unsigned int i = 0U; i < job->count; ++i)
    {
        const PspMeEffectRecord r = records[i];
        PspMeEffectQuad q;
        float px[4], py[4], pz[4];
        for (int k = 0; k < 4; ++k)
            pz[k] = r.posZ;
        const float x = (r.sizeX * r.scaleX) / 2.0f;
        const float y = (r.sizeY * r.scaleY) / 2.0f;
        unsigned int flags;
        unsigned int rejected = 0U;
        q.adjApplied = 0U;
        q.adjX = q.adjY = q.adjZ = q.adjW = 0.0f;
        if (r.kind == 2U || r.kind == 4U)
        {
            // ProjectCameraFacingQuad (kind 4: ...WithCallback, AdjustStageEffectDrawPosition)
            float ppx, ppy, ppz, prx, pry, prz;
            MeProjectTranslation(job, vpRows, r.posX, r.posY, r.posZ, 0.0f, 0.0f, 0.0f, &ppx, &ppy, &ppz);
            if (ppz < 0.0f || ppz > 1.0f)
            {
                rejected = 1U;
                for (int k = 0; k < 4; ++k)
                    px[k] = py[k] = 0.0f;
            }
            else
            {
                MeProjectTranslation(job, vpRows, r.posX, r.posY, r.posZ, job->camRight[0], job->camRight[1],
                                     job->camRight[2], &prx, &pry, &prz);
                const float dx = prx - ppx, dy = pry - ppy, dz = prz - ppz;
                float xOffset = sqrtf(dx * dx + dy * dy + dz * dz) * 0.5f;
                const float halfWidth = xOffset * r.sizeX * r.scaleX;
                const float halfHeight = xOffset * r.sizeY * r.scaleY;
                if (r.kind == 4U)
                {
                    // AdjustStageEffectDrawPosition(vm, &projectedPosition)
                    float pfX = r.matrix[3], pfY = r.matrix[4], pfZ = r.matrix[5];
                    float piX = r.matrix[6];
                    if (job->adjustEnabled)
                    {
                        const float pointX = ppx + pfX, pointY = ppy + pfY, pointZ = ppz + pfZ;
                        float dX = r.matrix[0] - pointX, dY = r.matrix[1] - pointY, dZ = r.matrix[2] - pointZ;
                        if (r.matrix[0] > -9999.0f)
                        {
                            dX += 32.0f;
                            dY += 16.0f;
                            dZ = 0.0f;
                            if (dX * dX + dY * dY + dZ * dZ < 25600.0f)
                            {
                                piX += 0.0005000000237487257f;
                                pfX += dX * piX;
                                pfY += dY * piX;
                                pfZ += dZ * piX;
                            }
                        }
                        dX = pointX - job->playerX;
                        dY = pointY - job->playerY;
                        dZ = pointZ - job->playerZ;
                        dX -= 32.0f;
                        dY -= 16.0f;
                        dZ = 0.0f;
                        if (dX * dX + dY * dY + dZ * dZ < 7744.0f)
                        {
                            pfX += dX * 0.019999999552965164f;
                            pfY += dY * 0.019999999552965164f;
                            pfZ += dZ * 0.019999999552965164f;
                        }
                    }
                    ppx += pfX;
                    ppy += pfY;
                    ppz += pfZ;
                    q.adjX = pfX; q.adjY = pfY; q.adjZ = pfZ; q.adjW = piX;
                    q.adjApplied = 1U;
                }
                xOffset = ppx;
                const float yOffset = ppy;
                const float sine = r.sine, cosine = r.cosine;
                { const float ax = -halfWidth, ay = -halfHeight; px[0] = ax * cosine - ay * sine + xOffset; py[0] = ax * sine + ay * cosine + yOffset; }
                { const float ax = halfWidth, ay = -halfHeight; px[1] = ax * cosine - ay * sine + xOffset; py[1] = ax * sine + ay * cosine + yOffset; }
                { const float ax = -halfWidth, ay = halfHeight; px[2] = ax * cosine - ay * sine + xOffset; py[2] = ax * sine + ay * cosine + yOffset; }
                { const float ax = halfWidth, ay = halfHeight; px[3] = ax * cosine - ay * sine + xOffset; py[3] = ax * sine + ay * cosine + yOffset; }
                if (r.anchor & 1U)
                    for (int k = 0; k < 4; ++k)
                        px[k] += halfWidth;
                if (r.anchor & 2U)
                    for (int k = 0; k < 4; ++k)
                        py[k] += halfHeight;
                for (int k = 0; k < 4; ++k)
                    pz[k] = ppz;
            }
            flags = 0U;
        }
        else if (r.kind == 3U)
        {
            // Project3DQuad: full world matrix (matrix2 with translation row replaced).
            float world[16];
            for (int k = 0; k < 16; ++k)
                world[k] = r.matrix[k];
            world[12] = (r.anchor & 1U) == 0U ? r.posX : fabsf(r.sizeX * r.scaleX / 2.0f) + r.posX;
            world[13] = (r.anchor & 2U) == 0U ? r.posY : fabsf(r.sizeY * r.scaleY / 2.0f) + r.posY;
            world[14] = r.posZ;
            float wv[16], m[16];
            MeMatrixMultiply(wv, world, job->view);
            MeMatrixMultiply(m, wv, job->proj);
            static const float corner[4][2] = {{-128.0f, -128.0f}, {128.0f, -128.0f}, {-128.0f, 128.0f}, {128.0f, 128.0f}};
            for (int k = 0; k < 4; ++k)
                MeTransformCoordViewport(m, corner[k][0], corner[k][1], 0.0f, job, &px[k], &py[k], &pz[k]);
            flags = 0U;
        }
        else if (r.kind == 1U)
        {
            const float xOffset = r.posX;
            const float yOffset = r.posY;
            const float sine = r.sine, cosine = r.cosine;
            // TranslateRotation(&v[k], ax, ay, ...): pos.x = ax*cos - ay*sin + xOffset; pos.y = ax*sin + ay*cos + yOffset
            {
                const float ax = -x, ay = -y;
                px[0] = ax * cosine - ay * sine + xOffset;
                py[0] = ax * sine + ay * cosine + yOffset;
            }
            {
                const float ax = x, ay = -y;
                px[1] = ax * cosine - ay * sine + xOffset;
                py[1] = ax * sine + ay * cosine + yOffset;
            }
            {
                const float ax = -x, ay = y;
                px[2] = ax * cosine - ay * sine + xOffset;
                py[2] = ax * sine + ay * cosine + yOffset;
            }
            {
                const float ax = x, ay = y;
                px[3] = ax * cosine - ay * sine + xOffset;
                py[3] = ax * sine + ay * cosine + yOffset;
            }
            if (r.anchor & 1U)
                for (int k = 0; k < 4; ++k)
                    px[k] += x;
            if (r.anchor & 2U)
                for (int k = 0; k < 4; ++k)
                    py[k] += y;
            flags = 0U;
        }
        else
        {
            const float spriteHalfWidth = x;
            const float spriteHalfHeight = y;
            if ((r.anchor & 1U) == 0U)
            {
                px[0] = px[2] = r.posX - spriteHalfWidth;
                px[1] = px[3] = spriteHalfWidth + r.posX;
            }
            else
            {
                px[0] = px[2] = r.posX;
                px[1] = px[3] = spriteHalfWidth + r.posX + spriteHalfWidth;
            }
            if ((r.anchor & 2U) == 0U)
            {
                py[0] = py[1] = r.posY - spriteHalfHeight;
                py[2] = py[3] = spriteHalfHeight + r.posY;
            }
            else
            {
                py[0] = py[1] = r.posY;
                py[2] = py[3] = spriteHalfHeight + r.posY + spriteHalfHeight;
            }
            flags = 1U;
        }
        // DrawInner
        for (int k = 0; k < 4; ++k)
        {
            px[k] += job->shakeX;
            py[k] += job->shakeY;
        }
        if (flags & 1U)
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
        q.u[0] = q.u[2] = r.uvStartX + r.scrollX;
        q.u[1] = q.u[3] = r.uvEndX + r.scrollX;
        q.v[0] = q.v[1] = r.uvStartY + r.scrollY;
        q.v[2] = q.v[3] = r.uvEndY + r.scrollY;
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
        q.culled = (rejected != 0U || maxX < job->vpX || maxY < job->vpY || minX > (job->vpX + job->vpW) || minY > (job->vpY + job->vpH))
                       ? 1U
                       : 0U;
        unsigned int color = r.flag17 ? r.color2 : r.color1;
        if (job->useMixColor)
        {
            const unsigned int m = job->mixColor;
            const unsigned int b = MeMixColors(color & 255U, m & 255U);
            const unsigned int g = MeMixColors((color >> 8) & 255U, (m >> 8) & 255U);
            const unsigned int rr = MeMixColors((color >> 16) & 255U, (m >> 16) & 255U);
            const unsigned int a = MeMixColors((color >> 24) & 255U, (m >> 24) & 255U);
            color = (a << 24) | (rr << 16) | (g << 8) | b;
        }
        q.color = color;
        for (int k = 0; k < 4; ++k)
        {
            q.x[k] = px[k];
            q.y[k] = py[k];
            q.z[k] = pz[k];
        }
        q.pad[0] = 0U;
        out[i] = q;
    }
}

// ------------------------------------------------------------ SC side -----
#if TH08_PSP_ME_EFFECT_SHADOW_ENABLED
namespace
{
constexpr unsigned kSlots = 2U;
PspMeEffectRecord gRecords[kSlots][TH08_ME_EFFECT_MAX_RECORDS] __attribute__((aligned(64)));
PspMeEffectQuad gScOut[kSlots][TH08_ME_EFFECT_MAX_RECORDS] __attribute__((aligned(64)));
PspMeEffectQuad gMeOut[kSlots][TH08_ME_EFFECT_MAX_RECORDS] __attribute__((aligned(64)));
PspMeEffectJob gJob[kSlots] __attribute__((aligned(64)));
unsigned gCount[kSlots];
unsigned gOverflow[kSlots];
unsigned gSlot = 0U;
bool gArmed = false;
bool gPending = false;
bool gSlotJobLive[kSlots];
unsigned gCulledCount[kSlots];
// stats
unsigned long gFrames = 0UL, gRecordsTotal = 0UL, gCulledTotal = 0UL, gJobs = 0UL, gJobsDone = 0UL;
unsigned long gMismatchPos = 0UL, gMismatchUv = 0UL, gMismatchColor = 0UL, gMismatchCull = 0UL, gCompared = 0UL;
unsigned long gOverflowTotal = 0UL, gMaxRecords = 0UL, gRanOnSc = 0UL, gRanOnMe = 0UL;
unsigned long gLateSkips = 0UL;
bool gSlotBusy = false;
unsigned gCullSamples = 0U;

template <typename T> inline volatile T *Uncached(T *p)
{
    return reinterpret_cast<volatile T *>(0x40000000u | reinterpret_cast<std::uintptr_t>(p));
}
} // namespace

extern "C" void th08_me_effect_capture_arm(int armed)
{
    gArmed = armed != 0;
    gPending = false;
}

extern "C" void th08_me_effect_capture_begin(float posX, float posY, float posZ, float scaleX, float scaleY,
                                             float sizeX, float sizeY, float sine, float cosine, float uvStartX,
                                             float uvEndX, float uvStartY, float uvEndY, float scrollX,
                                             float scrollY, unsigned int color1, unsigned int color2,
                                             unsigned int anchor, unsigned int rotated, unsigned int flag17,
                                             unsigned int kind, const float *matrix16)
{
    if (!gArmed || gSlotBusy)
        return;
    const unsigned n = gCount[gSlot];
    if (n >= TH08_ME_EFFECT_MAX_RECORDS)
    {
        ++gOverflow[gSlot];
        gPending = false;
        return;
    }
    PspMeEffectRecord &r = gRecords[gSlot][n];
    r.posX = posX; r.posY = posY; r.posZ = posZ;
    r.scaleX = scaleX; r.scaleY = scaleY;
    r.sizeX = sizeX; r.sizeY = sizeY;
    r.sine = sine; r.cosine = cosine;
    r.uvStartX = uvStartX; r.uvEndX = uvEndX; r.uvStartY = uvStartY; r.uvEndY = uvEndY;
    r.scrollX = scrollX; r.scrollY = scrollY;
    r.color1 = color1; r.color2 = color2;
    r.anchor = static_cast<unsigned char>(anchor);
    r.rotated = static_cast<unsigned char>(rotated);
    r.flag17 = static_cast<unsigned char>(flag17);
    r.kind = static_cast<unsigned char>(kind);
    if (matrix16 != NULL)
        std::memcpy(r.matrix, matrix16, sizeof(r.matrix));
    else
        std::memset(r.matrix, 0, sizeof(r.matrix));
    gPending = true;
}

extern "C" void th08_me_effect_capture_cull_note(const void *quadVertices)
{
    if (!gArmed || !gPending || gCullSamples >= 8U)
        return;
    ++gCullSamples;
    const PspMeEffectRecord &r = gRecords[gSlot][gCount[gSlot]];
    const PspMeEffectJob &j = gJob[gSlot];
    const unsigned char *v = static_cast<const unsigned char *>(quadVertices);
    float x[4], y[4], z[4];
    for (int k = 0; k < 4; ++k)
    {
        std::memcpy(&x[k], v + k * 28 + 0, 4);
        std::memcpy(&y[k], v + k * 28 + 4, 4);
        std::memcpy(&z[k], v + k * 28 + 8, 4);
    }
    th08::psp::BootLog("ME_EFFECT cullsample kind=%u pos=%d,%d,%d size=%d,%d scale=%d,%d "
                       "quad=(%d,%d)(%d,%d)(%d,%d)(%d,%d) z=%d/1000 vp=%d,%d,%d,%d\n",
                       static_cast<unsigned>(r.kind), static_cast<int>(r.posX), static_cast<int>(r.posY),
                       static_cast<int>(r.posZ), static_cast<int>(r.sizeX), static_cast<int>(r.sizeY),
                       static_cast<int>(r.scaleX * 100.0f), static_cast<int>(r.scaleY * 100.0f),
                       static_cast<int>(x[0]), static_cast<int>(y[0]), static_cast<int>(x[1]), static_cast<int>(y[1]),
                       static_cast<int>(x[2]), static_cast<int>(y[2]), static_cast<int>(x[3]), static_cast<int>(y[3]),
                       static_cast<int>(z[0] * 1000.0f), static_cast<int>(j.vpX), static_cast<int>(j.vpY),
                       static_cast<int>(j.vpW), static_cast<int>(j.vpH));
}

extern "C" void th08_me_effect_capture_end(const void *quadVertices)
{
    if (!gArmed || !gPending)
        return;
    gPending = false;
    const unsigned n = gCount[gSlot];
    PspMeEffectQuad &q = gScOut[gSlot][n];
    std::memset(&q, 0, sizeof(q));
    if (quadVertices == NULL)
    {
        q.culled = 1U;
        ++gCulledCount[gSlot];
    }
    else
    {
        // VertexTex1DiffuseXyzrhw: pos.xyz (12) w (4) diffuse (4) uv (8) = 28 bytes.
        const unsigned char *v = static_cast<const unsigned char *>(quadVertices);
        for (int k = 0; k < 4; ++k)
        {
            const unsigned char *e = v + k * 28;
            std::memcpy(&q.x[k], e + 0, 4);
            std::memcpy(&q.y[k], e + 4, 4);
            std::memcpy(&q.z[k], e + 8, 4);
            std::memcpy(&q.u[k], e + 20, 4);
            std::memcpy(&q.v[k], e + 24, 4);
        }
        std::memcpy(&q.color, v + 16, 4);
    }
    gCount[gSlot] = n + 1U;
}

namespace
{
void CompareSlot(unsigned slot)
{
    const unsigned n = gCount[slot];
    volatile PspMeEffectQuad *me = Uncached(gMeOut[slot]);
    for (unsigned i = 0U; i < n; ++i)
    {
        const PspMeEffectQuad &sc = gScOut[slot][i];
        PspMeEffectQuad m;
        std::memcpy(&m, const_cast<const PspMeEffectQuad *>(&me[i]), sizeof(m));
        ++gCompared;
        if (sc.culled != m.culled)
        {
            ++gMismatchCull;
            continue;
        }
        if (sc.culled)
            continue;
        if (std::memcmp(sc.x, m.x, sizeof(sc.x)) != 0 || std::memcmp(sc.y, m.y, sizeof(sc.y)) != 0 ||
            std::memcmp(sc.z, m.z, sizeof(sc.z)) != 0)
            ++gMismatchPos;
        if (std::memcmp(sc.u, m.u, sizeof(sc.u)) != 0 || std::memcmp(sc.v, m.v, sizeof(sc.v)) != 0)
            ++gMismatchUv;
        if (sc.color != m.color)
            ++gMismatchColor;
    }
}
} // namespace

// Job header filled by the game side each frame (viewport/shake/mix).
extern "C" void th08_me_effect_set_frame_state(float shakeX, float shakeY, float vpX, float vpY, float vpW,
                                               float vpH, unsigned int useMixColor, unsigned int mixColor,
                                               const float *view16, const float *proj16, const float *camRight3)
{
    PspMeEffectJob &j = gJob[gSlot];
    std::memcpy(j.view, view16, sizeof(j.view));
    std::memcpy(j.proj, proj16, sizeof(j.proj));
    j.camRight[0] = camRight3[0]; j.camRight[1] = camRight3[1]; j.camRight[2] = camRight3[2]; j.camRight[3] = 0.0f;
    j.shakeX = shakeX; j.shakeY = shakeY;
    j.vpX = vpX; j.vpY = vpY; j.vpW = vpW; j.vpH = vpH;
    j.useMixColor = useMixColor; j.mixColor = mixColor;
}

extern "C" void th08_me_effect_shadow_frame(void)
{
    ++gFrames;
    // 1. Finish/compare the previous slot.
    const unsigned prev = gSlot ^ 1U;
    if (gSlotJobLive[prev])
    {
        bool done = true;
#if TH08_PSP_ME_CORE_ENABLED
        if (th08_me_core_ready())
            done = th08_me_core_job_done(&gJob[prev]);
#endif
        if (done)
        {
            CompareSlot(prev);
            gSlotJobLive[prev] = false;
            ++gJobsDone;
        }
    }
    // 2. Submit the current slot (unless it is still owned by a late ME job).
    const unsigned n = gSlotBusy ? 0U : gCount[gSlot];
    gRecordsTotal += n;
    if (!gSlotBusy)
    {
        gCulledTotal += gCulledCount[gSlot];
        gOverflowTotal += gOverflow[gSlot];
    }
    if (n > gMaxRecords)
        gMaxRecords = n;
    if (n != 0U && !gSlotJobLive[gSlot])
    {
        PspMeEffectJob &j = gJob[gSlot];
        j.count = n;
        j.recordsAddr = reinterpret_cast<std::uintptr_t>(gRecords[gSlot]);
        j.outAddr = reinterpret_cast<std::uintptr_t>(gMeOut[gSlot]);
        sceKernelDcacheWritebackRange(gRecords[gSlot], sizeof(PspMeEffectRecord) * n);
        sceKernelDcacheWritebackRange(&j, sizeof(j));
        ++gJobs;
        bool submitted = false;
#if TH08_PSP_ME_CORE_ENABLED
        if (th08_me_core_ready())
        {
            submitted = th08_me_core_submit_job(&j) != 0;
            if (submitted)
                ++gRanOnMe;
        }
#endif
        if (!submitted)
        {
            // No ME: run the kernel here so the comparison still validates the
            // math (PPSSPP, PSP-1000, takeover failure).
            th08_me_effect_kernel(&j, gRecords[gSlot],
                                  const_cast<PspMeEffectQuad *>(reinterpret_cast<volatile PspMeEffectQuad *>(
                                      Uncached(gMeOut[gSlot]))));
            ++gRanOnSc;
        }
        gSlotJobLive[gSlot] = true;
    }
    // 3. Rotate.
    gSlot = prev;
    if (gSlotJobLive[gSlot])
    {
        // The ME is still working on this slot two frames later: keep its
        // input intact and skip capturing this frame instead of overwriting
        // what the ME is reading.
        ++gLateSkips;
        gSlotBusy = true;
    }
    else
    {
        gSlotBusy = false;
        gCount[gSlot] = 0U;
        gCulledCount[gSlot] = 0U;
        gOverflow[gSlot] = 0U;
    }
    gPending = false;
    if ((gFrames % 600UL) == 0UL)
        th08::psp::BootLog("ME_EFFECT stats frames=%lu records=%lu culled=%lu max=%lu overflow=%lu jobs=%lu done=%lu "
                           "on_me=%lu on_sc=%lu compared=%lu mismatch_pos=%lu mismatch_uv=%lu mismatch_color=%lu "
                           "mismatch_cull=%lu late=%lu\n",
                           gFrames, gRecordsTotal, gCulledTotal, gMaxRecords, gOverflowTotal, gJobs, gJobsDone,
                           gRanOnMe, gRanOnSc, gCompared, gMismatchPos, gMismatchUv, gMismatchColor, gMismatchCull, gLateSkips);
    if ((gFrames % 600UL) == 0UL)
        th08::psp::FlushBootLog();
}
#else
extern "C" void th08_me_effect_capture_arm(int) {}
extern "C" void th08_me_effect_capture_begin(float, float, float, float, float, float, float, float, float, float,
                                             float, float, float, float, float, unsigned int, unsigned int,
                                             unsigned int, unsigned int, unsigned int, unsigned int, const float *) {}
extern "C" void th08_me_effect_capture_end(const void *) {}
extern "C" void th08_me_effect_shadow_frame(void) {}
extern "C" void th08_me_effect_set_frame_state(float, float, float, float, float, float, unsigned int, unsigned int, const float *, const float *, const float *) {}
#endif
