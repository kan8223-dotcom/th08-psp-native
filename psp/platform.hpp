#pragma once

#include <cstdint>

namespace th08::psp
{
struct MemorySnapshot
{
    std::uint32_t totalFreeBytes;
    std::uint32_t largestFreeBlockBytes;
    std::uintptr_t edramBase;
    std::uint32_t edramBytes;
    std::uint32_t devkitVersion;
    int cpuClockMhz;
    int busClockMhz;
};

void PlatformInitialize();
void PlatformLogSafety();
// Idempotent and allocation/I/O-free. Safe before a potentially blocked exit.
void PlatformArmExitWatchdog();
bool PlatformRunning();
bool PlatformSuspended();
bool PlatformSelectButtonDown();
void PlatformRequestExit();
MemorySnapshot CaptureMemorySnapshot();
} // namespace th08::psp
