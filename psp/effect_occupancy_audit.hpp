#pragma once

#include <cstdint>

// Shadow audit for the PSP Effect occupancy sidecar (TH08_PSP_EFFECT_OCCUPANCY_
// FASTPATH): EffectManager::OnUpdate keeps visiting all 653 slots canonically
// and reports, per parent PERF_ATTR window, how the sidecar bit related to the
// authoritative `active` byte.  A false negative (bit clear, slot active) or a
// cleanup miss (bit clear, slot inactive with dynamic vertices still owned)
// would make the product skip wrong; both must stay at zero.  Mutually
// exclusive with the product switch.
#if defined(PSP) && defined(TH08_PSP_EFFECT_OCCUPANCY_AUDIT) && \
    TH08_PSP_EFFECT_OCCUPANCY_AUDIT
#define TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED 1

namespace th08::psp
{
void EffectOccupancyAuditNoteActive(bool tested);
void EffectOccupancyAuditNoteInactive(bool tested, bool ownsVertices);
void EffectOccupancyStatsResetWindow(bool active);
void EffectOccupancyStatsCancelWindow();
void EffectOccupancyStatsEmitWindow(std::int32_t stage,
                                    std::uint32_t baselineStageFrame,
                                    std::uint32_t stageFrame);
} // namespace th08::psp

#else
#define TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED 0
#endif
