#include "audio_cursor_audit.hpp"

#if TH08_PSP_AUDIO_CURSOR_STATS_ENABLED

#include "fileio.hpp"

#include <cstdint>
#include <cstring>
#include <limits>

namespace th08::psp
{
namespace
{
std::uint32_t gMixes = 0U;
std::uint32_t gEligible = 0U;
std::uint32_t gIneligibleRate = 0U;
std::uint32_t gIneligibleCursor = 0U;
std::uint32_t gFrames = 0U;
std::uint32_t gMismatch = 0U;
std::uint32_t gWrapMismatch = 0U;
std::uint32_t gCursorMismatch = 0U;
std::uint32_t gStopMismatch = 0U;
std::uint32_t gRate44100 = 0U;
std::uint32_t gRate22050 = 0U;
std::uint32_t gRate11025 = 0U;
std::uint32_t gRateOther = 0U;
std::uint32_t gMaxFrames = 0U;
bool gWindowActive = false;

void IncrementSaturating(std::uint32_t &value)
{
    if (value != std::numeric_limits<std::uint32_t>::max())
        ++value;
}

void NoteRate(unsigned int sampleRate)
{
    if (sampleRate == 44100U)
        IncrementSaturating(gRate44100);
    else if (sampleRate == 22050U)
        IncrementSaturating(gRate22050);
    else if (sampleRate == 11025U)
        IncrementSaturating(gRate11025);
    else
        IncrementSaturating(gRateOther);
}

void NoteMix(unsigned int sampleRate, bool eligible)
{
    IncrementSaturating(gMixes);
    NoteRate(sampleRate);
    if (eligible)
        IncrementSaturating(gEligible);
    else if (sampleRate != 0U && 44100U % sampleRate == 0U &&
             (((44100U / sampleRate) & ((44100U / sampleRate) - 1U)) == 0U))
        IncrementSaturating(gIneligibleCursor);
    else
        IncrementSaturating(gIneligibleRate);
}

void ClearWindowCounters()
{
    gMixes = gEligible = gIneligibleRate = gIneligibleCursor = 0U;
    gFrames = gMismatch = gWrapMismatch = gCursorMismatch = gStopMismatch = 0U;
    gRate44100 = gRate22050 = gRate11025 = gRateOther = 0U;
    gMaxFrames = 0U;
}
} // namespace

#if TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED
void AudioCursorAuditBeginMix(unsigned int sampleRate, bool eligible,
                              int outputFrames)
{
    if (!gWindowActive)
        return;
    NoteMix(sampleRate, eligible);
    if (outputFrames > 0 && static_cast<std::uint32_t>(outputFrames) > gMaxFrames)
        gMaxFrames = static_cast<std::uint32_t>(outputFrames);
}

void AudioCursorAuditCompareFrame(bool fixedContinues,
                                  std::uint32_t fixedFrame,
                                  std::uint32_t canonicalFrame)
{
    if (!gWindowActive)
        return;
    IncrementSaturating(gFrames);
    if (!fixedContinues)
        IncrementSaturating(gStopMismatch);
    else if (fixedFrame != canonicalFrame)
        IncrementSaturating(gMismatch);
}

void AudioCursorAuditEndMix(bool canonicalWrapped, bool fixedWrapped,
                            double canonicalCursor, double fixedCursor,
                            bool playing)
{
    if (!gWindowActive)
        return;
    if (canonicalWrapped != fixedWrapped)
        IncrementSaturating(gWrapMismatch);
    // Bit compare the two binary64 cursors without arithmetic.
    std::uint64_t a;
    std::uint64_t b;
    std::memcpy(&a, &canonicalCursor, sizeof(a));
    std::memcpy(&b, &fixedCursor, sizeof(b));
    if (playing && a != b)
        IncrementSaturating(gCursorMismatch);
}
#endif

void AudioCursorProductNoteMix(unsigned int sampleRate, bool eligible)
{
    if (!gWindowActive)
        return;
    NoteMix(sampleRate, eligible);
}

void AudioCursorStatsResetWindow(bool active)
{
    ClearWindowCounters();
    gWindowActive = active;
}

void AudioCursorStatsCancelWindow()
{
    if (gWindowActive)
        ClearWindowCounters();
    gWindowActive = false;
}

void AudioCursorStatsEmitWindow(std::int32_t stage,
                                std::uint32_t baselineStageFrame,
                                std::uint32_t stageFrame)
{
    if (!gWindowActive)
        return;
    BootLog(
        "AUDIO_CURSOR V1 st=%ld sf=%lu-%lu mode=%s mixes=%lu eligible=%lu "
        "inelig_rate=%lu inelig_cursor=%lu frames=%lu mismatch=%lu "
        "wrap_mismatch=%lu cursor_mismatch=%lu stop_mismatch=%lu "
        "r44100=%lu r22050=%lu r11025=%lu rother=%lu max_frames=%lu\n",
        static_cast<long>(stage),
        static_cast<unsigned long>(baselineStageFrame),
        static_cast<unsigned long>(stageFrame),
        TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED ? "audit" : "product",
        static_cast<unsigned long>(gMixes),
        static_cast<unsigned long>(gEligible),
        static_cast<unsigned long>(gIneligibleRate),
        static_cast<unsigned long>(gIneligibleCursor),
        static_cast<unsigned long>(gFrames),
        static_cast<unsigned long>(gMismatch),
        static_cast<unsigned long>(gWrapMismatch),
        static_cast<unsigned long>(gCursorMismatch),
        static_cast<unsigned long>(gStopMismatch),
        static_cast<unsigned long>(gRate44100),
        static_cast<unsigned long>(gRate22050),
        static_cast<unsigned long>(gRate11025),
        static_cast<unsigned long>(gRateOther),
        static_cast<unsigned long>(gMaxFrames));
}
} // namespace th08::psp

#endif // TH08_PSP_AUDIO_CURSOR_STATS_ENABLED
