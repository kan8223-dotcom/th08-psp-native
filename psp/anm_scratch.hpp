#pragma once

#include <cstddef>
#include <cstdint>

namespace th08::psp
{
struct AnmScratchLease
{
    void *base;
    std::size_t bytes;
    int anmIndex;
    std::uint32_t generation;
};

// Reserve the largest original TH08 ANM source before the general heap is
// fragmented. The backing block is retained until process exit and loaned to
// one ANM decode at a time.
bool AnmScratchInitialize();
bool AnmScratchTryAcquire(std::size_t bytes, int anmIndex, const char *owner,
                          AnmScratchLease *outLease);
bool AnmScratchRelease(const AnmScratchLease &lease);

// Transition framebuffer conversion and ANM decompression are mutually
// exclusive phases. Reserve the retained source scratch before the async
// loader starts, borrow a prefix, and return it before the first ANM decode.
bool AnmScratchReserveTransition(const char *owner);
void *AnmScratchTransitionBase();
bool AnmScratchTransitionSetActiveBytes(std::size_t bytes);
bool AnmScratchReleaseTransition();
bool AnmScratchTransitionActive();

bool AnmScratchContains(const void *memory);
void AnmScratchRejectGenericFree(void *memory);
std::size_t AnmScratchCapacity();
std::size_t AnmScratchActiveBytes();
bool AnmScratchBusy();
bool AnmScratchPoisoned();
std::uint32_t AnmScratchGeneration();
int AnmScratchOwnerIndex();
} // namespace th08::psp
