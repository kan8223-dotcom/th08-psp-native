#include "swap_triple.hpp"

#if TH08_PSP_SWAP_TRIPLE_ENABLED

#include "fileio.hpp"
#include "perf_attribution.hpp"
#include "swap_async.hpp"

#include <cstddef>
#include <cstdint>

#include <GL/gl.h>
#include <GL/glext.h>
#include <GLES/egl.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspkernel.h>
#include <pspthreadman.h>

// Vendored PSPGL headers (deps/pspgl-ge4/include); the layout is pinned by the
// same static_asserts as swap_async.cpp.
extern "C" {
#include "pspgl_internal.h"
#include "pspgl_buffers.h"
}

static_assert(offsetof(struct pspgl_surface, pixfmt) == 4U, "pspgl_surface layout");
static_assert(offsetof(struct pspgl_surface, pixelperline) == 12U, "pspgl_surface layout");
static_assert(offsetof(struct pspgl_surface, flags) == 14U, "pspgl_surface layout");
static_assert(offsetof(struct pspgl_surface, color_front) == 16U, "pspgl_surface layout");
static_assert(offsetof(struct pspgl_surface, color_back) == 20U, "pspgl_surface layout");

#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
// The peek must not be booked as a GE wait by the attribution wrapper.
extern "C" int __real_sceGeListSync(int qid, int syncType);
#define TH08_SWAP_TRIPLE_PEEK(qid) __real_sceGeListSync((qid), 1)
#else
#define TH08_SWAP_TRIPLE_PEEK(qid) sceGeListSync((qid), 1)
#endif

// deps/pspgl-ge4/pspgl-th08-ge4-v1.patch: a private buffer usage that makes
// __pspgl_buffer_init allocate from the GE4 upper 2 MiB tier only (the same
// shelf the texture promotion uses).  The lower tier keeps its 1.26 MB for
// PSPGL textures: r124 took the third buffer from there and starved the
// texture working set (per-frame evictions, draw CPU 9 -> 33 ms/tick).
#define TH08_PSPGL_UPPER_STATIC 0x60000001u
constexpr std::uintptr_t kUpperEdramBase = 0x04200000u;
constexpr std::uintptr_t kUpperEdramEnd = 0x04400000u;

namespace th08::psp
{
namespace
{
struct pspgl_buffer *gSpare = NULL;     // the buffer with no role right now
struct pspgl_buffer *gDisplayed = NULL; // what the display scans (or will, once the pending flip lands)
struct pspgl_buffer *gPendingBuf = NULL; // drawn, flip not yet visible
int gPendingQid = -1;
bool gPendingRequested = false;
int gLastQid = -1;
bool gActive = false;

unsigned long gPresents = 0U;
unsigned long gFlipsHook = 0U;
unsigned long gFlipsPresent = 0U;
unsigned long gFlipsForced = 0U;
unsigned long gDisplayWaits = 0U;
std::uint64_t gDisplayWaitUs = 0U;
std::uint64_t gForcedGeWaitUs = 0U;
unsigned long gFlipsFence = 0U;
unsigned long gFenceWaits = 0U;
std::uint64_t gFenceWaitUs = 0U;
unsigned long gDrains = 0U;

inline std::uint64_t NowUs()
{
    return static_cast<std::uint64_t>(sceKernelGetSystemTimeWide());
}

unsigned long gFlipErrors = 0U;
unsigned long gDisplayTimeouts = 0U;

void RequestFlip(unsigned long &counter)
{
    struct pspgl_surface *const surface = __pspgl_curctx->draw;
    if (sceDisplaySetFrameBuf(gPendingBuf->base, surface->pixelperline,
                              static_cast<int>(surface->pixfmt),
                              PSP_DISPLAY_SETBUF_NEXTFRAME) < 0)
        ++gFlipErrors;
    gPendingRequested = true;
    ++counter;
}
} // namespace

bool SwapTripleInitialize()
{
    if (gActive)
        return true;
    struct pspgl_surface *const surface = __pspgl_curctx->draw;
    if (surface == NULL || surface->color_front == NULL ||
        surface->color_back == NULL || surface->color_front == surface->color_back)
    {
        BootLog("SWAP_TRIPLE init=FAIL reason=surface\n");
        return false;
    }
    const unsigned long bytesPerPixel = surface->pixfmt == GE_RGBA_8888 ? 4UL : 2UL;
    const unsigned long bufferBytes =
        static_cast<unsigned long>(surface->height) * surface->pixelperline * bytesPerPixel;
    const size_t availBefore = __pspgl_vidmem_avail();
    gSpare = __pspgl_buffer_new(static_cast<GLsizeiptr>(bufferBytes), TH08_PSPGL_UPPER_STATIC);
    if (gSpare == NULL || gSpare->base == NULL)
    {
        BootLog("SWAP_TRIPLE init=FAIL reason=upper_vidmem pixfmt=%u bytes=%lu lower_avail=%lu\n",
                surface->pixfmt, bufferBytes, static_cast<unsigned long>(availBefore));
        if (gSpare != NULL)
            __pspgl_buffer_free(gSpare);
        gSpare = NULL;
        return false;
    }
    const std::uintptr_t spareBase =
        reinterpret_cast<std::uintptr_t>(gSpare->base) & 0x1fffffffu;
    if (spareBase < kUpperEdramBase || spareBase + bufferBytes > kUpperEdramEnd)
    {
        // Never take the third buffer from the lower tier (r124 thrash).
        BootLog("SWAP_TRIPLE init=FAIL reason=not_upper base=%p bytes=%lu\n",
                gSpare->base, bufferBytes);
        __pspgl_buffer_free(gSpare);
        gSpare = NULL;
        return false;
    }
    if (__pspgl_vidmem_avail() != availBefore)
    {
        BootLog("SWAP_TRIPLE init=FAIL reason=lower_tier_consumed avail=%lu->%lu\n",
                static_cast<unsigned long>(availBefore),
                static_cast<unsigned long>(__pspgl_vidmem_avail()));
        __pspgl_buffer_free(gSpare);
        gSpare = NULL;
        return false;
    }
    gSpare->flags |= BF_PINNED_FIXED;
    // The display controller must accept an upper-tier scan-out address.
    // Probe with an IMMEDIATE switch and restore at once (sub-millisecond,
    // during boot); a rejected or ignored address disables the mode.
    {
        void *oldTop = NULL;
        int oldWidth = 0;
        int oldFormat = 0;
        const int q0 = sceDisplayGetFrameBuf(&oldTop, &oldWidth, &oldFormat,
                                             PSP_DISPLAY_SETBUF_IMMEDIATE);
        const int set = sceDisplaySetFrameBuf(gSpare->base, surface->pixelperline,
                                              static_cast<int>(surface->pixfmt),
                                              PSP_DISPLAY_SETBUF_IMMEDIATE);
        const bool shows = SwapDisplayShows(gSpare->base);
        if (q0 >= 0 && oldTop != NULL)
            sceDisplaySetFrameBuf(oldTop, oldWidth, oldFormat, PSP_DISPLAY_SETBUF_IMMEDIATE);
        if (set < 0 || !shows)
        {
            BootLog("SWAP_TRIPLE init=FAIL reason=display_rejects_upper set=0x%08x shows=%d base=%p\n",
                    static_cast<unsigned int>(set), shows ? 1 : 0, gSpare->base);
            gSpare->flags = static_cast<unsigned char>(gSpare->flags & ~BF_PINNED_FIXED);
            __pspgl_buffer_free(gSpare);
            gSpare = NULL;
            return false;
        }
    }
    // PSPGL's front buffer is the one the display shows before our first
    // rotation (the SWAP_NOWAIT swap kept that contract).
    gDisplayed = surface->color_front;
    gPendingBuf = NULL;
    gPendingQid = -1;
    gPendingRequested = false;
    gActive = true;
    BootLog("SWAP_TRIPLE init=OK tier=upper pixfmt=%u bytes=%lu lower_avail_before=%lu lower_avail_after=%lu "
            "front=%p back=%p spare=%p surface_layout=verified\n",
            surface->pixfmt, bufferBytes, static_cast<unsigned long>(availBefore),
            static_cast<unsigned long>(__pspgl_vidmem_avail()),
            surface->color_front->base, surface->color_back->base, gSpare->base);
    return true;
}

bool SwapTripleActive()
{
    return gActive;
}

void SwapTriplePoll()
{
    if (!gActive || gPendingBuf == NULL || gPendingRequested)
        return;
    // Done, or an id the kernel already recycled: the lists finished.
    const int state = TH08_SWAP_TRIPLE_PEEK(gPendingQid);
    if (state <= 0)
        RequestFlip(gFlipsHook);
}

void SwapTripleNoteListEnqueued(int qid)
{
    gLastQid = qid;
    SwapTriplePoll();
}

void SwapTripleWaitPendingDone()
{
    if (!gActive || gPendingBuf == NULL || gPendingRequested)
        return;
    const std::uint64_t t0 = NowUs();
    sceGeListSync(gPendingQid, 0);
    gFenceWaitUs += NowUs() - t0;
    ++gFenceWaits;
    RequestFlip(gFlipsFence);
}

void SwapTripleDrain()
{
    if (!gActive)
        return;
    SwapTripleWaitPendingDone();
    sceGeDrawSync(0);
    ++gDrains;
}

namespace
{
// The NEXTFRAME switch is applied a little after the VBlank interrupt wakes
// sceDisplayWaitVblankStart: poll briefly before spending another VBlank.
void WaitDisplayShows(const void *base)
{
    // Bounded: a flip the display never applies (rejected address) must not
    // hang the game; after ~8 VBlanks give up and count it.
    for (int vblanks = 0; vblanks < 8; ++vblanks)
    {
        for (int i = 0; i < 20; ++i)
        {
            if (SwapDisplayShows(base))
                return;
            sceKernelDelayThread(100);
        }
        sceDisplayWaitVblankStart();
        if (SwapDisplayShows(base))
            return;
    }
    ++gDisplayTimeouts;
}
} // namespace

void SwapTriplePresent(std::uint64_t *waitedUs)
{
    *waitedUs = 0U;
    if (!gActive)
        return;
    struct pspgl_surface *const surface = __pspgl_curctx->draw;
    struct pspgl_buffer *const drawn = *surface->draw; // frame N, just flushed
    const int drawnQid = gLastQid;
    const std::uint64_t start = NowUs();

    if (gPendingBuf != NULL)
    {
        if (!gPendingRequested)
        {
            // Frame N-1 is still on the GE (a full frame later: rare).  This
            // sync is booked by the attribution wrapper as the swap's GE wait.
            const std::uint64_t t0 = NowUs();
            sceGeListSync(gPendingQid, 0);
            gForcedGeWaitUs += NowUs() - t0;
            RequestFlip(gFlipsForced);
        }
        // The buffer we are about to hand to the GE is the one currently on
        // screen; it is free only once the display scans frame N-1.
        if (!SwapDisplayShows(gPendingBuf->base))
        {
            ++gDisplayWaits;
            const std::uint64_t t0 = NowUs();
            WaitDisplayShows(gPendingBuf->base);
            gDisplayWaitUs += NowUs() - t0;
        }
        // Rotation: N-1 is displayed, its predecessor is free, N is pending.
        struct pspgl_buffer *const freed = gDisplayed;
        gDisplayed = gPendingBuf;
        gSpare = freed;
    }
    struct pspgl_buffer *const next = gSpare; // never displayed, never pending
    gSpare = drawn;
    gPendingBuf = drawn;
    gPendingQid = drawnQid;
    gPendingRequested = false;

    // Point PSPGL and the GE at the free buffer.  With SURF_DISPLAYED cleared
    // the setup emits only the PSM/DRAWBUF/DEPTHBUF commands into the fresh
    // display list (executed after frame N's lists): no GE sync, no display
    // call.  color_front stays what the display shows.
    surface->color_front = gDisplayed;
    surface->color_back = next;
    const unsigned char savedFlags = surface->flags;
    surface->flags = static_cast<unsigned char>(savedFlags & ~SURF_DISPLAYED);
    __pspgl_vidmem_setup_write_and_display_buffer(surface);
    surface->flags = savedFlags;

    // A light frame may already be done.
    if (TH08_SWAP_TRIPLE_PEEK(gPendingQid) <= 0)
        RequestFlip(gFlipsPresent);

    ++gPresents;
    if ((gPresents % 600UL) == 0UL)
    {
        BootLog("SWAP_TRIPLE stats presents=%lu flips_hook=%lu flips_present=%lu "
                "flips_forced=%lu forced_ge_wait_us=%llu flips_fence=%lu fence_waits=%lu "
                "fence_wait_us=%llu drains=%lu display_waits=%lu display_wait_us=%llu "
                "flip_errors=%lu display_timeouts=%lu\n",
                gPresents, gFlipsHook, gFlipsPresent, gFlipsForced,
                static_cast<unsigned long long>(gForcedGeWaitUs), gFlipsFence, gFenceWaits,
                static_cast<unsigned long long>(gFenceWaitUs), gDrains, gDisplayWaits,
                static_cast<unsigned long long>(gDisplayWaitUs), gFlipErrors, gDisplayTimeouts);
    }
    const std::uint64_t end = NowUs();
    *waitedUs = end >= start ? end - start : 0U;
}
} // namespace th08::psp

#endif // TH08_PSP_SWAP_TRIPLE_ENABLED
