#pragma once

// Single owner of -Wl,--wrap=sceGeListEnQueue: every PSPGL display-list
// submission passes through here and is handed to the observers/consumers
// that are compiled in (PERF_ENV's GE queue observer, SWAP_TRIPLE's flip
// poll).  Compiled only when at least one of them is enabled.
#include "perf_env.hpp"
#include "swap_triple.hpp"

#if TH08_PSP_PERF_ENV_ENABLED || TH08_PSP_SWAP_TRIPLE_ENABLED
#define TH08_PSP_GE_LIST_HOOK_ENABLED 1
#else
#define TH08_PSP_GE_LIST_HOOK_ENABLED 0
#endif
