#include "wait_probe.hpp"
#if TH08_PSP_WAIT_PROBE_ENABLED
#include <atomic>
#include <pspkernel.h>
#include <pspge.h>

extern "C" int __real_sceGeDrawSync(int syncType);

namespace th08::psp
{
namespace
{
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "wait probe must not lock");
static_assert(std::atomic<int>::is_always_lock_free, "wait probe must not lock");
std::atomic<int> gProbeMain{-1}, gProbeQid{-1}, gProbeResult{0};
std::atomic<std::uint32_t> gProbeSequence{0}, gProbeActive{0}, gProbeApi{0};
std::atomic<std::uint32_t> gProbeCaller{0}, gProbeStarted{0}, gProbePhase{0xffffffffU};
std::atomic<std::uint32_t> gObservedContext{0xffffffffU}, gProbeContext{0xffffffffU};

void BeginWrite()
{
    gProbeSequence.fetch_add(1U, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
}
void EndWrite() { gProbeSequence.fetch_add(1U, std::memory_order_release); }
}

void WaitProbeInitialize(int mainThread)
{
    // Called before any callback/worker starts; static data already empty.
    gProbeMain.store(mainThread, std::memory_order_release);
}

void WaitProbeObserve(std::int32_t stage, std::uint32_t frame)
{
    // One atomic word keeps stage/frame coherent without a 64-bit lock.
    const std::uint32_t packed = stage >= 0 && stage < 255 && frame < 0x00ffffffU
        ? (static_cast<std::uint32_t>(stage) << 24) | frame : 0xffffffffU;
    gObservedContext.store(packed, std::memory_order_relaxed);
}

bool WaitProbeBegin(WaitProbeApi api, int qid, int mode, std::uintptr_t caller,
                    std::uint32_t phase)
{
    // Peeks and other threads must not overwrite a pending main wait. A
    // nested same-thread wait retains its outer owner until that owner ends.
    if (mode != 0 || gProbeMain.load(std::memory_order_acquire) < 0 ||
        sceKernelGetThreadId() != gProbeMain.load(std::memory_order_relaxed) ||
        gProbeActive.load(std::memory_order_relaxed) != 0U)
        return false;
    const std::uint32_t now = sceKernelGetSystemTimeLow();
    BeginWrite();
    gProbeApi.store(static_cast<std::uint32_t>(api), std::memory_order_relaxed);
    gProbeQid.store(qid, std::memory_order_relaxed);
    gProbeCaller.store(static_cast<std::uint32_t>(caller), std::memory_order_relaxed);
    gProbeStarted.store(now, std::memory_order_relaxed);
    gProbeContext.store(gObservedContext.load(std::memory_order_relaxed), std::memory_order_relaxed);
    gProbePhase.store(phase, std::memory_order_relaxed);
    gProbeResult.store(0, std::memory_order_relaxed); // Only valid when active=0.
    gProbeActive.store(1U, std::memory_order_relaxed);
    EndWrite();
    return true;
}

void WaitProbeEnd(bool tracked, int result)
{
    if (!tracked) return;
    BeginWrite();
    gProbeResult.store(result, std::memory_order_relaxed);
    gProbeActive.store(0U, std::memory_order_relaxed);
    EndWrite();
}

WaitProbeSnapshot WaitProbeRead()
{
    WaitProbeSnapshot result;
    const std::uint32_t before = gProbeSequence.load(std::memory_order_acquire);
    result.sequence = before;
    if (before & 1U) return result; // Callback may have interrupted the writer.
    result.active = gProbeActive.load(std::memory_order_relaxed);
    result.api = gProbeApi.load(std::memory_order_relaxed);
    result.caller = gProbeCaller.load(std::memory_order_relaxed);
    result.startedUs = gProbeStarted.load(std::memory_order_relaxed);
    result.observedContext = gProbeContext.load(std::memory_order_relaxed);
    result.phase = gProbePhase.load(std::memory_order_relaxed);
    result.mainThread = gProbeMain.load(std::memory_order_relaxed);
    result.qid = gProbeQid.load(std::memory_order_relaxed);
    result.result = gProbeResult.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    result.consistent = before == gProbeSequence.load(std::memory_order_relaxed);
    return result; // Exactly one bounded read, no retry loop.
}
}

extern "C" __attribute__((noinline)) int __wrap_sceGeDrawSync(int syncType)
{
    const bool tracked = th08::psp::WaitProbeBegin(th08::psp::WaitProbeApi::GeDrawSync,
        -1, syncType, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)), 0xffffffffU);
    const int result = __real_sceGeDrawSync(syncType);
    th08::psp::WaitProbeEnd(tracked, result);
    return result;
}
#endif
