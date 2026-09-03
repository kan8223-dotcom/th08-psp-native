#pragma once

#include <cstdint>

// TH08_PSP_SWAP_ASYNC (child of TH08_PSP_SWAP_NOWAIT): Present performs PSPGL's
// front/back swap without its display step and hands the finished buffer to a
// tiny flip thread that waits for the GE (sceGeDrawSync) and issues
// sceDisplaySetFrameBuf(NEXTFRAME); the main thread continues with the next
// simulation ticks while the GE drains.  The first GE write of the next frame
// still waits for the flip and its VBlank, so the buffer on screen is never
// rendered into.  Frames, pixels, RNG and replay bytes are unchanged; only the
// CPU no longer idles for the GE tail.  Default OFF; PC oracle never sees it.
#if defined(PSP) && defined(TH08_PSP_SWAP_ASYNC) && TH08_PSP_SWAP_ASYNC
#define TH08_PSP_SWAP_ASYNC_ENABLED 1
#else
#define TH08_PSP_SWAP_ASYNC_ENABLED 0
#endif
// The display-identity query (below) is compiled for SWAP_NOWAIT too: the
// flip guard must not rely on the VBlank counter alone.  sceDisplayGetVcount
// and the NEXTFRAME switch are not the same instant on hardware (a request
// landing inside the VBlank interval is applied one VBlank later than the
// counter suggests), which let the GE draw into the buffer still on screen
// (r113: lower half black, flickering).  The guard now waits until the
// display reports the requested buffer as the one being scanned out.
#if defined(PSP) && ((defined(TH08_PSP_SWAP_NOWAIT) && TH08_PSP_SWAP_NOWAIT) || \
                     (defined(TH08_PSP_SWAP_ASYNC) && TH08_PSP_SWAP_ASYNC))
#define TH08_PSP_SWAP_QUERY_ENABLED 1
#else
#define TH08_PSP_SWAP_QUERY_ENABLED 0
#endif

#if TH08_PSP_SWAP_QUERY_ENABLED
namespace th08::psp
{
// Address of PSPGL's current front buffer (the one Present asked the display
// to show) and whether the display currently scans that buffer.
const void *SwapFrontBufferBase();
bool SwapDisplayShows(const void *base);
} // namespace th08::psp
#endif

#if TH08_PSP_SWAP_ASYNC_ENABLED
namespace th08::psp
{
// Creates the event flag and the flip thread.  Returns false (and logs) when
// the kernel objects could not be created; callers then keep the synchronous
// swap.
bool SwapAsyncInitialize();
bool SwapAsyncActive();
// Main thread, in Present: swap PSPGL's colour buffers (no GE sync, no
// display change) and request the flip of the buffer just rendered.
void SwapAsyncPresent();
// Main thread, before the next frame's first GE write: block until the flip
// thread issued the display change and that VBlank has passed.  The wait is
// returned in microseconds for attribution.
void SwapAsyncWaitFlipComplete(std::uint64_t *waitedUs);
} // namespace th08::psp
#endif
