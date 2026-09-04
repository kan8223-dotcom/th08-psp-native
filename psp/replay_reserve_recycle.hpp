#pragma once

// TH08_PSP_REPLAY_RESERVE_RECYCLE: at stage teardown the recorded input/FPS
// spans are copied into right-sized blocks and the two full-capacity blocks
// (0xd2f00 + 0x4000 bytes) are kept as the next stage's reservation instead
// of being shrunk in place.  The in-place shrink left a hole smaller than one
// capacity, so the next stage's reservation failed on the fragmented heap
// ("error: PSP replay recording reservation failed", stage 2 never loaded,
// R-046).  Saved replay bytes are unchanged: SaveReplay reads each stage
// through its end pointer, which is moved with the copy.  Default OFF; PC
// oracle never sees it.
#if defined(PSP) && defined(TH08_PSP_REPLAY_RESERVE_RECYCLE) && TH08_PSP_REPLAY_RESERVE_RECYCLE
#define TH08_PSP_REPLAY_RESERVE_RECYCLE_ENABLED 1
#else
#define TH08_PSP_REPLAY_RESERVE_RECYCLE_ENABLED 0
#endif
