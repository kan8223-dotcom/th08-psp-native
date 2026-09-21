// Frozen r247 SC mixer reference for regression tests. Same MIT license as src/modern/linux/linux_compat.cpp.
void Mix(Sint16 *output, int outputFrames)
    {
        const DWORD frameBytes = FrameBytes();
        if (!playing || !hasFormat || bytes.empty() || frameBytes == 0 || format.wFormatTag != WAVE_FORMAT_PCM)
            return;
        const DWORD sourceFrames = static_cast<DWORD>(bytes.size() / frameBytes);
        if (sourceFrames == 0) return;
        const double step = static_cast<double>(format.nSamplesPerSec) / 44100.0;
        const float gain = volume <= DSBVOLUME_MIN ? 0.0f : powf(10.0f, static_cast<float>(volume) / 2000.0f);
        const float panValue = pan < -10000 ? -1.0f : pan > 10000 ? 1.0f : static_cast<float>(pan) / 10000.0f;
        const float leftGain = gain * (panValue > 0.0f ? 1.0f - panValue : 1.0f);
        const float rightGain = gain * (panValue < 0.0f ? 1.0f + panValue : 1.0f);
        const DWORD oldPosition = position;
        bool wrapped = false;
#if TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED || TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED
        // Exact fixed-point cursor when 44100 / rate is a power of two and the
        // binary64 cursor is a multiple of that step (see audio_fixed_cursor_math.hpp).
        unsigned int fixedShift = 0U;
        std::uint64_t fixedCursor = 0U;
        const bool fixedEligible =
            th08::psp::AudioFixedCursorEligible(format.nSamplesPerSec, &fixedShift) &&
            th08::psp::AudioFixedCursorFromDouble(cursorFrame, fixedShift, &fixedCursor);
#endif
#if TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED
        th08::psp::AudioCursorAuditBeginMix(format.nSamplesPerSec, fixedEligible, outputFrames);
        bool fixedWrapped = false;
#endif
#if TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED
        th08::psp::AudioCursorProductNoteMix(format.nSamplesPerSec, fixedEligible);
#endif

        for (int frame = 0; frame < outputFrames && playing; ++frame)
        {
#if TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED
            DWORD sourceFrame;
            if (fixedEligible)
            {
                std::uint32_t fixedFrame = 0U;
                if (!th08::psp::AudioFixedCursorStep(&fixedCursor, fixedShift, sourceFrames,
                                                     looping, &fixedFrame, &wrapped))
                {
                    playing = false;
                    break;
                }
                sourceFrame = fixedFrame;
            }
            else
            {
                sourceFrame = static_cast<DWORD>(cursorFrame);
                if (sourceFrame >= sourceFrames)
                {
                    if (!looping) { playing = false; break; }
                    cursorFrame -= sourceFrames; sourceFrame = static_cast<DWORD>(cursorFrame); wrapped = true;
                }
            }
#else
            DWORD sourceFrame = static_cast<DWORD>(cursorFrame);
            if (sourceFrame >= sourceFrames)
            {
                if (!looping) { playing = false; break; }
                cursorFrame -= sourceFrames; sourceFrame = static_cast<DWORD>(cursorFrame); wrapped = true;
            }
#if TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED
            if (fixedEligible)
            {
                std::uint32_t fixedFrame = 0U;
                const bool fixedContinues = th08::psp::AudioFixedCursorStep(
                    &fixedCursor, fixedShift, sourceFrames, looping, &fixedFrame, &fixedWrapped);
                th08::psp::AudioCursorAuditCompareFrame(fixedContinues, fixedFrame, sourceFrame);
            }
#endif
#endif
            const BYTE *source = &bytes[sourceFrame * frameBytes];
            int left, right;
            if (format.wBitsPerSample == 8)
            {
                left = (static_cast<int>(source[0]) - 128) << 8;
                right = format.nChannels > 1 ? (static_cast<int>(source[1]) - 128) << 8 : left;
            }
            else if (format.wBitsPerSample == 16)
            {
                INT16 leftSample, rightSample;
                memcpy(&leftSample, source, sizeof(leftSample));
                if (format.nChannels > 1) memcpy(&rightSample, source + sizeof(INT16), sizeof(rightSample));
                else rightSample = leftSample;
                left = leftSample; right = rightSample;
            }
            else
                break;

            int mixedLeft = output[frame * 2] + static_cast<int>(left * leftGain);
            int mixedRight = output[frame * 2 + 1] + static_cast<int>(right * rightGain);
            if (mixedLeft < -32768) mixedLeft = -32768; else if (mixedLeft > 32767) mixedLeft = 32767;
            if (mixedRight < -32768) mixedRight = -32768; else if (mixedRight > 32767) mixedRight = 32767;
            output[frame * 2] = static_cast<Sint16>(mixedLeft);
            output[frame * 2 + 1] = static_cast<Sint16>(mixedRight);
#if TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED
            if (!fixedEligible)
                cursorFrame += step;
#else
            cursorFrame += step;
#endif
        }
#if TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED
        if (fixedEligible)
            cursorFrame = th08::psp::AudioFixedCursorToDouble(fixedCursor, fixedShift);
#endif
#if TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED
        if (fixedEligible)
            th08::psp::AudioCursorAuditEndMix(
                wrapped, fixedWrapped, cursorFrame,
                th08::psp::AudioFixedCursorToDouble(fixedCursor, fixedShift), playing);
#endif

        if (cursorFrame >= sourceFrames)
        {
            if (looping) { cursorFrame = fmod(cursorFrame, static_cast<double>(sourceFrames)); wrapped = true; }
            else { cursorFrame = sourceFrames; playing = false; }
        }
        position = static_cast<DWORD>(cursorFrame) * frameBytes;
        // Lap count for the streaming catch-up: any backwards move of the
        // cursor while looping is a wrap (both cursor implementations).
        if (looping && position < oldPosition)
            ++laps;
        for (size_t index = 0; index < notifications.size(); ++index)
        {
            const DWORD offset = notifications[index].dwOffset;
            if ((!wrapped && oldPosition <= offset && position > offset) ||
                (wrapped && (offset >= oldPosition || offset < position)))
            {
#if defined(PSP)
                th08::psp::AudioTelemetryRecordBgmNotify();
#endif
                SetEvent(notifications[index].hEventNotify);
            }
        }
    }
