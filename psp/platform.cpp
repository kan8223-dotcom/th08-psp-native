#include "platform.hpp"
#include "fileio.hpp"
#include "me_core.hpp"
#include "wait_probe.hpp"

#include <pspctrl.h>
#include <pspge.h>
#include <pspkernel.h>
#include <psploadexec.h>
#include <psppower.h>
#include <pspsdk.h>
#include <pspsysmem.h>
#include <atomic>

namespace th08::psp
{
namespace
{
volatile int gRunning = 1;
volatile int gSuspended = 0;
bool gSelectButtonDown = false;
// BootLog alone uses 4424 bytes in r232, before printf/libc/callback frames.
// Keep its callers well above the old 4096-byte callback stack.
constexpr unsigned int kCallbackStackBytes = 16U * 1024U;
constexpr unsigned int kExitDeadlineUs = 4U * 1000U * 1000U;
static_assert(std::atomic<unsigned int>::is_always_lock_free, "watchdog must not lock");
std::atomic<unsigned int> gExitWatchdogState{0U}; // 0 idle, 1 publishing, 2 armed
unsigned int gExitArmedAt = 0U;
std::atomic<unsigned int> gExitHeartbeat{0U}, gExitAttemptAt{0U}, gExitReturnAt{0U};
std::atomic<unsigned int> gExitAttemptState{0U}; // 0 not attempted, 1 entering ExitGame, 2 returned
std::atomic<unsigned int> gExitAttempts{0U};
int gExitWatchdogThread = -1, gExitWatchdogStart = -1, gMainThread = -1;

int ExitWatchdogThread(SceSize, void *)
{
    for (;;)
    {
        const unsigned int state = gExitWatchdogState.load(std::memory_order_acquire);
        const unsigned int now = sceKernelGetSystemTimeLow();
        gExitHeartbeat.store(now, std::memory_order_relaxed);
        if (state == 2U &&
            static_cast<unsigned int>(now - gExitArmedAt) >= kExitDeadlineUs &&
            (gExitAttemptState.load(std::memory_order_relaxed) == 0U ||
             static_cast<unsigned int>(now - gExitAttemptAt.load(std::memory_order_relaxed)) >= 1000000U))
        {
            // ColdReset only queues a power request, blocked by the HOME reboot
            // lock (and our ME lock). Start OS exit directly from this thread.
            // Never log, take a mutex, free live GE resources or join main/ME.
            gExitAttemptAt.store(now, std::memory_order_relaxed);
            gExitAttempts.fetch_add(1U, std::memory_order_relaxed);
            gExitAttemptState.store(1U, std::memory_order_release);
            th08_me_core_request_stop();
            sceKernelExitGame();
            // Normally non-returning. Preserve evidence and stay alive if the
            // firmware unexpectedly returns; do not report success or spin.
            gExitReturnAt.store(sceKernelGetSystemTimeLow(), std::memory_order_relaxed);
            gExitAttemptState.store(2U, std::memory_order_release);
        }
        sceKernelDelayThread(50000U);
    }
}

void LogCallbackContext(const char *event)
{
#if TH08_PSP_WAIT_PROBE_ENABLED
    const WaitProbeSnapshot wait = WaitProbeRead();
    const unsigned int capturedAt = sceKernelGetSystemTimeLow();
#endif
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    const int result = sceKernelReferThreadStatus(gMainThread, &info);
    BootLog("CALLBACK_CONTEXT event=%s stack_free=%d main_rc=%d main_status=%d wait_type=%d wait_id=0x%08x watchdog=%u\n",
            event, sceKernelGetThreadStackFreeSize(0), result, info.status, info.waitType,
            static_cast<unsigned int>(info.waitId), gExitWatchdogState.load(std::memory_order_acquire));
    const unsigned int attemptState = gExitAttemptState.load(std::memory_order_acquire);
    BootLog("EXIT_RESCUE event=%s action=exit_game state=%u attempts=%u heartbeat_us=%u attempt_us=%u returned_us=%u\n",
            event, attemptState, gExitAttempts.load(std::memory_order_relaxed),
            gExitHeartbeat.load(std::memory_order_relaxed), gExitAttemptAt.load(std::memory_order_relaxed),
            gExitReturnAt.load(std::memory_order_relaxed));
#if TH08_PSP_WAIT_PROBE_ENABLED
    if (!wait.consistent)
        BootLog("MAIN_WAIT event=%s consistent=0 seq=%lu\n", event, static_cast<unsigned long>(wait.sequence));
    else
        BootLog("MAIN_WAIT event=%s consistent=1 seq=%lu active=%lu api=%lu mode=0 main=0x%08x qid=%d caller_ra=0x%08lx observed_st=%lu observed_sf=%lu phase=%lu started_us=%lu age_us=%u result=%d\n",
                event, static_cast<unsigned long>(wait.sequence), static_cast<unsigned long>(wait.active),
                static_cast<unsigned long>(wait.api), static_cast<unsigned int>(wait.mainThread),
                wait.qid, static_cast<unsigned long>(wait.caller), static_cast<unsigned long>(wait.observedContext >> 24),
                static_cast<unsigned long>(wait.observedContext & 0x00ffffffU), static_cast<unsigned long>(wait.phase),
                static_cast<unsigned long>(wait.startedUs), static_cast<unsigned int>(capturedAt - wait.startedUs), wait.result);
#endif
}

int ExitCallback(int arg1, int arg2, void *)
{
    PlatformArmExitWatchdog();
    gRunning = 0;
    LogCallbackContext("exit");
    th08::psp::BootLog("EXIT_CALLBACK arg1=%d arg2=%d\n", arg1, arg2);
    th08::psp::FlushBootLogHard();
    return 0;
}

int PowerCallback(int, int powerInfo, void *)
{
    LogCallbackContext("power");
    th08::psp::BootLog("POWER_CALLBACK info=0x%08x\n", static_cast<unsigned>(powerInfo));
    th08::psp::FlushBootLogHard();
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
    gRunning = 1;
    gSuspended = 0;
    gSelectButtonDown = false;
    gMainThread = sceKernelGetThreadId();
#if TH08_PSP_WAIT_PROBE_ENABLED
    WaitProbeInitialize(gMainThread);
#endif
    gExitWatchdogState.store(0U, std::memory_order_relaxed);
    gExitAttemptState.store(0U, std::memory_order_relaxed);
    gExitAttempts.store(0U, std::memory_order_relaxed);
    gExitHeartbeat.store(0U, std::memory_order_relaxed);
    gExitAttemptAt.store(0U, std::memory_order_relaxed);
    gExitReturnAt.store(0U, std::memory_order_relaxed);
    gExitWatchdogThread = sceKernelCreateThread("th08_exit_watchdog", ExitWatchdogThread,
        0x10, 0x1000, PSP_THREAD_ATTR_USER, nullptr);
    if (gExitWatchdogThread >= 0)
        gExitWatchdogStart = sceKernelStartThread(gExitWatchdogThread, 0, nullptr);
    pspSdkDisableFPUExceptions();
    scePowerSetClockFrequency(333, 333, 166);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    const int callbackThread = sceKernelCreateThread(
        "th08_psp_callbacks", CallbackThread, 0x11, kCallbackStackBytes, PSP_THREAD_ATTR_USER, nullptr);
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

void PlatformLogSafety()
{
    BootLog("PLATFORM_SAFETY callback_stack=%u watchdog_thread=0x%08x watchdog_start=%d deadline_us=%u arm=before_log_and_teardown rescue=exit_game\n",
            kCallbackStackBytes, static_cast<unsigned int>(gExitWatchdogThread), gExitWatchdogStart, kExitDeadlineUs);
    BootLog("WAIT_PROBE enabled=%u ram_only=1 api_ids=1:list_sync,2:draw_sync,3:vblank_start\n", TH08_PSP_WAIT_PROBE_ENABLED);
}

void PlatformArmExitWatchdog()
{
    unsigned int expected = 0U;
    if (gExitWatchdogState.compare_exchange_strong(expected, 1U, std::memory_order_relaxed))
    {
        gExitArmedAt = sceKernelGetSystemTimeLow();
        gExitWatchdogState.store(2U, std::memory_order_release);
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

bool PlatformSelectButtonDown()
{
    SceCtrlData pad{};
    if (sceCtrlPeekBufferPositive(&pad, 1) > 0)
        gSelectButtonDown = (pad.Buttons & PSP_CTRL_SELECT) != 0;
    return gSelectButtonDown;
}

void PlatformRequestExit()
{
    PlatformArmExitWatchdog();
    gRunning = 0;
    th08::psp::BootLog("PLATFORM_REQUEST_EXIT\n");
    th08::psp::FlushBootLogHard();
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
