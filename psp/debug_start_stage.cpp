#include "debug_start_stage.hpp"
#if TH08_PSP_DEBUG_START_STAGE_ENABLED
#include "fileio.hpp"

extern "C" unsigned long th08_archive_entry_size(const char *name);
#include <pspiofilemgr.h>
#include <pspctrl.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace th08::psp
{
namespace
{
int gAutoStartState = -1;      // -1 unknown, 0 off, 1 armed, 2 done
unsigned long gAutoStartCalls = 0;
bool DebugFileContains(const char *needle)
{
    char path[640];
    const char *directory = GameDirectory();
    if (directory == nullptr ||
        std::snprintf(path, sizeof(path), "%s/TH08PSP_DEBUG_STAGE.txt", directory) < 0)
        return false;
    const SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0)
        return false;
    char text[48];
    const int got = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    if (got <= 0)
        return false;
    text[got] = 0;
    return std::strstr(text, needle) != nullptr;
}
} // namespace

unsigned int DebugAutoStartButtons()
{
    if (gAutoStartState == -1)
    {
        gAutoStartState = DebugFileContains("auto") ? 1 : 0;
        BootLog("DEBUG_AUTOSTART armed=%d\n", gAutoStartState);
    }
    if (gAutoStartState != 1)
        return 0U;
    ++gAutoStartCalls;
    if (gAutoStartCalls > 6000UL)
    {
        gAutoStartState = 2;
        return 0U;
    }
    // FillKeyboard runs about twice per frame: tap for ~4 frames every ~2 s.
    const unsigned long phase = gAutoStartCalls % 240UL;
    return (gAutoStartCalls > 400UL && phase < 8UL) ? static_cast<unsigned int>(PSP_CTRL_CROSS) : 0U;
}

static void LogArchiveEntrySizes()
{
    static const char *const kNames[] = {
        "stg1bg.anm", "stg2bg.anm", "stg3bg.anm", "stg4abg.anm", "stg5bg.anm", "stg6bg.anm", "stg7bg.anm", "stg8bg.anm",
        "stg1enm.anm", "stg2enm.anm", "stg3enm.anm", "stg4aenm.anm", "stg4benm.anm", "stg5enm.anm", "stg6enm.anm", "stg7enm.anm", "stg8enm.anm",
        "stgenm_sk.anm", "stgenm_ym.anm", "stgenm_al.anm", "stgenm_rm.anm", "stgenm_yy.anm", "stgenm_yk.anm",
        "eff01.anm", "eff02.anm", "eff03.anm", "eff04a.anm", "eff04b.anm", "eff05.anm", "eff06.anm", "eff07.anm", "eff08.anm",
        "eff09sk.anm", "eff09ym.anm", "eff09al.anm", "eff09rm.anm", "eff09yy.anm", "eff09yk.anm",
        "stg1txt.anm", "stg2txt.anm", "stg3txt.anm", "stg4atxt.anm", "stg4btxt.anm", "stg5txt.anm", "stg6txt.anm", "stg7txt.anm", "stg8txt.anm",
        "enemy.anm", "etama.anm", "front.anm", "title.anm", "ascii.anm", "text.anm", "loading.anm",
        "pl00.anm", "pl01.anm", "pl02.anm", "pl03.anm", "pl04.anm", "pl05.anm", "pl06.anm", "pl07.anm", "pl08.anm", "pl09.anm", "pl10.anm", "pl11.anm",
        "face_rm00.anm", "face_mr00.anm", "face_sk00.anm", "face_yk00.anm", "face_st01.anm", "face_st02.anm", "face_st03.anm", "face_st04a.anm", "face_st04b.anm", "face_st05.anm", "face_st06a.anm", "face_st06b.anm",
        "ecldata1.ecl", "ecldata2.ecl", "ecldata3.ecl", "ecldata4a.ecl", "ecldata4b.ecl", "ecldata5.ecl", "ecldata6.ecl", "ecldata7.ecl", "ecldata8.ecl",
        "stage1.std", "stage2.std", "stage3.std", "stage4a.std", "stage4b.std", "stage5.std", "stage6.std", "stage7.std", "stage8.std",
        "msg1.dat", "msg2.dat", "msg3.dat", "msg4a.dat", "msg4b.dat", "msg5.dat", "msg6a.dat", "msg6b.dat", "msg7.dat",
    };
    unsigned long total = 0;
    for (const char *name : kNames)
    {
        const unsigned long bytes = th08_archive_entry_size(name);
        total += bytes;
        BootLog("ARCHIVE_ENTRY_SIZE name=%s bytes=%lu\n", name, bytes);
    }
    BootLog("ARCHIVE_ENTRY_SIZE total_listed=%lu\n", total);
}

int DebugStartStageOverride()
{
    static bool listed = false;
    if (!listed)
    {
        listed = true;
        LogArchiveEntrySizes();
    }
    char path[640];
    const char *directory = GameDirectory();
    if (directory == nullptr ||
        std::snprintf(path, sizeof(path), "%s/TH08PSP_DEBUG_STAGE.txt", directory) < 0)
        return -1;
    const SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0)
        return -1;
    char text[48];
    const int got = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    if (got <= 0)
        return -1;
    text[got] = 0;
    int stage = -1;
    if (std::strncmp(text, "4a", 2) == 0) stage = 3;
    else if (std::strncmp(text, "4b", 2) == 0) stage = 4;
    else if (std::strncmp(text, "6a", 2) == 0) stage = 6;
    else if (std::strncmp(text, "6b", 2) == 0) stage = 7;
    else if (text[0] >= '1' && text[0] <= '3') stage = text[0] - '1';
    else if (text[0] == '5') stage = 5;
    // Optional fragmentation hog: "4b hog=900000" allocates that many bytes in
    // 24 KiB pieces and frees every other piece, mimicking a played-out heap.
    const char *hog = std::strstr(text, "hog=");
    if (hog != nullptr)
    {
        const unsigned long want = std::strtoul(hog + 4, nullptr, 10);
        unsigned long got = 0; unsigned long freed = 0; void *pieces[256]; int n = 0;
        while (got < want && n < 256)
        {
            pieces[n] = std::malloc(24 * 1024);
            if (pieces[n] == nullptr) break;
            got += 24 * 1024; ++n;
        }
        for (int i = 1; i < n; i += 2) { std::free(pieces[i]); freed += 24 * 1024; }
        BootLog("DEBUG_HOG requested=%lu allocated=%lu freed_alternating=%lu pieces=%d\n", want, got, freed, n);
    }
    BootLog("DEBUG_START_STAGE file=%s value=%.4s stage=%d\n", path, text, stage);
    gAutoStartState = 2;
    FlushBootLog();
    return stage;
}
} // namespace th08::psp
#endif
