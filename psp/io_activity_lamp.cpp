#include "io_activity_lamp.hpp"

#include "fileio.hpp"
#include "io_serialize.hpp"

#include <kubridge.h>
#include <pspkernel.h>

#include <cstdint>
#include <cstring>

namespace th08::psp
{
namespace
{

constexpr int kPspGoModel = 4;
constexpr std::uint32_t kActivityHoldUs = 200U * 1000U;
constexpr std::uint32_t kSlowThresholdUs = 100U * 1000U;
constexpr std::uint32_t kSlowHoldUs = 3U * 1000U * 1000U;

std::uint32_t gEnabled = 0U;
std::uint32_t gActiveDepth = 0U;
std::uint32_t gActivityEver = 0U;
std::uint32_t gSlowEver = 0U;
std::uint32_t gLastActivityUs = 0U;
std::uint32_t gLastSlowUs = 0U;
std::uint32_t gSlowSerial = 0U;
#if TH08_PSP_IO_SERIALIZE_ENABLED
SceLwMutexWorkarea gIoMutex;
std::uint32_t gSerializeReady = 0U;
#endif

std::uint32_t TimeLowUs()
{
    return static_cast<std::uint32_t>(sceKernelGetSystemTimeLow());
}

const char *KindName(IoActivityKind kind)
{
    switch (kind)
    {
    case IoActivityKind::Open: return "open";
    case IoActivityKind::Read: return "read";
    case IoActivityKind::Write: return "write";
    case IoActivityKind::Seek: return "seek";
    case IoActivityKind::Metadata: return "metadata";
    case IoActivityKind::Sync: return "sync";
    case IoActivityKind::Close: return "close";
    case IoActivityKind::Directory: return "directory";
    }
    return "unknown";
}
#if TH08_PSP_IO_SERIALIZE_ENABLED
// Recent-operation ring: start (S) and end (E) records, dumped when an
// operation crosses the slow threshold so the sequence around a stall is
// visible.  Written under the device lock on the ef0 model.
constexpr unsigned kTraceEntries = 32U;
struct IoTraceEntry
{
    std::uint32_t timeUs;
    std::uint32_t durationUs;
    std::uint32_t thread;
    const void *address;
    char path[24];
    char threadName[16];
    std::uint8_t kind;
    std::uint8_t isEnd;
};
IoTraceEntry gTrace[kTraceEntries];
std::uint32_t gTraceNext = 0U;
void TracePush(IoActivityKind kind, bool isEnd, std::uint32_t durationUs,
               const char *detail, const void *address)
{
    const std::uint32_t slot = __atomic_fetch_add(&gTraceNext, 1U, __ATOMIC_RELAXED) % kTraceEntries;
    IoTraceEntry &e = gTrace[slot];
    e.timeUs = TimeLowUs();
    e.durationUs = durationUs;
    e.thread = static_cast<std::uint32_t>(sceKernelGetThreadId());
    e.address = address;
    e.kind = static_cast<std::uint8_t>(kind);
    e.isEnd = isEnd ? 1U : 0U;
    e.path[0] = 0;
    if (detail != nullptr)
    {
        const std::size_t length = std::strlen(detail);
        const std::size_t keep = length < sizeof(e.path) - 1 ? length : sizeof(e.path) - 1;
        std::memcpy(e.path, detail + (length - keep), keep);
        e.path[keep] = 0;
    }
    SceKernelThreadInfo info;
    std::memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    e.threadName[0] = 0;
    if (sceKernelReferThreadStatus(0, &info) >= 0)
    {
        std::strncpy(e.threadName, info.name, sizeof(e.threadName) - 1);
        e.threadName[sizeof(e.threadName) - 1] = 0;
    }
}
void TraceDump(std::uint32_t serial)
{
    const std::uint32_t now = TimeLowUs();
    const std::uint32_t next = __atomic_load_n(&gTraceNext, __ATOMIC_RELAXED);
    const std::uint32_t count = next < kTraceEntries ? next : kTraceEntries;
    BootLog("IO_TRACE serial=%lu entries=%lu (age_ms kind S/E dur_ms thread name addr path)\n",
            static_cast<unsigned long>(serial), static_cast<unsigned long>(count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const IoTraceEntry &e = gTrace[(next - count + i) % kTraceEntries];
        BootLog("IO_TRACE  %6lu %-9s %c %6lu 0x%08x %-15s %p %s\n",
                static_cast<unsigned long>((now - e.timeUs) / 1000U),
                KindName(static_cast<IoActivityKind>(e.kind)), e.isEnd ? 'E' : 'S',
                static_cast<unsigned long>(e.durationUs / 1000U),
                static_cast<unsigned int>(e.thread), e.threadName, e.address, e.path);
    }
}
#endif

} // namespace

void IoActivityLampInitialize()
{
    const int model = kuKernelGetModel();
    const std::uint32_t enabled = model == kPspGoModel ? 1U : 0U;
#if TH08_PSP_IO_SERIALIZE_ENABLED
    // One recursive mutex for every storage transaction on the ef0 model.
    const bool serialize =
        enabled != 0U &&
        sceKernelCreateLwMutex(&gIoMutex, "TH08IoSerial",
                               PSP_LW_MUTEX_ATTR_RECURSIVE, 0, nullptr) >= 0;
    __atomic_store_n(&gSerializeReady, serialize ? 1U : 0U, __ATOMIC_RELEASE);
    BootLog("IO_SERIALIZE build=1 model=%d enabled=%d\n", model, serialize ? 1 : 0);
#endif
    __atomic_store_n(&gActiveDepth, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&gActivityEver, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&gSlowEver, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&gLastActivityUs, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&gLastSlowUs, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&gSlowSerial, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&gEnabled, enabled, __ATOMIC_RELEASE);

    BootLog(
        "IO_LAMP build=1 model=%d enabled=%lu activity_hold_us=%lu "
        "slow_threshold_us=%lu slow_hold_us=%lu colors=amber_red "
        "main_thread_stall_visibility=post_return\n",
        model, static_cast<unsigned long>(enabled),
        static_cast<unsigned long>(kActivityHoldUs),
        static_cast<unsigned long>(kSlowThresholdUs),
        static_cast<unsigned long>(kSlowHoldUs));
}

bool IoSerializeEnabled()
{
#if TH08_PSP_IO_SERIALIZE_ENABLED
    return __atomic_load_n(&gSerializeReady, __ATOMIC_ACQUIRE) != 0U;
#else
    return false;
#endif
}

bool IoActivityLampEnabled()
{
    return __atomic_load_n(&gEnabled, __ATOMIC_ACQUIRE) != 0U;
}

IoActivityLampState IoActivityLampQuery()
{
    if (!IoActivityLampEnabled())
        return IoActivityLampState::Off;

    const std::uint32_t now = TimeLowUs();
    if (__atomic_load_n(&gSlowEver, __ATOMIC_ACQUIRE) != 0U)
    {
        const std::uint32_t lastSlow =
            __atomic_load_n(&gLastSlowUs, __ATOMIC_RELAXED);
        if (now - lastSlow < kSlowHoldUs)
            return IoActivityLampState::Slow;
    }

    if (__atomic_load_n(&gActiveDepth, __ATOMIC_ACQUIRE) != 0U)
        return IoActivityLampState::Activity;

    if (__atomic_load_n(&gActivityEver, __ATOMIC_ACQUIRE) != 0U)
    {
        const std::uint32_t lastActivity =
            __atomic_load_n(&gLastActivityUs, __ATOMIC_RELAXED);
        if (now - lastActivity < kActivityHoldUs)
            return IoActivityLampState::Activity;
    }
    return IoActivityLampState::Off;
}

IoActivityScope::IoActivityScope(IoActivityKind kind, const char *detail,
                                 bool recordSlowLog, const void *address)
    : kind_(kind), detail_(detail), startedUs_(0U), armed_(false),
      recordSlowLog_(recordSlowLog), locked_(false), address_(address)
{
#if TH08_PSP_IO_SERIALIZE_ENABLED
    if (__atomic_load_n(&gSerializeReady, __ATOMIC_ACQUIRE) != 0U &&
        sceKernelLockLwMutex(&gIoMutex, 1, nullptr) >= 0)
        locked_ = true;
#endif
#if TH08_PSP_IO_SERIALIZE_ENABLED
    if (locked_)
        TracePush(kind_, false, 0U, detail_, address_);
#endif
    if (!IoActivityLampEnabled())
        return;

    startedUs_ = TimeLowUs();
    __atomic_store_n(&gLastActivityUs, startedUs_, __ATOMIC_RELAXED);
    __atomic_store_n(&gActivityEver, 1U, __ATOMIC_RELEASE);
    __atomic_add_fetch(&gActiveDepth, 1U, __ATOMIC_ACQ_REL);
    armed_ = true;
}

IoActivityScope::~IoActivityScope()
{
    if (!armed_)
    {
#if TH08_PSP_IO_SERIALIZE_ENABLED
        if (locked_)
            sceKernelUnlockLwMutex(&gIoMutex, 1);
#endif
        return;
    }

    const std::uint32_t finishedUs = TimeLowUs();
    const std::uint32_t durationUs = finishedUs - startedUs_;
    __atomic_store_n(&gLastActivityUs, finishedUs, __ATOMIC_RELAXED);
    __atomic_store_n(&gActivityEver, 1U, __ATOMIC_RELEASE);
    const std::uint32_t previousDepth =
        __atomic_fetch_sub(&gActiveDepth, 1U, __ATOMIC_ACQ_REL);
    if (previousDepth == 0U)
        __atomic_store_n(&gActiveDepth, 0U, __ATOMIC_RELEASE);

#if TH08_PSP_IO_SERIALIZE_ENABLED
    if (locked_)
        TracePush(kind_, true, durationUs, detail_, address_);
    const bool dumpTrace = locked_ && durationUs >= kSlowThresholdUs && recordSlowLog_;
    if (locked_)
    {
        locked_ = false;
        sceKernelUnlockLwMutex(&gIoMutex, 1);
    }
#endif
    if (durationUs < kSlowThresholdUs)
        return;

    __atomic_store_n(&gLastSlowUs, finishedUs, __ATOMIC_RELAXED);
    __atomic_store_n(&gSlowEver, 1U, __ATOMIC_RELEASE);
    const std::uint32_t serial =
        __atomic_add_fetch(&gSlowSerial, 1U, __ATOMIC_RELAXED);
    if (!recordSlowLog_)
        return;
    // Do not flush here.  A synchronous diagnostic write on the same ef0
    // path would perturb the event being diagnosed.  The existing buffered
    // BOOT.LOG stream persists this at its normal checkpoint/final cadence.
    BootLog("IO_LAMP SLOW serial=%lu op=%s duration_us=%lu thread=0x%08x addr=%p detail=%s\n",
            static_cast<unsigned long>(serial), KindName(kind_),
            static_cast<unsigned long>(durationUs),
            static_cast<unsigned int>(sceKernelGetThreadId()), address_,
            detail_ != nullptr ? detail_ : "<handle>");
#if TH08_PSP_IO_SERIALIZE_ENABLED
    if (dumpTrace)
        TraceDump(serial);
#endif
}

} // namespace th08::psp
