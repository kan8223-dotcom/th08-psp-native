#pragma once

#if defined(PSP) && defined(TH08_PSP_LASER_TRIG_CACHE) && TH08_PSP_LASER_TRIG_CACHE
#define TH08_PSP_LASER_TRIG_ENABLED 1
#include "ZunMath.hpp"
#include "fileio.hpp"
#include <cstdint>
#include <cstring>

namespace th08::psp
{
struct LaserTrigEntry
{
    std::uint32_t key = 0, valid = 0;
    float sine = 0, cosine = 0;
};
struct LaserTrigStats
{
    unsigned long calls = 0, hits = 0, misses = 0, df = 0, fallback = 0;
    unsigned long compared = 0, mismatch = 0, rotations = 0, rotationMismatch = 0, frames = 0;
};
inline LaserTrigEntry gLaserTrigCache[512];
inline LaserTrigStats gLaserTrigStats;

inline std::uint32_t LaserTrigBits(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
inline unsigned int LaserTrigIndex(std::uint32_t key)
{
    key ^= key >> 16U;
    key *= 0x7feb352dU;
    key ^= key >> 15U;
    return static_cast<unsigned int>(key & 511U);
}

inline void LaserSinCos(float angle, float *sine, float *cosine)
{
    ++gLaserTrigStats.calls;
    const std::uint32_t key = LaserTrigBits(angle);
    const bool cacheable = (key & 0x7fffffffU) < 0x7f800000U;
    LaserTrigEntry &entry = gLaserTrigCache[LaserTrigIndex(key)];
    float s, c;
    if (cacheable && entry.valid != 0U && entry.key == key)
    {
        ++gLaserTrigStats.hits;
        s = entry.sine;
        c = entry.cosine;
    }
    else
    {
        ++gLaserTrigStats.misses;
#if TH08_PSP_TRIG_DF_FASTPATH_ENABLED
        if (cacheable && TrigDfSinCosMul(angle, 1.0f, &c, &s, TrigDfSite::SinCosUnit))
            ++gLaserTrigStats.df;
        else
#endif
        {
            ++gLaserTrigStats.fallback;
            // Preserve the existing binary64 fallback and Sin -> Cos order.
            s = static_cast<float>(X87CompatibleSin64(angle));
            c = static_cast<float>(X87CompatibleCos64(angle));
        }
        if (cacheable)
        {
            entry.key = key;
            entry.sine = s;
            entry.cosine = c;
            entry.valid = 1U;
        }
    }
#if defined(TH08_PSP_LASER_TRIG_AUDIT) && TH08_PSP_LASER_TRIG_AUDIT
    const float wantS = X87CompatibleSin(angle);
    const float wantC = X87CompatibleCos(angle);
    ++gLaserTrigStats.compared;
    if (LaserTrigBits(s) != LaserTrigBits(wantS) || LaserTrigBits(c) != LaserTrigBits(wantC))
    {
        if (++gLaserTrigStats.mismatch <= 4UL)
            BootLog("LASER_TRIG mismatch key=%08lx sin=%08lx/%08lx cos=%08lx/%08lx\n",
                static_cast<unsigned long>(key), static_cast<unsigned long>(LaserTrigBits(s)),
                static_cast<unsigned long>(LaserTrigBits(wantS)), static_cast<unsigned long>(LaserTrigBits(c)),
                static_cast<unsigned long>(LaserTrigBits(wantC)));
        entry.valid = 0U;
        s = wantS;
        c = wantC;
    }
#endif
    *sine = s;
    *cosine = c;
}

inline void LaserRotate(Float3 *out, Float3 *point, float angle)
{
    float s, c;
    LaserSinCos(angle, &s, &c);
    // Keep the existing binary64 product/sum and final f32 rounding. No VFPU,
    // f32-only multiply/add, angle symmetry, or collision-branch shortcut.
    out->x = X87CompatibleMulSub(c, point->x, s, point->y);
    out->y = X87CompatibleMulAdd(c, point->y, s, point->x);
#if defined(TH08_PSP_LASER_TRIG_AUDIT) && TH08_PSP_LASER_TRIG_AUDIT
    Float3 reference;
    Rotate(&reference, point, angle);
    ++gLaserTrigStats.rotations;
    if (LaserTrigBits(out->x) != LaserTrigBits(reference.x) ||
        LaserTrigBits(out->y) != LaserTrigBits(reference.y))
    {
        if (++gLaserTrigStats.rotationMismatch <= 4UL)
            BootLog("LASER_TRIG rotation_mismatch key=%08lx\n", static_cast<unsigned long>(LaserTrigBits(angle)));
        out->x = reference.x;
        out->y = reference.y;
    }
#endif
}

inline void LaserTrigFrame()
{
    if ((++gLaserTrigStats.frames % 600UL) == 0UL)
        BootLog("LASER_TRIG stats frames=%lu calls=%lu hits=%lu misses=%lu df=%lu fallback=%lu "
                "compared=%lu mismatch=%lu rotations=%lu rotation_mismatch=%lu\n",
            gLaserTrigStats.frames, gLaserTrigStats.calls, gLaserTrigStats.hits, gLaserTrigStats.misses,
            gLaserTrigStats.df, gLaserTrigStats.fallback, gLaserTrigStats.compared, gLaserTrigStats.mismatch,
            gLaserTrigStats.rotations, gLaserTrigStats.rotationMismatch);
}
} // namespace th08::psp
#else
#define TH08_PSP_LASER_TRIG_ENABLED 0
#endif
