#pragma once

#include <cstddef>
#include <cstdint>

namespace th08::psp
{
// PSPGL spills immutable textures from GE eDRAM to Main RAM.  Reserve one
// contiguous run-lifetime arena before frontend allocations can fragment the
// heap, then admit only explicitly scoped renderer storage.
bool RenderResourceArenaInitialize();

class RenderResourceAllocationScope
{
  public:
    explicit RenderResourceAllocationScope(const char *owner);
    ~RenderResourceAllocationScope();

    RenderResourceAllocationScope(const RenderResourceAllocationScope &) = delete;
    RenderResourceAllocationScope &operator=(const RenderResourceAllocationScope &) = delete;

  private:
    bool entered_;
};

// SDL_image and libpng allocate their decoded image and row workspaces through
// malloc/calloc rather than C++ new or memalign.  This narrower scope opts only
// the full-screen image decoder into the arena-backed C allocator wrappers;
// ordinary renderer scopes retain their existing allocation behaviour.
class SurfaceDecodeAllocationScope
{
  public:
    explicit SurfaceDecodeAllocationScope(const char *owner);
    ~SurfaceDecodeAllocationScope();

    SurfaceDecodeAllocationScope(const SurfaceDecodeAllocationScope &) = delete;
    SurfaceDecodeAllocationScope &operator=(const SurfaceDecodeAllocationScope &) = delete;

  private:
    bool entered_;
};

void *RenderResourceArenaAllocate(std::size_t bytes, std::size_t alignment,
                                  const char *owner);
// Logs a census of live arena blocks (>= minBytes) to the boot log and flushes.
void RenderResourceArenaLogCensus(const char *tag, std::size_t minBytes);
// Largest free block payload currently available (0 when the arena is absent).
std::size_t RenderResourceArenaLargestFree();
// True when a block of `bytes` could be carved right now (true before init).
bool RenderResourceArenaCanAllocate(std::size_t bytes);
void *RenderResourceArenaReallocate(void *memory, std::size_t bytes,
                                    std::size_t alignment, const char *owner);

// The tri-state API distinguishes a normal external pointer from an arena
// pointer that must be consumed even after corruption. Quarantined means the
// pointer was rejected and absorbed; only structural metadata corruption
// globally poisons the arena. The bool compatibility wrapper below keeps
// existing delete/free call sites source-compatible.
enum class RenderResourceArenaFreeResult : std::uint32_t
{
    NotOwned = 0,
    Freed = 1,
    Quarantined = 2,
};

RenderResourceArenaFreeResult RenderResourceArenaTryFree(void *memory);
bool RenderResourceArenaFree(void *memory);
// Broad quarantine-span test used before generic malloc/realloc inspection;
// it deliberately includes the one-past-end address.
bool RenderResourceArenaContains(const void *memory);
bool RenderResourceAllocationScopeActive();
bool SurfaceDecodeAllocationScopeActive();
const char *RenderResourceAllocationOwner();

struct RenderResourceArenaSnapshot
{
    std::uint32_t capacityBytes;
    std::uint32_t liveBytes;
    std::uint32_t peakBytes;
    std::uint32_t freeBytes;
    std::uint32_t largestFreeBytes;
    std::uint32_t allocationCount;
    std::uint32_t failureCount;
    std::uint32_t liveAllocations;
    std::uint32_t quarantineCount;
    std::uint32_t scopeContentionCount;
    std::uint32_t poisoned;
};

RenderResourceArenaSnapshot CaptureRenderResourceArenaSnapshot();
} // namespace th08::psp
