#pragma once

#include <cstdint>

namespace th08::psp
{
// Opcode 135 owns a complete 0x24b0-byte ECL context/call-stack block.  The
// PSP path keeps the original one-allocation-per-live-child lifetime, but
// sources those small, short-lived blocks from already-reserved Main RAM when
// possible.  There is deliberately no fixed child-block capacity: the normal
// heap remains the exact fallback when the render-resource arena is full.
void *EnemyChildEclAllocate();
void EnemyChildEclFree(void *memory);

struct EnemyChildEclMemorySnapshot
{
    std::uint32_t blockBytes;
    std::uint32_t liveBlocks;
    std::uint32_t peakBlocks;
    std::uint32_t liveBytes;
    std::uint32_t peakBytes;
    std::uint32_t arenaLiveBlocks;
    std::uint32_t arenaPeakBlocks;
    std::uint32_t heapLiveBlocks;
    std::uint32_t heapPeakBlocks;
    std::uint32_t arenaAllocations;
    std::uint32_t arenaMisses;
    std::uint32_t heapFallbacks;
    std::uint32_t allocationFailures;
    std::uint32_t freeFailures;
};

EnemyChildEclMemorySnapshot CaptureEnemyChildEclMemorySnapshot();
} // namespace th08::psp
