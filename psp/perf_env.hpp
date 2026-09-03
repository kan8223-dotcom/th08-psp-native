#pragma once

#include <cstdint>

// Per-window execution-environment record for the hardware clock question
// (R-038): effective CPU/bus clock at window start and end, a fresh timer
// calibration, the main thread's kernel run clocks and preemption counts over
// the window, and the sceGeListSync waits reached outside the swap context
// (PSPGL display-list rollover inside the draw chain), attributed to the
// innermost PERF_ATTR phase.  Child of TH08_PSP_PERF_ATTRIBUTION; one
// PERF_ENV V1 line per parent window, no per-frame I/O.

#if defined(PSP) && defined(TH08_PSP_PERF_ENV) && TH08_PSP_PERF_ENV
#define TH08_PSP_PERF_ENV_ENABLED 1

struct PspGeListArgs; // <pspge.h>

namespace th08::psp
{
// Innermost active PerfAttributionScope phase (PerfAttributionPhase value) or
// kPerfEnvNoScope when none is active; maintained by PerfAttributionScope.
constexpr std::uint8_t kPerfEnvNoScope = 22U;
extern std::uint8_t gPerfEnvCurrentPhase;

void PerfEnvInitialize(int mainThreadId);
void PerfEnvWindowStart(bool active);
void PerfEnvCancelWindow();
void PerfEnvNoteUnscopedGeWait(std::uint8_t phase, std::uint64_t durationUs);
// Flip-guard waits (TH08_PSP_SWAP_NOWAIT / SWAP_ASYNC): time the main thread
// spent before the next frame's first GE write waiting for the pending flip.
// It stays inside the DrawFrame phase of the main record (draw frame other)
// and is reported here so it is never double counted.
void PerfEnvNoteFlipWait(std::uint64_t durationUs);
// Main-loop work outside every PERF_ATTR scope, timed by the game loop:
// 0 = SoundPlayer::ProcessQueues, 1 = MemoryTelemetrySampleGameFrame,
// 2 = the loop head (timestamp/viewport), 3 = the trailing Sleep(0) yield,
// 4 = the outer loop between two Render calls (PeekMessage/SDL pump),
// 5 = Render calls that ran no tick (1/60 gate; includes their slot-3 yield),
// 6 = after Present returned (MarkPspRenderPresented, PERF_ATTR window
//     emission and its log write), 7 = the render-cadence tick.
constexpr std::uint8_t kPerfEnvMainSlots = 8U;
void PerfEnvNoteMain(std::uint8_t slot, std::uint64_t durationUs);
// GE queue observer (-Wl,--wrap=sceGeListEnQueue): at every PSPGL list
// submission the previous list is peeked.  Still queued/drawing means the GE
// was busy for the whole interval since the previous submission (certain
// busy time); done means it idled for an unknown part of it.  The swap's GE
// wait is certain busy time as well; the span from a frame's first
// submission to its swap completion is the upper bound.  GE busy per frame
// therefore lies in [busy + swap wait, span].
void PerfEnvNoteGeSwapWait(std::uint64_t durationUs);
void PerfEnvNoteGeFrameEnd(std::uint64_t nowUs);
// Body of the sceGeListEnQueue wrap (owned by psp/ge_list_hook.cpp): peeks
// the previous list, submits this one through __real_sceGeListEnQueue and
// returns its queue id.
int PerfEnvGeListEnqueue(const void *list, void *stall, int cbid,
                         PspGeListArgs *arg);
void PerfEnvEmitWindow(std::int32_t stage, std::uint32_t baselineStageFrame,
                       std::uint32_t stageFrame, std::uint64_t wallUs);
} // namespace th08::psp

#else
#define TH08_PSP_PERF_ENV_ENABLED 0
#endif
