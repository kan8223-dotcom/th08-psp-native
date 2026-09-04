#pragma once

// TH08_PSP_DIALOGUE_SNAPSHOT_DIAG: hardware diagnostic for the black dialogue
// background (R-045).  Logs the pixel statistics of every native framebuffer
// readback (non-zero count, samples, displayed address) and, while a
// conversation is shown, draws the restored snapshot as a flat magenta quad
// for 8 of every 60 restore frames so the viewer can tell "quad hidden" from
// "texture black".  Default OFF; PC oracle never sees it.
#if defined(PSP) && defined(TH08_PSP_DIALOGUE_SNAPSHOT_DIAG) && TH08_PSP_DIALOGUE_SNAPSHOT_DIAG
#define TH08_PSP_DIALOGUE_SNAPSHOT_DIAG_ENABLED 1
#else
#define TH08_PSP_DIALOGUE_SNAPSHOT_DIAG_ENABLED 0
#endif
