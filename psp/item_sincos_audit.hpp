#pragma once

#include <cstdint>

#if defined(PSP) && defined(TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT) && \
    TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT
#define TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED 1
#else
#define TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_SINCOS_FASTPATH) && \
    TH08_PSP_ITEM_SINCOS_FASTPATH
#define TH08_PSP_ITEM_SINCOS_FASTPATH_PRODUCT_ENABLED 1
#else
#define TH08_PSP_ITEM_SINCOS_FASTPATH_PRODUCT_ENABLED 0
#endif

#if TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED && \
    TH08_PSP_ITEM_SINCOS_FASTPATH_PRODUCT_ENABLED
#error "ITEM_SINCOS audit and product switches are mutually exclusive"
#endif

#if TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED || \
    TH08_PSP_ITEM_SINCOS_FASTPATH_PRODUCT_ENABLED
#define TH08_PSP_ITEM_SINCOS_STATS_ENABLED 1

#include "item_sincos_fastpath_math.hpp"

namespace th08::psp
{
#if TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED
// Shadow audit (M0).  ItemManager keeps the canonical FromAngleMagnitude
// call; the audit runs the double-float fast path on the same angle and
// magnitude, compares both binary32 component bits, and times canonical /
// fast on every 32nd velocity computation.  Nothing here writes game state.
bool ItemSinCosAuditBeginCall(std::uint64_t *startUs);
void ItemSinCosAuditAfterCanonical(float angle, float magnitude,
                                   float canonicalX, float canonicalY,
                                   bool sampled,
                                   std::uint64_t canonicalStartUs);
#endif

// Product counters: one saturating increment per velocity computation.
void ItemSinCosProductNote(ItemSinCosFastpathReason reason);

// Window lifecycle owned by the parent PERF_ATTR 600-stage-tick window.
void ItemSinCosStatsResetWindow(bool active);
void ItemSinCosStatsCancelWindow();
void ItemSinCosStatsEmitWindow(std::int32_t stage,
                               std::uint32_t baselineStageFrame,
                               std::uint32_t stageFrame);
} // namespace th08::psp

#else
#define TH08_PSP_ITEM_SINCOS_STATS_ENABLED 0
#endif
