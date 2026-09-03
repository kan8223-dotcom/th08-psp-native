#pragma once

#include <cstdint>

#if defined(PSP) && defined(TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT) && \
    TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT
#define TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED 1
#else
#define TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_ATAN2_FASTPATH) && \
    TH08_PSP_ITEM_ATAN2_FASTPATH
#define TH08_PSP_ITEM_ATAN2_FASTPATH_PRODUCT_ENABLED 1
#else
#define TH08_PSP_ITEM_ATAN2_FASTPATH_PRODUCT_ENABLED 0
#endif

#if TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED && \
    TH08_PSP_ITEM_ATAN2_FASTPATH_PRODUCT_ENABLED
#error "ITEM_ATAN2 audit and product switches are mutually exclusive"
#endif

#if TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED || \
    TH08_PSP_ITEM_ATAN2_FASTPATH_PRODUCT_ENABLED
#define TH08_PSP_ITEM_ATAN2_STATS_ENABLED 1

#include "item_atan2_fastpath_math.hpp"

namespace th08::psp
{
// Shadow audit (M0).  ItemManager keeps the canonical
// Player::AngleToPoint call; the audit recomputes the same f32 deltas, runs
// the double-float fast path, compares result bits, records raw-delta and
// angle exact repeats per item slot, and times canonical / fast / velocity on
// every 32nd autocollect call.  Nothing here writes game state.
#if TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED
bool ItemAtan2AuditBeginCall(std::uint64_t *startUs);
void ItemAtan2AuditAfterCanonical(std::uint32_t slot, float playerX,
                                  float playerY, float itemX, float itemY,
                                  float canonicalAngle, bool sampled,
                                  std::uint64_t canonicalStartUs);
std::uint64_t ItemAtan2AuditReadClock();
void ItemAtan2AuditEndVelocity(std::uint64_t startUs);
#endif

// Product counters: one saturating increment per autocollect angle.
void ItemAtan2ProductNote(ItemAtan2FastpathReason reason);

// Window lifecycle owned by the parent PERF_ATTR 600-stage-tick window.
void ItemAtan2StatsResetWindow(bool active);
void ItemAtan2StatsCancelWindow();
void ItemAtan2StatsEmitWindow(std::int32_t stage,
                              std::uint32_t baselineStageFrame,
                              std::uint32_t stageFrame);
} // namespace th08::psp

#else
#define TH08_PSP_ITEM_ATAN2_STATS_ENABLED 0
#endif
