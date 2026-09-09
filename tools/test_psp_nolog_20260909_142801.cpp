// Actual fileio.cpp with deterministic I/O: compile with LOGGING=0 and =1.
#include "pspiofilemgr.h"
#include "pspthreadman.h"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Handle { std::string path; std::size_t position; };
std::map<std::string, std::string> files;
std::map<int, Handle> handles;
std::mutex logMutex;
int nextFd = 1;
unsigned opens, writes, syncs, rotations, removals, locks, creates, stats, reads;
unsigned mkdirs, chdirs;
unsigned maxWrite = 37;
bool failWrite;
std::string gameRoot;
}
extern "C" int sceKernelCreateLwMutex(SceLwMutexWorkarea *, const char *, SceUInt32, int, u32 *)
{ ++creates; return 0; }
extern "C" int sceKernelLockLwMutex(SceLwMutexWorkarea *, int, unsigned int *)
{ logMutex.lock(); ++locks; return 0; }
extern "C" int sceKernelUnlockLwMutex(SceLwMutexWorkarea *, int)
{ logMutex.unlock(); return 0; }
extern "C" int sceIoGetstat(const char *path, SceIoStat *st)
{
    ++stats;
    const std::string p(path);
    std::memset(st, 0, sizeof(*st));
    if (p == gameRoot + "/th8/th08.dat") { st->st_size = 46838025; return 0; }
    if (p == gameRoot + "/th8/thbgm.dat") { st->st_size = 449961024; return 0; }
    return -1;
}
extern "C" SceUID sceIoOpen(const char *path, int flags, int)
{
    ++opens;
    if (flags & PSP_O_TRUNC) files[path].clear();
    const int fd = nextFd++;
    handles[fd] = {path, (flags & PSP_O_APPEND) ? files[path].size() : 0};
    return fd;
}
extern "C" int sceIoRead(SceUID fd, void *data, unsigned int bytes)
{
    ++reads;
    auto &h = handles.at(fd);
    const auto &s = files.at(h.path);
    const auto n = std::min<std::size_t>(bytes, s.size() - h.position);
    std::memcpy(data, s.data() + h.position, n); h.position += n;
    return static_cast<int>(n);
}
extern "C" int sceIoWrite(SceUID fd, const void *data, unsigned int bytes)
{
    ++writes;
    if (failWrite) return -1;
    auto &h = handles.at(fd);
    auto &s = files[h.path];
    const unsigned n = std::min(bytes, maxWrite);
    s.resize(std::max(s.size(), h.position + n));
    std::memcpy(&s[h.position], data, n); h.position += n;
    return static_cast<int>(n);
}
extern "C" int sceIoClose(SceUID fd) { handles.erase(fd); return 0; }
extern "C" int sceIoChdir(const char *) { ++chdirs; return 0; }
extern "C" int sceIoMkdir(const char *, int) { ++mkdirs; return 0; }
extern "C" SceUID sceIoDopen(const char *) { return -1; }
extern "C" int sceIoDread(SceUID, SceIoDirent *) { return 0; }
extern "C" int sceIoDclose(SceUID) { return 0; }
extern "C" int sceIoSync(const char *, unsigned int) { ++syncs; return 0; }
extern "C" int sceIoRemove(const char *p) { ++removals; files.erase(p); return 0; }
extern "C" int sceIoRename(const char *a, const char *b)
{
    ++rotations;
    auto i = files.find(a);
    if (i == files.end()) return -1;
    files[b] = i->second; files.erase(i); return 0;
}
#include "../psp/fileio.cpp"

int main(int argc, char **argv)
{
    using namespace th08::psp;
    assert(argc == 2);
    gameRoot = std::string(argv[1]) + "/PSP/GAME/TH08PSP";
    const std::string log = gameRoot + "/TH08PSP_BOOT.LOG";
    files[log] = "old crash evidence";
    files[gameRoot + "/TH08PSP_BOOT.PREV.LOG"] = "older evidence";
    files[gameRoot + "/th8/th08.dat"] = "PBGZ";
    files[gameRoot + "/th8/thbgm.dat"] = "ZWAV";
    const auto originals = files;
    int formatted = -1;
    BootLog(nullptr);  // valid even before initialization
    assert(FlushBootLogHard() == !TH08_PSP_LOGGING);
    FileIoInitialize((gameRoot + "/EBOOT.PBP").c_str());
    FileIoInitialize("ef0:/must/not/replace/EBOOT.PBP");
    assert(gameRoot == GameDirectory());
    assert(log == BootLogPath());
    assert(chdirs == 1 && mkdirs == 2);
    BootLog("x%n", &formatted);
    assert(formatted == (TH08_PSP_LOGGING ? 1 : -1));
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([t] {
        for (int n = 0; n < 6000; ++n)
            BootLog("THREAD t=%d n=%d payload=01234567890123456789\n", t, n);
    });
    for (auto &t : threads) t.join();
    assert(FlushBootLog());
    assert(FlushBootLogHard());
    for (int n = 0; n < 20; ++n) assert(FinalizeBootLog());
    BootLog("after final\n");
#if TH08_PSP_LOGGING
    assert(opens && writes && syncs && rotations && removals && locks && creates);
    assert(files[log].find("BOOT_LOG FINAL orderly=1") != std::string::npos);
    assert(files[log].find("after final") == std::string::npos);
    assert(files[gameRoot + "/TH08PSP_BOOT.PREV.LOG"] == "old crash evidence");
#else
    assert(opens == 0 && writes == 0 && syncs == 0 && rotations == 0 && removals == 0);
    assert(locks == 0 && creates == 0 && stats == 0 && reads == 0);
    assert(files == originals);
    errno = EDOM;
    BootLog("preserve errno %d", 42); FlushBootLogHard(); FinalizeBootLog();
    assert(errno == EDOM);
#endif
    // Real archive discovery and ordinary save writes are not logging.
    const auto &data = DiscoverOriginalData();
    assert(data.ready && std::string(data.root) == gameRoot + "/th8");
    assert(reads == 2);
    const std::string save(1000, 's');
    assert(WriteFileExact((gameRoot + "/score.dat").c_str(), save.data(), save.size()));
    assert(files[gameRoot + "/score.dat"] == save);
    assert(handles.empty());
    assert(!WriteFileExact(nullptr, save.data(), save.size()));
    assert(!WriteFileExact("invalid", nullptr, 1));
    failWrite = true;
    assert(!WriteFileExact((gameRoot + "/replay/test.rpy").c_str(), save.data(), save.size()));
    assert(handles.empty());
    std::printf("PASS logging=%d device=%s: startup/24000 records/flush/finalize/rotation/errno/discovery/save/short-write/error\n", TH08_PSP_LOGGING, argv[1]);
}
