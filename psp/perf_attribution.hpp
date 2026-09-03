#pragma once

#include <cstdint>

#if defined(PSP) && defined(TH08_PSP_PERF_ATTRIBUTION) && \
    TH08_PSP_PERF_ATTRIBUTION
#define TH08_PSP_PERF_ATTRIBUTION_ENABLED 1

namespace th08::psp
{
enum class PerfAttributionPhase : std::uint8_t
{
    CalcChain,
    DrawFrame,
    DrawChain,
    PlayerUpdate,
    PlayerDraw,
    EnemyUpdate,
    EnemyDraw,
    EffectUpdate,
    EffectDrawMain,
    EffectDrawBullet,
    EffectDrawBackground,
    BulletUpdateInclusive,
    BulletDrawInclusive,
    ItemUpdate,
    ItemDraw,
    PresentOuter,
    PresentPreSwap,
    PresentSwap,
    PresentPostSwap,
    GeWaitSwap,
    VblankWaitSwap,
    VblankWaitCadence,
    Count,
};

enum class PerfAttributionWaitContext : std::uint8_t
{
    None,
    Swap,
    Cadence,
};

class PerfAttributionScope
{
public:
    explicit PerfAttributionScope(PerfAttributionPhase phase);
    ~PerfAttributionScope();

    PerfAttributionScope(const PerfAttributionScope &) = delete;
    PerfAttributionScope &operator=(const PerfAttributionScope &) = delete;

private:
    PerfAttributionPhase phase_;
    std::uint64_t startUs_;
    bool active_;
    // Innermost-phase bookkeeping for the optional soft-float census; unused
    // (and never written) when TH08_PSP_SOFTFLOAT_CENSUS is off.
    std::uint8_t previousPhase_;
};

// Context contains no timer call.  The two linker wrappers time only the
// synchronous main-thread waits reached while one of these scopes is active.
class PerfAttributionWaitContextScope
{
public:
    explicit PerfAttributionWaitContextScope(
        PerfAttributionWaitContext context);
    ~PerfAttributionWaitContextScope();

    PerfAttributionWaitContextScope(
        const PerfAttributionWaitContextScope &) = delete;
    PerfAttributionWaitContextScope &operator=(
        const PerfAttributionWaitContextScope &) = delete;

private:
    PerfAttributionWaitContext previous_;
};

// Initialize after FileIoInitialize(), on the thread that runs WinMain.
void PerfAttributionInitialize();

// Called after a completed Present, never on simulation-only cadence ticks.
// Windows are rearmed on stage/frame rewind, replay/demo identity, or cadence
// changes, and emit exactly one compact line per 600 uninterrupted stage ticks.
void PerfAttributionAfterPresent(std::int32_t stage,
                                 std::uint32_t stageFrame,
                                 std::uint8_t cadenceMode,
                                 bool replay,
                                 std::uint8_t demoReplay);
} // namespace th08::psp

#else
#define TH08_PSP_PERF_ATTRIBUTION_ENABLED 0
#endif
