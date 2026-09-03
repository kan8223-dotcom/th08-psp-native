#pragma once

#include "runtime_telemetry_config.hpp"

#include <cstdint>

namespace th08::psp
{
// All writers can run on the SDL audio or BGM worker thread.  The
// implementation therefore uses only 32-bit atomic operations: no logging,
// allocation, locking, or PCM mutation is permitted on those paths.
struct AudioTelemetrySnapshot
{
    std::uint32_t callbackCount;
    std::uint32_t callbackFrames;
    std::uint32_t nonzeroSamples;
    std::uint32_t peakAmplitude;
    std::uint32_t activeVoicesCurrent;
    std::uint32_t activeVoicesPeak;
    std::uint32_t playSubmits;
    std::uint32_t bgmNotifySignals;
    std::uint32_t bgmRefills;
    std::uint32_t bgmRefillBytes;
    std::uint32_t bgmRefillSkips;
    std::uint32_t bgmRefillFailures;
    // SDL does not expose a reliable device-underrun counter on this backend.
    // Keep the absence machine-readable instead of reporting a guessed zero.
    std::uint32_t exactUnderrunAvailable;
};

#if TH08_PSP_RUNTIME_TELEMETRY
void AudioTelemetryReset();
void AudioTelemetryRecordCallback(std::uint32_t frames,
                                  std::uint32_t nonzeroSamples,
                                  std::uint32_t peakAmplitude,
                                  std::uint32_t activeVoices);
void AudioTelemetryRecordPlaySubmit();
void AudioTelemetryRecordBgmNotify();
void AudioTelemetryRecordBgmRefillSuccess(std::uint32_t nominalBytes);
void AudioTelemetryRecordBgmRefillSkip();
void AudioTelemetryRecordBgmRefillFailure();
AudioTelemetrySnapshot CaptureAudioTelemetrySnapshot();
#else
inline void AudioTelemetryReset() {}
inline void AudioTelemetryRecordCallback(std::uint32_t, std::uint32_t,
                                         std::uint32_t, std::uint32_t) {}
inline void AudioTelemetryRecordPlaySubmit() {}
inline void AudioTelemetryRecordBgmNotify() {}
inline void AudioTelemetryRecordBgmRefillSuccess(std::uint32_t) {}
inline void AudioTelemetryRecordBgmRefillSkip() {}
inline void AudioTelemetryRecordBgmRefillFailure() {}
inline AudioTelemetrySnapshot CaptureAudioTelemetrySnapshot()
{
    return AudioTelemetrySnapshot{};
}
#endif
} // namespace th08::psp
