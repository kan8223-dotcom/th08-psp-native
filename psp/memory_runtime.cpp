#include "memory_telemetry.hpp"

#if TH08_PSP_RUNTIME_TELEMETRY
#error memory_runtime.cpp is only for telemetry-free PSP builds
#endif

#include "anm_scratch.hpp"
#include "render_resource_arena.hpp"
#include "stage_pool_arena.hpp"

#include <cstddef>
#include <cstdlib>

// Product allocation routing.  This deliberately retains every ownership and
// lifetime rule from the diagnostic allocator wrappers while compiling out
// malloc_usable_size scans, atomics, high-water counters and allocation-event
// recording.
extern "C" void *th08_psp_tracked_malloc(std::size_t size, const char *)
{
    if (size == 0U)
        size = 1U;
    return std::malloc(size);
}

extern "C" void *th08_psp_tracked_realloc(void *memory, std::size_t size,
                                           const char *owner)
{
    if (memory == nullptr)
        return th08_psp_tracked_malloc(size, owner);

    // A renderer-arena allocation has no generic realloc contract.  Preserve
    // the diagnostic build's fail-closed behavior without touching metadata
    // using newlib's malloc inspection functions.
    if (th08::psp::RenderResourceArenaContains(memory))
        return nullptr;

    if (size == 0U)
        size = 1U;
    return std::realloc(memory, size);
}

extern "C" void th08_psp_tracked_free(void *memory)
{
    if (memory == nullptr)
        return;
    if (th08::psp::StagePoolArenaFreeIdleTransient(memory))
        return;
    if (th08::psp::AnmScratchContains(memory))
    {
        th08::psp::AnmScratchRejectGenericFree(memory);
        return;
    }
    if (th08::psp::RenderResourceArenaFree(memory))
        return;
    std::free(memory);
}

