#include "kcall.h"
#include <pspsdk.h>
#include <pspsysevent.h>
#include <psputilsforkernel.h>
#include <pspintrman.h>


PSP_MODULE_INFO("th08audio_kcall", 0x1006, 1, 2);
PSP_NO_CREATE_MAIN_THREAD();

int kcall(FCall const f, const unsigned int seg) {
  const unsigned int addr = (seg | (unsigned int)f);
  sceKernelIcacheInvalidateAll();
  /*
  switch (seg) {
    case 1: return ((FCall)(0x80000000 | (unsigned int)f))();
    case 2: return ((FCall)(0x40000000 | (unsigned int)f))();
    case 3: return ((FCall)(0xa0000000 | (unsigned int)f))();
  }
  */
  return ((FCall)addr)();
}

int kcall(FPCall const f, const unsigned int seg, void* const param) {
  const unsigned int addr = (seg | (unsigned int)f);
  sceKernelIcacheInvalidateAll();
  return ((FPCall)addr)(param);
}

// Retain the original system handler so a failed takeover or orderly STOP
// cannot leave a firmware callback pointing into an unloaded game EBOOT.
static PspSysEventHandler* ownedEvent;
static PspSysEventHandlerFunc previousHandler;
static PspSysEventHandlerFunc installedHandler;

int kinit(const void* const handler) {
  if (!handler) {
    const int intr = sceKernelCpuSuspendIntr();
    int result = 0;
    if (ownedEvent) {
      if (ownedEvent->handler != installedHandler) result = -2;
      else {
        ownedEvent->handler = previousHandler;
        ownedEvent = NULL;
      }
    }
    sceKernelCpuResumeIntr(intr);
    return result;
  }
  if (ownedEvent) return -2;
  PspSysEventHandler* seh = sceKernelReferSysEventHandler();
  while (seh != NULL) {
    if (seh->name[3] == 'M' && seh->name[4] == 'e' && seh->name[5] == 'R') {
      const int intr = sceKernelCpuSuspendIntr();
      ownedEvent = seh;
      previousHandler = seh->handler;
      installedHandler = (PspSysEventHandlerFunc)handler;
      seh->handler = (PspSysEventHandlerFunc)handler;
      sceKernelCpuResumeIntr(intr);
      
      // sceKernelUnregisterSysEventHandler(seh);
      // meLibRpc.handler = (PspSysEventHandlerFunc)handler;
      // sceKernelRegisterSysEventHandler(&meLibRpc);
      
      return 0;
    }
    seh = seh->next;
  }
  return -1;
}

int module_start(SceSize args, void *argp) {
  return 0;
}

int module_stop() {
  return 0;
}
