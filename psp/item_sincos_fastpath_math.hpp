#pragma once

#include "double_float_math.hpp"

#include <cstdint>

// Bit-exact float-result sin/cos-multiply fast path for the Item autocollect
// velocity.  The canonical values are
//   x = (f32)(cos((f64)angle) * (f64)magnitude)
//   y = (f32)(sin((f64)angle) * (f64)magnitude)
// (X87CompatibleCosMul / X87CompatibleSinMul; the x87 trig cache stores the
// same binary64 libm results).  This header evaluates both products with
// double-float arithmetic (binary32 hardware ops only) to about 2^-45
// relative precision and accepts each binary32 rounding only when it is
// provably safe; otherwise it declines and the caller runs the canonical path.
//
// Domain: |angle| <= 4 (Item angles come from atan2, so |angle| <= pi), so
// the reduction needs at most k = +-2 quarter turns.  pi/2 is carried as three
// binary32 parts (residual 2^-78), and the reduced argument is kept above
// 2^-24 so both components stay away from zero and from denormals.

namespace th08::psp
{
enum class ItemSinCosFastpathReason : std::uint8_t
{
    Accepted = 0,
    ZeroInput = 1,
    NonFinite = 2,
    MagnitudeRange = 3,
    TinyReduced = 4,
    RoundingBoundary = 5,
    Count = 6,
};

namespace sincos_detail
{
using namespace df_detail;

constexpr float kHalfPiHi = 1.5707963705062866f;      // 0x3fc90fdb
constexpr float kHalfPiMid = -4.371138828673793e-08f; // 0xb33bbd2e
constexpr float kHalfPiLo = -1.7151245100058819e-15f; // 0xa6f72ced
constexpr float kTwoOverPi = 0.6366197466850281f;
constexpr float kMaxAngle = 4.0f;
constexpr float kMinReduced = 5.9604644775390625e-08f; // 2^-24

// 1/n! as double-float {hi, lo}; residual below 2e-15 relative.
constexpr DoubleFloat kInvFact2 = {0.5f, 0.0f};
constexpr DoubleFloat kInvFact3 = {0.1666666716337204f, -4.967053879312289e-09f};
constexpr DoubleFloat kInvFact4 = {0.0416666679084301f, -1.2417634698280722e-09f};
constexpr DoubleFloat kInvFact5 = {0.008333333767950535f, -4.34617203337595e-10f};
constexpr DoubleFloat kInvFact6 = {0.0013888889225199819f, -3.3631094437103215e-11f};
constexpr DoubleFloat kInvFact7 = {0.00019841270113829523f, -2.725596874933456e-12f};
constexpr DoubleFloat kInvFact8 = {2.4801587642286904e-05f, -3.40699609366682e-13f};
constexpr DoubleFloat kInvFact9 = {2.7557318844628753e-06f, 3.793571224297229e-14f};
constexpr DoubleFloat kInvFact10 = {2.755731998149713e-07f, -7.575112209051195e-15f};
constexpr DoubleFloat kInvFact11 = {2.5052107943679403e-08f, 4.4176230446483665e-16f};
constexpr DoubleFloat kInvFact12 = {2.0876755879584152e-09f, 1.1082839147459852e-16f};
constexpr DoubleFloat kInvFact13 = {1.6059044372074283e-10f, -5.352526511562726e-18f};
constexpr DoubleFloat kInvFact14 = {1.147074536050896e-11f, 2.372207689231238e-19f};
constexpr DoubleFloat kInvFact15 = {7.647163609812713e-13f, 1.2200710471178288e-20f};
constexpr DoubleFloat kInvFact16 = {4.7794772561329454e-14f, 7.62544404448643e-22f};

// Total error bound relative to each product, covering the double-float
// evaluation (about 2^-45), the canonical binary64 sin/cos error (below 1 ulp)
// and the canonical product's binary64 rounding.  2^-38 leaves a wide margin.
constexpr float kRelativeErrorBound = 3.637978807091713e-12f; // 2^-38
constexpr std::uint32_t kMinMagnitudeExponent = 67U;         // 2^-60
constexpr std::uint32_t kMaxMagnitudeExponent = 187U;        // 2^+60

// sin(r) and cos(r) for |r| <= pi/4 by their Taylor series in double-float.
// Truncation after r^15 (sin) and r^16 (cos) is below 2^-54 relative.
inline void DfSinCosReduced(DoubleFloat r, DoubleFloat *sine,
                            DoubleFloat *cosine)
{
    const DoubleFloat w = DfMul(r, r);
    DoubleFloat s = DfNeg(kInvFact15);
    s = DfAdd(DfMul(s, w), kInvFact13);
    s = DfAdd(DfMul(s, w), DfNeg(kInvFact11));
    s = DfAdd(DfMul(s, w), kInvFact9);
    s = DfAdd(DfMul(s, w), DfNeg(kInvFact7));
    s = DfAdd(DfMul(s, w), kInvFact5);
    s = DfAdd(DfMul(s, w), DfNeg(kInvFact3));
    s = DfAdd(DfMul(s, w), {1.0f, 0.0f});
    *sine = DfMul(r, s);

    DoubleFloat c = kInvFact16;
    c = DfAdd(DfMul(c, w), DfNeg(kInvFact14));
    c = DfAdd(DfMul(c, w), kInvFact12);
    c = DfAdd(DfMul(c, w), DfNeg(kInvFact10));
    c = DfAdd(DfMul(c, w), kInvFact8);
    c = DfAdd(DfMul(c, w), DfNeg(kInvFact6));
    c = DfAdd(DfMul(c, w), kInvFact4);
    c = DfAdd(DfMul(c, w), DfNeg(kInvFact2));
    *cosine = DfAdd(DfMul(c, w), {1.0f, 0.0f});
}
} // namespace sincos_detail

// Evaluates cos(angle)*magnitude and sin(angle)*magnitude as double-floats
// when the input guards pass.  Returns Accepted with *x/*y set, or the guard
// reason (outputs untouched).
inline ItemSinCosFastpathReason ItemSinCosFastpathEvaluate(float angle,
                                                           float magnitude,
                                                           DoubleFloat *x,
                                                           DoubleFloat *y)
{
    using namespace sincos_detail;
    const std::uint32_t aBits = FloatBits(angle);
    const std::uint32_t mBits = FloatBits(magnitude);
    const std::uint32_t aExp = (aBits >> 23U) & 0xffU;
    const std::uint32_t mExp = (mBits >> 23U) & 0xffU;
    if (aExp == 0xffU || mExp == 0xffU)
        return ItemSinCosFastpathReason::NonFinite;
    if ((aBits & 0x7fffffffU) == 0U || (mBits & 0x7fffffffU) == 0U)
        return ItemSinCosFastpathReason::ZeroInput;
    if (mExp < kMinMagnitudeExponent || mExp > kMaxMagnitudeExponent)
        return ItemSinCosFastpathReason::MagnitudeRange;
    const float absAngle = BitsFloat(aBits & 0x7fffffffU);
    if (absAngle > kMaxAngle)
        return ItemSinCosFastpathReason::MagnitudeRange;

    // k = nearest quarter turn, r = angle - k pi/2 with a three-part pi/2.
    const float kf = angle * kTwoOverPi;
    int k = static_cast<int>(kf + (kf >= 0.0f ? 0.5f : -0.5f));
    if (k > 2)
        k = 2;
    if (k < -2)
        k = -2;
    const float kFloat = static_cast<float>(k);
    // k * part is exact for |k| <= 2 (a power of two or zero).
    DoubleFloat r = TwoSum(angle, -(kFloat * kHalfPiHi));
    r = DfAdd(r, {-(kFloat * kHalfPiMid), 0.0f});
    r = DfAdd(r, {-(kFloat * kHalfPiLo), 0.0f});
    const float absR = BitsFloat(FloatBits(r.hi) & 0x7fffffffU);
    if (absR < kMinReduced)
        return ItemSinCosFastpathReason::TinyReduced;

    DoubleFloat s;
    DoubleFloat c;
    DfSinCosReduced(r, &s, &c);
    DoubleFloat sine;
    DoubleFloat cosine;
    switch (k & 3)
    {
    case 0:
        sine = s;
        cosine = c;
        break;
    case 1:
        sine = c;
        cosine = DfNeg(s);
        break;
    case 2:
        sine = DfNeg(s);
        cosine = DfNeg(c);
        break;
    default:
        sine = DfNeg(c);
        cosine = s;
        break;
    }
    *x = DfMulF(cosine, magnitude);
    *y = DfMulF(sine, magnitude);
    return ItemSinCosFastpathReason::Accepted;
}

// Returns true with *outX/*outY set to the canonical binary32 products when
// both roundings are provably safe; false (with a reason) otherwise so the
// caller must run FromAngleMagnitude.  Never modifies the outputs on false.
inline bool ItemSinCosFastpathTry(float angle, float magnitude, float *outX,
                                  float *outY,
                                  ItemSinCosFastpathReason *reason)
{
    using namespace sincos_detail;
    DoubleFloat x{0.0f, 0.0f};
    DoubleFloat y{0.0f, 0.0f};
    const ItemSinCosFastpathReason guard =
        ItemSinCosFastpathEvaluate(angle, magnitude, &x, &y);
    if (guard != ItemSinCosFastpathReason::Accepted)
    {
        *reason = guard;
        return false;
    }
    float fx = 0.0f;
    float fy = 0.0f;
    if (!DfAcceptBinary32(x, kRelativeErrorBound, &fx) ||
        !DfAcceptBinary32(y, kRelativeErrorBound, &fy))
    {
        *reason = ItemSinCosFastpathReason::RoundingBoundary;
        return false;
    }
    *outX = fx;
    *outY = fy;
    *reason = ItemSinCosFastpathReason::Accepted;
    return true;
}
} // namespace th08::psp
