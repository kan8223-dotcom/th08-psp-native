#pragma once

#include <cstdint>

// Bit-exact double-float fast paths for the x87-compatible trig helpers in
// src/ZunMath.hpp (FromAngleMagnitude, FromRotatedVec2, X87CompatibleSin/Cos
// and their *Mul forms, X87CompatibleAtan2).  The math is the Item fast path
// (psp/item_sincos_fastpath_math.hpp, psp/item_atan2_fastpath_math.hpp):
// a binary32 double-float evaluation whose result is accepted only when it
// is provably inside the same binary32 rounding cell as the canonical
// binary64 libm path, and the canonical path runs otherwise.
//
// TH08_PSP_TRIG_DF_FASTPATH_AUDIT keeps every canonical call and shadow
// compares; TH08_PSP_TRIG_DF_FASTPATH replaces accepted calls.  Both default
// OFF and are mutually exclusive.  The PC oracle build never sees them.

#if defined(PSP) && defined(TH08_PSP_TRIG_DF_FASTPATH_AUDIT) && \
    TH08_PSP_TRIG_DF_FASTPATH_AUDIT
#define TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED 1
#else
#define TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_TRIG_DF_FASTPATH) && \
    TH08_PSP_TRIG_DF_FASTPATH
#define TH08_PSP_TRIG_DF_FASTPATH_ENABLED 1
#else
#define TH08_PSP_TRIG_DF_FASTPATH_ENABLED 0
#endif

#if TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED && TH08_PSP_TRIG_DF_FASTPATH_ENABLED
#error "TRIG_DF_FASTPATH audit and product switches are mutually exclusive"
#endif

#if TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED || TH08_PSP_TRIG_DF_FASTPATH_ENABLED
#define TH08_PSP_TRIG_DF_STATS_ENABLED 1
#else
#define TH08_PSP_TRIG_DF_STATS_ENABLED 0
#endif

#if TH08_PSP_TRIG_DF_STATS_ENABLED

namespace th08::psp
{
enum class TrigDfSite : std::uint8_t
{
    FromAngleMagnitude = 0, // Float3::FromAngleMagnitude (cos*m, sin*m)
    FromRotatedVec2 = 1,    // Float3::FromRotatedVec2 (cos*vx, sin*vy)
    SinCosMul = 2,          // X87CompatibleSinMul / X87CompatibleCosMul
    SinCosUnit = 3,         // X87CompatibleSin / X87CompatibleCos
    Atan2 = 4,              // X87CompatibleAtan2
    Count = 5,
};

#if TH08_PSP_TRIG_DF_FASTPATH_ENABLED
// Returns true and writes both (f32)(cos64(angle)*m) and (f32)(sin64(angle)*m)
// when the fast path is provably bit-identical; returns false otherwise and
// leaves the outputs untouched so the caller runs the canonical helper.
bool TrigDfSinCosMul(float angle, float magnitude, float *cosMul,
                     float *sinMul, TrigDfSite site);
bool TrigDfAtan2(float y, float x, float *out);
#endif

#if TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED
// Shadow-compare after the canonical helper produced its results.
void TrigDfAuditSinCosMul(float angle, float magnitude,
                          float canonicalCosMul, float canonicalSinMul,
                          TrigDfSite site);
void TrigDfAuditAtan2(float y, float x, float canonical);
#endif

void TrigDfStatsResetWindow(bool active);
void TrigDfStatsCancelWindow();
void TrigDfStatsEmitWindow(std::int32_t stage, std::uint32_t baselineStageFrame,
                           std::uint32_t stageFrame);
} // namespace th08::psp

#endif // TH08_PSP_TRIG_DF_STATS_ENABLED
