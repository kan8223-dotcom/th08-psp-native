#pragma once

#include <cmath>
#if defined(PSP) && defined(TH08_PSP_X87_TRIG_CACHE) && \
    TH08_PSP_X87_TRIG_CACHE
#include "x87_trig_cache.hpp"
#endif

namespace th08::psp
{
// DrawRadialTrail's zero-secondary-radius branch emits two radii for the same
// angle.  Preserve the modern port's canonical binary64 trig boundary while
// evaluating that pure presentation value once per angular sample.  The
// products still remain binary64 until their sole binary32 store.
//
// This helper deliberately has no static storage, counters, VFPU dependency,
// or game types.  OFF/ON builds therefore have identical additional BSS and
// independent host/PSPSDK translation units can audit it without TH08 runtime.
struct CanonicalRadialSinCos
{
    double cosine;
    double sine;
};

inline double EvaluateCanonicalRadialCos(float angle)
{
#if defined(PSP) && defined(TH08_PSP_X87_TRIG_CACHE) && \
    TH08_PSP_X87_TRIG_CACHE
    return X87TrigCacheCos64(angle);
#else
    return std::cos(static_cast<double>(angle));
#endif
}

inline double EvaluateCanonicalRadialSin(float angle)
{
#if defined(PSP) && defined(TH08_PSP_X87_TRIG_CACHE) && \
    TH08_PSP_X87_TRIG_CACHE
    return X87TrigCacheSin64(angle);
#else
    return std::sin(static_cast<double>(angle));
#endif
}

inline float CanonicalRadialCosMul(const CanonicalRadialSinCos &trig,
                                   float magnitude)
{
    return static_cast<float>(
        trig.cosine * static_cast<double>(magnitude));
}

inline float CanonicalRadialSinMul(const CanonicalRadialSinCos &trig,
                                   float magnitude)
{
    return static_cast<float>(
        trig.sine * static_cast<double>(magnitude));
}
} // namespace th08::psp
