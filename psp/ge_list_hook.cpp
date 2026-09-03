#include "ge_list_hook.hpp"

#if TH08_PSP_GE_LIST_HOOK_ENABLED

#include <pspge.h>

extern "C" int __real_sceGeListEnQueue(const void *list, void *stall, int cbid,
                                       PspGeListArgs *arg);

extern "C" int __wrap_sceGeListEnQueue(const void *list, void *stall, int cbid,
                                       PspGeListArgs *arg)
{
#if TH08_PSP_PERF_ENV_ENABLED
    const int qid = th08::psp::PerfEnvGeListEnqueue(list, stall, cbid, arg);
#else
    const int qid = __real_sceGeListEnQueue(list, stall, cbid, arg);
#endif
#if TH08_PSP_SWAP_TRIPLE_ENABLED
    th08::psp::SwapTripleNoteListEnqueued(qid);
#endif
    return qid;
}

#endif // TH08_PSP_GE_LIST_HOOK_ENABLED
