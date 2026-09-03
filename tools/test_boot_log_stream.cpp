#include "pspiofilemgr.h"
#include "pspthreadman.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::string gFile;
std::size_t gPosition = 0;
unsigned int gMaxWrite = 0xffffffffU;
unsigned int gFailWrites = 0;
unsigned int gWriteCalls = 0;
unsigned int gFailWriteCall = 0;
unsigned int gSyncCalls = 0;
unsigned int gFailSyncCallA = 0;
unsigned int gFailSyncCallB = 0;
std::mutex gKernelMutex;
std::atomic<unsigned int> gKernelLockCalls{0};
}

extern "C" int sceKernelCreateLwMutex(SceLwMutexWorkarea *, const char *,
                                       SceUInt32, int, u32 *)
{
    return 0;
}

extern "C" int sceKernelLockLwMutex(SceLwMutexWorkarea *, int, unsigned int *)
{
    gKernelMutex.lock();
    ++gKernelLockCalls;
    return 0;
}

extern "C" int sceKernelUnlockLwMutex(SceLwMutexWorkarea *, int)
{
    gKernelMutex.unlock();
    return 0;
}

extern "C" int sceIoGetstat(const char *, SceIoStat *)
{
    return -1;
}

extern "C" SceUID sceIoOpen(const char *, int flags, int)
{
    if ((flags & PSP_O_TRUNC) != 0)
    {
        gFile.clear();
    }
    gPosition = (flags & PSP_O_APPEND) != 0 ? gFile.size() : 0;
    return 1;
}

extern "C" int sceIoRead(SceUID, void *, unsigned int)
{
    return -1;
}

extern "C" int sceIoWrite(SceUID, const void *data, unsigned int bytes)
{
    ++gWriteCalls;
    if (gFailWriteCall == gWriteCalls || gFailWrites != 0)
    {
        if (gFailWrites != 0)
        {
            --gFailWrites;
        }
        return -1;
    }
    const unsigned int written = std::min(bytes, gMaxWrite);
    if (gPosition + written > gFile.size())
    {
        gFile.resize(gPosition + written);
    }
    std::memcpy(&gFile[gPosition], data, written);
    gPosition += written;
    return static_cast<int>(written);
}

extern "C" int sceIoClose(SceUID)
{
    return 0;
}

extern "C" int sceIoChdir(const char *)
{
    return 0;
}

extern "C" int sceIoMkdir(const char *, int)
{
    return 0;
}

extern "C" SceUID sceIoDopen(const char *)
{
    return -1;
}

extern "C" int sceIoDread(SceUID, SceIoDirent *)
{
    return 0;
}

extern "C" int sceIoDclose(SceUID)
{
    return 0;
}

extern "C" int sceIoSync(const char *, unsigned int)
{
    ++gSyncCalls;
    if (gFailSyncCallA == gSyncCalls || gFailSyncCallB == gSyncCalls)
    {
        return -1;
    }
    return 0;
}

// Exercise the production implementation against the deterministic mock I/O
// above. Including the implementation also covers its private stream state.
#include "../psp/fileio.cpp"

namespace
{
bool Check(bool condition, const char *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "boot-log stream test failed: %s\n", message);
    }
    return condition;
}

std::size_t CountOccurrences(const std::string &haystack, const char *needle)
{
    std::size_t count = 0;
    std::size_t offset = 0;
    const std::size_t needleBytes = std::strlen(needle);
    while ((offset = haystack.find(needle, offset)) != std::string::npos)
    {
        ++count;
        offset += needleBytes;
    }
    return count;
}
} // namespace

int main()
{
    th08::psp::FileIoInitialize("ms0:/PSP/GAME/TH08PSP/EBOOT.PBP");
    gMaxWrite = 37;

    for (unsigned int sequence = 0; sequence < 600; ++sequence)
    {
        th08::psp::BootLog("ORDER seq=%04u payload=abcdefghijklmnopqrstuvwxyz0123456789\n",
                           sequence);
    }
    if (!Check(th08::psp::FlushBootLog(), "initial chunk flush") ||
        !Check(gFile.size() > 16383U, "stream grows beyond the legacy 16 KiB cap"))
    {
        return 1;
    }

    std::size_t previous = 0;
    for (unsigned int sequence = 0; sequence < 600; ++sequence)
    {
        char needle[32];
        std::snprintf(needle, sizeof(needle), "ORDER seq=%04u", sequence);
        const std::size_t found = gFile.find(needle, previous);
        if (!Check(found != std::string::npos && found >= previous,
                   "records retain their call order"))
        {
            return 1;
        }
        previous = found;
    }

    constexpr unsigned int kWriterCount = 6;
    constexpr unsigned int kRecordsPerWriter = 300;
    std::vector<std::thread> writers;
    for (unsigned int writer = 0; writer < kWriterCount; ++writer)
    {
        writers.emplace_back([writer]() {
            for (unsigned int sequence = 0; sequence < kRecordsPerWriter; ++sequence)
            {
                th08::psp::BootLog("THREAD writer=%u seq=%04u payload=serialized\n",
                                   writer, sequence);
            }
        });
    }
    for (std::thread &writer : writers)
    {
        writer.join();
    }
    if (!Check(th08::psp::FlushBootLog(), "concurrent writer flush") ||
        !Check(gKernelLockCalls.load() >= kWriterCount * kRecordsPerWriter,
               "every concurrent writer enters the kernel lock"))
    {
        return 1;
    }
    for (unsigned int writer = 0; writer < kWriterCount; ++writer)
    {
        for (unsigned int sequence = 0; sequence < kRecordsPerWriter; ++sequence)
        {
            char needle[64];
            std::snprintf(needle, sizeof(needle),
                          "THREAD writer=%u seq=%04u payload=serialized\n",
                          writer, sequence);
            if (!Check(CountOccurrences(gFile, needle) == 1,
                       "concurrent records remain complete and unique"))
            {
                return 1;
            }
        }
    }

    th08::psp::BootLog("RECOVERY witness=before_injected_failure\n");
    gFailWrites = 1;
    if (!Check(!th08::psp::FlushBootLog(), "injected write failure is reported") ||
        !Check(th08::psp::FlushBootLog(), "unwritten suffix is retryable") ||
        !Check(gFile.find("RECOVERY witness=before_injected_failure\n") != std::string::npos,
               "record survives retry") ||
        !Check(gFile.find("BOOT_LOG WRITE_FAILURE_RECOVERED") != std::string::npos,
               "recovered write failure is explicit"))
    {
        return 1;
    }

    const std::string payload(3900, 'X');
    for (unsigned int sequence = 0; sequence < 300; ++sequence)
    {
        th08::psp::BootLog("CAP seq=%04u %s\n", sequence, payload.c_str());
    }
    if (!Check(th08::psp::FlushBootLog(), "pre-final content flush"))
    {
        return 1;
    }

    const unsigned int syncCallsBeforeFinal = gSyncCalls;
    const std::size_t bytesBeforeFinal = gFile.size();
    gFailWriteCall = gWriteCalls + 3;
    gFailSyncCallA = syncCallsBeforeFinal + 1;
    gFailSyncCallB = syncCallsBeforeFinal + 2;
    if (!Check(!th08::psp::FinalizeBootLog(),
               "FINAL write failure leaves a retryable queued suffix") ||
        !Check(gFile.size() > bytesBeforeFinal,
               "FINAL failure occurs after a real partial write"))
    {
        return 1;
    }
    const std::size_t partialFinal = gFile.find("BOOT_LOG FINAL", bytesBeforeFinal);
    if (!Check(partialFinal != std::string::npos &&
                   gFile.find('\n', partialFinal) == std::string::npos,
               "partial FINAL is not mistaken for a complete witness"))
    {
        return 1;
    }

    th08::psp::BootLog("MUST_NOT_APPEAR_AFTER_FINAL_REQUEST\n");
    if (!Check(!th08::psp::FinalizeBootLog(),
               "post-FINAL sync failure remains retryable") ||
        !Check(CountOccurrences(gFile, "BOOT_LOG FINAL") == 1,
               "FINAL is queued and written exactly once") ||
        !Check(gFile.find("witness=orderly_exit durability=not_asserted") !=
                   std::string::npos,
               "FINAL makes only the documented orderly witness") ||
        !Check(gFile.find("pre_final_sync_result=-1 post_final_sync=pending") !=
                   std::string::npos,
               "FINAL records the failed pre-sync result honestly") ||
        !Check(gFile.find("sync_failures_before_final=1") != std::string::npos,
               "FINAL counters are explicitly scoped to pre-FINAL state"))
    {
        return 1;
    }

    const std::size_t sizeAfterFinalWrite = gFile.size();
    if (!Check(th08::psp::FinalizeBootLog(),
               "sync-only retry completes finalization") ||
        !Check(gFile.size() == sizeAfterFinalWrite,
               "sync retry performs no duplicate file write") ||
        !Check(gFile.size() <= 1024U * 1024U, "file remains within its 1 MiB bound") ||
        !Check(gFile.find("BOOT_LOG TRUNCATED limit=1048576") != std::string::npos,
               "file limit emits a truncation marker") ||
        !Check(gFile.find("BOOT_LOG FINAL orderly=1 witness=orderly_exit ") !=
                   std::string::npos &&
                   gFile.find("content_status=TRUNCATED") != std::string::npos,
               "orderly terminal marker records truncation") ||
        !Check(!gFile.empty() && gFile.back() == '\n', "terminal marker is complete") ||
        !Check(gSyncCalls == syncCallsBeforeFinal + 3,
               "pre-sync, failed post-sync, and sync-only retry are distinct"))
    {
        return 1;
    }

    const std::size_t finalizedSize = gFile.size();
    th08::psp::BootLog("MUST_NOT_APPEAR\n");
    if (!Check(th08::psp::FlushBootLog(), "finalization result is idempotent") ||
        !Check(gFile.size() == finalizedSize &&
                   gFile.find("MUST_NOT_APPEAR") == std::string::npos &&
                   gFile.find("MUST_NOT_APPEAR_AFTER_FINAL_REQUEST") == std::string::npos,
               "records after FINAL are rejected"))
    {
        return 1;
    }

    std::printf("boot-log stream test passed: bytes=%lu short_write_max=%u\n",
                static_cast<unsigned long>(gFile.size()), gMaxWrite);
    return 0;
}
