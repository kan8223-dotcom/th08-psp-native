#include "io_activity_lamp.hpp"

#include "fileio.hpp"

#include <kubridge.h>
#include <pspkernel.h>

#include <cstdint>

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

} // namespace

void IoActivityLampInitialize()
{
    const int model = kuKernelGetModel();
    const std::uint32_t enabled = model == kPspGoModel ? 1U : 0U;
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
                                 bool recordSlowLog)
    : kind_(kind), detail_(detail), startedUs_(0U), armed_(false),
      recordSlowLog_(recordSlowLog)
{
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
        return;

    const std::uint32_t finishedUs = TimeLowUs();
    const std::uint32_t durationUs = finishedUs - startedUs_;
    __atomic_store_n(&gLastActivityUs, finishedUs, __ATOMIC_RELAXED);
    __atomic_store_n(&gActivityEver, 1U, __ATOMIC_RELEASE);
    const std::uint32_t previousDepth =
        __atomic_fetch_sub(&gActiveDepth, 1U, __ATOMIC_ACQ_REL);
    if (previousDepth == 0U)
        __atomic_store_n(&gActiveDepth, 0U, __ATOMIC_RELEASE);

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
    BootLog("IO_LAMP SLOW serial=%lu op=%s duration_us=%lu detail=%s\n",
            static_cast<unsigned long>(serial), KindName(kind_),
            static_cast<unsigned long>(durationUs),
            detail_ != nullptr ? detail_ : "<handle>");
}

} // namespace th08::psp
