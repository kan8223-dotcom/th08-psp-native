#pragma once

#include <cstdint>

#if defined(PSP) && defined(TH08_PSP_DRAW_PRIORITY_SUBPROFILE) && \
    TH08_PSP_DRAW_PRIORITY_SUBPROFILE
#define TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED 1

namespace th08::psp
{
// RunDrawChain calls this exactly once per frame which will be presented.
// The returned gate remains fixed until EndDrawChain, including every
// CHAIN_CALLBACK_RESULT_EXECUTE_AGAIN invocation in that chain pass.
bool DrawPrioritySubprofileBeginDrawChain(std::uint64_t &startUs);
std::uint64_t DrawPrioritySubprofileReadClock();
void DrawPrioritySubprofileRecordCallback(int priority,
                                          std::uint64_t startUs);
void DrawPrioritySubprofileEndDrawChain(std::uint64_t startUs);

// Nested measurement inside priority 7.  The false path performs no clock
// read, and callers keep the canonical EffectManager call unconditional.
bool DrawPrioritySubprofileBeginEffectBackground(std::uint64_t &startUs);
void DrawPrioritySubprofileEndEffectBackground(std::uint64_t startUs);

// These lifecycle hooks are owned by the existing 600-stage-tick attribution
// window.  Incomplete/rearmed windows are discarded rather than logged.
void DrawPrioritySubprofileResetWindow(bool active);
void DrawPrioritySubprofileCancelWindow();
void DrawPrioritySubprofileEmitWindow(std::int32_t stage,
                                      std::uint32_t baselineStageFrame,
                                      std::uint32_t stageFrame,
                                      std::uint32_t presentedFrames,
                                      std::uint8_t cadenceMode);

// GE submission census.  The D3D8 compat layer reports every draw submission
// (count of calls and logical vertices); the sampled draw-chain pass
// attributes the deltas to the priority bin of the running callback, and a
// second bounded record (DRAW_PRIO_GE V1) is emitted after the timing one.
void DrawPrioritySubprofileNoteDraw(std::uint32_t vertices);
// Priority of the callback about to run (every presented frame, sampled or
// not); cleared by EndDrawChain.  Unscoped sceGeListSync waits (PERF_ENV)
// are attributed to it.
void DrawPrioritySubprofileNoteCallbackPriority(int priority);
void DrawPrioritySubprofileNoteGeWait(std::uint64_t durationUs);
void DrawPrioritySubprofileEmitGeWindow(std::int32_t stage,
                                        std::uint32_t baselineStageFrame,
                                        std::uint32_t stageFrame);
} // namespace th08::psp

#else
#define TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED 0
#endif
