#pragma once

#include <cstddef>
#include <cstdint>

namespace th08::psp
{

// Gameplay math must keep the binary64 libm boundary used to reproduce the
// retail x87 result.  This cache stores that exact binary64 result; it never
// substitutes a VFPU approximation.  Collisions only cause a canonical libm
// recomputation, so cache state cannot change simulation output.
// Ownership is the PSP game-frame thread: current startup/setup/audio/platform
// workers do not call these gameplay wrappers, and asynchronous telemetry MARK
// records must never inspect this unsynchronised hot store.
constexpr std::size_t kX87TrigCacheEntryCount = 512U;

struct X87TrigCacheStats
{
    // One interval cannot approach 2^32 calls.  Keeping these counters native
    // width avoids adding software 64-bit increments to the very hot lookup
    // whose cost this telemetry is intended to measure.
    std::uint32_t sinRequests;
    std::uint32_t cosRequests;
    std::uint32_t sinHits;
    std::uint32_t cosHits;
    std::uint32_t entryReplacements;
    std::uint32_t nonFiniteFallbacks;
};

double X87TrigCacheSin64(float angle);
double X87TrigCacheCos64(float angle);
void X87TrigCacheSinCos64(float angle, double *sine, double *cosine);

void X87TrigCacheReset();
// Return and clear only interval counters.  Cached values remain warm, so a
// telemetry boundary cannot perturb gameplay timing or output.
X87TrigCacheStats X87TrigCacheTake();
X87TrigCacheStats X87TrigCachePeek();
std::size_t X87TrigCacheStorageBytes();

} // namespace th08::psp
