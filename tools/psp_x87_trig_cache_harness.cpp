#include "src/ZunMath.hpp"
#include "psp/radial_trig_reuse.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace
{
template <typename T>
bool SameBits(T left, T right)
{
    static_assert(sizeof(T) == sizeof(std::uint64_t),
                  "This helper is for binary64 values");
    std::uint64_t leftBits;
    std::uint64_t rightBits;
    std::memcpy(&leftBits, &left, sizeof(leftBits));
    std::memcpy(&rightBits, &right, sizeof(rightBits));
    return leftBits == rightBits;
}

std::uint32_t FloatBits(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool CheckAngle(float angle)
{
    const double directSin = std::sin(static_cast<double>(angle));
    const double directCos = std::cos(static_cast<double>(angle));
    const double cachedSin = th08::psp::X87TrigCacheSin64(angle);
    const double cachedCos = th08::psp::X87TrigCacheCos64(angle);
    if (!SameBits(directSin, cachedSin) || !SameBits(directCos, cachedCos))
        return false;

    // Exercise the exact multiply/add/store boundary used by the gameplay
    // helpers, not merely the cached binary64 value itself.
    const float magnitude = -17.25f;
    const float addend = 301.75f;
    const float directFinal = static_cast<float>(
        directSin * static_cast<double>(magnitude) +
        static_cast<double>(addend));
    const float cachedFinal = static_cast<float>(
        cachedSin * static_cast<double>(magnitude) +
        static_cast<double>(addend));
    if (FloatBits(directFinal) != FloatBits(cachedFinal))
        return false;

    const std::int32_t integerFactor = -37;
    return
        FloatBits(th08::X87CompatibleSin(angle)) ==
            FloatBits(static_cast<float>(directSin)) &&
        FloatBits(th08::X87CompatibleCos(angle)) ==
            FloatBits(static_cast<float>(directCos)) &&
        FloatBits(th08::X87CompatibleSinMul(angle, magnitude)) ==
            FloatBits(static_cast<float>(
                directSin * static_cast<double>(magnitude))) &&
        FloatBits(th08::X87CompatibleCosMul(angle, magnitude)) ==
            FloatBits(static_cast<float>(
                directCos * static_cast<double>(magnitude))) &&
        FloatBits(th08::X87CompatibleSinMulAdd(
                      angle, magnitude, addend)) ==
            FloatBits(static_cast<float>(
                directSin * static_cast<double>(magnitude) +
                static_cast<double>(addend))) &&
        FloatBits(th08::X87CompatibleCosMulAdd(
                      angle, magnitude, addend)) ==
            FloatBits(static_cast<float>(
                directCos * static_cast<double>(magnitude) +
                static_cast<double>(addend))) &&
        FloatBits(th08::X87CompatibleSinMulInt(
                      angle, magnitude, integerFactor)) ==
            FloatBits(static_cast<float>(
                directSin * static_cast<double>(magnitude) *
                static_cast<double>(integerFactor))) &&
        FloatBits(th08::X87CompatibleCosMulInt(
                      angle, magnitude, integerFactor)) ==
            FloatBits(static_cast<float>(
                directCos * static_cast<double>(magnitude) *
                static_cast<double>(integerFactor))) &&
        SameBits(th08::psp::EvaluateCanonicalRadialSin(angle), directSin) &&
        SameBits(th08::psp::EvaluateCanonicalRadialCos(angle), directCos);
}
} // namespace

int main()
{
    th08::psp::X87TrigCacheReset();
    if (th08::psp::X87TrigCacheStorageBytes() !=
        th08::psp::kX87TrigCacheEntryCount * 24U +
            sizeof(th08::psp::X87TrigCacheStats))
    {
        std::fprintf(stderr, "x87-trig-cache: storage contract failed\n");
        return 1;
    }

    const float fixed[] = {
        0.0f, -0.0f, 1.0f, -1.0f,
        3.1415927410125732421875f,
        -3.1415927410125732421875f,
        6.283185482025146484375f,
        0.052359879016876220703125f,
        12345.5f, -12345.5f,
    };
    for (float angle : fixed)
    {
        if (!CheckAngle(angle) || !CheckAngle(angle))
        {
            std::fprintf(stderr, "x87-trig-cache: fixed mismatch angle=%08x\n",
                         FloatBits(angle));
            return 2;
        }
    }

    // More than one full cache of deterministic finite inputs forces direct
    // map replacements; a second immediate request must still hit exactly.
    std::uint32_t state = 0x4d595df4U;
    for (std::uint32_t i = 0; i < 200000U; ++i)
    {
        state = state * 1664525U + 1013904223U;
        const std::int32_t signedValue =
            static_cast<std::int32_t>(state & 0x001fffffU) - 0x00100000;
        const float angle = static_cast<float>(signedValue) / 127.0f;
        if (!CheckAngle(angle) || !CheckAngle(angle))
        {
            std::fprintf(stderr,
                         "x87-trig-cache: randomized mismatch i=%u angle=%08x\n",
                         i, FloatBits(angle));
            return 3;
        }
    }

    double sine;
    double cosine;
    th08::psp::X87TrigCacheSinCos64(0.75f, &sine, &cosine);
    if (!SameBits(sine, std::sin(0.75)) ||
        !SameBits(cosine, std::cos(0.75)))
    {
        std::fprintf(stderr, "x87-trig-cache: pair mismatch\n");
        return 4;
    }

    const double infinity = std::numeric_limits<double>::infinity();
    const double nonFiniteSin = th08::psp::X87TrigCacheSin64(
        static_cast<float>(infinity));
    const double nonFiniteCos = th08::psp::X87TrigCacheCos64(
        std::numeric_limits<float>::quiet_NaN());
    if (!std::isnan(nonFiniteSin) || !std::isnan(nonFiniteCos))
    {
        std::fprintf(stderr, "x87-trig-cache: non-finite fallback failed\n");
        return 5;
    }

    const th08::psp::X87TrigCacheStats stats =
        th08::psp::X87TrigCachePeek();
    if (stats.sinHits == 0U || stats.cosHits == 0U ||
        stats.entryReplacements == 0U || stats.nonFiniteFallbacks != 2U ||
        stats.sinHits > stats.sinRequests || stats.cosHits > stats.cosRequests)
    {
        std::fprintf(stderr, "x87-trig-cache: telemetry contract failed\n");
        return 6;
    }

    const th08::psp::X87TrigCacheStats taken =
        th08::psp::X87TrigCacheTake();
    const th08::psp::X87TrigCacheStats cleared =
        th08::psp::X87TrigCachePeek();
    if (std::memcmp(&taken, &stats, sizeof(stats)) != 0 ||
        cleared.sinRequests != 0U || cleared.cosRequests != 0U ||
        cleared.sinHits != 0U || cleared.cosHits != 0U ||
        cleared.entryReplacements != 0U ||
        cleared.nonFiniteFallbacks != 0U)
    {
        std::fprintf(stderr, "x87-trig-cache: interval take failed\n");
        return 7;
    }

    // Taking telemetry must not flush the value store.
    (void)th08::psp::X87TrigCacheSin64(0.75f);
    const th08::psp::X87TrigCacheStats warm =
        th08::psp::X87TrigCachePeek();
    if (warm.sinRequests != 1U || warm.sinHits != 1U)
    {
        std::fprintf(stderr, "x87-trig-cache: take flushed entries\n");
        return 8;
    }

    std::printf(
        "x87-trig-cache: PASS entries=%zu storage=%zu "
        "sin=%lu/%lu cos=%lu/%lu replacements=%lu nonfinite=%lu\n",
        th08::psp::kX87TrigCacheEntryCount,
        th08::psp::X87TrigCacheStorageBytes(),
        static_cast<unsigned long>(stats.sinHits),
        static_cast<unsigned long>(stats.sinRequests),
        static_cast<unsigned long>(stats.cosHits),
        static_cast<unsigned long>(stats.cosRequests),
        static_cast<unsigned long>(stats.entryReplacements),
        static_cast<unsigned long>(stats.nonFiniteFallbacks));
    return 0;
}
