#pragma once

#include <cstdint>

#if defined(PSP) && defined(TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT) && \
    TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT
#define TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED 1
#else
#define TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_AUDIO_FIXED_CURSOR) && \
    TH08_PSP_AUDIO_FIXED_CURSOR
#define TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED 1
#else
#define TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED 0
#endif

#if TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED && TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED
#error "AUDIO_FIXED_CURSOR audit and product switches are mutually exclusive"
#endif

#if TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED || TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED
#define TH08_PSP_AUDIO_CURSOR_STATS_ENABLED 1

#include "audio_fixed_cursor_math.hpp"

namespace th08::psp
{
// All counters are incremented from the SDL audio thread and read at the
// parent PERF_ATTR window boundary on the game thread; the increments are
// plain integer stores (a window boundary may lose or double-count one
// callback's worth, which is acceptable for a census).
#if TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED
void AudioCursorAuditBeginMix(unsigned int sampleRate, bool eligible,
                              int outputFrames);
void AudioCursorAuditCompareFrame(bool fixedContinues,
                                  std::uint32_t fixedFrame,
                                  std::uint32_t canonicalFrame);
void AudioCursorAuditEndMix(bool canonicalWrapped, bool fixedWrapped,
                            double canonicalCursor, double fixedCursor,
                            bool playing);
#endif
void AudioCursorProductNoteMix(unsigned int sampleRate, bool eligible);

void AudioCursorStatsResetWindow(bool active);
void AudioCursorStatsCancelWindow();
void AudioCursorStatsEmitWindow(std::int32_t stage,
                                std::uint32_t baselineStageFrame,
                                std::uint32_t stageFrame);
} // namespace th08::psp

#else
#define TH08_PSP_AUDIO_CURSOR_STATS_ENABLED 0
#endif
