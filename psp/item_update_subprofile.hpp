#pragma once

#include <cstdint>

#if defined(PSP) && defined(TH08_PSP_ITEM_UPDATE_SUBPROFILE) && \
    TH08_PSP_ITEM_UPDATE_SUBPROFILE
#define TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED 1

#include "item_update_subprofile_math.hpp"

namespace th08::psp
{
// ItemManager::OnUpdate calls this exactly once per simulation tick before it
// walks the active list.  The returned gate and rotation stay fixed for the
// whole traversal.  A false result means no counter or timer work this tick.
bool ItemUpdateSubprofileBeginTick(std::uint32_t &rotation);
void ItemUpdateSubprofileEndTick(std::uint32_t itemCount,
                                 const ItemUpdateTickCounters &counters);

// Sampled items only.  RecordSection reads the clock once and attributes the
// interval since startUs to one section.
std::uint64_t ItemUpdateSubprofileReadClock();
void ItemUpdateSubprofileRecordSection(ItemUpdateSection section,
                                       std::uint64_t startUs);

// Window lifecycle owned by the parent PERF_ATTR 600-stage-tick window.
// Incomplete/rearmed windows are discarded rather than logged.
void ItemUpdateSubprofileResetWindow(bool active);
void ItemUpdateSubprofileCancelWindow();
void ItemUpdateSubprofileEmitWindow(std::int32_t stage,
                                    std::uint32_t baselineStageFrame,
                                    std::uint32_t stageFrame);
} // namespace th08::psp

#else
#define TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED 0
#endif
