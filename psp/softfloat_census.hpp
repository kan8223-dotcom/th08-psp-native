#pragma once

#include <cstdint>

// Default-off soft-float census: linker-wrapped libgcc binary64 helpers and
// libm entry points count calls per PERF_ATTR phase and per operation class.
// It is a child of PERF_ATTRIBUTION (same 600-stage-tick windows).  Counters
// are plain increments (no timer, no heap, no per-call log); the wrappers add
// one call frame and a few integer ops per wrapped operation.
#if defined(PSP) && defined(TH08_PSP_SOFTFLOAT_CENSUS) && \
    TH08_PSP_SOFTFLOAT_CENSUS
#define TH08_PSP_SOFTFLOAT_CENSUS_ENABLED 1

namespace th08::psp
{
// Innermost active PerfAttributionScope phase (PerfAttributionPhase value),
// or kSoftfloatPhaseNone outside every scope.  Maintained by the scope
// constructor/destructor in perf_attribution.cpp.
constexpr std::uint8_t kSoftfloatPhaseNone = 22U;
constexpr std::uint8_t kSoftfloatPhaseBuckets = 23U;
extern volatile std::uint8_t gSoftfloatCurrentPhase;

// Window lifecycle owned by the parent PERF_ATTR window.
void SoftfloatCensusResetWindow(bool active);
void SoftfloatCensusCancelWindow();
void SoftfloatCensusEmitWindow(std::int32_t stage,
                               std::uint32_t baselineStageFrame,
                               std::uint32_t stageFrame);
} // namespace th08::psp

#else
#define TH08_PSP_SOFTFLOAT_CENSUS_ENABLED 0
#endif
