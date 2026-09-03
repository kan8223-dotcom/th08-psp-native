#include "psp/render_cadence.hpp"

#include <cstdint>
#include <cstdio>

namespace
{
bool Check(bool condition, const char *message)
{
    if (!condition)
        std::fprintf(stderr, "render-cadence: FAIL: %s\n", message);
    return condition;
}

struct RunTotals
{
    unsigned int draws;
    unsigned int coveredTicks;
};

RunTotals Run(th08::psp::RenderCadence &cadence, unsigned int ticks,
              bool selectDown = false)
{
    RunTotals totals{};
    for (unsigned int tick = 0; tick < ticks; ++tick)
    {
        const th08::psp::RenderCadenceTickResult result =
            cadence.Tick(selectDown);
        if (result.draw)
        {
            ++totals.draws;
            totals.coveredTicks += result.simulatedTicksCovered;
        }
    }
    return totals;
}
} // namespace

int main()
{
    using th08::psp::RenderCadence;
    using th08::psp::RenderCadenceTickResult;

    const std::uint8_t configuredInitialMode =
        th08::psp::ConfiguredRenderCadenceInitialMode();
    const std::uint8_t configuredInitialDivisor =
        static_cast<std::uint8_t>(configuredInitialMode + 1U);

    RenderCadence cadence;
    cadence.Reset(false);
    if (!Check(cadence.InitialMode() == configuredInitialMode &&
                   cadence.Mode() == configuredInitialMode &&
                   cadence.SelectEdgeCount() == 0,
               "reset must select and record the configured initial mode"))
        return 1;

    const RunTotals initial = Run(cadence, 600);
    if (!Check(initial.draws == 600U / configuredInitialDivisor &&
                   initial.coveredTicks == 600,
               "configured mode must cover every simulation tick exactly"))
        return 1;

    cadence.Reset(false);
    RenderCadenceTickResult result = cadence.Tick(true);
    const std::uint8_t firstSelectedMode =
        static_cast<std::uint8_t>((configuredInitialMode + 1U) % 3U);
    if (!Check(result.modeChanged && cadence.Mode() == firstSelectedMode &&
                   cadence.SelectEdgeCount() == 1,
               "first SELECT rising edge must change mode and count once"))
        return 1;
    for (unsigned int held = 0; held < 30; ++held)
    {
        result = cadence.Tick(true);
        if (!Check(!result.modeChanged && cadence.Mode() == firstSelectedMode &&
                       cadence.SelectEdgeCount() == 1,
                   "held SELECT must neither retrigger nor increment edges"))
            return 1;
    }

    cadence.Tick(false);
    result = cadence.Tick(true);
    const std::uint8_t secondSelectedMode =
        static_cast<std::uint8_t>((firstSelectedMode + 1U) % 3U);
    if (!Check(result.modeChanged && cadence.Mode() == secondSelectedMode &&
                   cadence.SelectEdgeCount() == 2,
               "second SELECT rising edge must change mode and count once"))
        return 1;
    cadence.Tick(false);
    while (cadence.PendingSimulationTicks() != 0)
        cadence.Tick(false);

    const RunTotals persisted = Run(cadence, 600, false);
    if (!Check(cadence.InitialMode() == configuredInitialMode &&
                   cadence.Mode() == secondSelectedMode &&
                   cadence.SelectEdgeCount() == 2 &&
                   persisted.draws == 600U / (secondSelectedMode + 1U) &&
                   persisted.coveredTicks == 600,
               "mode and edge count must persist until an explicit reset"))
        return 1;

    result = cadence.Tick(true);
    const std::uint8_t thirdSelectedMode =
        static_cast<std::uint8_t>((secondSelectedMode + 1U) % 3U);
    if (!Check(result.modeChanged && cadence.Mode() == thirdSelectedMode &&
                   cadence.SelectEdgeCount() == 3,
               "third SELECT rising edge must change mode and count once"))
        return 1;

    cadence.Reset(true);
    result = cadence.Tick(true);
    if (!Check(!result.modeChanged &&
                   cadence.InitialMode() == configuredInitialMode &&
                   cadence.Mode() == configuredInitialMode &&
                   cadence.SelectEdgeCount() == 0,
               "reset must prime a held SELECT without a synthetic edge"))
        return 1;

    cadence.Reset(false);
    unsigned int simulatedTicks = 0;
    unsigned int accountedTicks = 0;
    for (unsigned int tick = 0; tick < 1000; ++tick)
    {
        const bool down = (tick % 97U) == 0U;
        result = cadence.Tick(down);
        ++simulatedTicks;
        if (result.draw)
            accountedTicks += result.simulatedTicksCovered;
    }
    accountedTicks += cadence.PendingSimulationTicks();
    if (!Check(accountedTicks == simulatedTicks,
               "mode changes must neither lose nor duplicate simulation ticks"))
        return 1;
    if (!Check(cadence.SelectEdgeCount() == 11,
               "edge counter must count rising edges independently of mode"))
        return 1;

    th08::psp::ResetRenderCadence(false);
    if (!Check(th08::psp::InitialRenderCadenceMode() ==
                       configuredInitialMode &&
                   th08::psp::CurrentRenderCadenceMode() ==
                       configuredInitialMode &&
                   th08::psp::RenderCadenceSelectEdgeCount() == 0,
               "global scheduler must expose its initial/current/edge state"))
        return 1;
    th08::psp::TickRenderCadence(true);
    if (!Check(th08::psp::CurrentRenderCadenceMode() == firstSelectedMode &&
                   th08::psp::RenderCadenceSelectEdgeCount() == 1,
               "global scheduler must expose SELECT edge changes"))
        return 1;

    std::printf("render-cadence: initial_mode=%u PASS\n",
                static_cast<unsigned int>(configuredInitialMode));
    return 0;
}
