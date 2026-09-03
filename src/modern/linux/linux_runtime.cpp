#include "modern/windows_runtime.hpp"

#if defined(PSP)
#include "boot_checkpoint.hpp"
#include "fileio.hpp"
#if defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
#include "io_activity_lamp.hpp"
#endif
#else
#include <execinfo.h>
#endif
#include <fcntl.h>
#if !defined(PSP)
#include <signal.h>
#endif
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <windows.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPTSTR, int);

namespace th08
{
struct AnmVm;
struct Effect;

int __fastcall EffectRandomSplashInit(Effect *);
int __fastcall EffectRandomSplashUpdate(Effect *);
int __fastcall EffectRandomSplashBigInit(Effect *);
int __fastcall EffectOrbitInit(Effect *);
int __fastcall EffectOrbitUpdate(Effect *);

int __fastcall UpdateExpandingWavyRadialTrail(Effect *);
int __fastcall UpdateExpandingPositiveDiagonalRadialTrail(Effect *);
int __fastcall UpdateExpandingNegativeDiagonalRadialTrail(Effect *);
int __fastcall UpdateExpandingOctagonalRadialTrail(Effect *);
int __fastcall UpdateExpandingTwelveSegmentRadialTrail(Effect *);
int __fastcall UpdateBarrierRadialEffect(Effect *);
int __fastcall InitializeBarrierRadialEffect(Effect *);
int __fastcall InitializeRotatingBarrierRadialEffect(Effect *);
int __fastcall UpdateExpandingOrthogonalRadialTrail(Effect *);

int __fastcall InitializeTintedBossTrackingCameraParticle(Effect *);
int __fastcall UpdateTintedBossTrackingCameraParticle(Effect *);
int __fastcall InitializeRisingBossTrackingCameraParticle(Effect *);
int __fastcall UpdateRisingBossTrackingCameraParticle(Effect *);
int __fastcall InitializeRandomDirectionalOffset(Effect *);
int __fastcall UpdateDirectionalOffset60(Effect *);
int __fastcall TrackPlayerUntilAnimationEnds(Effect *);
int __fastcall UpdateDirectionalOffset240(Effect *);
int __fastcall UpdateSpinningCameraParticle(Effect *);
int __fastcall InitializeSpinningCameraParticle(Effect *);
int __fastcall InitializeDirectionalOffset(Effect *);
int __fastcall UpdateEasedDirectionalOffset(Effect *);
int __fastcall KeepTrailAlive(Effect *);
int __fastcall InitializeTrailOffset(Effect *);
int __fastcall InitializeRadialTrail(Effect *);
int __fastcall InitializeAlternateLayerRadialTrail(Effect *);
int __fastcall SyncRadialTrailRadius(Effect *);
int __fastcall SyncRadialTrailShape(Effect *);
int __fastcall UpdateTimedRadialTrail(Effect *);
int __fastcall UpdateFadingRadialTrail(Effect *);
int __fastcall SyncAnchoredRadialTrail(Effect *);

// This retail table entry points at an AnmVm member. On the 32-bit Linux ABI
// its code entry receives `this` as the first stack argument, matching the
// reconstructed effect callback invocation.
extern "C" int UpdatePulsingRadialTrailCallback(AnmVm *) asm("_ZN4th085AnmVm24UpdatePulsingRadialTrailEv");

namespace modern
{
struct ModernEffectTemplate
{
    int32_t scriptIdx;
    uintptr_t update;
    uintptr_t initialize;
};

// These aliases bind the semantic target globals exported at fixed addresses
// by th08-layout.ld.  Keep the compatibility storage types local to the Linux
// runtime: the reconstructed VC7 declarations retain their original owners.
extern int32_t g_ModernLastSpellCountStorage asm("_ZN4th0816g_LastSpellCountE");
extern ModernEffectTemplate g_ModernEffectTemplatesStorage[66]
    asm("_ZN4th0817g_EffectTemplatesE");
extern int32_t g_ModernGuiStageClearBonusesStorage[9]
    asm("_ZN4th0822g_GuiStageClearBonusesE");
extern uint32_t g_ModernGuiMessageTextColorsStorage[12][4]
    asm("_ZN4th0822g_GuiMessageTextColorsE");

namespace
{
int g_argumentCount;
char **g_arguments;
#if !defined(PSP)
volatile sig_atomic_t g_reportingCrash;

void WriteCrashLine(int file, const char *line)
{
    if (line != NULL) write(file, line, strlen(line));
}

void ReportFatalSignal(int signalNumber, siginfo_t *signalInfo, void *)
{
    if (g_reportingCrash)
        _exit(128 + signalNumber);
    g_reportingCrash = 1;

    int file = open("modern-crash.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file >= 0)
    {
        char line[160];
        snprintf(line, sizeof(line), "signal=%d fault-address=%p pid=%ld\n", signalNumber,
                 signalInfo != NULL ? signalInfo->si_addr : NULL, static_cast<long>(getpid()));
        WriteCrashLine(file, line);

        void *frames[64];
        int frameCount = backtrace(frames, sizeof(frames) / sizeof(frames[0]));
        backtrace_symbols_fd(frames, frameCount, file);
        fsync(file);
        close(file);
    }

    signal(signalNumber, SIG_DFL);
    raise(signalNumber);
    _exit(128 + signalNumber);
}

void InstallSignalHandler(int signalNumber)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = ReportFatalSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(signalNumber, &action, NULL);
}
#endif

uintptr_t CodeAddress(int (__fastcall *callback)(AnmVm *))
{
    return reinterpret_cast<uintptr_t>(callback);
}

uintptr_t CodeAddress(int (__fastcall *callback)(Effect *))
{
    return reinterpret_cast<uintptr_t>(callback);
}

void InitializeTargetData()
{
    static const ModernEffectTemplate effectTemplates[66] = {
        {28, 0, 0}, {29, 0, 0}, {30, 0, 0},
        {31, CodeAddress(EffectRandomSplashUpdate), CodeAddress(EffectRandomSplashBigInit)},
        {36, CodeAddress(EffectRandomSplashUpdate), CodeAddress(EffectRandomSplashInit)},
        {37, CodeAddress(EffectRandomSplashUpdate), CodeAddress(EffectRandomSplashInit)},
        {38, CodeAddress(EffectRandomSplashUpdate), CodeAddress(EffectRandomSplashInit)},
        {39, CodeAddress(EffectRandomSplashUpdate), CodeAddress(EffectRandomSplashInit)},
        {40, CodeAddress(EffectRandomSplashUpdate), CodeAddress(EffectRandomSplashInit)},
        {41, CodeAddress(EffectRandomSplashUpdate), CodeAddress(EffectRandomSplashInit)},
        {42, CodeAddress(EffectRandomSplashUpdate), CodeAddress(EffectRandomSplashInit)},
        {43, CodeAddress(EffectRandomSplashUpdate), CodeAddress(EffectRandomSplashInit)},
        {44, 0, 0},
        {45, CodeAddress(EffectOrbitUpdate), CodeAddress(EffectOrbitInit)},
        {45, CodeAddress(EffectOrbitUpdate), CodeAddress(EffectOrbitInit)},
        {45, CodeAddress(EffectOrbitUpdate), CodeAddress(EffectOrbitInit)},
        {0, 0, 0},
        {32, CodeAddress(UpdateDirectionalOffset60), CodeAddress(InitializeRandomDirectionalOffset)},
        {33, CodeAddress(UpdateDirectionalOffset240), CodeAddress(InitializeRandomDirectionalOffset)},
        {51, CodeAddress(UpdateSpinningCameraParticle), CodeAddress(InitializeSpinningCameraParticle)},
        {56, 0, 0},
        {52, CodeAddress(UpdateEasedDirectionalOffset), CodeAddress(InitializeDirectionalOffset)},
        {54, CodeAddress(TrackPlayerUntilAnimationEnds), 0},
        {104, CodeAddress(KeepTrailAlive), 0},
        {104, CodeAddress(KeepTrailAlive), 0},
        {35, 0, 0},
        {53, CodeAddress(UpdateEasedDirectionalOffset), CodeAddress(InitializeDirectionalOffset)},
        {34, CodeAddress(UpdateDirectionalOffset60), CodeAddress(InitializeRandomDirectionalOffset)},
        {57, 0, 0}, {58, 0, 0}, {59, 0, 0}, {60, 0, 0},
        {48, 0, 0}, {49, 0, 0}, {50, 0, 0},
        {88, CodeAddress(SyncRadialTrailRadius), CodeAddress(InitializeRadialTrail)},
        {88, CodeAddress(UpdateBarrierRadialEffect), CodeAddress(InitializeBarrierRadialEffect)},
        {92, CodeAddress(UpdateBarrierRadialEffect), CodeAddress(InitializeRotatingBarrierRadialEffect)},
        {71, 0, 0},
        {76, CodeAddress(SyncRadialTrailRadius), CodeAddress(InitializeRadialTrail)},
        {81, CodeAddress(SyncRadialTrailShape), CodeAddress(InitializeRadialTrail)},
        {82, CodeAddress(UpdatePulsingRadialTrailCallback), CodeAddress(InitializeRadialTrail)},
        {83, CodeAddress(UpdateExpandingWavyRadialTrail), CodeAddress(InitializeRadialTrail)},
        {83, CodeAddress(UpdateExpandingPositiveDiagonalRadialTrail), CodeAddress(InitializeRadialTrail)},
        {83, CodeAddress(UpdateExpandingNegativeDiagonalRadialTrail), CodeAddress(InitializeRadialTrail)},
        {83, CodeAddress(UpdateExpandingOctagonalRadialTrail), CodeAddress(InitializeRadialTrail)},
        {84, CodeAddress(UpdateExpandingTwelveSegmentRadialTrail), CodeAddress(InitializeRadialTrail)},
        {72, 0, 0},
        {85, CodeAddress(UpdateExpandingOrthogonalRadialTrail), CodeAddress(InitializeRadialTrail)},
        {86, CodeAddress(SyncRadialTrailRadius), CodeAddress(InitializeRadialTrail)},
        {80, CodeAddress(UpdateTimedRadialTrail), CodeAddress(InitializeRadialTrail)},
        {73, CodeAddress(UpdateTintedBossTrackingCameraParticle),
         CodeAddress(InitializeTintedBossTrackingCameraParticle)},
        {77, CodeAddress(SyncRadialTrailRadius), CodeAddress(InitializeRadialTrail)},
        {88, CodeAddress(UpdateFadingRadialTrail), CodeAddress(InitializeRadialTrail)},
        {88, CodeAddress(UpdateFadingRadialTrail), CodeAddress(InitializeRadialTrail)},
        {87, CodeAddress(SyncRadialTrailShape), CodeAddress(InitializeRadialTrail)},
        {96, CodeAddress(SyncRadialTrailShape), CodeAddress(InitializeAlternateLayerRadialTrail)},
        {55, 0, 0},
        {100, CodeAddress(SyncRadialTrailShape), CodeAddress(InitializeAlternateLayerRadialTrail)},
        {78, CodeAddress(SyncRadialTrailRadius), CodeAddress(InitializeRadialTrail)},
        {102, 0, CodeAddress(InitializeTrailOffset)},
        {103, 0, CodeAddress(InitializeTrailOffset)},
        {75, 0, 0},
        {74, CodeAddress(UpdateRisingBossTrackingCameraParticle),
         CodeAddress(InitializeRisingBossTrackingCameraParticle)},
        {77, CodeAddress(SyncAnchoredRadialTrail), CodeAddress(InitializeRadialTrail)},
        {98, CodeAddress(SyncRadialTrailShape), CodeAddress(InitializeAlternateLayerRadialTrail)},
    };
    static const int32_t stageScoreTables[9] = {
        1000000, 1500000, 2000000, 2500000, 2500000, 3000000, 4000000, 6000000, 6660000,
    };
    static const uint32_t messageTextColors[12][4] = {
        {0x00e8f0ff, 0x00f0e8ff, 0x00ffe8f0, 0x00ffe8f0},
        {0x00e8f0ff, 0x00f0e8ff, 0x00ffe8f0, 0x00ffe8f0},
        {0x00e8f0ff, 0x00f0e8ff, 0x00ffe8f0, 0x00ffe8f0},
        {0x00e8f0ff, 0x00f0e8ff, 0x00ffe8f0, 0x00ffe8f0},
        {0x00e8f0ff, 0x00f0e8ff, 0x00ffe8f0, 0x00ffe8f0},
        {0x00e8f0ff, 0x00f0e8ff, 0x00ffe8f0, 0x00ffe8f0},
        {0x00e8f0ff, 0x00f0e8ff, 0x00ffe8f0, 0x00ffe8f0},
        {0x00e8f0ff, 0x00f0e8ff, 0x00ffe8f0, 0x00ffe8f0},
        {0x00e8f0ff, 0x00f0e8ff, 0x00ffe8f0, 0x00ffe8f0},
        {0x00e8f0ff, 0x00f0e8ff, 0x00ffe8f0, 0x00ffe8f0},
        {0x00e8f0ff, 0x00f0e8ff, 0x00ffe8f0, 0x00ffe8f0},
        {0x00e8f0ff, 0x00f0e8ff, 0x00ffe8f0, 0x00ffe8f0},
    };
    g_ModernLastSpellCountStorage = 43;
    memcpy(g_ModernEffectTemplatesStorage, effectTemplates, sizeof(effectTemplates));
    memcpy(g_ModernGuiStageClearBonusesStorage, stageScoreTables, sizeof(stageScoreTables));
    memcpy(g_ModernGuiMessageTextColorsStorage, messageTextColors, sizeof(messageTextColors));
}
}

bool ConfigureDataDirectory()
{
#if defined(PSP)
    // Keep the process working directory at the EBOOT directory so writable
    // state (configuration, score, replays, snapshots, and logs) stays on the
    // Memory Stick install.  linux_compat.cpp resolves only the two immutable
    // retail archives through DataDirectory().
    const char *gameDirectory = th08::psp::GameDirectory();
    const th08::psp::DataDiscovery &data = th08::psp::DiscoverOriginalData();
    if (gameDirectory == NULL || gameDirectory[0] == '\0' ||
        chdir(gameDirectory) != 0)
    {
        fprintf(stderr, "th08-psp: unable to enter game directory: %s\n",
                gameDirectory != NULL ? gameDirectory : "<null>");
        return false;
    }
    if (!data.ready || data.root[0] == '\0')
    {
        fprintf(stderr, "th08-psp: original th08.dat/thbgm.dat are unavailable\n");
        return false;
    }
    return true;
#else
    const char *directory = NULL;
    for (int index = 1; index < g_argumentCount; ++index)
    {
        if (strcmp(g_arguments[index], "--data-dir") == 0)
        {
            if (++index >= g_argumentCount)
            {
                fprintf(stderr, "th08-modern: --data-dir requires a directory path\n");
                return false;
            }
            directory = g_arguments[index];
        }
        else if (strncmp(g_arguments[index], "--data-dir=", 11) == 0)
        {
            directory = g_arguments[index] + 11;
        }
    }

    if (directory != NULL && (directory[0] == '\0' || chdir(directory) != 0))
    {
        fprintf(stderr, "th08-modern: unable to enter data directory: %s\n", directory);
        return false;
    }

    struct stat info;
    if (stat("th08.dat", &info) != 0 || !S_ISREG(info.st_mode))
    {
        fprintf(stderr, "th08-modern: selected directory does not contain th08.dat\n");
        return false;
    }
    unlink("modern-files.txt");
    unlink("modern-crash.txt");
    unlink("modern-render.txt");
    unlink("modern-enemy-render.csv");
    return true;
#endif
}

void InstallCrashReporter()
{
    InitializeTargetData();
#if !defined(PSP)
    InstallSignalHandler(SIGSEGV);
    InstallSignalHandler(SIGABRT);
    InstallSignalHandler(SIGFPE);
    InstallSignalHandler(SIGILL);
    InstallSignalHandler(SIGBUS);
#endif
}

void LogArchiveRequest(const char *path)
{
#if defined(PSP)
    char logPath[640];
    const char *gameDirectory = th08::psp::GameDirectory();
    if (gameDirectory == NULL ||
        snprintf(logPath, sizeof(logPath), "%s/%s", gameDirectory,
                 "modern-files.txt") < 0)
        return;
#if defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
    th08::psp::IoActivityScope ioActivity(
        th08::psp::IoActivityKind::Write, logPath);
#endif
    FILE *file = fopen(logPath, "ab");
#else
    FILE *file = fopen("modern-files.txt", "ab");
#endif
    if (file == NULL)
        return;
    fprintf(file, "thread=%08lx path=%s\n", (unsigned long)GetCurrentThreadId(), path != NULL ? path : "<null>");
    fclose(file);
}

void SetArguments(int argc, char **argv)
{
    g_argumentCount = argc;
    g_arguments = argv;
}
} // namespace modern
} // namespace th08

#if defined(PSP)
extern "C" int th08_psp_run_engine(int argc, char **argv)
{
    TH08_PSP_BOOT_CHECKPOINT("runtime", "before_set_arguments", 0);
    th08::modern::SetArguments(argc, argv);
    TH08_PSP_BOOT_CHECKPOINT("runtime", "after_set_arguments", 0);
    TH08_PSP_BOOT_CHECKPOINT("runtime", "before_winmain", 0);
    const int result = WinMain(NULL, NULL, NULL, 0);
    TH08_PSP_BOOT_CHECKPOINT("runtime", "after_winmain", result);
    return result;
}
#else
int main(int argc, char **argv)
{
    th08::modern::SetArguments(argc, argv);
    return WinMain(NULL, NULL, NULL, 0);
}
#endif
