#include "fileio.hpp"
#include "platform.hpp"
#include "video.hpp"

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <pspmoduleinfo.h>

#include <cstdint>

#if !defined(TH08_PSP_PORT) || !defined(TH08_PSP_SC_ONLY)
#error The PSP bootstrap requires the PSP port and SC-only build gates.
#endif

#if defined(TH08_PSP_COPROCESSOR_PATH)
#error Coprocessor features are forbidden in the initial PSP bootstrap.
#endif

PSP_MODULE_INFO("TH08 PSP Native", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(512);
PSP_HEAP_SIZE_KB(-2048);

static_assert(sizeof(void *) == 4, "PSP runtime pointers must be 32-bit");

namespace
{
void LogProbe(const char *name, const th08::psp::FileProbe &probe)
{
    th08::psp::BootLog("DATA %s result=%s bytes=%llu magic=%.4s\n",
                       name,
                       th08::psp::ProbeResultName(probe.result),
                       static_cast<unsigned long long>(probe.observedBytes),
                       probe.observedMagic);
}

void LogMemory(const char *phase, const th08::psp::MemorySnapshot &memory)
{
    th08::psp::BootLog(
        "MEM phase=%s total_free=%lu largest_free=%lu edram_base=0x%08lx edram_bytes=%lu\n",
        phase,
        memory.totalFreeBytes,
        memory.largestFreeBlockBytes,
        static_cast<unsigned long>(memory.edramBase),
        memory.edramBytes);
}
} // namespace

int main(int argc, char **argv)
{
    th08::psp::PlatformInitialize();
    th08::psp::FileIoInitialize(argc > 0 ? argv[0] : nullptr);

    th08::psp::BootLog("TH08 PSP SC-ONLY BOOTSTRAP\n");
    th08::psp::BootLog("STATUS UNTESTED ON HARDWARE\n");
    th08::psp::BootLog("BUILD id=%s\n", TH08_PSP_BUILD_ID);
    th08::psp::BootLog("SOURCE th08_portable=%s\n", TH08_UPSTREAM_COMMIT);
    th08::psp::BootLog("SOURCE th08_oracle=%s\n", TH08_ORACLE_COMMIT);
    th08::psp::BootLog("SOURCE th07_backend=%s\n", TH07_BACKEND_COMMIT);
    th08::psp::BootLog("FEATURE SC_ONLY=1 audio=NOT_LINKED engine=NOT_LINKED\n");
    th08::psp::BootLog("PATH game=%s\n", th08::psp::GameDirectory());
    th08::psp::BootLog("PATH log=%s\n", th08::psp::BootLogPath());

    const th08::psp::MemorySnapshot startupMemory = th08::psp::CaptureMemorySnapshot();
    LogMemory("startup", startupMemory);
    th08::psp::BootLog("SYSTEM devkit=0x%08lx clock_cpu=%d clock_bus=%d\n",
                       startupMemory.devkitVersion,
                       startupMemory.cpuClockMhz,
                       startupMemory.busClockMhz);

    const th08::psp::DataDiscovery &data = th08::psp::DiscoverOriginalData();
    th08::psp::BootLog("DATA root=%s ready=%d\n",
                       data.root[0] != '\0' ? data.root : "not-found",
                       data.ready ? 1 : 0);
    LogProbe("th08.dat", data.gameArchive);
    LogProbe("thbgm.dat", data.bgmArchive);

    const th08::psp::MemorySnapshot postProbeMemory = th08::psp::CaptureMemorySnapshot();
    LogMemory("post_data_probe", postProbeMemory);

    const bool videoReady = th08::psp::VideoInitialize();
    th08::psp::BootLog("VIDEO gu_init=%d framebuffer=RGB565 double_buffer=1 depth=16\n",
                       videoReady ? 1 : 0);
    if (videoReady)
    {
        th08::psp::RenderBootstrapStatus(data, startupMemory, postProbeMemory);
    }
    th08::psp::BootLog("PHASE bootstrap_ready\n");
    th08::psp::FlushBootLog();

    std::uint32_t previousButtons = 0;
    while (th08::psp::PlatformRunning())
    {
        if (!th08::psp::PlatformSuspended())
        {
            SceCtrlData pad{};
            if (sceCtrlPeekBufferPositive(&pad, 1) > 0)
            {
                const std::uint32_t buttons = pad.Buttons;
                const std::uint32_t pressed = buttons & ~previousButtons;
                previousButtons = buttons;
                if ((pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_START)) != 0)
                {
                    th08::psp::PlatformRequestExit();
                }
            }
        }
        sceDisplayWaitVblankStart();
    }

    th08::psp::BootLog("PHASE orderly_exit\n");
    LogMemory("exit", th08::psp::CaptureMemorySnapshot());
    th08::psp::FlushBootLog();
    th08::psp::VideoShutdown();
    sceKernelExitGame();
    return 0;
}
