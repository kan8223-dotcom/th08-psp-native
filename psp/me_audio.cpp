#include "me_audio.hpp"
#if TH08_PSP_ME_AUDIO_ENABLED
#include "fileio.hpp"
#include <atomic>
#include <cstring>
#include <kubridge.h>
#include <pspkernel.h>
#include <psppower.h>
// TH07's Main-RAM stack transport, not its rejected local-eDRAM audio store.
#define TH08_ME_MAIN_RAM_STACK 1
#define DISABLE_VME_MINIMAL_CONFIG 1
extern "C" {
#include "me-core.h"
alignas(64) unsigned char gTh07MeMainStackArea[TH07_ME_MAIN_STACK_AREA_BYTES];
}

namespace th08::psp
{
namespace
{
enum : unsigned { Booting = 0, Ready = 1, Stopped = 2, Fault = 3 };
struct alignas(64) Mailbox
{
    volatile unsigned command, state, stop, done, producer;
    unsigned padding[11];
};
Mailbox mailbox{};
MeAudioJob packet{};
std::atomic<unsigned> active{0}, ownsCore{0};
static_assert(std::atomic<unsigned>::is_always_lock_free, "exit callback must not lock");
bool attempted = false, powerLocked = false;
const char *feature = "DISABLED";
unsigned callbacks = 0, completed = 0, fallbacks = 0, timeouts = 0;
unsigned buildUs = 0, waitUs = 0, waitMaxUs = 0, scUs = 0, scCalls = 0, buildStart = 0;

volatile Mailbox *Box()
{ return reinterpret_cast<volatile Mailbox *>(reinterpret_cast<std::uintptr_t>(&mailbox) | 0x40000000U); }
unsigned Physical(const void *p)
{ return unsigned(reinterpret_cast<std::uintptr_t>(p)) & 0x1fffffffU; }
bool LowRam(const void *p, unsigned size)
{ const unsigned a = Physical(p); return a >= 0x08800000U && a < 0x0a000000U && size <= 0x0a000000U - a; }
unsigned Lines(unsigned bytes) { return (bytes + 63U) & ~63U; }
void Sync() { __asm__ volatile("sync" ::: "memory"); }
bool AudioSelfTest();
bool StackGuards(unsigned alias = 0x40000000U)
{
    const volatile unsigned char *p = reinterpret_cast<const volatile unsigned char *>(
        Physical(gTh07MeMainStackArea) | alias);
    for (unsigned i = 0; i < TH07_ME_MAIN_STACK_GUARD_BYTES; ++i)
        if (p[i] != 0x69 || p[TH07_ME_MAIN_STACK_GUARD_BYTES + TH07_ME_MAIN_STACK_BYTES + i] != 0x69)
            return false;
    return true;
}
}

// Runs only on ME. This service owns no renderer or live sound-buffer pointer.
extern "C" void meLibOnProcess(void)
{
    volatile Mailbox *box = Box();
    meLibDcacheWritebackInvalidateAll();
    meLibIcacheInvalidateAll();
    box->state = Ready;
    Sync();
    for (;;)
    {
        if (box->stop && !box->producer && !box->command)
        {
            meLibDcacheWritebackInvalidateAll();
            box->command = 0;
            box->done = 1;
            Sync();
            box->state = StackGuards() ? Stopped : Fault;
            Sync();
            for (;;) meLibHalt();
        }
        if (!box->command)
        {
            // Back off using local instructions, not thousands of RAM polls.
            for (unsigned i = 0; i < 256; ++i)
                __asm__ volatile("nop; nop; nop; nop;" ::: "memory");
            continue;
        }
        MeAudioJob *job = reinterpret_cast<MeAudioJob *>(Physical(&packet) | 0x80000000U);
        meLibDcacheInvalidateRange(unsigned(job), 64 + sizeof(job->voices));
        for (unsigned i = 0; i < job->count && i < MeAudioMaxVoices; ++i)
            meLibDcacheInvalidateRange(unsigned(job->pcm[i]),
                Lines(job->voices[i].snapshotFrames * job->voices[i].frameBytes));
        const unsigned control = job->fcsr;
        __asm__ volatile("ctc1 %0, $31; nop; nop" :: "r"(control) : "memory");
        MeAudioKernel(*job);
        if (!StackGuards(0x80000000U)) job->result = 0;
        meLibDcacheWritebackRange(unsigned(job->output), Lines(job->frames * 4));
        meLibDcacheWritebackRange(unsigned(job), 64 + sizeof(job->voices));
        Sync();
        box->command = 0;
        Sync();
        box->done = 1;
        Sync();
    }
}
extern "C" void meLibOnSleep(void) { MeAudioRequestStop(); }
extern "C" __attribute__((noinline, aligned(4))) void meLibOnWake(void) {}

int MeAudioInit()
{
    if (attempted) return active.load();
    attempted = true;
    SceIoStat stat{};
    if (sceIoGetstat("ms0:/PSP/SYSTEM/ppsspp.ini", &stat) >= 0)
    {
        feature = "AUDIO_SKIPPED_PPSSPP";
        BootLog("ME_AUDIO init=SKIPPED_PPSSPP storage=MAIN_RAM edram=0\n");
        return 0;
    }
    if (kuKernelGetModel() < 1 || !LowRam(&packet, sizeof(packet)) ||
        !LowRam(&mailbox, sizeof(mailbox)) ||
        !LowRam(gTh07MeMainStackArea, sizeof(gTh07MeMainStackArea)))
    {
        feature = "AUDIO_REFUSED_LAYOUT";
        BootLog("ME_AUDIO init=REFUSED_LAYOUT\n");
        return 0;
    }
    if (scePowerLock(0) != 0)
    { feature = "AUDIO_POWER_LOCK_FAILED"; return 0; }
    powerLocked = true;
    std::memset(gTh07MeMainStackArea, 0x69, sizeof(gTh07MeMainStackArea));
    sceKernelDcacheWritebackInvalidateAll();
    ownsCore.store(1, std::memory_order_release);
    if (Box()->stop)
    {
        ownsCore.store(0, std::memory_order_release);
        scePowerUnlock(0); powerLocked = false;
        return 0;
    }
    const int mapper = meLibDefaultInit();
    if (mapper < 0)
    {
        // The mapper fails before reset, but kinit may already have replaced
        // the firmware callback. Restore it before permitting normal exit.
        if (mapper == -1 && kinit(nullptr) != 0)
        {
            feature = "AUDIO_CALLBACK_RESTORE_FAILED";
            BootLog("ME_AUDIO init=CALLBACK_RESTORE_FAILED cold_reboot_required=1\n");
            FlushBootLogHard();
            for (;;) sceKernelDelayThread(1000);
        }
        ownsCore.store(0, std::memory_order_release);
        scePowerUnlock(0); powerLocked = false;
        feature = "AUDIO_INIT_FAILED";
        BootLog("ME_AUDIO init=FAILED mapper=%d\n", mapper);
        return 0;
    }
    const unsigned start = sceKernelGetSystemTimeLow();
    while (Box()->state == Booting && sceKernelGetSystemTimeLow() - start < 2000000U)
        sceKernelDelayThread(1000);
    if (Box()->state != Ready || !StackGuards())
    {
        feature = "AUDIO_NO_READY";
        BootLog("ME_AUDIO init=NO_READY storage=MAIN_RAM edram=0\n");
        MeAudioShutdown();
        return 0;
    }
    if (Box()->stop) { MeAudioShutdown(); return 0; }
    active.store(1, std::memory_order_release);
    if (!AudioSelfTest())
    {
        feature = "AUDIO_SELFTEST_FAILED";
        BootLog("ME_AUDIO selftest=FAILED output_and_cursors=0\n");
        MeAudioShutdown();
        return 0;
    }
    feature = "AUDIO_ONLY";
    BootLog("ME_AUDIO selftest=PASS output_and_cursors=1 voices=8 frames=1024\n");
    BootLog("ME_AUDIO init=READY mapper=%d packet=0x%08x bytes=%u stack=MAIN_RAM storage=MAIN_RAM edram=0 nonaudio=0\n",
            mapper, Physical(&packet), unsigned(sizeof(packet)));
    return 1;
}

void MeAudioRequestStop()
{
    active.store(0, std::memory_order_release);
    Box()->stop = 1;
    Sync();
}
bool MeAudioCanExit()
{
    // STOP alone is insufficient: shutdown must also retire the kernel's
    // callback into this EBOOT before an emergency watchdog may unload it.
    return !ownsCore.load(std::memory_order_acquire);
}
void MeAudioShutdown()
{
    MeAudioRequestStop();
    bool warned = false;
    const unsigned start = sceKernelGetSystemTimeLow();
    while (ownsCore.load(std::memory_order_acquire) &&
           !(Box()->state == Stopped && Box()->command == 0 && Box()->producer == 0))
    {
        if (!warned && sceKernelGetSystemTimeLow() - start > 500000U)
        {
            BootLog("ME_AUDIO stop=UNACKNOWLEDGED memory_retained=1 cold_reboot_required=1\n");
            FlushBootLogHard();
            warned = true;
        }
        // Never unload worker code/snapshots while ME can still access them.
        sceKernelDelayThread(1000);
    }
    if (ownsCore.load(std::memory_order_acquire) && kinit(nullptr) != 0)
    {
        BootLog("ME_AUDIO stop=CALLBACK_RESTORE_FAILED cold_reboot_required=1\n");
        FlushBootLogHard();
        for (;;) sceKernelDelayThread(1000);
    }
    if (powerLocked) { scePowerUnlock(0); powerLocked = false; }
    ownsCore.store(0, std::memory_order_release);
    BootLog("ME_AUDIO final callbacks=%u on_me=%u fallback=%u timeouts=%u build_avg_us=%u wait_avg_us=%u wait_max_us=%u sc_avg_us=%u stopped=%u edram=0\n",
        callbacks, completed, fallbacks, timeouts, completed ? buildUs / completed : 0,
        completed ? waitUs / completed : 0, waitMaxUs, scCalls ? scUs / scCalls : 0,
        Box()->state == Stopped);
}
const char *MeAudioFeature() { return feature; }

MeAudioJob *MeAudioPrepare(unsigned frames)
{
    ++callbacks;
    // Announce publication before checking stop. Worker cannot ACK/unload
    // between our eligibility check and command publication.
    Box()->producer = 1;
    Sync();
    if (!active.load(std::memory_order_acquire) || Box()->stop || !frames || frames > MeAudioMaxFrames)
    { MeAudioRelease(); return nullptr; }
    buildStart = sceKernelGetSystemTimeLow();
    packet.frames = frames;
    packet.count = 0;
    packet.result = 0;
    __asm__ volatile("cfc1 %0, $31" : "=r"(packet.fcsr));
    return &packet;
}
bool MeAudioAdd(MeAudioJob *job, const MeAudioVoice &voice, const unsigned char *pcm)
{
    if (job != &packet || job->count == MeAudioMaxVoices) return false;
    const unsigned n = job->count;
    job->voices[n] = voice;
    if (!MeAudioSnapshot(job->voices[n], pcm, job->frames, job->pcm[n])) return false;
    ++job->count;
    return true;
}
bool MeAudioRun(MeAudioJob *job, std::int16_t *output)
{
    if (job != &packet || !job->count || !active.load(std::memory_order_acquire) || Box()->stop)
        return false;
    sceKernelDcacheWritebackInvalidateRange(job, 64 + sizeof(job->voices));
    for (unsigned i = 0; i < job->count; ++i)
        sceKernelDcacheWritebackRange(job->pcm[i], Lines(job->voices[i].snapshotFrames * job->voices[i].frameBytes));
    sceKernelDcacheWritebackInvalidateRange(job->output, Lines(job->frames * 4));
    buildUs += sceKernelGetSystemTimeLow() - buildStart;
    Box()->done = 0;
    Sync();
    Box()->command = 1;
    Sync();
    const unsigned start = sceKernelGetSystemTimeLow();
    while (!Box()->done)
    {
        if (sceKernelGetSystemTimeLow() - start >= 8000U || Box()->stop)
        {
            ++timeouts;
            // The quarantined packet, PCM and output are NEVER reused. A late
            // ME can finish safely; no original PCM or engine pointer escapes.
            MeAudioRequestStop();
            return false;
        }
        sceKernelDelayThread(20);
    }
    if (!active.load(std::memory_order_acquire) || Box()->state != Ready) return false;
    const unsigned waited = sceKernelGetSystemTimeLow() - start;
    waitUs += waited; if (waited > waitMaxUs) waitMaxUs = waited;
    sceKernelDcacheInvalidateRange(job, 64 + sizeof(job->voices));
    if (job->result != 1 || !StackGuards()) { MeAudioRequestStop(); return false; }
    sceKernelDcacheInvalidateRange(job->output, Lines(job->frames * 4));
    std::memcpy(output, job->output, job->frames * 4);
    ++completed;
    return true;
}
void MeAudioRelease() { Sync(); Box()->producer = 0; Sync(); }
void MeAudioNoteSc(unsigned us) { ++fallbacks; ++scCalls; scUs += us; }
namespace
{
bool AudioSelfTest()
{
    // Boot-only gate, before SDL starts. Dedicated reference storage cannot
    // race a real callback or overwrite the worker's output.
    static unsigned char source[MeAudioPcmBytes];
    static MeAudioVoice initial[8], expectedVoices[8];
    static std::int16_t expected[MeAudioMaxFrames * 2], actual[MeAudioMaxFrames * 2];
    const unsigned lengths[8] = {1, 3, 17, 511, 1024, 33, 89, 1023};
    for (unsigned i = 0; i < sizeof(source); ++i) source[i] = (i * 137U + i / 7) & 255;
    MeAudioJob *job = MeAudioPrepare(MeAudioMaxFrames);
    if (!job) return false;
    for (unsigned i = 0; i < 8; ++i)
    {
        MeAudioVoice v{};
        v.sourceFrames = lengths[i]; v.bits = i & 1 ? 8 : 16;
        v.channels = i & 2 ? 1 : 2; v.frameBytes = v.channels * (v.bits / 8);
        v.shift = i % 3; v.looping = i & 1;
        v.cursor = (std::uint64_t(v.sourceFrames) << v.shift) - 1;
        v.leftGain = i & 1 ? 0.73125f : 1.0f;
        v.rightGain = i & 2 ? 0.125f : 1.0f;
        if (!MeAudioAdd(job, v, source)) { MeAudioRelease(); return false; }
        initial[i] = job->voices[i];
    }
    if (!MeAudioKernel(*job)) { MeAudioRelease(); return false; }
    std::memcpy(expected, job->output, sizeof(expected));
    std::memcpy(expectedVoices, job->voices, sizeof(expectedVoices));
    std::memcpy(job->voices, initial, sizeof(initial));
    job->result = 0;
    const bool ran = MeAudioRun(job, actual);
    const bool same = ran && std::memcmp(expected, actual, sizeof(expected)) == 0 &&
        std::memcmp(expectedVoices, job->voices, sizeof(expectedVoices)) == 0;
    MeAudioRelease();
    callbacks = completed = fallbacks = timeouts = buildUs = waitUs = waitMaxUs = scUs = scCalls = 0;
    return same;
}
}
} // namespace th08::psp
#endif
