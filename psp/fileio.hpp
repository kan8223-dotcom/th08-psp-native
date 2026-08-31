#pragma once

#include <cstddef>
#include <cstdint>

namespace th08::psp
{
enum class ProbeResult : std::uint8_t
{
    Ready,
    Missing,
    WrongSize,
    WrongMagic,
    IoError,
};

struct FileProbe
{
    ProbeResult result;
    std::uint64_t observedBytes;
    char observedMagic[5];
};

struct DataDiscovery
{
    bool ready;
    char root[512];
    FileProbe gameArchive;
    FileProbe bgmArchive;
};

void FileIoInitialize(const char *launchArgument);
const char *GameDirectory();
const char *BootLogPath();
const DataDiscovery &DiscoverOriginalData();
const char *ProbeResultName(ProbeResult result);
void BootLog(const char *format, ...) __attribute__((format(printf, 1, 2)));
bool FlushBootLog();
} // namespace th08::psp
