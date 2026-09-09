#pragma once
// TH08_PSP_USAGE_METER: in-game SC/ME utilisation history graphs (XP task
// manager style, ported from the TH07 PSP meter spec).  SC% = (frame period -
// idle waits) / period; ME% comes from ME job cycle counters (0 until ME
// jobs exist).  Drawn by the renderer at Present.  Default OFF.
#include <cstdint>
#if defined(PSP) && defined(TH08_PSP_USAGE_METER) && TH08_PSP_USAGE_METER
#define TH08_PSP_USAGE_METER_ENABLED 1
#else
#define TH08_PSP_USAGE_METER_ENABLED 0
#endif
namespace th08::psp
{
constexpr unsigned kUsageMeterHistory = 64U;
// Idle time inside the current frame (flip guard, VBlank, pacing sleeps).
void UsageMeterNoteWait(std::uint64_t waitUs);
// ME kernel cycles consumed since the last frame (CP0 Count at half clock).
void UsageMeterAddMeCycles(std::uint32_t cycles);
// Called once per Present: closes the frame and pushes SC%/ME% samples.
void UsageMeterEndFrame(std::uint64_t nowUs);
// History rings (values 0..200, percent).  head = newest index.
const std::uint8_t *UsageMeterScHistory();
const std::uint8_t *UsageMeterMeHistory();
unsigned UsageMeterHead();
unsigned UsageMeterLastSc();
unsigned UsageMeterLastMe();
} // namespace th08::psp
