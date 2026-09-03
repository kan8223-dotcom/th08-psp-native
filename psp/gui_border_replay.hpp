#pragma once

#include <cstdint>

// TH08_PSP_GUI_BORDER_REPLAY: Gui::DrawGameScene draws the 123 static HUD
// border tiles (frontVms[13] x 113, frontVms[14] x 10) through
// DrawNoRotation every presented frame.  Their six-vertex records only
// depend on the two sprites' state, the AnmManager mix/shake state and the
// viewport, so the product switch replays the last canonical append
// (one memcpy plus the first-tile texture/blend state calls) whenever that
// key is unchanged, and re-runs the canonical loop otherwise.  The audit
// switch keeps the canonical loop and compares its output with the replay
// candidate.  Pixels, order and draw-call boundaries are the canonical ones.
#if defined(PSP) && defined(TH08_PSP_GUI_BORDER_REPLAY_AUDIT) && \
    TH08_PSP_GUI_BORDER_REPLAY_AUDIT
#define TH08_PSP_GUI_BORDER_REPLAY_AUDIT_ENABLED 1
#else
#define TH08_PSP_GUI_BORDER_REPLAY_AUDIT_ENABLED 0
#endif
#if defined(PSP) && defined(TH08_PSP_GUI_BORDER_REPLAY) && \
    TH08_PSP_GUI_BORDER_REPLAY
#define TH08_PSP_GUI_BORDER_REPLAY_ENABLED 1
#else
#define TH08_PSP_GUI_BORDER_REPLAY_ENABLED 0
#endif
#if TH08_PSP_GUI_BORDER_REPLAY_AUDIT_ENABLED && TH08_PSP_GUI_BORDER_REPLAY_ENABLED
#error "GUI_BORDER_REPLAY audit and product switches are mutually exclusive"
#endif
#if TH08_PSP_GUI_BORDER_REPLAY_AUDIT_ENABLED || TH08_PSP_GUI_BORDER_REPLAY_ENABLED
#define TH08_PSP_GUI_BORDER_STATS_ENABLED 1
#else
#define TH08_PSP_GUI_BORDER_STATS_ENABLED 0
#endif

#if TH08_PSP_GUI_BORDER_STATS_ENABLED

namespace th08
{
struct AnmVm;
struct AnmManager;
} // namespace th08

namespace th08::psp
{
// Defined in src/AnmManager.cpp (shared sprite batch internals).
bool GuiBorderBatchActive();
bool GuiBorderBatchCanAppend(const AnmManager *manager, unsigned int vertexCount);

// Replaces the three canonical tile loops of Gui::DrawGameScene (same calls,
// same order); vm13/vm14 are &frontVms[13] / &frontVms[14].
void GuiBorderDrawTiles(AnmVm *vm13, AnmVm *vm14);

void GuiBorderStatsResetWindow(bool active);
void GuiBorderStatsCancelWindow();
void GuiBorderStatsEmitWindow(std::int32_t stage, std::uint32_t baselineStageFrame,
                              std::uint32_t stageFrame);
} // namespace th08::psp

#endif
