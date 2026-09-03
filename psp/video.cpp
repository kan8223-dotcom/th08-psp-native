#include "video.hpp"

#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>

#include <cstdint>

namespace th08::psp
{
namespace
{
constexpr int kScreenWidth = 480;
constexpr int kScreenHeight = 272;
constexpr int kBufferWidth = 512;
constexpr int kFrameBytes = kBufferWidth * kScreenHeight * 2;
constexpr int kSecondFrameOffset = kFrameBytes;
constexpr int kDepthOffset = kFrameBytes * 2;

// The fallback screen emits only a few dozen GU commands.  A 64 KiB list is
// ample and avoids reserving another full MiB in the gameplay image.
alignas(16) std::uint32_t gDisplayList[16 * 1024]{};
bool gVideoInitialized = false;

struct ColorVertex
{
    std::uint32_t color;
    std::uint16_t x;
    std::uint16_t y;
    std::uint16_t z;
};

void DrawRectangle(int left, int top, int right, int bottom, std::uint32_t color)
{
    auto *vertices = static_cast<ColorVertex *>(sceGuGetMemory(2 * sizeof(ColorVertex)));
    vertices[0].color = color;
    vertices[0].x = static_cast<std::uint16_t>(left);
    vertices[0].y = static_cast<std::uint16_t>(top);
    vertices[0].z = 0;
    vertices[1].color = color;
    vertices[1].x = static_cast<std::uint16_t>(right);
    vertices[1].y = static_cast<std::uint16_t>(bottom);
    vertices[1].z = 0;
    sceGuDrawArray(GU_SPRITES,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, nullptr, vertices);
}

std::uint32_t ToKiB(std::uint32_t bytes)
{
    return bytes / 1024U;
}
} // namespace

bool VideoInitialize()
{
    sceGuInit();
    sceGuStart(GU_DIRECT, gDisplayList);
    sceGuDrawBuffer(GU_PSM_5650, reinterpret_cast<void *>(0), kBufferWidth);
    sceGuDispBuffer(kScreenWidth, kScreenHeight,
                    reinterpret_cast<void *>(kSecondFrameOffset), kBufferWidth);
    sceGuDepthBuffer(reinterpret_cast<void *>(kDepthOffset), kBufferWidth);
    sceGuOffset(2048 - (kScreenWidth / 2), 2048 - (kScreenHeight / 2));
    sceGuViewport(2048, 2048, kScreenWidth, kScreenHeight);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, kScreenWidth, kScreenHeight);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuClearColor(0xFF180C08U);
    sceGuClearDepth(0);
    sceGuFinish();
    sceGuSync(0, 0);
    sceGuDisplay(GU_TRUE);
    gVideoInitialized = true;
    return true;
}

void RenderBootstrapStatus(const DataDiscovery &data,
                           const MemorySnapshot &startupMemory,
                           const MemorySnapshot &postProbeMemory)
{
    if (!gVideoInitialized)
    {
        return;
    }

    sceGuStart(GU_DIRECT, gDisplayList);
    sceGuClearColor(0xFF180C08U);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    DrawRectangle(0, 0, kScreenWidth, 28, 0xFF4A2410U);
    DrawRectangle(12, 48, 468, 52, 0xFF805020U);
    DrawRectangle(12, 202, 468, 206, data.ready ? 0xFF40C060U : 0xFF4050D0U);
    DrawRectangle(12, 246, 468, 260, 0xFF302018U);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();

    const std::uintptr_t uncachedEdram =
        reinterpret_cast<std::uintptr_t>(sceGeEdramGetAddr()) | 0x40000000U;
    pspDebugScreenInitEx(reinterpret_cast<void *>(uncachedEdram),
                         PSP_DISPLAY_PIXEL_FORMAT_565, 0);
    pspDebugScreenEnableBackColor(0);
    pspDebugScreenSetTextColor(0x00FFFFFFU);
    pspDebugScreenSetXY(2, 1);
    pspDebugScreenPrintf("TH08 PSP NATIVE - SC-ONLY BOOTSTRAP\n");
    pspDebugScreenPrintf("Build: %s\n", TH08_PSP_BUILD_ID);
    pspDebugScreenPrintf("\nRetail data: %s\n", data.ready ? "READY" : "NOT READY");
    pspDebugScreenPrintf("Root: %.58s\n", data.root[0] != '\0' ? data.root : "not found");
    pspDebugScreenPrintf("th08.dat : %-11s %llu bytes magic %.4s\n",
                         ProbeResultName(data.gameArchive.result),
                         static_cast<unsigned long long>(data.gameArchive.observedBytes),
                         data.gameArchive.observedMagic);
    pspDebugScreenPrintf("thbgm.dat: %-11s %llu bytes magic %.4s\n",
                         ProbeResultName(data.bgmArchive.result),
                         static_cast<unsigned long long>(data.bgmArchive.observedBytes),
                         data.bgmArchive.observedMagic);
    pspDebugScreenPrintf("\nRAM free: %lu KiB -> %lu KiB\n",
                         ToKiB(startupMemory.totalFreeBytes),
                         ToKiB(postProbeMemory.totalFreeBytes));
    pspDebugScreenPrintf("Largest block: %lu KiB\n",
                         ToKiB(postProbeMemory.largestFreeBlockBytes));
    pspDebugScreenPrintf("GE eDRAM: %lu KiB\n", ToKiB(postProbeMemory.edramBytes));
    pspDebugScreenPrintf("Clock: %d/%d MHz\n",
                         postProbeMemory.cpuClockMhz, postProbeMemory.busClockMhz);
    pspDebugScreenPrintf("\nThis build does not contain the playable engine yet.\n");
    pspDebugScreenPrintf("UNTESTED ON HARDWARE\n");
    pspDebugScreenPrintf("Press CIRCLE or START to exit. HOME is supported.\n");
}

void RenderEngineFailureStatus(const char *phase, const char *state, int result,
                               const MemorySnapshot &memory)
{
    if (!gVideoInitialized)
    {
        return;
    }

    sceGuStart(GU_DIRECT, gDisplayList);
    sceGuClearColor(0xFF100808U);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    DrawRectangle(0, 0, kScreenWidth, 30, 0xFF501010U);
    DrawRectangle(12, 62, 468, 66, 0xFF4050D0U);
    DrawRectangle(12, 226, 468, 230, 0xFF302018U);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();

    const std::uintptr_t uncachedEdram =
        reinterpret_cast<std::uintptr_t>(sceGeEdramGetAddr()) | 0x40000000U;
    pspDebugScreenInitEx(reinterpret_cast<void *>(uncachedEdram),
                         PSP_DISPLAY_PIXEL_FORMAT_565, 0);
    pspDebugScreenEnableBackColor(0);
    pspDebugScreenSetTextColor(0x00FFFFFFU);
    pspDebugScreenSetXY(2, 1);
    pspDebugScreenPrintf("TH08 PSP STARTUP FAILED\n");
    pspDebugScreenPrintf("Build: %s\n", TH08_PSP_BUILD_ID);
    pspDebugScreenPrintf("\nLast checkpoint:\n");
    pspDebugScreenPrintf("  phase: %.55s\n", phase != nullptr ? phase : "<null>");
    pspDebugScreenPrintf("  state: %.55s\n", state != nullptr ? state : "<null>");
    pspDebugScreenPrintf("  result: %d (0x%08x)\n", result,
                         static_cast<unsigned int>(result));
    pspDebugScreenPrintf("\nKernel free: %lu KiB\n",
                         ToKiB(memory.totalFreeBytes));
    pspDebugScreenPrintf("Largest kernel block: %lu KiB\n",
                         ToKiB(memory.largestFreeBlockBytes));
    pspDebugScreenPrintf("GE eDRAM visible: %lu KiB\n",
                         ToKiB(memory.edramBytes));
    pspDebugScreenPrintf("\nSee TH08PSP-BOOT.LOG for the flushed trace.\n");
    pspDebugScreenPrintf("Press CIRCLE or START to exit. HOME is supported.\n");
}

void VideoShutdown()
{
    if (gVideoInitialized)
    {
        sceGuTerm();
        gVideoInitialized = false;
    }
}
} // namespace th08::psp
