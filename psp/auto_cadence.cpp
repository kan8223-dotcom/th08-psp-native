#include "auto_cadence.hpp"

#if defined(PSP) && defined(TH08_PSP_AUTO_CADENCE) && TH08_PSP_AUTO_CADENCE

#include "fileio.hpp"
#include "render_cadence.hpp"
#include "Player.hpp"

#ifndef TH08_PSP_AUTO_CADENCE_BOMB_MODE
#define TH08_PSP_AUTO_CADENCE_BOMB_MODE 2 // 20 draws/s while a bomb is in use (0 = disabled)
#endif

namespace th08::psp
{
namespace
{
constexpr unsigned kWindow = 12U;
constexpr float kDemote60To30 = 20.0f, kPromote30To60 = 18.0f;
constexpr float kDemote30To20 = 30.0f, kPromote20To30 = 25.0f;
std::uint32_t gCalcUs[kWindow];
std::uint32_t gDrawUsLast = 0U;   // moving average of the last kDrawWindow draw+present costs (us)
constexpr unsigned kDrawWindow = 8U;
std::uint32_t gDrawUs[kDrawWindow];
unsigned gDrawHead = 0U, gDrawFilled = 0U;
unsigned gHead = 0U, gFilled = 0U;
std::uint64_t gPendingCalcUs = 0U;
std::uint64_t gWaitUsSinceDraw = 0U;
unsigned long gLastSwitchTick = 0UL;
bool gBombForced = false;
std::uint8_t gModeBeforeBomb = 0U;
unsigned long gBombForces = 0UL;
constexpr unsigned long kDwellTicks = 60UL;
unsigned long gTicks = 0UL, gSwitches = 0UL, gTicksInMode[3] = {0UL, 0UL, 0UL};
float gLastEstimate = 0.0f, gMaxEstimate = 0.0f;
} // namespace

void AutoCadenceNoteCalc(std::uint64_t us) { gPendingCalcUs = us; }
void AutoCadenceNoteDraw(std::uint64_t us)
{
    const std::uint64_t net = us > gWaitUsSinceDraw ? us - gWaitUsSinceDraw : 0U;
    gWaitUsSinceDraw = 0U;
    gDrawUs[gDrawHead] = static_cast<std::uint32_t>(net > 100000ULL ? 100000ULL : net);
    gDrawHead = (gDrawHead + 1U) % kDrawWindow;
    if (gDrawFilled < kDrawWindow)
        ++gDrawFilled;
    std::uint64_t sum = 0U;
    for (unsigned i = 0U; i < gDrawFilled; ++i)
        sum += gDrawUs[i];
    gDrawUsLast = static_cast<std::uint32_t>(sum / gDrawFilled);
}

void AutoCadenceTick(bool drewThisTick)
{
    (void)drewThisTick;
    gCalcUs[gHead] = static_cast<std::uint32_t>(gPendingCalcUs > 100000ULL ? 100000ULL : gPendingCalcUs);
    gHead = (gHead + 1U) % kWindow;
    if (gFilled < kWindow)
        ++gFilled;
    std::uint64_t sum = 0U;
    for (unsigned i = 0U; i < gFilled; ++i)
        sum += gCalcUs[i];
    // Cost of a 60-draw frame: one tick's calc plus one draw.
    const float estimate = (static_cast<float>(sum) / static_cast<float>(gFilled) + static_cast<float>(gDrawUsLast)) / 1000.0f;
    gLastEstimate = estimate;
    if (estimate > gMaxEstimate)
        gMaxEstimate = estimate;
    ++gTicks;
    const std::uint8_t mode = CurrentRenderCadenceMode();
    gTicksInMode[mode < 3U ? mode : 2U] += 1UL;
#if TH08_PSP_AUTO_CADENCE_BOMB_MODE > 0
    // A bomb spikes the frame cost at once: switch as soon as it starts and
    // hand the mode back when it ends, ahead of the moving average.
    if (RenderCadenceSelectEdgeCount() == 0U)
    {
        const bool bombActive = th08::g_Player.bombState.isInUse != 0;
        if (bombActive && !gBombForced)
        {
            gBombForced = true;
            gModeBeforeBomb = mode;
            if (mode != TH08_PSP_AUTO_CADENCE_BOMB_MODE)
            {
                SetRenderCadenceModeAuto(TH08_PSP_AUTO_CADENCE_BOMB_MODE);
                ++gSwitches;
            }
            ++gBombForces;
            BootLog("AUTO_CADENCE bomb start %u->%u tick=%lu\n", static_cast<unsigned>(mode),
                    static_cast<unsigned>(TH08_PSP_AUTO_CADENCE_BOMB_MODE), gTicks);
        }
        else if (!bombActive && gBombForced)
        {
            gBombForced = false;
            SetRenderCadenceModeAuto(gModeBeforeBomb);
            if (gModeBeforeBomb != mode)
                ++gSwitches;
            gFilled = 0U;
            gHead = 0U;
            gLastSwitchTick = gTicks;
            BootLog("AUTO_CADENCE bomb end ->%u tick=%lu\n", static_cast<unsigned>(gModeBeforeBomb), gTicks);
        }
        if (gBombForced)
            return; // the bomb owns the mode
    }
#endif
    if (gFilled >= kWindow && RenderCadenceSelectEdgeCount() == 0U && gTicks - gLastSwitchTick >= kDwellTicks)
    {
        std::uint8_t wanted = mode;
        if (mode == 0U && estimate > kDemote60To30)
            wanted = 1U;
        else if (mode == 1U && estimate > kDemote30To20)
            wanted = 2U;
        else if (mode == 1U && estimate < kPromote30To60)
            wanted = 0U;
        else if (mode == 2U && estimate < kPromote20To30)
            wanted = 1U;
        if (wanted != mode)
        {
            SetRenderCadenceModeAuto(wanted);
            ++gSwitches;
            BootLog("AUTO_CADENCE switch %u->%u estimate_ms=%.2f draw_ms=%.2f tick=%lu switches=%lu\n",
                    static_cast<unsigned>(mode), static_cast<unsigned>(wanted), estimate,
                    static_cast<float>(gDrawUsLast) / 1000.0f, gTicks, gSwitches);
            gFilled = 0U; // start a fresh window in the new mode
            gHead = 0U;
            gLastSwitchTick = gTicks;
        }
    }
    if ((gTicks % 600UL) == 0UL)
    {
        BootLog("AUTO_CADENCE stats ticks=%lu mode=%u switches=%lu in60=%lu in30=%lu in20=%lu estimate_ms=%.2f max_ms=%.2f manual=%lu bombs=%lu\n",
                gTicks, static_cast<unsigned>(mode), gSwitches, gTicksInMode[0], gTicksInMode[1], gTicksInMode[2],
                gLastEstimate, gMaxEstimate, static_cast<unsigned long>(RenderCadenceSelectEdgeCount()), gBombForces);
        gMaxEstimate = 0.0f;
    }
}
} // namespace th08::psp
extern "C" void th08_psp_auto_cadence_note_wait(unsigned long long us) { th08::psp::gWaitUsSinceDraw += us; }

#else
namespace th08::psp
{
void AutoCadenceNoteCalc(std::uint64_t) {}
void AutoCadenceNoteDraw(std::uint64_t) {}
void AutoCadenceTick(bool) {}
} // namespace th08::psp
extern "C" void th08_psp_auto_cadence_note_wait(unsigned long long) {}
#endif
