#include "ZunResult.hpp"
#include "anm_scratch.hpp"
#include "backbuffer_shadow.hpp"
#include "fileio.hpp"
#include "memory_telemetry.hpp"
#include "render_resource_arena.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>

namespace std
{
void __throw_bad_alloc() __attribute__((noreturn));
}

// Record the exact failed allocation and its call site.  The PSP C++ runtime's
// stock operator new only prints std::bad_alloc after unwinding, which loses
// both pieces of information needed to tune the 64 MiB memory layout.
void *operator new(std::size_t size)
{
    if (size == 0)
        size = 1;
    void *memory = nullptr;
    if (th08::psp::RenderResourceAllocationScopeActive())
    {
        const char *owner = th08::psp::RenderResourceAllocationOwner();
        // The transient backbuffer shadow borrows the idle ANM scratch first so
        // a texture-filled renderer arena cannot abort a text capture.
        if (owner != nullptr && std::strcmp(owner, "backbuffer shadow") == 0)
            memory = th08::psp::BackbufferShadowAcquire(size);
        if (memory == nullptr)
            memory = th08::psp::RenderResourceArenaAllocate(size, alignof(std::max_align_t), owner);
    }
    else
    {
        memory = th08_psp_tracked_malloc(size, "operator_new");
    }
    if (memory == nullptr)
    {
        const struct mallinfo heap = mallinfo();
        std::fprintf(stderr,
                     "TH08PSP NEW_FAIL size=%lu caller=%p arena=%lu used=%lu "
                     "free=%lu top=%lu\n",
                     static_cast<unsigned long>(size), __builtin_return_address(0),
                     static_cast<unsigned long>(heap.arena),
                     static_cast<unsigned long>(heap.uordblks),
                     static_cast<unsigned long>(heap.fordblks),
                     static_cast<unsigned long>(heap.keepcost));
        th08::psp::BootLog("NEW_FAIL size=%lu caller=%p arena=%lu used=%lu free=%lu top=%lu\n",
                           static_cast<unsigned long>(size), __builtin_return_address(0),
                           static_cast<unsigned long>(heap.arena),
                           static_cast<unsigned long>(heap.uordblks),
                           static_cast<unsigned long>(heap.fordblks),
                           static_cast<unsigned long>(heap.keepcost));
        th08::psp::FlushBootLog();
        std::__throw_bad_alloc();
    }
    return memory;
}

void *operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void *memory) noexcept
{
    if (th08::psp::BackbufferShadowRelease(memory))
        return;
    if (th08::psp::AnmScratchContains(memory))
    {
        th08::psp::AnmScratchRejectGenericFree(memory);
        return;
    }
    if (th08::psp::RenderResourceArenaFree(memory))
        return;
    th08_psp_tracked_free(memory);
}

void operator delete[](void *memory) noexcept
{
    if (th08::psp::BackbufferShadowRelease(memory))
        return;
    if (th08::psp::AnmScratchContains(memory))
    {
        th08::psp::AnmScratchRejectGenericFree(memory);
        return;
    }
    if (th08::psp::RenderResourceArenaFree(memory))
        return;
    th08_psp_tracked_free(memory);
}

void operator delete(void *memory, std::size_t) noexcept
{
    if (th08::psp::BackbufferShadowRelease(memory))
        return;
    if (th08::psp::AnmScratchContains(memory))
    {
        th08::psp::AnmScratchRejectGenericFree(memory);
        return;
    }
    if (th08::psp::RenderResourceArenaFree(memory))
        return;
    th08_psp_tracked_free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept
{
    if (th08::psp::BackbufferShadowRelease(memory))
        return;
    if (th08::psp::AnmScratchContains(memory))
    {
        th08::psp::AnmScratchRejectGenericFree(memory);
        return;
    }
    if (th08::psp::RenderResourceArenaFree(memory))
        return;
    th08_psp_tracked_free(memory);
}

namespace th08
{
struct Enemy;

namespace modern
{
// EnemyManager keeps the desktop render-audit hook under the shared portable
// build gate.  The audit captures desktop framebuffers and has no PSP runtime
// role; returning false makes its guarded draw replacement unreachable.
bool IsEnemyRenderAuditEnabled()
{
    return false;
}

ZunResult AuditEnemyPrimaryDraw(Enemy *)
{
    return ZUN_ERROR;
}
} // namespace modern
} // namespace th08

// Newlib's glob implementation references these BSD process-account helpers,
// but the PSP libc archive only provides declarations.  The EBOOT has no
// set-user-ID concept or login database; these results preserve glob's normal,
// unprivileged behavior and make unsupported ~user expansion fail cleanly.
extern "C" int issetugid()
{
    return 0;
}

extern "C" char *getlogin()
{
    return nullptr;
}
