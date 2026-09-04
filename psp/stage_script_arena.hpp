#pragma once

// TH08_PSP_STAGE_SCRIPT_ARENA: stage scripts loaded from the archive at every
// stage start (.ecl enemy scripts, .std stage scripts, .msg dialogue) are
// allocated from the renderer resource arena instead of the newlib heap.  The
// heap sits at its ceiling with ~200 KiB of fragmented slack during play, and
// the 51 KiB stage-2 ECL failed to fit (R-056); the arena keeps >1 MiB of
// contiguous space.  Falls back to the heap when the arena cannot serve.
// Frees route through th08_psp_tracked_free, which already recognises arena
// pointers.  Default OFF; PC oracle never sees it.
#if defined(PSP) && defined(TH08_PSP_STAGE_SCRIPT_ARENA) && TH08_PSP_STAGE_SCRIPT_ARENA
#define TH08_PSP_STAGE_SCRIPT_ARENA_ENABLED 1
#else
#define TH08_PSP_STAGE_SCRIPT_ARENA_ENABLED 0
#endif
