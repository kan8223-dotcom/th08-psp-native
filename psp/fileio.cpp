#include "fileio.hpp"

#include <pspiofilemgr.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace th08::psp
{
namespace
{
constexpr std::uint64_t kTh08DatBytes = 46838025ULL;
constexpr std::uint64_t kThBgmDatBytes = 449961024ULL;

char gGameDirectory[512] = "ms0:/PSP/GAME/TH08PSP";
char gLaunchDevice[8] = "ms0:";
char gBootLogPath[640] = "ms0:/PSP/GAME/TH08PSP/TH08PSP_BOOT.LOG";
char gLogBuffer[16384]{};
std::size_t gLogLength = 0;
bool gInitialized = false;
bool gDiscoveryComplete = false;
DataDiscovery gDiscovery{};

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

    char stateDirectory[640];
    if (JoinPath(stateDirectory, sizeof(stateDirectory), gGameDirectory, "replay"))
    {
        sceIoMkdir(stateDirectory, 0777);
    }
    if (JoinPath(stateDirectory, sizeof(stateDirectory), gGameDirectory, "snapshot"))
    {
        sceIoMkdir(stateDirectory, 0777);
    }
    gInitialized = true;
}

const char *GameDirectory()
{
    return gGameDirectory;
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

void BootLog(const char *format, ...)
{
    if (format == nullptr || gLogLength >= sizeof(gLogBuffer) - 1)
    {
        return;
    }

    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(gLogBuffer + gLogLength,
                                       sizeof(gLogBuffer) - gLogLength,
                                       format, arguments);
    va_end(arguments);
    if (written <= 0)
    {
        return;
    }

    const std::size_t available = sizeof(gLogBuffer) - gLogLength;
    const std::size_t appended = static_cast<std::size_t>(written) < available
                                     ? static_cast<std::size_t>(written)
                                     : available - 1;
    gLogLength += appended;
}

bool FlushBootLog()
{
    const SceUID file = sceIoOpen(gBootLogPath,
                                  PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC,
                                  0777);
    if (file < 0)
    {
        return false;
    }
    const int written = sceIoWrite(file, gLogBuffer, static_cast<unsigned int>(gLogLength));
    sceIoClose(file);
    return written == static_cast<int>(gLogLength);
}
} // namespace th08::psp
