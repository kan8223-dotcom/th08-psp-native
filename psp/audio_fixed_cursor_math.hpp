#pragma once

#include <cstdint>

// Bit-identical fixed-point replacement for the SC audio mixer's binary64
// resampling cursor.  The canonical mixer keeps `double cursorFrame` and
// advances it by `step = nSamplesPerSec / 44100.0` per output frame, taking
// `static_cast<DWORD>(cursorFrame)` as the source frame and subtracting the
// source length once on wrap.  When 44100 / rate is a power of two the step is
// an exact binary fraction 2^-shift, every cursor value is a multiple of it,
// and no binary64 rounding ever happens; the same sequence is then produced
// exactly by an integer cursor in units of 2^-shift frames.  Any other rate,
// or a cursor that is not a multiple of 2^-shift, is not eligible and the
// caller keeps the canonical binary64 path.

namespace th08::psp
{
inline bool AudioFixedCursorEligible(unsigned int sampleRate,
                                     unsigned int *shift)
{
    if (sampleRate == 0U || 44100U % sampleRate != 0U)
        return false;
    const unsigned int quotient = 44100U / sampleRate;
    if ((quotient & (quotient - 1U)) != 0U)
        return false;
    unsigned int s = 0U;
    while ((1U << s) < quotient)
        ++s;
    *shift = s;
    return true;
}

// Converts the canonical cursor to fixed point when it is exactly
// representable (non-negative multiple of 2^-shift below 2^53).
inline bool AudioFixedCursorFromDouble(double cursorFrame, unsigned int shift,
                                       std::uint64_t *fixed)
{
    if (!(cursorFrame >= 0.0))
        return false;
    const double scaled =
        cursorFrame * static_cast<double>(1U << shift); // exact scaling
    if (!(scaled < 9007199254740992.0))
        return false;
    const std::uint64_t value = static_cast<std::uint64_t>(scaled);
    if (static_cast<double>(value) != scaled)
        return false;
    *fixed = value;
    return true;
}

// Exact for fixed < 2^53 (a power-of-two division never rounds).
inline double AudioFixedCursorToDouble(std::uint64_t fixed, unsigned int shift)
{
    return static_cast<double>(fixed) / static_cast<double>(1U << shift);
}

// One canonical loop iteration's bookkeeping on the fixed cursor.  Returns
// false when the voice stops (non-looping end); otherwise stores the source
// frame, updates *wrapped exactly like the canonical path, and advances by
// one output frame (2^-shift source frames).
inline bool AudioFixedCursorStep(std::uint64_t *fixed, unsigned int shift,
                                 std::uint32_t sourceFrames, bool looping,
                                 std::uint32_t *sourceFrame, bool *wrapped)
{
    std::uint64_t frame = *fixed >> shift;
    if (frame >= sourceFrames)
    {
        if (!looping)
            return false;
        *fixed -= static_cast<std::uint64_t>(sourceFrames) << shift;
        frame = *fixed >> shift;
        *wrapped = true;
    }
    *sourceFrame = static_cast<std::uint32_t>(frame);
    *fixed += 1U;
    return true;
}
} // namespace th08::psp
