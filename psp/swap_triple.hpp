#pragma once

#include <cstdint>

// TH08_PSP_SWAP_TRIPLE (child of TH08_PSP_SWAP_NOWAIT, exclusive with
// SWAP_ASYNC): a third colour buffer lets the GE finish frame N while the CPU
// simulates and draws frame N+1 into a buffer that is neither on screen nor
// waiting to go on screen.  Present no longer waits for the GE (the 5-7 ms
// tail measured on hardware) and the next frame's first colour write no
// longer waits for the flip (up to one VBlank at 60 draws).  Single thread:
// the flip for frame N (sceDisplaySetFrameBuf NEXTFRAME) is issued from the
// main thread as soon as a poll (every PSPGL list submission, the loop head,
// or the next Present) finds frame N's last display list done; the buffer
// that Present hands to the GE next is chosen only after the display reports
// the previous frame as the one being scanned out, so the GE never writes the
// buffer on screen (the r113 tearing lesson).  Frames, pixels, RNG and replay
// bytes are unchanged.  If the third buffer cannot be allocated in the lower
// eDRAM tier the device keeps the NOWAIT double-buffer path.  Default OFF; PC
// oracle never sees it.
#if defined(PSP) && defined(TH08_PSP_SWAP_TRIPLE) && TH08_PSP_SWAP_TRIPLE
#define TH08_PSP_SWAP_TRIPLE_ENABLED 1
#else
#define TH08_PSP_SWAP_TRIPLE_ENABLED 0
#endif

#if TH08_PSP_SWAP_TRIPLE_ENABLED
namespace th08::psp
{
// After the EGL surface exists and before the first Present: allocates the
// third buffer.  Returns false (and logs) when it could not; callers then keep
// the NOWAIT double-buffer swap.
bool SwapTripleInitialize();
bool SwapTripleActive();
// Main thread, in Present after glFlush: rotate the buffers without waiting
// for the GE.  Waits only until the previous frame is actually displayed
// (usually already true).  The waited time is returned for attribution.
void SwapTriplePresent(std::uint64_t *waitedUs);
// Main thread: issue the pending flip once its display lists are done.
void SwapTriplePoll();
// From the sceGeListEnQueue hook: remember the newest list id and poll.
void SwapTripleNoteListEnqueued(int qid);
// Main thread: block until the pending frame's display lists are done (and
// request its flip).  Callers that recycle GE-read memory on the old
// "Present synced the GE" contract (the Bullet direct-GE arena reset in
// BeginScene) use this as the fence instead.
void SwapTripleWaitPendingDone();
// Main thread: drain the whole GE (pending frame + every queued list) before
// memory the GE may still read is released or repurposed (stage arena
// transitions, device reset).
void SwapTripleDrain();
} // namespace th08::psp
#endif
