#pragma once

#include <cstddef>
#include <cstdint>

namespace th08::psp
{
inline constexpr std::size_t kEnemyPoolLogicalCount = 480U;
inline constexpr std::size_t kEnemyPoolStorageCount = 481U;
inline constexpr std::size_t kBulletPoolLogicalCount = 0x600U;
inline constexpr std::size_t kBulletPoolStorageCount = 0x601U;
inline constexpr std::size_t kLaserPoolStorageCount = 0x100U;
inline constexpr std::size_t kItemPoolLogicalCount = 2096U;
inline constexpr std::size_t kItemPoolStorageCount = 2097U;
// One bit per logical Bullet records that the immutable transform program has
// reached a terminal instruction.  Keep the 1,536-bit sidecar exactly 192
// bytes and reserve it in both OFF/ON builds so isolated A/B pool addresses
// and heap geometry remain identical.
inline constexpr std::size_t kBulletTransformTerminalBytes =
    ((kBulletPoolLogicalCount + 31U) / 32U) * sizeof(std::uint32_t);

// 1,536 active bits, one five-word render-only angle cache per logical Bullet,
// and the transform-terminal bits above.  These live inside the retained
// stage arena, adding neither .bss nor a late heap allocation.  The existing
// page-alignment slack absorbs the terminal sidecar without growing the arena.
inline constexpr std::size_t kBulletRuntimeCacheBytes =
    ((kBulletPoolLogicalCount + 31U) / 32U) * sizeof(std::uint32_t) +
    kBulletPoolLogicalCount * 5U * sizeof(std::uint32_t) +
    kBulletTransformTerminalBytes;
static_assert(kBulletTransformTerminalBytes == 192U,
              "Bullet transform terminal sidecar must remain exactly 192 bytes");

// One contiguous, cached Main RAM allocation owns all four CPU-side gameplay
// pools. No GE/eDRAM/ME memory is used. Pointers remain fixed until EndStage.
// PrepareIdle reserves the backing without publishing pool pointers, allowing
// setup-only resources to borrow it and finish before BeginStage constructs
// and binds the gameplay pools.
bool StagePoolArenaPrepareIdle();
bool StagePoolArenaBeginStage();
bool StagePoolArenaEndStage(bool retainBacking);

// Valid from pool binding through EndStage. BulletManager owns the bytes and
// clears them from Initialize; the arena deliberately treats them as opaque.
void *StagePoolArenaBulletRuntimeCacheStorage();

// The gameplay pools and frontend resources are phase-exclusive.  While no
// stage is bound, a bounded set of non-overlapping transient lifetimes may
// share the pool payload (for example score decode, title image input and
// score serialization). Generic free routes each exact pointer back here, so
// none is handed to newlib. The logical gameplay pool sizes are unchanged.
void *StagePoolArenaAcquireIdleTransient(std::size_t bytes, const char *owner);
bool StagePoolArenaFreeIdleTransient(void *memory);
bool StagePoolArenaContains(const void *memory);

bool StagePoolArenaIsAllocated();
bool StagePoolArenaIsBound();
bool StagePoolArenaGuardsIntact();
std::uintptr_t StagePoolArenaBase();
std::size_t StagePoolArenaReservedBytes();
std::size_t StagePoolArenaPayloadBytes();
std::uint32_t StagePoolArenaGeneration();
std::size_t StagePoolArenaTransientActiveBytes();
std::size_t StagePoolArenaTransientPeakBytes();
std::uint32_t StagePoolArenaTransientLoanCount();
std::uint32_t StagePoolArenaTransientFailureCount();
std::uint32_t StagePoolArenaTransientQuarantineCount();
} // namespace th08::psp
