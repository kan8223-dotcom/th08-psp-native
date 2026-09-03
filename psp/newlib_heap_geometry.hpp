#pragma once

#include <cstdint>

namespace th08::psp
{
// A UINT32_MAX value is deliberately unambiguous in the text telemetry.  A
// zero-sized largest chunk is a valid result for an empty explicit arena, so
// it must not also mean that the private newlib ABI check failed.
constexpr std::uint32_t kNewlibHeapMetricUnknown = UINT32_MAX;

enum NewlibHeapScanError : std::uint32_t
{
    kNewlibHeapScanErrorNone = 0,
    kNewlibHeapScanErrorUnsupportedAbi = 1U << 0,
    kNewlibHeapScanErrorMissingSymbol = 1U << 1,
    kNewlibHeapScanErrorHeapRange = 1U << 2,
    kNewlibHeapScanErrorTopChunk = 1U << 3,
    kNewlibHeapScanErrorBinLink = 1U << 4,
    kNewlibHeapScanErrorChunkRange = 1U << 5,
    kNewlibHeapScanErrorChunkMetadata = 1U << 6,
    kNewlibHeapScanErrorFreeCount = 1U << 7,
    kNewlibHeapScanErrorFreeBytes = 1U << 8,
    kNewlibHeapScanErrorTraversalLimit = 1U << 9,
    kNewlibHeapScanErrorBinPlacement = 1U << 10,
    kNewlibHeapScanErrorBinBlock = 1U << 11,
    kNewlibHeapScanErrorLastRemainder = 1U << 12,
    kNewlibHeapScanErrorBinOrder = 1U << 13,
};

struct NewlibHeapGeometrySnapshot
{
    std::uint32_t arenaBytes;
    std::uint32_t usedBytes;
    std::uint32_t freeBytes;
    std::uint32_t topChunkBytes;
    std::uint32_t freeChunkCount;

    // Raw dlmalloc chunk size, including its boundary-tag overhead.  Use
    // largestNoGrowRequestBytes when deciding whether malloc(request) can be
    // satisfied without asking _sbrk_r to grow the heap.
    std::uint32_t largestFreeChunkBytes;
    std::uint32_t largestNoGrowRequestBytes;

    std::uint32_t scanValid;
    std::uint32_t scanErrorCount;
    std::uint32_t scanErrorFlags;
};

// Scans the classic newlib dlmalloc bins/top once while its allocator lock is
// held.  The function performs no allocation, deallocation, stdio, mallinfo or
// sbrk operation and leaves allocator ordering intact.
NewlibHeapGeometrySnapshot CaptureNewlibHeapGeometry();
} // namespace th08::psp
