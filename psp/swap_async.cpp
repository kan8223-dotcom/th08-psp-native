#include "swap_async.hpp"

#if TH08_PSP_SWAP_QUERY_ENABLED

#include "fileio.hpp"

#include <cstddef>
#include <cstdint>

#include <GLES/egl.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspkernel.h>
#include <pspthreadman.h>

// Vendored from the PSPGL tree the frozen libGL_th08_ge4.a was built from
// (deps/pspgl-ge4/include, see README).  The layout is cross-checked below
// against the archive's own eglSwapBuffers / vidmem code, which reads
// pixelperline at +12, flags at +14, color_front at +16 and color_back at +20
// of the surface and swap_interval at +2208 of the context.
extern "C" {
#include "pspgl_internal.h"
#include "pspgl_buffers.h"
}

static_assert(offsetof(struct pspgl_surface, pixfmt) == 4U, "pspgl_surface layout");
static_assert(offsetof(struct pspgl_surface, pixelperline) == 12U, "pspgl_surface layout");
static_assert(offsetof(struct pspgl_surface, flags) == 14U, "pspgl_surface layout");
static_assert(offsetof(struct pspgl_surface, color_front) == 16U, "pspgl_surface layout");
static_assert(offsetof(struct pspgl_surface, color_back) == 20U, "pspgl_surface layout");
static_assert(offsetof(struct pspgl_context, swap_interval) == 2208U, "pspgl_context layout");

namespace th08::psp
{
namespace
{
inline const void *NormalizeVramAddress(const void *address)
{
    return reinterpret_cast<const void *>(
        reinterpret_cast<std::uintptr_t>(address) & 0x1fffffffU);
}
} // namespace

const void *SwapFrontBufferBase()
{
    return __pspgl_curctx->draw->color_front->base;
}

bool SwapDisplayShows(const void *base)
{
    void *top = NULL;
    int width = 0;
    int format = 0;
    if (sceDisplayGetFrameBuf(&top, &width, &format, PSP_DISPLAY_SETBUF_IMMEDIATE) < 0)
        return true; // no query available: never spin forever
    return NormalizeVramAddress(top) == NormalizeVramAddress(base);
}

#if TH08_PSP_SWAP_ASYNC_ENABLED
namespace
{
constexpr unsigned int kFlipRequested = 1U;
constexpr unsigned int kFlipIssued = 2U;

struct FlipRequest
{
    void *base;
    unsigned int pixelPerLine;
    unsigned int pixelFormat;
};

SceUID gEventFlag = -1;
SceUID gThread = -1;
bool gActive = false;
volatile bool gPending = false;
volatile FlipRequest gRequest = {NULL, 0U, 0U};
volatile unsigned int gFlipVcount = 0U;
volatile std::uint32_t gFlipsIssued = 0U;

int FlipThread(SceSize, void *)
{
    for (;;)
    {
        if (sceKernelWaitEventFlag(gEventFlag, kFlipRequested,
                                   PSP_EVENT_WAITAND | PSP_EVENT_WAITCLEAR,
                                   NULL, NULL) < 0)
            break;
        // Every list queued so far belongs to the frame being flipped: the
        // main thread cannot queue the next frame before this flip completed.
        sceGeDrawSync(0);
        sceDisplaySetFrameBuf(gRequest.base,
                              static_cast<int>(gRequest.pixelPerLine),
                              static_cast<int>(gRequest.pixelFormat),
                              PSP_DISPLAY_SETBUF_NEXTFRAME);
        gFlipVcount = sceDisplayGetVcount();
        ++gFlipsIssued;
        sceKernelSetEventFlag(gEventFlag, kFlipIssued);
    }
    return 0;
}
} // namespace

bool SwapAsyncInitialize()
{
    if (gActive)
        return true;
    gEventFlag = sceKernelCreateEventFlag("th08flip", 0, 0, NULL);
    if (gEventFlag < 0)
    {
        BootLog("SWAP_ASYNC init=FAIL event=0x%08x\n",
                static_cast<unsigned int>(gEventFlag));
        return false;
    }
    const int mainPriority = sceKernelGetThreadCurrentPriority();
    const int priority = mainPriority > 17 ? mainPriority - 1 : mainPriority;
    gThread = sceKernelCreateThread("th08flip", FlipThread, priority, 0x1000,
                                    0, NULL);
    if (gThread < 0 || sceKernelStartThread(gThread, 0, NULL) < 0)
    {
        BootLog("SWAP_ASYNC init=FAIL thread=0x%08x\n",
                static_cast<unsigned int>(gThread));
        return false;
    }
    gActive = true;
    BootLog("SWAP_ASYNC init=OK main_priority=%d flip_priority=%d "
            "surface_layout=verified\n",
            mainPriority, priority);
    return true;
}

bool SwapAsyncActive()
{
    return gActive;
}

void SwapAsyncPresent()
{
    struct pspgl_surface *const surface = __pspgl_curctx->draw;
    // PSPGL swaps color_front/color_back and points the GE at the new draw
    // buffer; with SURF_DISPLAYED cleared it neither syncs the GE nor touches
    // the display, which is exactly the flip thread's job.
    const unsigned char savedFlags = surface->flags;
    surface->flags = static_cast<unsigned char>(savedFlags & ~SURF_DISPLAYED);
    eglSwapBuffers(reinterpret_cast<EGLDisplay>(0),
                   reinterpret_cast<EGLSurface>(surface));
    surface->flags = savedFlags;
    gRequest.base = surface->color_front->base;
    gRequest.pixelPerLine = surface->pixelperline;
    gRequest.pixelFormat = surface->pixfmt;
    gPending = true;
    sceKernelSetEventFlag(gEventFlag, kFlipRequested);
}

void SwapAsyncWaitFlipComplete(std::uint64_t *waitedUs)
{
    *waitedUs = 0U;
    if (!gPending)
        return;
    const std::uint64_t start =
        static_cast<std::uint64_t>(sceKernelGetSystemTimeWide());
    sceKernelWaitEventFlag(gEventFlag, kFlipIssued,
                           PSP_EVENT_WAITAND | PSP_EVENT_WAITCLEAR, NULL, NULL);
    // Wait for the display to actually scan the requested buffer; the
    // VBlank counter alone is not the switch instant on hardware.
    while (!SwapDisplayShows(gRequest.base))
        sceDisplayWaitVblankStart();
    (void)gFlipVcount;
    gPending = false;
    const std::uint64_t end =
        static_cast<std::uint64_t>(sceKernelGetSystemTimeWide());
    *waitedUs = end >= start ? end - start : 0U;
}
#endif // TH08_PSP_SWAP_ASYNC_ENABLED
} // namespace th08::psp

#endif // TH08_PSP_SWAP_QUERY_ENABLED
