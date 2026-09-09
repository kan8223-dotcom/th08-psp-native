#pragma once

#if defined(PSP) && defined(TH08_PSP_QUAD_VFPU) && TH08_PSP_QUAD_VFPU
#define TH08_PSP_QUAD_VFPU_ENABLED 1
#include "fileio.hpp"
#include <cstdint>
#include <cstring>

namespace th08::psp
{
struct QuadVfpuStats
{
    unsigned long frames = 0, calls = 0, accepted = 0, fallback = 0;
    unsigned long compared = 0, mismatch = 0;
};
inline QuadVfpuStats gQuadVfpu;

inline std::uint32_t QuadFloatBits(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline bool QuadVfpuSafe(float value)
{
    const std::uint32_t magnitude = QuadFloatBits(value) & 0x7fffffffU;
    // Bounded normal inputs: no NaN/Inf, underflowing products, or huge
    // intermediates. Zero (including -0) is allowed and audited separately.
    return magnitude == 0U || (magnitude >= 0x35800000U && magnitude <= 0x47800000U);
}

inline void QuadVfpuRaw(void *quad, float x, float y, float sine, float cosine, float xOffset, float yOffset)
{
    // M000 is transient in libpspmath/SDL. PSPGL's retained matrix is M700.
    // Keep all prefixes/arithmetic/stores together; GCC does not allocate
    // VFPU registers. Only X/Y of the proven 28-byte vertex layout are stored.
    __asm__ volatile(
        "mtv %1, S000\n"
        "mtv %2, S001\n"
        "mtv %3, S002\n"
        "mtv %4, S003\n"
        "mtv %5, S010\n"
        "mtv %6, S011\n"
        "vmul.q C020, C000[x,x,y,y], C000[w,z,z,w]\n"
        "vsub.q C030, C020[-x,x,-x,x], C020[-z,-z,z,z]\n"
        "vadd.q C030, C030, C010[x,x,x,x]\n"
        "vadd.q C000, C020[-y,y,-y,y], C020[-w,-w,w,w]\n"
        "vadd.q C000, C000, C010[y,y,y,y]\n"
        "sv.s S030, 0(%0)\n"
        "sv.s S000, 4(%0)\n"
        "sv.s S031, 28(%0)\n"
        "sv.s S001, 32(%0)\n"
        "sv.s S032, 56(%0)\n"
        "sv.s S002, 60(%0)\n"
        "sv.s S033, 84(%0)\n"
        "sv.s S003, 88(%0)\n"
        : : "r"(quad), "r"(x), "r"(y), "r"(sine), "r"(cosine), "r"(xOffset), "r"(yOffset)
        : "memory");
}

#if defined(TH08_PSP_QUAD_VFPU_AUDIT) && TH08_PSP_QUAD_VFPU_AUDIT
inline bool QuadVfpuCompare(const void *quad, float x, float y, float sine, float cosine,
                             float xOffset, float yOffset)
{
    bool same = true;
    for (unsigned int i = 0; i < 4U; ++i)
    {
        const float sx = (i & 1U) ? x : -x;
        const float sy = (i & 2U) ? y : -y;
        // Explicit scalar rounding boundaries; no vector dot or fused MAD.
        volatile float xc = sx * cosine, ys = sy * sine;
        volatile float xs = sx * sine, yc = sy * cosine;
        volatile float rx = xc - ys, ry = xs + yc;
        const float wantX = rx + xOffset, wantY = ry + yOffset;
        float got[2];
        std::memcpy(got, static_cast<const unsigned char *>(quad) + i * 28U, sizeof(got));
        same = same && QuadFloatBits(wantX) == QuadFloatBits(got[0]) &&
                       QuadFloatBits(wantY) == QuadFloatBits(got[1]);
    }
    return same;
}

inline void QuadVfpuSelfTest()
{
    alignas(16) unsigned char quad[112];
    const float edge[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 0x1p-20f, -0x1p-20f, 65536.0f};
    unsigned long checked = 0, mismatch = 0;
    for (float x : edge) for (float y : edge) for (float s : edge) for (float c : edge)
    {
        QuadVfpuRaw(quad, x, y, s, c, 0.0f, -0.0f);
        ++checked;
        if (!QuadVfpuCompare(quad, x, y, s, c, 0.0f, -0.0f)) ++mismatch;
    }
    std::uint32_t rng = 0x23108U;
    for (unsigned int i = 0; i < 8192U; ++i)
    {
        float a[6];
        for (unsigned int j = 0; j < 6U; ++j)
        {
            rng ^= rng << 13U; rng ^= rng >> 17U; rng ^= rng << 5U;
            a[j] = static_cast<float>(static_cast<int>(rng & 65535U) - 32768) / 64.0f;
        }
        QuadVfpuRaw(quad, a[0], a[1], a[2], a[3], a[4], a[5]);
        ++checked;
        if (!QuadVfpuCompare(quad, a[0], a[1], a[2], a[3], a[4], a[5])) ++mismatch;
    }
    BootLog("QUAD_VFPU selftest compared=%lu mismatch=%lu\n", checked, mismatch);
    gQuadVfpu.mismatch += mismatch;
}
#endif

inline bool RenderQuadRotateVfpu(void *quad, float x, float y, float sine, float cosine,
                                  float xOffset, float yOffset)
{
    ++gQuadVfpu.calls;
    if (!QuadVfpuSafe(x) || !QuadVfpuSafe(y) || !QuadVfpuSafe(sine) || !QuadVfpuSafe(cosine) ||
        !QuadVfpuSafe(xOffset) || !QuadVfpuSafe(yOffset))
    {
        ++gQuadVfpu.fallback;
        return false;
    }
    QuadVfpuRaw(quad, x, y, sine, cosine, xOffset, yOffset);
#if defined(TH08_PSP_QUAD_VFPU_AUDIT) && TH08_PSP_QUAD_VFPU_AUDIT
    ++gQuadVfpu.compared;
    if (!QuadVfpuCompare(quad, x, y, sine, cosine, xOffset, yOffset))
    {
        if (++gQuadVfpu.mismatch <= 4UL)
            BootLog("QUAD_VFPU mismatch x=%08lx y=%08lx s=%08lx c=%08lx ox=%08lx oy=%08lx\n",
                static_cast<unsigned long>(QuadFloatBits(x)), static_cast<unsigned long>(QuadFloatBits(y)),
                static_cast<unsigned long>(QuadFloatBits(sine)), static_cast<unsigned long>(QuadFloatBits(cosine)),
                static_cast<unsigned long>(QuadFloatBits(xOffset)), static_cast<unsigned long>(QuadFloatBits(yOffset)));
        ++gQuadVfpu.fallback;
        return false;
    }
#endif
    ++gQuadVfpu.accepted;
    return true;
}

inline void RenderQuadVfpuFrame()
{
#if defined(TH08_PSP_QUAD_VFPU_AUDIT) && TH08_PSP_QUAD_VFPU_AUDIT
    if (gQuadVfpu.frames == 0UL) QuadVfpuSelfTest();
#endif
    if ((++gQuadVfpu.frames % 600UL) == 0UL)
        BootLog("QUAD_VFPU stats frames=%lu calls=%lu accepted=%lu fallback=%lu compared=%lu mismatch=%lu\n",
            gQuadVfpu.frames, gQuadVfpu.calls, gQuadVfpu.accepted, gQuadVfpu.fallback,
            gQuadVfpu.compared, gQuadVfpu.mismatch);
}
} // namespace th08::psp
#else
#define TH08_PSP_QUAD_VFPU_ENABLED 0
#endif
