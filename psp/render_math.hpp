#pragma once

#include "render_perf_telemetry.hpp"

#include <cmath>

// TH08's reconstruction defines legacy u32 as unsigned int, while current
// PSPSDK's pspmath.h pulls a uint32_t-based legacy typedef that is unsigned
// long for this ABI.  Import only the stable C function used here so the two
// unrelated typedef sets cannot collide.
extern "C" void vfpu_sincos(float radians, float *outSin, float *outCos);

namespace th08::psp
{
// TH07's hardware-tested renderer uses VFPU only for presentation math.  Keep
// that boundary explicit here: callers must be draw-only paths whose results
// cannot feed gameplay state, collision, RNG, or replay serialization.
inline void RenderSinCos(float angle, float *outSin, float *outCos)
{
#if defined(TH08_PSP_RENDER_VFPU) && TH08_PSP_RENDER_VFPU
    constexpr float kVfpuSafeAngle = 16.0f * 3.14159265358979323846f;
    if (std::isfinite(angle) && angle >= -kVfpuSafeAngle &&
        angle <= kVfpuSafeAngle)
    {
        vfpu_sincos(angle, outSin, outCos);
        RenderPerfNoteVfpuSinCos();
        return;
    }
#endif
    *outSin = sinf(angle);
    *outCos = cosf(angle);
}
} // namespace th08::psp
