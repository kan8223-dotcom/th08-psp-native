#pragma once

#include <cstdint>
#include <cstring>

// Double-float (Dekker/Knuth) arithmetic built from binary32 hardware
// add/sub/mul/div only.  Shared by the Item atan2 and sin/cos fast paths.
//
// Requirements: IEEE binary32 add/sub/mul/div with round-to-nearest-even and
// no fused multiply-add (build with -ffp-contract=off; psp-gcc has no madd.s).
// Callers keep every intermediate above 2^-90 so Allegrex flush-to-zero of
// denormals cannot matter.

namespace th08::psp
{
struct DoubleFloat
{
    float hi;
    float lo;
};

namespace df_detail
{
inline std::uint32_t FloatBits(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline float BitsFloat(std::uint32_t bits)
{
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline DoubleFloat TwoSum(float a, float b)
{
    const float s = a + b;
    const float bb = s - a;
    const float err = (a - (s - bb)) + (b - bb);
    return {s, err};
}

// Requires |a| >= |b| (or a == 0).
inline DoubleFloat QuickTwoSum(float a, float b)
{
    const float s = a + b;
    const float err = b - (s - a);
    return {s, err};
}

inline void Split(float a, float &hi, float &lo)
{
    const float t = 4097.0f * a; // 2^12 + 1 for a 24-bit significand
    hi = t - (t - a);
    lo = a - hi;
}

inline DoubleFloat TwoProd(float a, float b)
{
    const float p = a * b;
    float ah;
    float al;
    float bh;
    float bl;
    Split(a, ah, al);
    Split(b, bh, bl);
    const float err = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
    return {p, err};
}

inline DoubleFloat DfNeg(DoubleFloat x)
{
    return {-x.hi, -x.lo};
}

inline DoubleFloat DfAdd(DoubleFloat x, DoubleFloat y)
{
    DoubleFloat s = TwoSum(x.hi, y.hi);
    const DoubleFloat t = TwoSum(x.lo, y.lo);
    s.lo += t.hi;
    s = QuickTwoSum(s.hi, s.lo);
    s.lo += t.lo;
    return QuickTwoSum(s.hi, s.lo);
}

inline DoubleFloat DfMul(DoubleFloat x, DoubleFloat y)
{
    DoubleFloat p = TwoProd(x.hi, y.hi);
    p.lo += x.hi * y.lo + x.lo * y.hi;
    return QuickTwoSum(p.hi, p.lo);
}

inline DoubleFloat DfMulF(DoubleFloat x, float y)
{
    DoubleFloat p = TwoProd(x.hi, y);
    p.lo += x.lo * y;
    return QuickTwoSum(p.hi, p.lo);
}

// Two-term long division: relative error about 2^-46.
inline DoubleFloat DfDiv(DoubleFloat x, DoubleFloat y)
{
    const float q1 = x.hi / y.hi;
    DoubleFloat r = DfAdd(x, DfNeg(DfMulF(y, q1)));
    const float q2 = r.hi / y.hi;
    r = DfAdd(r, DfNeg(DfMulF(y, q2)));
    const float q3 = r.hi / y.hi;
    const DoubleFloat q = QuickTwoSum(q1, q2);
    return DfAdd(q, {q3, 0.0f});
}

// a.hi is the binary32 nearest to a.hi + a.lo.  Accepts a.hi as the
// binary32 rounding of any real value within relativeBound * |a| of a.hi +
// a.lo, i.e. when that whole interval stays strictly inside a.hi's rounding
// cell (the half ulp toward zero is used, which is smaller at a power of two).
// Returns false for tiny, non-finite or boundary-adjacent values.
inline bool DfAcceptBinary32(DoubleFloat a, float relativeBound, float *out)
{
    const std::uint32_t hiBits = FloatBits(a.hi);
    const std::uint32_t hiExp = (hiBits >> 23U) & 0xffU;
    if (hiExp < 2U || hiExp == 0xffU)
        return false;
    const bool powerOfTwo = (hiBits & 0x007fffffU) == 0U;
    const std::uint32_t halfUlpExp = hiExp - 24U - (powerOfTwo ? 1U : 0U);
    const float halfUlp = BitsFloat(halfUlpExp << 23U);
    const float absLo = BitsFloat(FloatBits(a.lo) & 0x7fffffffU);
    const float absHi = BitsFloat(hiBits & 0x7fffffffU);
    const float margin = halfUlp - absLo;
    const float bound = absHi * relativeBound;
    if (!(margin > bound))
        return false;
    *out = a.hi;
    return true;
}
} // namespace df_detail
} // namespace th08::psp
