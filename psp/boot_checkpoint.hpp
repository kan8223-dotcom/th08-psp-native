#pragma once

#if defined(PSP)
// Allocation-independent startup breadcrumbs.  Every call is synchronously
// flushed to the per-launch boot log so the last completed operation survives
// a hard failure in SDL, PSPGL, or the GE4 aperture transition.
extern "C" void th08_psp_boot_checkpoint(const char *phase,
                                          const char *state,
                                          int result);
#define TH08_PSP_BOOT_CHECKPOINT(phase, state, result) \
    th08_psp_boot_checkpoint((phase), (state), (result))
#else
#define TH08_PSP_BOOT_CHECKPOINT(phase, state, result) ((void)0)
#endif
