#include "x87_trig_cache.hpp"
#include "runtime_telemetry_config.hpp"

#if defined(TH08_PSP_X87_TRIG_CACHE) && TH08_PSP_X87_TRIG_CACHE

#include <cmath>
#include <cstring>

namespace th08::psp
{
namespace
{
enum : std::uint32_t
{
    kSinValid = 1U,
    kCosValid = 2U,
};

struct alignas(8) X87TrigCacheEntry
{
    std::uint32_t key;
    std::uint32_t validMask;
    double sine;
    double cosine;
};

static_assert(sizeof(X87TrigCacheEntry) == 24U,
              "Unexpected x87 trig cache entry size");
static_assert((kX87TrigCacheEntryCount &
               (kX87TrigCacheEntryCount - 1U)) == 0U,
              "x87 trig cache size must be a power of two");

X87TrigCacheEntry gEntries[kX87TrigCacheEntryCount];
#if TH08_PSP_RUNTIME_TELEMETRY
X87TrigCacheStats gStats;
#define TH08_PSP_X87_NOTE(field) (++gStats.field)
#else
#define TH08_PSP_X87_NOTE(field) ((void)0)
#endif

std::uint32_t FloatBits(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::size_t CacheIndex(std::uint32_t bits)
{
    // A small integer avalanche prevents common angle mantissas from
    // clustering while keeping lookup cheaper than even one binary64 libm
    // call on Allegrex.
    bits ^= bits >> 16U;
    bits *= 0x7feb352dU;
    bits ^= bits >> 15U;
    return static_cast<std::size_t>(bits) &
           (kX87TrigCacheEntryCount - 1U);
}

bool IsFiniteBits(std::uint32_t bits)
{
    return (bits & 0x7f800000U) != 0x7f800000U;
}

X87TrigCacheEntry *FindEntry(std::uint32_t bits)
{
    X87TrigCacheEntry &entry = gEntries[CacheIndex(bits)];
    if (entry.validMask != 0U && entry.key != bits)
    {
        TH08_PSP_X87_NOTE(entryReplacements);
        entry.validMask = 0U;
    }
    entry.key = bits;
    return &entry;
}
} // namespace

double X87TrigCacheSin64(float angle)
{
    TH08_PSP_X87_NOTE(sinRequests);
    const std::uint32_t bits = FloatBits(angle);
    if (!IsFiniteBits(bits))
    {
        TH08_PSP_X87_NOTE(nonFiniteFallbacks);
        return std::sin(static_cast<double>(angle));
    }

    X87TrigCacheEntry *entry = FindEntry(bits);
    if ((entry->validMask & kSinValid) != 0U)
    {
        TH08_PSP_X87_NOTE(sinHits);
        return entry->sine;
    }

    entry->sine = std::sin(static_cast<double>(angle));
    entry->validMask |= kSinValid;
    return entry->sine;
}

double X87TrigCacheCos64(float angle)
{
    TH08_PSP_X87_NOTE(cosRequests);
    const std::uint32_t bits = FloatBits(angle);
    if (!IsFiniteBits(bits))
    {
        TH08_PSP_X87_NOTE(nonFiniteFallbacks);
        return std::cos(static_cast<double>(angle));
    }

    X87TrigCacheEntry *entry = FindEntry(bits);
    if ((entry->validMask & kCosValid) != 0U)
    {
        TH08_PSP_X87_NOTE(cosHits);
        return entry->cosine;
    }

    entry->cosine = std::cos(static_cast<double>(angle));
    entry->validMask |= kCosValid;
    return entry->cosine;
}

void X87TrigCacheSinCos64(float angle, double *sine, double *cosine)
{
    if (sine != nullptr)
        *sine = X87TrigCacheSin64(angle);
    if (cosine != nullptr)
        *cosine = X87TrigCacheCos64(angle);
}

void X87TrigCacheReset()
{
    std::memset(gEntries, 0, sizeof(gEntries));
#if TH08_PSP_RUNTIME_TELEMETRY
    std::memset(&gStats, 0, sizeof(gStats));
#endif
}

X87TrigCacheStats X87TrigCacheTake()
{
#if TH08_PSP_RUNTIME_TELEMETRY
    const X87TrigCacheStats result = gStats;
    std::memset(&gStats, 0, sizeof(gStats));
    return result;
#else
    return X87TrigCacheStats{};
#endif
}

X87TrigCacheStats X87TrigCachePeek()
{
#if TH08_PSP_RUNTIME_TELEMETRY
    return gStats;
#else
    return X87TrigCacheStats{};
#endif
}

std::size_t X87TrigCacheStorageBytes()
{
#if TH08_PSP_RUNTIME_TELEMETRY
    return sizeof(gEntries) + sizeof(gStats);
#else
    return sizeof(gEntries);
#endif
}

#undef TH08_PSP_X87_NOTE
} // namespace th08::psp

#endif
