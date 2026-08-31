#include "platform.hpp"

#include <pspctrl.h>
#include <pspge.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspsdk.h>
#include <pspsysmem.h>

namespace th08::psp
{
namespace
{
volatile int gRunning = 1;
volatile int gSuspended = 0;

int ExitCallback(int, int, void *)
{
    gRunning = 0;
    return 0;
}

int PowerCallback(int, int powerInfo, void *)
{
    if ((powerInfo & PSP_POWER_CB_SUSPENDING) != 0)
    {
        gSuspended = 1;
    }
    if ((powerInfo & PSP_POWER_CB_RESUME_COMPLETE) != 0)
    {
        gSuspended = 0;
    }
    return 0;
}

int CallbackThread(SceSize, void *)
{
    const int exitCallback = sceKernelCreateCallback("th08_psp_exit", ExitCallback, nullptr);
    if (exitCallback >= 0)
    {
        sceKernelRegisterExitCallback(exitCallback);
    }

    const int powerCallback = sceKernelCreateCallback("th08_psp_power", PowerCallback, nullptr);
    if (powerCallback >= 0)
    {
        scePowerRegisterCallback(-1, powerCallback);
    }

    while (gRunning != 0)
    {
        sceKernelSleepThreadCB();
    }
    return 0;
}

int PowerKeepAliveThread(SceSize, void *)
{
    while (gRunning != 0)
    {
        scePowerTick(PSP_POWER_TICK_ALL);
        sceKernelDelayThread(1000U * 1000U);
    }
    return 0;
}
} // namespace

void PlatformInitialize()
{
    pspSdkDisableFPUExceptions();
    scePowerSetClockFrequency(333, 333, 166);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    const int callbackThread = sceKernelCreateThread(
        "th08_psp_callbacks", CallbackThread, 0x11, 0x1000, PSP_THREAD_ATTR_USER, nullptr);
    if (callbackThread >= 0)
    {
        sceKernelStartThread(callbackThread, 0, nullptr);
    }

    const int keepAliveThread = sceKernelCreateThread(
        "th08_psp_power_keepalive", PowerKeepAliveThread, 0x20, 0x1000,
        PSP_THREAD_ATTR_USER, nullptr);
    if (keepAliveThread >= 0)
    {
        sceKernelStartThread(keepAliveThread, 0, nullptr);
    }
}

bool PlatformRunning()
{
    return gRunning != 0;
}

bool PlatformSuspended()
{
    return gSuspended != 0;
}

void PlatformRequestExit()
{
    gRunning = 0;
}

MemorySnapshot CaptureMemorySnapshot()
{
    MemorySnapshot snapshot{};
    snapshot.totalFreeBytes = static_cast<std::uint32_t>(sceKernelTotalFreeMemSize());
    snapshot.largestFreeBlockBytes = static_cast<std::uint32_t>(sceKernelMaxFreeMemSize());
    snapshot.edramBase = reinterpret_cast<std::uintptr_t>(sceGeEdramGetAddr());
    snapshot.edramBytes = sceGeEdramGetSize();
    snapshot.devkitVersion = static_cast<std::uint32_t>(sceKernelDevkitVersion());
    snapshot.cpuClockMhz = scePowerGetCpuClockFrequencyInt();
    snapshot.busClockMhz = scePowerGetBusClockFrequencyInt();
    return snapshot;
}
} // namespace th08::psp
