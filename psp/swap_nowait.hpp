#pragma once

// TH08_PSP_SWAP_NOWAIT: Present asks PSPGL for sceDisplaySetFrameBuf(NEXTFRAME)
// without the trailing VBlank wait (swap interval 0).  The 60 Hz pacing is
// owned entirely by WaitForPspRenderCadence, and the D3D8 compat device waits
// for the pending flip before the next frame's first GE write, so the GE never
// renders into the buffer still on screen.  A late frame no longer loses up to
// one VBlank period per present.  Default OFF; PC oracle never sees it.
#if defined(PSP) && defined(TH08_PSP_SWAP_NOWAIT) && TH08_PSP_SWAP_NOWAIT
#define TH08_PSP_SWAP_NOWAIT_ENABLED 1
#else
#define TH08_PSP_SWAP_NOWAIT_ENABLED 0
#endif

// TH08_PSP_FLIP_GUARD_COLOR_ONLY (child of SWAP_NOWAIT): the flip guard runs
// only before GE writes that reach the colour buffer (BeginScene, Draw,
// DrawIndexed, Clear with TARGET or STENCIL).  A depth-only Clear never
// touches the buffer on screen: PSPGL's glClear enters GE clear mode with
// GU_DEPTH_BUFFER_BIT alone.  GameManager's per-tick Z clear inside the calc
// chain therefore no longer waits for the display to show the previous frame
// (r118 hardware: 7.45 ms per present booked inside Calc); the wait moves to
// the draw chain's first colour write, which the simulation ticks in between
// have usually already covered.  Pixels, RNG and replay bytes are unchanged.
// Default OFF; PC oracle never sees it.
#if TH08_PSP_SWAP_NOWAIT_ENABLED && defined(TH08_PSP_FLIP_GUARD_COLOR_ONLY) && \
    TH08_PSP_FLIP_GUARD_COLOR_ONLY
#define TH08_PSP_FLIP_GUARD_COLOR_ONLY_ENABLED 1
#else
#define TH08_PSP_FLIP_GUARD_COLOR_ONLY_ENABLED 0
#endif
