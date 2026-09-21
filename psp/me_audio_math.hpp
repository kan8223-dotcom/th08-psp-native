#pragma once
// TH08's existing float-gain, per-voice saturated mixer, with job-owned PCM.
// No engine pointers, OS services, ME-local eDRAM, or persistent sound cache.
#include <cstdint>
#include <cstring>
#include "audio_fixed_cursor_math.hpp"

namespace th08::psp
{
constexpr unsigned MeAudioMaxFrames = 1024;
constexpr unsigned MeAudioMaxVoices = 32;
constexpr unsigned MeAudioPcmBytes = MeAudioMaxFrames * 4;

struct alignas(64) MeAudioVoice
{
    std::uint64_t cursor;
    std::uint32_t sourceFrames, frameBytes, channels, bits, shift, looping;
    float leftGain, rightGain;
    std::uint32_t firstFrame, snapshotFrames, playing, wrapped;
    std::uint32_t reserved[2];
};
static_assert(sizeof(MeAudioVoice) == 64, "one voice owns one cache line");

struct alignas(64) MeAudioJob
{
    std::uint32_t frames, count, fcsr, result;
    std::uint32_t reserved[12];
    MeAudioVoice voices[MeAudioMaxVoices];
    unsigned char pcm[MeAudioMaxVoices][MeAudioPcmBytes];
    std::int16_t output[MeAudioMaxFrames * 2];
};
static_assert(sizeof(MeAudioJob) % 64 == 0, "whole cache lines only");

inline bool MeAudioSnapshot(MeAudioVoice &v, const unsigned char *source,
                            unsigned frames, unsigned char *snapshot)
{
    if (!source || !snapshot || !frames || frames > MeAudioMaxFrames ||
        !v.sourceFrames || v.shift > 15 ||
        (v.channels != 1 && v.channels != 2) || (v.bits != 8 && v.bits != 16) ||
        v.frameBytes != v.channels * (v.bits / 8) ||
        v.sourceFrames > UINT32_MAX / v.frameBytes ||
        v.cursor > (std::uint64_t(v.sourceFrames) << v.shift))
        return false;
    // A fractional start can touch one more input frame than frames>>shift.
    const unsigned fraction = unsigned(v.cursor & ((1U << v.shift) - 1));
    const unsigned needed = ((fraction + frames - 1) >> v.shift) + 1;
    v.snapshotFrames = v.sourceFrames < needed ? v.sourceFrames : needed;
    v.firstFrame = v.snapshotFrames == v.sourceFrames ? 0 :
        unsigned((v.cursor >> v.shift) % v.sourceFrames);
    const unsigned tail = v.sourceFrames - v.firstFrame;
    const unsigned first = tail < v.snapshotFrames ? tail : v.snapshotFrames;
    std::memcpy(snapshot, source + v.firstFrame * v.frameBytes, first * v.frameBytes);
    if (first < v.snapshotFrames)
        std::memcpy(snapshot + first * v.frameBytes, source,
                    (v.snapshotFrames - first) * v.frameBytes);
    v.playing = 1;
    v.wrapped = 0;
    return true;
}

inline bool MeAudioKernel(MeAudioJob &job)
{
    job.result = 0;
    if (!job.frames || job.frames > MeAudioMaxFrames || job.count > MeAudioMaxVoices)
        return false;
    std::memset(job.output, 0, job.frames * 4);
    for (unsigned n = 0; n < job.count; ++n)
    {
        MeAudioVoice &v = job.voices[n];
        bool wrapped = false;
        for (unsigned f = 0; f < job.frames; ++f)
        {
            std::uint32_t frame;
            if (!AudioFixedCursorStep(&v.cursor, v.shift, v.sourceFrames,
                                      v.looping != 0, &frame, &wrapped))
            { v.playing = 0; break; }
            const unsigned offset = frame >= v.firstFrame ? frame - v.firstFrame :
                frame + v.sourceFrames - v.firstFrame;
            if (offset >= v.snapshotFrames) return false;
            const unsigned char *p = job.pcm[n] + offset * v.frameBytes;
            int left, right;
            if (v.bits == 8)
            {
                left = (int(p[0]) - 128) * 256;
                right = v.channels == 2 ? (int(p[1]) - 128) * 256 : left;
            }
            else
            {
                left = std::int16_t(unsigned(p[0]) | (unsigned(p[1]) << 8));
                right = v.channels == 2 ?
                    std::int16_t(unsigned(p[2]) | (unsigned(p[3]) << 8)) : left;
            }
            int l = job.output[f * 2] + int(left * v.leftGain);
            int r = job.output[f * 2 + 1] + int(right * v.rightGain);
            if (l < -32768) l = -32768; else if (l > 32767) l = 32767;
            if (r < -32768) r = -32768; else if (r > 32767) r = 32767;
            job.output[f * 2] = std::int16_t(l);
            job.output[f * 2 + 1] = std::int16_t(r);
        }
        v.wrapped = wrapped;
    }
    job.result = 1;
    return true;
}
} // namespace th08::psp
