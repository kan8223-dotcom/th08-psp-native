#pragma once

#include "double_float_math.hpp"

#include <cstdint>

// TH08_PSP_TRIG_DF_TABLE=1: sin/cos of the reduced argument come from a node
// table (a = k/256) plus a short series in h = r - a instead of the degree
// 15/16 Taylor series.  Same reduction, quadrant handling, products and
// acceptance rule, so accepted results are unchanged; only the evaluation
// cost drops (about 10 DfMul + 6 DfAdd instead of 19 DfMul + 19 DfAdd).
#if defined(TH08_PSP_TRIG_DF_TABLE) && TH08_PSP_TRIG_DF_TABLE
#define TH08_PSP_TRIG_DF_TABLE_ENABLED 1
#include "trig_df_table_data.hpp"
#else
#define TH08_PSP_TRIG_DF_TABLE_ENABLED 0
#endif

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
#if TH08_PSP_TRIG_DF_TABLE_ENABLED
constexpr float kTrigDfNodeScale = 256.0f;
constexpr float kTrigDfNodeStep = 0.00390625f; // 2^-8

inline DoubleFloat DfAddF(DoubleFloat x, float y)
{
    DoubleFloat s = TwoSum(x.hi, y);
    s.lo += x.lo;
    return QuickTwoSum(s.hi, s.lo);
}

// sin(r), cos(r) for |r| <= 0.86 as r = a + h, a = k/256 the nearest node
// (|h| <= 2^-9 + 2^-24): sin(a+h) = sin a cos h + cos a sin h.
// Series truncation: sin h to h^5 (next term h^7/5040, below 2^-60 relative),
// cos h to h^4 (next term h^6/720, below 2^-63).  The double-float arithmetic
// keeps the result within about 2^-44 relative, inside the 2^-38 acceptance
// bound.  Returns false when r lies outside the table (never for |r| <= 0.86).
inline bool DfSinCosReducedTable(DoubleFloat r, DoubleFloat *sine,
                                 DoubleFloat *cosine)
{
    const float kf = r.hi * kTrigDfNodeScale;
    const int k = static_cast<int>(kf + (kf >= 0.0f ? 0.5f : -0.5f));
    const int index = k < 0 ? -k : k;
    if (index > kTrigDfNodeMax)
        return false;
    const float a = static_cast<float>(k) * kTrigDfNodeStep; // exact
    const DoubleFloat h = DfAddF(r, -a);                     // r.hi - a is exact
    const DoubleFloat w = DfMul(h, h);                       // |w| <= 2^-18 (+ a little)
    // w^2 <= 2^-36: the w^2 terms are below 2^-40 of the result, so one binary32
    // product carries them with error below 2^-64.
    const float w2 = w.hi * w.hi;
    // sin h = h * (1 - w/6 + w^2/120)
    DoubleFloat sp = DfNeg(DfMul(w, kInvFact3));
    sp.lo += w2 * kInvFact5.hi;
    sp = DfAddF(sp, 1.0f);
    const DoubleFloat sh = DfMul(h, sp);
    // cos h - 1 = -w/2 + w^2/24: the halving is exact.
    DoubleFloat cm = {-0.5f * w.hi, -0.5f * w.lo};
    cm.lo += w2 * kInvFact4.hi;
    const DoubleFloat ch = DfAddF(cm, 1.0f);
    const TrigDfNode &node = kTrigDfNodes[index];
    DoubleFloat sa = {node.sinHi, node.sinLo};
    if (k < 0)
        sa = DfNeg(sa);
    const DoubleFloat ca = {node.cosHi, node.cosLo};
    *sine = DfAdd(DfMul(sa, ch), DfMul(ca, sh));
    // cos(a + h) = ca - sa*sh + ca*(ch - 1).  |ca*(ch - 1)| <= 2^-19 while
    // cos r >= 0.65, so that term needs only binary32: cm.hi + cm.lo keeps the
    // w^2/24 part (2^-21 of cm.hi) and the product error stays below 2^-43.
    DoubleFloat c = DfAdd(ca, DfNeg(DfMul(sa, sh)));
    c = QuickTwoSum(c.hi, c.lo + ca.hi * (cm.hi + cm.lo));
    *cosine = c;
    return true;
}
#endif
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
#if TH08_PSP_TRIG_DF_TABLE_ENABLED
    r = DfAddF(r, -(kFloat * kHalfPiMid)); // same value as DfAdd with a {x, 0} operand
    r = DfAddF(r, -(kFloat * kHalfPiLo));
#else
    r = DfAdd(r, {-(kFloat * kHalfPiMid), 0.0f});
    r = DfAdd(r, {-(kFloat * kHalfPiLo), 0.0f});
#endif
    const float absR = BitsFloat(FloatBits(r.hi) & 0x7fffffffU);
    if (absR < kMinReduced)
        return ItemSinCosFastpathReason::TinyReduced;

    DoubleFloat s;
    DoubleFloat c;
#if TH08_PSP_TRIG_DF_TABLE_ENABLED
    if (!DfSinCosReducedTable(r, &s, &c))
        return ItemSinCosFastpathReason::MagnitudeRange;
#else
    DfSinCosReduced(r, &s, &c);
#endif
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
