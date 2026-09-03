#pragma once

#include "double_float_math.hpp"

#include <cstdint>
#include <cstring>

// Bit-exact float-result atan2 fast path for the Item autocollect angle.
//
// The canonical value is X87CompatibleAtan2(y, x) == (f32)atan2((f64)y, (f64)x),
// which on Allegrex runs newlib's software binary64 atan2.  This header
// evaluates atan2 with double-float (Dekker/Knuth) arithmetic built from
// binary32 hardware add/sub/mul/div only, to roughly 2^-44 relative
// precision, and then accepts the binary32 rounding only when the value is
// provably farther from every binary32 rounding boundary than the total
// error bound.  Every other case (zero, non-finite, extreme magnitude, tiny
// ratio, boundary proximity) reports a fallback so the caller runs the
// canonical path.  The product result is therefore identical to the canonical
// binary32 result whenever the fast path accepts.
//
// Requirements: IEEE binary32 add/sub/mul/div with round-to-nearest-even and
// no fused multiply-add (build with -ffp-contract=off; psp-gcc has no madd.s).
// Denormal flush-to-zero on Allegrex is harmless because every intermediate
// stays above 2^-90 under the magnitude guards below.

namespace th08::psp
{
enum class ItemAtan2FastpathReason : std::uint8_t
{
    Accepted = 0,
    ZeroInput = 1,
    NonFinite = 2,
    MagnitudeRange = 3,
    TinyRatio = 4,
    RoundingBoundary = 5,
    Count = 6,
};

namespace atan2_detail
{
using namespace df_detail;

// atan(k/16), k = 0..16, as {fl32(v), fl32(v - fl32(v))}: residual < 2e-15 rel.
constexpr DoubleFloat kAtanTable[17] = {
    {0.0f, 0.0f},
    {0.06241881102323532f, -1.0272779293885037e-09f},
    {0.12435499578714371f, -1.240382241363136e-09f},
    {0.18534794449806213f, 5.49763257140512e-09f},
    {0.244978666305542f, -3.1786777654474463e-09f},
    {0.30288487672805786f, -8.353086222712136e-09f},
    {0.3587706685066223f, 1.7639498750554594e-09f},
    {0.4124104380607605f, 3.5366267692182873e-09f},
    {0.46364760398864746f, 5.01215868808913e-09f},
    {0.5123894810676575f, -2.0756919738573743e-08f},
    {0.5585992932319641f, 2.2111597886009804e-08f},
    {0.6022873520851135f, -5.950149262190507e-09f},
    {0.6435011029243469f, 5.868937336117597e-09f},
    {0.6823165416717529f, 1.3202995141625706e-08f},
    {0.7188299894332886f, 1.0188335508587443e-08f},
    {0.7531512975692749f, -1.660708015549517e-08f},
    {0.7853981852531433f, -2.1855694143368964e-08f},
};
constexpr DoubleFloat kHalfPi = {1.5707963705062866f, -4.371138828673793e-08f};
constexpr DoubleFloat kPi = {3.1415927410125732f, -8.742277657347586e-08f};
constexpr DoubleFloat kInv3 = {0.3333333432674408f, -9.934107758624577e-09f};
constexpr DoubleFloat kInv5 = {0.20000000298023224f, -2.9802322831784522e-09f};
constexpr DoubleFloat kInv7 = {0.1428571492433548f, -6.38621200366174e-09f};
constexpr DoubleFloat kInv9 = {0.1111111119389534f, -8.278422947149977e-10f};
constexpr DoubleFloat kInv11 = {0.09090909361839294f, -2.709302115988521e-09f};

// Total error bound of the double-float evaluation relative to |result|,
// including the canonical binary64 atan2's own sub-ulp error.  The host
// harness measures roughly 2^-44 to 2^-46; 2^-38 leaves a wide margin and
// still makes the boundary fallback rate about 2^-14 per call.
constexpr float kRelativeErrorBound = 3.637978807091713e-12f; // 2^-38
constexpr float kTinyRatio = 9.5367431640625e-07f;            // 2^-20
constexpr std::uint32_t kMinExponent = 67U;                   // 2^-60
constexpr std::uint32_t kMaxExponent = 187U;                  // 2^+60
} // namespace atan2_detail

// Evaluates atan2(y, x) as a double-float when the input guards pass.
// Returns Accepted with *value set, or the guard reason (value untouched).
// The rounding-boundary decision is made separately by ItemAtan2FastpathTry
// so a host harness can measure the raw double-float error.
inline ItemAtan2FastpathReason ItemAtan2FastpathEvaluate(float y, float x,
                                                         DoubleFloat *value)
{
    using namespace atan2_detail;
    const std::uint32_t xBits = FloatBits(x);
    const std::uint32_t yBits = FloatBits(y);
    const std::uint32_t xExp = (xBits >> 23U) & 0xffU;
    const std::uint32_t yExp = (yBits >> 23U) & 0xffU;
    if (xExp == 0xffU || yExp == 0xffU)
        return ItemAtan2FastpathReason::NonFinite;
    if ((xBits & 0x7fffffffU) == 0U || (yBits & 0x7fffffffU) == 0U)
        return ItemAtan2FastpathReason::ZeroInput;
    if (xExp < kMinExponent || xExp > kMaxExponent || yExp < kMinExponent ||
        yExp > kMaxExponent)
        return ItemAtan2FastpathReason::MagnitudeRange;

    const float ax = BitsFloat(xBits & 0x7fffffffU);
    const float ay = BitsFloat(yBits & 0x7fffffffU);
    const bool swapped = ay > ax;
    const float mn = swapped ? ax : ay;
    const float mx = swapped ? ay : ax;
    if (mn < mx * kTinyRatio)
        return ItemAtan2FastpathReason::TinyRatio;

    // t = mn / mx in [2^-20, 1].
    const DoubleFloat t = DfDiv({mn, 0.0f}, {mx, 0.0f});
    int k = static_cast<int>(t.hi * 16.0f + 0.5f);
    if (k < 0)
        k = 0;
    if (k > 16)
        k = 16;
    const float c = static_cast<float>(k) * 0.0625f;

    // u = (t - c) / (1 + t c), |u| <= 1/32.
    const DoubleFloat num = DfAdd(t, {-c, 0.0f});
    const DoubleFloat den = DfAdd({1.0f, 0.0f}, DfMulF(t, c));
    const DoubleFloat u = DfDiv(num, den);
    const DoubleFloat w = DfMul(u, u);

    // atan(u) = u (1 - w/3 + w^2/5 - w^3/7 + w^4/9 - w^5/11 ...); the omitted
    // w^6/13 term is below 2^-65 |u| for |u| <= 1/32.
    DoubleFloat p = DfNeg(kInv11);
    p = DfAdd(DfMul(p, w), kInv9);
    p = DfAdd(DfMul(p, w), DfNeg(kInv7));
    p = DfAdd(DfMul(p, w), kInv5);
    p = DfAdd(DfMul(p, w), DfNeg(kInv3));
    p = DfAdd(DfMul(p, w), {1.0f, 0.0f});
    DoubleFloat a = DfAdd(kAtanTable[k], DfMul(u, p));

    if (swapped)
        a = DfAdd(kHalfPi, DfNeg(a));
    if ((xBits & 0x80000000U) != 0U)
        a = DfAdd(kPi, DfNeg(a));
    if ((yBits & 0x80000000U) != 0U)
        a = DfNeg(a);
    *value = a;
    return ItemAtan2FastpathReason::Accepted;
}

// Returns true and stores the binary32 canonical result in *out when the
// evaluation is provably safe; returns false (with a reason) otherwise so the
// caller must run X87CompatibleAtan2(y, x).  Never modifies *out on false.
inline bool ItemAtan2FastpathTry(float y, float x, float *out,
                                 ItemAtan2FastpathReason *reason)
{
    using namespace atan2_detail;
    DoubleFloat a{0.0f, 0.0f};
    const ItemAtan2FastpathReason guard = ItemAtan2FastpathEvaluate(y, x, &a);
    if (guard != ItemAtan2FastpathReason::Accepted)
    {
        *reason = guard;
        return false;
    }

    // Shared rounding-cell acceptance: the whole error interval must stay
    // strictly inside a.hi's binary32 cell (tiny/non-finite also decline).
    if (!DfAcceptBinary32(a, kRelativeErrorBound, out))
    {
        const std::uint32_t hiExp = (FloatBits(a.hi) >> 23U) & 0xffU;
        *reason = (hiExp < 2U || hiExp == 0xffU)
                      ? ItemAtan2FastpathReason::MagnitudeRange
                      : ItemAtan2FastpathReason::RoundingBoundary;
        return false;
    }
    *reason = ItemAtan2FastpathReason::Accepted;
    return true;
}
} // namespace th08::psp
