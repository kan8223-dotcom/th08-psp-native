#pragma once
#include "inttypes.hpp"
#ifdef TH08_MODERN_PORT
#include <math.h>
#endif
#if defined(PSP) && defined(TH08_PSP_X87_TRIG_CACHE) && \
    TH08_PSP_X87_TRIG_CACHE
#include "x87_trig_cache.hpp"
#endif
#if defined(PSP)
#include "trig_df_fastpath.hpp"
#endif

#define ZUN_MIN(x, y) ((x) < (y) ? (x) : (y))
#define ZUN_MAX(x, y) ((x) > (y) ? (x) : (y))
#define ZUN_PI ((f32)(3.14159265358979323846))
#define ZUN_2PI ((f32)(ZUN_PI * 2.0f))

namespace th08
{

#ifdef TH08_MODERN_PORT
// The retail float wrappers leave the x87 FSIN/FCOS result in extended
// precision and round once when it is stored as f32.  PSPDEV newlib's sinf
// and cosf can differ by one ULP, which is enough to desynchronise ECL state.
// A binary64 libm result followed by one f32 store matches the retail wrapper
// for the audited TH08 gameplay-angle domain on both PC and Allegrex.
inline f64 X87CompatibleSin64(f32 angle)
{
#if defined(PSP) && defined(TH08_PSP_X87_TRIG_CACHE) && \
    TH08_PSP_X87_TRIG_CACHE
    return psp::X87TrigCacheSin64(angle);
#else
    return sin(static_cast<f64>(angle));
#endif
}

inline f64 X87CompatibleCos64(f32 angle)
{
#if defined(PSP) && defined(TH08_PSP_X87_TRIG_CACHE) && \
    TH08_PSP_X87_TRIG_CACHE
    return psp::X87TrigCacheCos64(angle);
#else
    return cos(static_cast<f64>(angle));
#endif
}

inline f32 X87CompatibleSin(f32 angle)
{
#if TH08_PSP_TRIG_DF_FASTPATH_ENABLED
    f32 cosine = 0.0f;
    f32 sine = 0.0f;
    if (psp::TrigDfSinCosMul(angle, 1.0f, &cosine, &sine, psp::TrigDfSite::SinCosUnit))
        return sine;
#endif
#if TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED
    const f32 canonicalSine = static_cast<f32>(X87CompatibleSin64(angle));
    psp::TrigDfAuditSinCosMul(angle, 1.0f, static_cast<f32>(X87CompatibleCos64(angle)),
                              canonicalSine, psp::TrigDfSite::SinCosUnit);
    return canonicalSine;
#endif
    return static_cast<f32>(X87CompatibleSin64(angle));
}

inline f32 X87CompatibleCos(f32 angle)
{
#if TH08_PSP_TRIG_DF_FASTPATH_ENABLED
    f32 cosine = 0.0f;
    f32 sine = 0.0f;
    if (psp::TrigDfSinCosMul(angle, 1.0f, &cosine, &sine, psp::TrigDfSite::SinCosUnit))
        return cosine;
#endif
#if TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED
    const f32 canonicalCosine = static_cast<f32>(X87CompatibleCos64(angle));
    psp::TrigDfAuditSinCosMul(angle, 1.0f, canonicalCosine,
                              static_cast<f32>(X87CompatibleSin64(angle)), psp::TrigDfSite::SinCosUnit);
    return canonicalCosine;
#endif
    return static_cast<f32>(X87CompatibleCos64(angle));
}

// The retail x86 build evaluates FSINCOS and the following multiply in the
// x87 register stack, then rounds once when the result is stored as f32.
// Calling sinf/cosf rounds before the multiply and moves team-specific option
// and shot origins by one ULP on non-x87 targets.  A binary64 intermediate is
// sufficient to reproduce the final binary32 result for the TH08 angle range
// while giving PC and Allegrex the same evaluation boundary.
inline f32 X87CompatibleSinMul(f32 angle, f32 magnitude)
{
#if TH08_PSP_TRIG_DF_FASTPATH_ENABLED
    f32 cosMul = 0.0f;
    f32 sinMul = 0.0f;
    if (psp::TrigDfSinCosMul(angle, magnitude, &cosMul, &sinMul, psp::TrigDfSite::SinCosMul))
        return sinMul;
#endif
#if TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED
    const f32 canonicalSinMul = static_cast<f32>(
        X87CompatibleSin64(angle) * static_cast<f64>(magnitude));
    psp::TrigDfAuditSinCosMul(
        angle, magnitude,
        static_cast<f32>(X87CompatibleCos64(angle) * static_cast<f64>(magnitude)),
        canonicalSinMul, psp::TrigDfSite::SinCosMul);
    return canonicalSinMul;
#endif
    return static_cast<f32>(
        X87CompatibleSin64(angle) * static_cast<f64>(magnitude));
}

inline f32 X87CompatibleCosMul(f32 angle, f32 magnitude)
{
#if TH08_PSP_TRIG_DF_FASTPATH_ENABLED
    f32 cosMul = 0.0f;
    f32 sinMul = 0.0f;
    if (psp::TrigDfSinCosMul(angle, magnitude, &cosMul, &sinMul, psp::TrigDfSite::SinCosMul))
        return cosMul;
#endif
#if TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED
    const f32 canonicalCosMul = static_cast<f32>(
        X87CompatibleCos64(angle) * static_cast<f64>(magnitude));
    psp::TrigDfAuditSinCosMul(
        angle, magnitude, canonicalCosMul,
        static_cast<f32>(X87CompatibleSin64(angle) * static_cast<f64>(magnitude)),
        psp::TrigDfSite::SinCosMul);
    return canonicalCosMul;
#endif
    return static_cast<f32>(
        X87CompatibleCos64(angle) * static_cast<f64>(magnitude));
}

// Player bomb paths add an f32 origin while the trigonometric result and the
// product are still live in the x87 register stack.  Preserve that complete
// call -> multiply -> add -> store boundary; using X87Compatible* first would
// insert an extra f32 rounding before the multiply.
inline f32 X87CompatibleSinMulAdd(
    f32 angle, f32 magnitude, f32 addend)
{
    return static_cast<f32>(
        X87CompatibleSin64(angle) * static_cast<f64>(magnitude) +
        static_cast<f64>(addend));
}

inline f32 X87CompatibleCosMulAdd(
    f32 angle, f32 magnitude, f32 addend)
{
    return static_cast<f32>(
        X87CompatibleCos64(angle) * static_cast<f64>(magnitude) +
        static_cast<f64>(addend));
}

// The ECL polar-motion helpers multiply the unrounded trigonometric result by
// both a float speed and an integer duration before the sole f32 store.  Keep
// this separate from X87Compatible*Mul: multiplying that helper's f32 result
// by duration would introduce a retail-incompatible intermediate rounding.
inline f32 X87CompatibleSinMulInt(
    f32 angle, f32 magnitude, i32 integerFactor)
{
    return static_cast<f32>(
        X87CompatibleSin64(angle) * static_cast<f64>(magnitude) *
        static_cast<f64>(integerFactor));
}

inline f32 X87CompatibleCosMulInt(
    f32 angle, f32 magnitude, i32 integerFactor)
{
    return static_cast<f32>(
        X87CompatibleCos64(angle) * static_cast<f64>(magnitude) *
        static_cast<f64>(integerFactor));
}

// MSVC's retail call promotes the two f32 arguments to binary64 for atan2,
// then rounds the returned binary64 value once at the f32 store.  C++ math
// overloads otherwise select atan2f on modern targets.
inline f32 X87CompatibleAtan2(f32 y, f32 x)
{
#if TH08_PSP_TRIG_DF_FASTPATH_ENABLED
    f32 angle = 0.0f;
    if (psp::TrigDfAtan2(y, x, &angle))
        return angle;
#endif
#if TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED
    const f32 canonicalAngle = static_cast<f32>(
        atan2(static_cast<f64>(y), static_cast<f64>(x)));
    psp::TrigDfAuditAtan2(y, x, canonicalAngle);
    return canonicalAngle;
#endif
    return static_cast<f32>(
        atan2(static_cast<f64>(y), static_cast<f64>(x)));
}

// Rotate stores sinf/cosf into f32 locals first.  x87 then evaluates both
// products and the add/subtract without another f32 rounding.  Products of
// binary32 values and their final sum fit the binary64 boundary needed for
// the same final binary32 result on PC and Allegrex.
inline f32 X87CompatibleMulSub(f32 leftA, f32 leftB,
                               f32 rightA, f32 rightB)
{
    return static_cast<f32>(
        static_cast<f64>(leftA) * static_cast<f64>(leftB) -
        static_cast<f64>(rightA) * static_cast<f64>(rightB));
}

inline f32 X87CompatibleMulAdd(f32 leftA, f32 leftB,
                               f32 rightA, f32 rightB)
{
    return static_cast<f32>(
        static_cast<f64>(leftA) * static_cast<f64>(leftB) +
        static_cast<f64>(rightA) * static_cast<f64>(rightB));
}
#endif

inline void IncrementIfBelow(u32 *value, u32 threshold)
{
    if (*value < threshold)
    {
        (*value)++;
    }
}

/* ZUN name: FVector */
struct Float3
{
    Float3()
    {
    }

    Float3(float x, float y, float z);

    void FromAngleMagnitude(float angle, float magnitude)
    {
#ifdef TH08_MODERN_PORT
#if TH08_PSP_TRIG_DF_FASTPATH_ENABLED
        if (psp::TrigDfSinCosMul(angle, magnitude, &this->x, &this->y,
                                 psp::TrigDfSite::FromAngleMagnitude))
            return;
        this->x = static_cast<f32>(
            X87CompatibleCos64(angle) * static_cast<f64>(magnitude));
        this->y = static_cast<f32>(
            X87CompatibleSin64(angle) * static_cast<f64>(magnitude));
#elif TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED
        this->x = static_cast<f32>(
            X87CompatibleCos64(angle) * static_cast<f64>(magnitude));
        this->y = static_cast<f32>(
            X87CompatibleSin64(angle) * static_cast<f64>(magnitude));
        psp::TrigDfAuditSinCosMul(angle, magnitude, this->x, this->y,
                                  psp::TrigDfSite::FromAngleMagnitude);
#else
        this->x = X87CompatibleCosMul(angle, magnitude);
        this->y = X87CompatibleSinMul(angle, magnitude);
#endif
#else
        __asm
        {
            mov eax, this
            fld angle
            fsincos
            fmul [magnitude]
            fstp [eax] /* this->x */
            fmul [magnitude]
            fstp [eax + 4] /* this->y */
        }
#endif
    }

    void FromRotatedVec2(float angle, float vecX, float vecY)
    {
#ifdef TH08_MODERN_PORT
#if TH08_PSP_TRIG_DF_FASTPATH_ENABLED
        f32 cosMul = 0.0f;
        f32 sinMul = 0.0f;
        if (psp::TrigDfSinCosMul(angle, vecX, &cosMul, &sinMul,
                                 psp::TrigDfSite::FromRotatedVec2))
            this->x = cosMul;
        else
            this->x = static_cast<f32>(
                X87CompatibleCos64(angle) * static_cast<f64>(vecX));
        if (psp::TrigDfSinCosMul(angle, vecY, &cosMul, &sinMul,
                                 psp::TrigDfSite::FromRotatedVec2))
            this->y = sinMul;
        else
            this->y = static_cast<f32>(
                X87CompatibleSin64(angle) * static_cast<f64>(vecY));
#elif TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED
        this->x = static_cast<f32>(
            X87CompatibleCos64(angle) * static_cast<f64>(vecX));
        this->y = static_cast<f32>(
            X87CompatibleSin64(angle) * static_cast<f64>(vecY));
        psp::TrigDfAuditSinCosMul(
            angle, vecX, this->x,
            static_cast<f32>(X87CompatibleSin64(angle) * static_cast<f64>(vecX)),
            psp::TrigDfSite::FromRotatedVec2);
        psp::TrigDfAuditSinCosMul(
            angle, vecY,
            static_cast<f32>(X87CompatibleCos64(angle) * static_cast<f64>(vecY)),
            this->y, psp::TrigDfSite::FromRotatedVec2);
#else
        this->x = X87CompatibleCosMul(angle, vecX);
        this->y = X87CompatibleSinMul(angle, vecY);
#endif
#else
        __asm
        {
            mov eax, this
            fld angle
            fsincos
            fmul [vecX]
            fstp [eax] /* this->x */
            fmul [vecY]
            fstp [eax + 4] /* this->y */
        }
#endif
    }

    // FUNCTION: th08 0x40b460 FOLDED
    operator float *()
    {
        return (float *)this;
    }

    Float3 operator+(const Float3 &other) const;
    Float3 operator-(const Float3 &other) const;
    Float3 operator*(f32 scalar) const;
    Float3 operator/(f32 scalar) const;
    Float3 operator-() const;
    Float3 *operator*=(f32 scalar);
    Float3 *operator/=(f32 scalar);

    Float3 *operator+=(const Float3 &other)
    {
        this->x += other.x;
        this->y += other.y;
        this->z += other.z;

        return this;
    }

    Float3 *operator-=(const Float3 &other)
    {
        this->x -= other.x;
        this->y -= other.y;
        this->z -= other.z;

        return this;
    }

    float x;
    float y;
    float z;
};

/* ZUN name: FVector2 */
struct Float2
{
    float x;
    float y;
};

struct ZunRect
{
    f32 left;
    f32 top;
    f32 right;
    f32 bottom;
};

f32 AddNormalizeAngle(f32 a, f32 b);
f32 VectorAngle(f32 y, f32 x);
void Rotate(Float3 *outVector, Float3 *point, f32 angle);

} // namespace th08

#ifdef TH08_MODERN_PORT
#define sincos(in, out_sine, out_cosine)                                                                               \
    {                                                                                                                  \
        out_sine = sinf(in);                                                                                           \
        out_cosine = cosf(in);                                                                                         \
    }
#else
#define sincos(in, out_sine, out_cosine)                                                                               \
    {                                                                                                                  \
        __asm { \
        __asm fld in \
        __asm fsincos \
        __asm fstp out_cosine \
        __asm fstp out_sine }                                            \
    }
#endif
