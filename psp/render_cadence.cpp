#include "render_cadence.hpp"

#ifndef TH08_PSP_RENDER_CADENCE_INITIAL_MODE
#define TH08_PSP_RENDER_CADENCE_INITIAL_MODE 0
#endif

static_assert(TH08_PSP_RENDER_CADENCE_INITIAL_MODE == 0 ||
                  TH08_PSP_RENDER_CADENCE_INITIAL_MODE == 1 ||
                  TH08_PSP_RENDER_CADENCE_INITIAL_MODE == 2,
              "TH08_PSP_RENDER_CADENCE_INITIAL_MODE must be 0, 1 or 2");

namespace th08::psp
{
namespace
{
constexpr std::uint8_t kRenderCadenceModeCount = 3;
constexpr std::uint8_t kConfiguredInitialMode =
    static_cast<std::uint8_t>(TH08_PSP_RENDER_CADENCE_INITIAL_MODE);

RenderCadence gRenderCadence;
std::uint8_t gCurrentDrawSimulationTicks = 1;
} // namespace

RenderCadence::RenderCadence()
    : selectEdgeCount(0),
      initialMode(kConfiguredInitialMode), mode(kConfiguredInitialMode),
      pendingSimulationTicks(0),
      selectWasDown(false)
{
}

void RenderCadence::Reset(bool selectDown)
{
    selectEdgeCount = 0;
    initialMode = kConfiguredInitialMode;
    mode = initialMode;
    pendingSimulationTicks = 0;
    // Priming from the physical level prevents a SELECT button held during
    // startup from becoming a synthetic toggle on the first game tick.
    selectWasDown = selectDown;
}

RenderCadenceTickResult RenderCadence::Tick(bool selectDown)
{
    RenderCadenceTickResult result{};
    result.modeChanged = selectDown && !selectWasDown;
    selectWasDown = selectDown;

    if (result.modeChanged)
    {
        ++selectEdgeCount;
        mode = static_cast<std::uint8_t>((mode + 1U) % kRenderCadenceModeCount);
    }

    ++pendingSimulationTicks;
    result.mode = mode;
    result.divisor = Divisor();
    result.draw = pendingSimulationTicks >= result.divisor;
    if (result.draw)
    {
        // Preserve every simulated tick across a mid-cycle mode change.  The
        // FPS accounting consumes this exact value instead of guessing from
        // either the persisted PC configuration or the selected divisor.
        result.simulatedTicksCovered = pendingSimulationTicks;
        pendingSimulationTicks = 0;
    }

    return result;
}

std::uint8_t ConfiguredRenderCadenceInitialMode()
{
    return kConfiguredInitialMode;
}

std::uint8_t RenderCadence::InitialMode() const
{
    return initialMode;
}

std::uint8_t RenderCadence::Mode() const
{
    return mode;
}

std::uint8_t RenderCadence::Divisor() const
{
    return static_cast<std::uint8_t>(mode + 1U);
}

std::uint8_t RenderCadence::PendingSimulationTicks() const
{
    return pendingSimulationTicks;
}

std::uint32_t RenderCadence::SelectEdgeCount() const
{
    return selectEdgeCount;
}

void ResetRenderCadence(bool selectDown)
{
    gRenderCadence.Reset(selectDown);
    gCurrentDrawSimulationTicks = 1;
}

RenderCadenceTickResult TickRenderCadence(bool selectDown)
{
    const RenderCadenceTickResult result = gRenderCadence.Tick(selectDown);
    if (result.draw)
        gCurrentDrawSimulationTicks = result.simulatedTicksCovered;
    return result;
}

std::uint8_t InitialRenderCadenceMode()
{
    return gRenderCadence.InitialMode();
}

std::uint8_t CurrentRenderCadenceMode()
{
    return gRenderCadence.Mode();
}

std::uint8_t CurrentDrawSimulationTicks()
{
    return gCurrentDrawSimulationTicks;
}

std::uint32_t RenderCadenceSelectEdgeCount()
{
    return gRenderCadence.SelectEdgeCount();
}
} // namespace th08::psp
