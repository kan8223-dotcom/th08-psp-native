#include "fileio.hpp"

#if defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
#include "io_activity_lamp.hpp"
#endif

#include <pspiofilemgr.h>
#include <pspthreadman.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace th08::psp
{
namespace
{
constexpr std::uint64_t kTh08DatBytes = 46838025ULL;
constexpr std::uint64_t kThBgmDatBytes = 449961024ULL;

// BOOT.LOG is diagnostic state, not a frame trace. Keep its RAM footprint
// fixed and amortize Memory Stick traffic into page-sized chunks. Normal
// messages stop before the tail reserve so a truncation record and the orderly
// FINAL record still have somewhere to go.
constexpr std::size_t kBootLogBufferBytes = 4096;
constexpr std::size_t kBootLogRecordBytes = 4096;
constexpr std::uint64_t kBootLogFileLimitBytes = 1024ULL * 1024ULL;
constexpr std::uint64_t kBootLogTailReserveBytes = 4096ULL;
constexpr std::uint64_t kBootLogNormalLimitBytes =
    kBootLogFileLimitBytes - kBootLogTailReserveBytes;
constexpr std::uint32_t kBootLogRecoveryMarkerLimit = 8;

enum class BootLogFinalState : std::uint8_t
{
    Accepting,
    Preparing,
    FinalQueued,
    FinalWritten,
    Synced,
};

char gGameDirectory[512] = "ms0:/PSP/GAME/TH08PSP";
char gLaunchDevice[8] = "ms0:";
char gBootLogPath[640] = "ms0:/PSP/GAME/TH08PSP/TH08PSP_BOOT.LOG";
char gLogBuffer[kBootLogBufferBytes]{};
std::size_t gLogLength = 0;
std::size_t gLogWriteOffset = 0;
std::uint64_t gLogBytesWritten = 0;
std::uint64_t gLogDroppedBytes = 0;
std::uint32_t gLogChunksWritten = 0;
std::uint32_t gLogWriteFailures = 0;
std::uint32_t gLogReportedWriteFailures = 0;
std::uint32_t gLogRecoveryMarkers = 0;
std::uint32_t gLogDroppedRecords = 0;
bool gLogFileReset = false;
bool gLogLimitReached = false;
bool gLogLimitMarkerQueued = false;
bool gLogRecordTruncationMarkerQueued = false;
BootLogFinalState gLogFinalState = BootLogFinalState::Accepting;
bool gFinalPreSyncAttempted = false;
int gFinalPreSyncResult = 0;
std::uint32_t gLogSyncFailures = 0;
SceLwMutexWorkarea gBootLogMutex{};
bool gBootLogMutexReady = false;
bool gInitialized = false;
bool gDiscoveryComplete = false;
DataDiscovery gDiscovery{};

class BootLogStateGuard
{
public:
    BootLogStateGuard()
        : acquired_(gBootLogMutexReady &&
                    sceKernelLockLwMutex(&gBootLogMutex, 1, nullptr) >= 0)
    {
    }

    ~BootLogStateGuard()
    {
        if (acquired_)
        {
            sceKernelUnlockLwMutex(&gBootLogMutex, 1);
        }
    }

    bool Acquired() const
    {
        return acquired_;
    }

private:
    bool acquired_;
};

void NoteBootLogWriteFailure()
{
    if (gLogWriteFailures != UINT32_MAX)
    {
        ++gLogWriteFailures;
    }
}

void NoteBootLogDrop(std::size_t bytes)
{
    if (gLogDroppedRecords != UINT32_MAX)
    {
        ++gLogDroppedRecords;
    }
    const std::uint64_t dropped = static_cast<std::uint64_t>(bytes);
    if (UINT64_MAX - gLogDroppedBytes < dropped)
    {
        gLogDroppedBytes = UINT64_MAX;
    }
    else
    {
        gLogDroppedBytes += dropped;
    }
}

void NoteBootLogSyncFailure()
{
    if (gLogSyncFailures != UINT32_MAX)
    {
        ++gLogSyncFailures;
    }
}

std::uint64_t BootLogAccountedBytes()
{
    return gLogBytesWritten +
           static_cast<std::uint64_t>(gLogLength - gLogWriteOffset);
}

bool WriteBootLogChunk()
{
    if (gLogWriteOffset == gLogLength)
    {
        gLogLength = 0;
        gLogWriteOffset = 0;
        return true;
    }

#if defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
    // BOOT.LOG itself is a 4 KiB buffered diagnostic stream.  Include its
    // ef0 reopen/write/close transaction in the lamp, but suppress the slow
    // textual record to avoid recursively logging while its mutex is held.
    IoActivityScope ioActivity(IoActivityKind::Write, "TH08PSP_BOOT.LOG",
                               false);
#endif

    const int flags = gLogFileReset
                          ? PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND
                          : PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC;
    const SceUID file = sceIoOpen(gBootLogPath, flags, 0666);
    if (file < 0)
    {
        NoteBootLogWriteFailure();
        return false;
    }

    if (!gLogFileReset)
    {
        // Opening with PSP_O_TRUNC established this launch's stream even if a
        // later write fails. A retry must append only the unwritten suffix.
        gLogFileReset = true;
        gLogBytesWritten = 0;
    }

    bool complete = true;
    while (gLogWriteOffset < gLogLength)
    {
        const std::size_t remaining = gLogLength - gLogWriteOffset;
        const unsigned int request = static_cast<unsigned int>(remaining);
        const int written = sceIoWrite(file, gLogBuffer + gLogWriteOffset, request);
        if (written <= 0 || static_cast<unsigned int>(written) > request)
        {
            complete = false;
            NoteBootLogWriteFailure();
            break;
        }
        gLogWriteOffset += static_cast<std::size_t>(written);
        gLogBytesWritten += static_cast<std::uint64_t>(written);
    }

    const int closeResult = sceIoClose(file);
    if (closeResult < 0)
    {
        complete = false;
        NoteBootLogWriteFailure();
    }

    if (gLogWriteOffset == gLogLength)
    {
        gLogLength = 0;
        gLogWriteOffset = 0;
        if (gLogChunksWritten != UINT32_MAX)
        {
            ++gLogChunksWritten;
        }
    }
    return complete;
}

bool QueueBootLogInternal(const char *record, std::size_t bytes)
{
    if (record == nullptr || bytes == 0 || bytes > sizeof(gLogBuffer))
    {
        return false;
    }
    if (BootLogAccountedBytes() + static_cast<std::uint64_t>(bytes) >
        kBootLogFileLimitBytes)
    {
        return false;
    }
    if (sizeof(gLogBuffer) - gLogLength < bytes)
    {
        if (!WriteBootLogChunk())
        {
            return false;
        }
    }
    std::memcpy(gLogBuffer + gLogLength, record, bytes);
    gLogLength += bytes;
    return true;
}

void QueueBootLogRecoveryMarker()
{
    if (gLogReportedWriteFailures == gLogWriteFailures)
    {
        return;
    }

    // Bound diagnostic self-noise if a failing Memory Stick repeatedly
    // recovers. FINAL always carries the aggregate counts.
    if (gLogRecoveryMarkers >= kBootLogRecoveryMarkerLimit)
    {
        gLogReportedWriteFailures = gLogWriteFailures;
        return;
    }

    char marker[256];
    const int length = std::snprintf(
        marker,
        sizeof(marker),
        "BOOT_LOG WRITE_FAILURE_RECOVERED failures=%lu dropped_records=%lu "
        "dropped_bytes_at_least=%llu\n",
        static_cast<unsigned long>(gLogWriteFailures),
        static_cast<unsigned long>(gLogDroppedRecords),
        static_cast<unsigned long long>(gLogDroppedBytes));
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(marker) &&
        QueueBootLogInternal(marker, static_cast<std::size_t>(length)))
    {
        gLogReportedWriteFailures = gLogWriteFailures;
        ++gLogRecoveryMarkers;
    }
}

void QueueBootLogLimitMarker(std::size_t firstDroppedBytes)
{
    if (gLogLimitMarkerQueued)
    {
        return;
    }

    char marker[256];
    const int length = std::snprintf(
        marker,
        sizeof(marker),
        "BOOT_LOG TRUNCATED limit=%llu first_dropped_bytes=%lu "
        "further_records=discarded_until_final\n",
        static_cast<unsigned long long>(kBootLogFileLimitBytes),
        static_cast<unsigned long>(firstDroppedBytes));
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(marker) &&
        QueueBootLogInternal(marker, static_cast<std::size_t>(length)))
    {
        gLogLimitMarkerQueued = true;
    }
}

void QueueBootLogRecordTooLargeMarker(std::size_t requiredBytes)
{
    if (gLogRecordTruncationMarkerQueued)
    {
        return;
    }

    char marker[192];
    const int length = std::snprintf(
        marker,
        sizeof(marker),
        "BOOT_LOG RECORD_TRUNCATED required_bytes=%lu max_record_bytes=%lu\n",
        static_cast<unsigned long>(requiredBytes),
        static_cast<unsigned long>(kBootLogRecordBytes - 1));
    if (length > 0 && static_cast<std::size_t>(length) < sizeof(marker))
    {
        gLogRecordTruncationMarkerQueued =
            QueueBootLogInternal(marker, static_cast<std::size_t>(length));
    }
}

bool JoinPath(char *out, std::size_t outSize, const char *root, const char *relative)
{
    const int length = std::snprintf(out, outSize, "%s/%s", root, relative);
    return length >= 0 && static_cast<std::size_t>(length) < outSize;
}

bool AsciiPathEqual(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0')
    {
        unsigned char a = static_cast<unsigned char>(*left++);
        unsigned char b = static_cast<unsigned char>(*right++);
        if (a >= static_cast<unsigned char>('A') && a <= static_cast<unsigned char>('Z'))
        {
            a = static_cast<unsigned char>(a - static_cast<unsigned char>('A') +
                                           static_cast<unsigned char>('a'));
        }
        if (b >= static_cast<unsigned char>('A') && b <= static_cast<unsigned char>('Z'))
        {
            b = static_cast<unsigned char>(b - static_cast<unsigned char>('A') +
                                           static_cast<unsigned char>('a'));
        }
        if (a != b)
        {
            return false;
        }
    }
    return *left == *right;
}

FileProbe ProbeFile(const char *path, const char expectedMagic[4], std::uint64_t expectedBytes)
{
    FileProbe probe{};
    probe.result = ProbeResult::Missing;

    SceIoStat stat{};
    const int statResult = sceIoGetstat(path, &stat);
    if (statResult < 0)
    {
        return probe;
    }

    probe.observedBytes = static_cast<std::uint64_t>(stat.st_size);
    if (probe.observedBytes != expectedBytes)
    {
        probe.result = ProbeResult::WrongSize;
        return probe;
    }

    const SceUID file = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (file < 0)
    {
        probe.result = ProbeResult::IoError;
        return probe;
    }

    const int bytesRead = sceIoRead(file, probe.observedMagic, 4);
    sceIoClose(file);
    if (bytesRead != 4)
    {
        probe.result = ProbeResult::IoError;
        return probe;
    }
    probe.observedMagic[4] = '\0';
    if (std::memcmp(probe.observedMagic, expectedMagic, 4) != 0)
    {
        probe.result = ProbeResult::WrongMagic;
        return probe;
    }

    probe.result = ProbeResult::Ready;
    return probe;
}

DataDiscovery ProbeRoot(const char *root)
{
    DataDiscovery discovery{};
    std::snprintf(discovery.root, sizeof(discovery.root), "%s", root);

    char gamePath[640];
    char bgmPath[640];
    if (!JoinPath(gamePath, sizeof(gamePath), root, "th08.dat") ||
        !JoinPath(bgmPath, sizeof(bgmPath), root, "thbgm.dat"))
    {
        discovery.gameArchive.result = ProbeResult::IoError;
        discovery.bgmArchive.result = ProbeResult::IoError;
        return discovery;
    }

    discovery.gameArchive = ProbeFile(gamePath, "PBGZ", kTh08DatBytes);
    discovery.bgmArchive = ProbeFile(bgmPath, "ZWAV", kThBgmDatBytes);
    discovery.ready = discovery.gameArchive.result == ProbeResult::Ready &&
                      discovery.bgmArchive.result == ProbeResult::Ready;
    return discovery;
}

bool TryCandidate(const char *candidate, DataDiscovery &bestFailure)
{
    if (candidate == nullptr || candidate[0] == '\0')
    {
        return false;
    }
    const DataDiscovery current = ProbeRoot(candidate);
    if (current.ready)
    {
        gDiscovery = current;
        return true;
    }

    const bool bestHasNoObservedFile =
        bestFailure.gameArchive.result == ProbeResult::Missing &&
        bestFailure.bgmArchive.result == ProbeResult::Missing;
    const bool currentHasObservedFile =
        current.gameArchive.result != ProbeResult::Missing ||
        current.bgmArchive.result != ProbeResult::Missing;
    if (bestFailure.root[0] == '\0' || (bestHasNoObservedFile && currentHasObservedFile))
    {
        bestFailure = current;
    }
    return false;
}

bool ScanSiblingInstalls(const char *device, DataDiscovery &bestFailure)
{
    char gameRoot[64];
    if (!JoinPath(gameRoot, sizeof(gameRoot), device, "PSP/GAME"))
    {
        return false;
    }

    const SceUID directory = sceIoDopen(gameRoot);
    if (directory < 0)
    {
        return false;
    }

    bool found = false;
    SceIoDirent entry{};
    while (!found && sceIoDread(directory, &entry) > 0)
    {
        if (entry.d_name[0] != '\0' && std::strcmp(entry.d_name, ".") != 0 &&
            std::strcmp(entry.d_name, "..") != 0 && FIO_S_ISDIR(entry.d_stat.st_mode))
        {
            char installRoot[512];
            char candidate[512];
            if (JoinPath(installRoot, sizeof(installRoot), gameRoot, entry.d_name))
            {
                if (JoinPath(candidate, sizeof(candidate), installRoot, "th8") &&
                    TryCandidate(candidate, bestFailure))
                {
                    found = true;
                }
                else if (JoinPath(candidate, sizeof(candidate), installRoot, "TH08") &&
                         TryCandidate(candidate, bestFailure))
                {
                    found = true;
                }
                else if (TryCandidate(installRoot, bestFailure))
                {
                    found = true;
                }
            }
        }
        std::memset(&entry, 0, sizeof(entry));
    }
    sceIoDclose(directory);
    return found;
}

bool ScanKnownDeviceRoots(const char *device, DataDiscovery &bestFailure)
{
    constexpr const char *kKnownNames[] = {
        "th8", "TH08", "eiyashou", "ImperishableNight",
    };
    for (const char *name : kKnownNames)
    {
        char candidate[512];
        if (JoinPath(candidate, sizeof(candidate), device, name) &&
            TryCandidate(candidate, bestFailure))
        {
            return true;
        }
    }
    return TryCandidate(device, bestFailure);
}

void SetLaunchPath(const char *launchArgument)
{
    if (launchArgument == nullptr)
    {
        return;
    }

    const char *colon = std::strchr(launchArgument, ':');
    const char *slash = std::strrchr(launchArgument, '/');
    if (colon == nullptr || slash == nullptr || colon > slash)
    {
        return;
    }

    const std::size_t directoryLength = static_cast<std::size_t>(slash - launchArgument);
    if (directoryLength == 0 || directoryLength >= sizeof(gGameDirectory))
    {
        return;
    }
    std::memcpy(gGameDirectory, launchArgument, directoryLength);
    gGameDirectory[directoryLength] = '\0';

    const std::size_t deviceLength = static_cast<std::size_t>(colon - launchArgument + 1);
    if (deviceLength > 0 && deviceLength < sizeof(gLaunchDevice))
    {
        std::memcpy(gLaunchDevice, launchArgument, deviceLength);
        gLaunchDevice[deviceLength] = '\0';
    }
}
} // namespace

void FileIoInitialize(const char *launchArgument)
{
    if (gInitialized)
    {
        return;
    }
    SetLaunchPath(launchArgument);
    JoinPath(gBootLogPath, sizeof(gBootLogPath), gGameDirectory, "TH08PSP_BOOT.LOG");
    sceIoChdir(gGameDirectory);

    // The workarea is static and never touches newlib. All later BOOT.LOG
    // state and file operations are serialized across the main and gameplay
    // setup threads by this kernel lightweight mutex. Fail closed if the
    // kernel cannot establish the lock: an unlocked diagnostic sink must not
    // be allowed to corrupt memory or record order.
    gBootLogMutexReady =
        sceKernelCreateLwMutex(&gBootLogMutex,
                               "TH08BootLog",
                               PSP_LW_MUTEX_ATTR_THFIFO,
                               0,
                               nullptr) >= 0;

    char stateDirectory[640];
    if (JoinPath(stateDirectory, sizeof(stateDirectory), gGameDirectory, "replay"))
    {
        sceIoMkdir(stateDirectory, 0777);
    }
    if (JoinPath(stateDirectory, sizeof(stateDirectory), gGameDirectory, "snapshot"))
    {
        sceIoMkdir(stateDirectory, 0777);
    }

    // Establish an empty per-launch file immediately. This prevents a crash
    // before the first full chunk from leaving a previous run looking current.
    const SceUID bootLog = sceIoOpen(gBootLogPath,
                                     PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC,
                                     0666);
    if (bootLog >= 0)
    {
        gLogFileReset = true;
        if (sceIoClose(bootLog) < 0)
        {
            NoteBootLogWriteFailure();
        }
    }
    else
    {
        NoteBootLogWriteFailure();
    }
    gInitialized = true;
}

const char *GameDirectory()
{
    return gGameDirectory;
}

const char *DataDirectory()
{
    return DiscoverOriginalData().root;
}

const char *BootLogPath()
{
    return gBootLogPath;
}

const DataDiscovery &DiscoverOriginalData()
{
    if (gDiscoveryComplete)
    {
        return gDiscovery;
    }

    DataDiscovery bestFailure{};
    bestFailure.gameArchive.result = ProbeResult::Missing;
    bestFailure.bgmArchive.result = ProbeResult::Missing;

    char candidate[512];
    bool found = JoinPath(candidate, sizeof(candidate), gGameDirectory, "th8") &&
                 TryCandidate(candidate, bestFailure);
    if (!found)
    {
        found = JoinPath(candidate, sizeof(candidate), gGameDirectory, "TH08") &&
                TryCandidate(candidate, bestFailure);
    }
    if (!found)
    {
        found = TryCandidate(gGameDirectory, bestFailure);
    }
    if (!found)
    {
        found = ScanSiblingInstalls(gLaunchDevice, bestFailure);
    }
    if (!found)
    {
        found = ScanKnownDeviceRoots(gLaunchDevice, bestFailure);
    }

    const char *otherDevice = AsciiPathEqual(gLaunchDevice, "ef0:") ? "ms0:" : "ef0:";
    if (!found)
    {
        found = ScanSiblingInstalls(otherDevice, bestFailure);
    }
    if (!found)
    {
        found = ScanKnownDeviceRoots(otherDevice, bestFailure);
    }

    if (!found)
    {
        gDiscovery = bestFailure;
    }
    gDiscoveryComplete = true;
    return gDiscovery;
}

const char *ProbeResultName(ProbeResult result)
{
    switch (result)
    {
    case ProbeResult::Ready:
        return "READY";
    case ProbeResult::Missing:
        return "MISSING";
    case ProbeResult::WrongSize:
        return "WRONG_SIZE";
    case ProbeResult::WrongMagic:
        return "WRONG_MAGIC";
    case ProbeResult::IoError:
        return "IO_ERROR";
    }
    return "UNKNOWN";
}

namespace
{
bool FlushBootLogLocked()
{
    bool complete = WriteBootLogChunk();
    if (complete && gLogReportedWriteFailures != gLogWriteFailures)
    {
        QueueBootLogRecoveryMarker();
        complete = WriteBootLogChunk();
    }
    return complete;
}

bool QueueBootLogFinalMarkerLocked()
{
    const char *contentStatus = "OK";
    if (gLogLimitReached)
    {
        contentStatus = "TRUNCATED";
    }
    else if (gLogDroppedRecords != 0)
    {
        contentStatus = "RECOVERED_WITH_LOSS";
    }
    else if (gLogWriteFailures != 0)
    {
        contentStatus = "RECOVERED";
    }

    char marker[512];
    const int length = std::snprintf(
        marker,
        sizeof(marker),
        "BOOT_LOG FINAL orderly=1 witness=orderly_exit durability=not_asserted "
        "content_status=%s pre_final_sync_result=%d post_final_sync=pending "
        "bytes_before_final=%llu chunks_before_final=%lu "
        "write_failures_before_final=%lu sync_failures_before_final=%lu "
        "dropped_records=%lu dropped_bytes_at_least=%llu limit_reached=%d\n",
        contentStatus,
        gFinalPreSyncResult,
        static_cast<unsigned long long>(gLogBytesWritten),
        static_cast<unsigned long>(gLogChunksWritten),
        static_cast<unsigned long>(gLogWriteFailures),
        static_cast<unsigned long>(gLogSyncFailures),
        static_cast<unsigned long>(gLogDroppedRecords),
        static_cast<unsigned long long>(gLogDroppedBytes),
        gLogLimitReached ? 1 : 0);
    return length > 0 && static_cast<std::size_t>(length) < sizeof(marker) &&
           QueueBootLogInternal(marker, static_cast<std::size_t>(length));
}
} // namespace

void BootLog(const char *format, ...)
{
    if (format == nullptr)
    {
        return;
    }

    // Formatting is local and allocation-free. Lock only the shared stream
    // state and I/O so gameplay setup does not wait on another thread's
    // snprintf work.
    char record[kBootLogRecordBytes];
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(record, sizeof(record), format, arguments);
    va_end(arguments);
    if (written <= 0)
    {
        return;
    }

    BootLogStateGuard guard;
    if (!guard.Acquired() || gLogFinalState != BootLogFinalState::Accepting)
    {
        return;
    }

    const std::size_t required = static_cast<std::size_t>(written);
    if (required >= sizeof(record))
    {
        NoteBootLogDrop(required);
        QueueBootLogRecordTooLargeMarker(required);
        return;
    }

    if (gLogLimitReached)
    {
        NoteBootLogDrop(required);
        return;
    }

    if (BootLogAccountedBytes() + static_cast<std::uint64_t>(required) >
        kBootLogNormalLimitBytes)
    {
        gLogLimitReached = true;
        NoteBootLogDrop(required);
        QueueBootLogLimitMarker(required);
        return;
    }

    if (sizeof(gLogBuffer) - gLogLength < required)
    {
        const std::uint32_t failuresBefore = gLogWriteFailures;
        if (!WriteBootLogChunk())
        {
            NoteBootLogDrop(required);
            return;
        }
        if (gLogWriteFailures != failuresBefore ||
            gLogReportedWriteFailures != gLogWriteFailures)
        {
            QueueBootLogRecoveryMarker();
        }
        if (sizeof(gLogBuffer) - gLogLength < required && !WriteBootLogChunk())
        {
            NoteBootLogDrop(required);
            return;
        }
    }

    std::memcpy(gLogBuffer + gLogLength, record, required);
    gLogLength += required;
}

bool FlushBootLog()
{
    BootLogStateGuard guard;
    if (!guard.Acquired())
    {
        return false;
    }
    if (gLogFinalState == BootLogFinalState::Synced)
    {
        return true;
    }
    if (gLogFinalState != BootLogFinalState::Accepting)
    {
        return false;
    }
    if (!FlushBootLogLocked())
    {
        return false;
    }

    // Startup checkpoints exist to survive exactly the hard-fault/power-off
    // case where a close alone may still leave Memory Stick writes queued.
    // This path is never used per frame; pay the device-sync cost so the last
    // completed SDL/PSPGL/GE4 boundary is a durable diagnostic witness.
    if (sceIoSync(gLaunchDevice, 0) < 0)
    {
        NoteBootLogSyncFailure();
        return false;
    }
    return true;
}

bool FinalizeBootLog()
{
    BootLogStateGuard guard;
    if (!guard.Acquired())
    {
        return false;
    }

    if (gLogFinalState == BootLogFinalState::Synced)
    {
        return true;
    }
    if (gLogFinalState == BootLogFinalState::Accepting)
    {
        // This transition rejects every later BootLog call, including calls
        // from the gameplay setup thread after shutdown has begun.
        gLogFinalState = BootLogFinalState::Preparing;
    }

    if (gLogFinalState == BootLogFinalState::Preparing)
    {
        if (!FlushBootLogLocked())
        {
            return false;
        }

        if (!gFinalPreSyncAttempted)
        {
            gFinalPreSyncResult = sceIoSync(gLaunchDevice, 0);
            gFinalPreSyncAttempted = true;
            if (gFinalPreSyncResult < 0)
            {
                NoteBootLogSyncFailure();
            }
        }

        // Queue FINAL once. If writing it later fails, gLogWriteOffset points
        // at the exact suffix for the next FinalizeBootLog attempt.
        if (!QueueBootLogFinalMarkerLocked())
        {
            return false;
        }
        gLogFinalState = BootLogFinalState::FinalQueued;
    }

    if (gLogFinalState == BootLogFinalState::FinalQueued)
    {
        const bool writeComplete = WriteBootLogChunk();
        if (gLogLength == 0 && gLogWriteOffset == 0)
        {
            // Even when close reports an error, the complete marker must not
            // be queued a second time. A later sync is the only safe retry.
            gLogFinalState = BootLogFinalState::FinalWritten;
        }
        if (!writeComplete || gLogFinalState != BootLogFinalState::FinalWritten)
        {
            return false;
        }
    }

    if (gLogFinalState == BootLogFinalState::FinalWritten)
    {
        if (sceIoSync(gLaunchDevice, 0) < 0)
        {
            NoteBootLogSyncFailure();
            return false;
        }
        gLogFinalState = BootLogFinalState::Synced;
    }
    return gLogFinalState == BootLogFinalState::Synced;
}

bool WriteFileExact(const char *path, const void *data, std::size_t bytes)
{
    if (path == nullptr || (data == nullptr && bytes != 0))
    {
        return false;
    }

    const SceUID file = sceIoOpen(path,
                                  PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC,
                                  0666);
    if (file < 0)
    {
        return false;
    }

    const auto *source = static_cast<const unsigned char *>(data);
    std::size_t offset = 0;
    bool complete = true;
    while (offset < bytes)
    {
        const std::size_t remainingBytes = bytes - offset;
        const unsigned int request = remainingBytes > 0x7fffffffu
                                         ? 0x7fffffffu
                                         : static_cast<unsigned int>(remainingBytes);
        const int written = sceIoWrite(file, source + offset, request);
        if (written <= 0 || static_cast<unsigned int>(written) > request)
        {
            complete = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }

    const int closeResult = sceIoClose(file);
    return complete && offset == bytes && closeResult >= 0;
}
} // namespace th08::psp
