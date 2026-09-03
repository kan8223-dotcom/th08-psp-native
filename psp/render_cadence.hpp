#pragma once

#include <cstdint>

namespace th08::psp
{
std::uint8_t ConfiguredRenderCadenceInitialMode();

struct RenderCadenceTickResult
{
    bool draw;
    bool modeChanged;
    std::uint8_t mode;
    std::uint8_t divisor;
    std::uint8_t simulatedTicksCovered;
};

// PSP presentation policy only.  Every call to Tick represents one canonical
// 60 Hz game tick; SELECT may change how many of those ticks are covered by a
// draw, but never enters the game's input stream or changes simulation order.
class RenderCadence
{
public:
    RenderCadence();

    void Reset(bool selectDown);
    RenderCadenceTickResult Tick(bool selectDown);
    std::uint8_t InitialMode() const;
    std::uint8_t Mode() const;
    std::uint8_t Divisor() const;
    std::uint8_t PendingSimulationTicks() const;
    std::uint32_t SelectEdgeCount() const;

private:
    std::uint32_t selectEdgeCount;
    std::uint8_t initialMode;
    std::uint8_t mode;
    std::uint8_t pendingSimulationTicks;
    bool selectWasDown;
};

void ResetRenderCadence(bool selectDown);
RenderCadenceTickResult TickRenderCadence(bool selectDown);
std::uint8_t InitialRenderCadenceMode();
std::uint8_t CurrentRenderCadenceMode();
std::uint8_t CurrentDrawSimulationTicks();
std::uint32_t RenderCadenceSelectEdgeCount();
} // namespace th08::psp
