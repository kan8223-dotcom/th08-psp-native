#include "audio_telemetry.hpp"

namespace
{
volatile std::uint32_t gCallbackCount = 0;
volatile std::uint32_t gCallbackFrames = 0;
volatile std::uint32_t gNonzeroSamples = 0;
volatile std::uint32_t gPeakAmplitude = 0;
volatile std::uint32_t gActiveVoicesCurrent = 0;
volatile std::uint32_t gActiveVoicesPeak = 0;
volatile std::uint32_t gPlaySubmits = 0;
volatile std::uint32_t gBgmNotifySignals = 0;
volatile std::uint32_t gBgmRefills = 0;
volatile std::uint32_t gBgmRefillBytes = 0;
volatile std::uint32_t gBgmRefillSkips = 0;
volatile std::uint32_t gBgmRefillFailures = 0;

void AtomicStore(volatile std::uint32_t *target, std::uint32_t value)
{
    __sync_lock_test_and_set(target, value);
}

std::uint32_t AtomicLoad(volatile std::uint32_t *source)
{
    return __sync_fetch_and_add(source, 0U);
}

void AtomicMax(volatile std::uint32_t *target, std::uint32_t candidate)
{
    std::uint32_t observed = AtomicLoad(target);
    while (candidate > observed)
    {
        if (__sync_bool_compare_and_swap(target, observed, candidate))
            return;
        observed = AtomicLoad(target);
    }
}
} // namespace

namespace th08::psp
{
void AudioTelemetryReset()
{
    // MemoryTelemetryInitialize calls this before DirectSound starts, but use
    // atomic stores so the reset contract remains safe if initialization order
    // changes later.
    AtomicStore(&gCallbackCount, 0);
    AtomicStore(&gCallbackFrames, 0);
    AtomicStore(&gNonzeroSamples, 0);
    AtomicStore(&gPeakAmplitude, 0);
    AtomicStore(&gActiveVoicesCurrent, 0);
    AtomicStore(&gActiveVoicesPeak, 0);
    AtomicStore(&gPlaySubmits, 0);
    AtomicStore(&gBgmNotifySignals, 0);
    AtomicStore(&gBgmRefills, 0);
    AtomicStore(&gBgmRefillBytes, 0);
    AtomicStore(&gBgmRefillSkips, 0);
    AtomicStore(&gBgmRefillFailures, 0);
}

void AudioTelemetryRecordCallback(std::uint32_t frames,
                                  std::uint32_t nonzeroSamples,
                                  std::uint32_t peakAmplitude,
                                  std::uint32_t activeVoices)
{
    __sync_fetch_and_add(&gCallbackCount, 1U);
    __sync_fetch_and_add(&gCallbackFrames, frames);
    __sync_fetch_and_add(&gNonzeroSamples, nonzeroSamples);
    AtomicMax(&gPeakAmplitude, peakAmplitude);
    AtomicStore(&gActiveVoicesCurrent, activeVoices);
    AtomicMax(&gActiveVoicesPeak, activeVoices);
}

void AudioTelemetryRecordPlaySubmit()
{
    __sync_fetch_and_add(&gPlaySubmits, 1U);
}

void AudioTelemetryRecordBgmNotify()
{
    __sync_fetch_and_add(&gBgmNotifySignals, 1U);
}

void AudioTelemetryRecordBgmRefillSuccess(std::uint32_t nominalBytes)
{
    __sync_fetch_and_add(&gBgmRefills, 1U);
    __sync_fetch_and_add(&gBgmRefillBytes, nominalBytes);
}

void AudioTelemetryRecordBgmRefillSkip()
{
    __sync_fetch_and_add(&gBgmRefillSkips, 1U);
}

void AudioTelemetryRecordBgmRefillFailure()
{
    __sync_fetch_and_add(&gBgmRefillFailures, 1U);
}

AudioTelemetrySnapshot CaptureAudioTelemetrySnapshot()
{
    AudioTelemetrySnapshot snapshot{};
    snapshot.callbackCount = AtomicLoad(&gCallbackCount);
    snapshot.callbackFrames = AtomicLoad(&gCallbackFrames);
    snapshot.nonzeroSamples = AtomicLoad(&gNonzeroSamples);
    snapshot.peakAmplitude = AtomicLoad(&gPeakAmplitude);
    snapshot.activeVoicesCurrent = AtomicLoad(&gActiveVoicesCurrent);
    snapshot.activeVoicesPeak = AtomicLoad(&gActiveVoicesPeak);
    snapshot.playSubmits = AtomicLoad(&gPlaySubmits);
    snapshot.bgmNotifySignals = AtomicLoad(&gBgmNotifySignals);
    snapshot.bgmRefills = AtomicLoad(&gBgmRefills);
    snapshot.bgmRefillBytes = AtomicLoad(&gBgmRefillBytes);
    snapshot.bgmRefillSkips = AtomicLoad(&gBgmRefillSkips);
    snapshot.bgmRefillFailures = AtomicLoad(&gBgmRefillFailures);
    snapshot.exactUnderrunAvailable = 0;
    return snapshot;
}
} // namespace th08::psp
