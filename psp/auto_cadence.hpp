#pragma once
// TH08_PSP_AUTO_CADENCE: pick the render cadence (60/30/20 draws per second,
// simulation always 60 Hz) from the measured cost of a full 60-draw frame:
// moving average over the last 12 simulation ticks of (calc time of the tick
// + cost of the most recent draw+present, cadence wait excluded).  Thresholds
// in milliseconds with hysteresis: 60->30 above 20.0, 30->60 below 18.0,
// 30->20 above 30.0, 20->30 below 25.0.  Never overrides a manual SELECT.
#include <cstdint>

namespace th08::psp
{
void AutoCadenceNoteCalc(std::uint64_t us);
void AutoCadenceNoteDraw(std::uint64_t us);   // BeginScene..Present, without the cadence wait
} // namespace th08::psp
// Waits inside the draw/present that are not CPU cost (flip guard, display,
// fence): subtracted from the next AutoCadenceNoteDraw.
extern "C" void th08_psp_auto_cadence_note_wait(unsigned long long us);
namespace th08::psp
{
void AutoCadenceTick(bool drewThisTick);      // once per simulation tick, after the draw/skip
} // namespace th08::psp
