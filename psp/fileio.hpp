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
const char *DataDirectory();
const char *BootLogPath();
const DataDiscovery &DiscoverOriginalData();
const char *ProbeResultName(ProbeResult result);
void BootLog(const char *format, ...) __attribute__((format(printf, 1, 2)));
// Commits the current chunk without ending the stream. BootLog continues by
// appending to the same per-launch file after a successful flush.
bool FlushBootLog();
// Advances a retryable finalization state machine. A complete FINAL line is an
// orderly-exit witness only; it records the pre-final sync result and makes no
// durability claim. true additionally means the post-FINAL device sync passed.
// On false, call again: queued bytes resume at their unwritten offset and a
// post-FINAL sync failure retries only the sync, never a duplicate FINAL line.
bool FinalizeBootLog();
// Allocation-free PSP kernel I/O. The destination is truncated only after it
// has been opened successfully; short writes are completed in a loop.
bool WriteFileExact(const char *path, const void *data, std::size_t bytes);
} // namespace th08::psp
