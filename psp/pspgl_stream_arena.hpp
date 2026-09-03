#pragma once

// TH08_PSP_PSPGL_STREAM_ARENA: link the stream-arena PSPGL archive
// (deps/pspgl-ge4/libGL_th08_ge4_streamarena_v1.a) and use a render-arena
// parity-0 frame lease for every GL_STREAM_DRAW_ARB copy PSPGL makes for
// client-array and native-copy draws.  Upstream PSPGL memalign()s one block per draw and
// frees it when the display list completes.  The lease removes that per-draw
// heap traffic, then returns immediately after synchronous Present has fenced
// every referencing list.  Parity 1 is never selected, so only one 256 KiB
// half is leased.  It therefore consumes no render-arena memory while
// stage images are decoded.  Async/triple retirement is intentionally outside
// this switch's contract.  Default OFF; the PC oracle never sees it.
#if defined(PSP) && defined(TH08_PSP_PSPGL_STREAM_ARENA) && TH08_PSP_PSPGL_STREAM_ARENA
#define TH08_PSP_PSPGL_STREAM_ARENA_ENABLED 1
#else
#define TH08_PSP_PSPGL_STREAM_ARENA_ENABLED 0
#endif
