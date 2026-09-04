#pragma once

// TH08_PSP_DEBUG_START_STAGE: development aid.  When a new game starts from
// the title screen, read <game dir>/TH08PSP_DEBUG_STAGE.txt ("1".."3", "4a",
// "4b", "5", "6a", "6b") and begin at that stage instead of stage 1, so a
// stage-load problem can be reproduced without playing through.  Never ship
// with it on; replays recorded this way are not valid.  Default OFF.
#if defined(PSP) && defined(TH08_PSP_DEBUG_START_STAGE) && TH08_PSP_DEBUG_START_STAGE
#define TH08_PSP_DEBUG_START_STAGE_ENABLED 1
#else
#define TH08_PSP_DEBUG_START_STAGE_ENABLED 0
#endif

namespace th08::psp
{
// Returns the stage index (STAGE1 == 0 .. STAGE6B == 7) or -1 when no override.
int DebugStartStageOverride();
// Title auto-advance: when TH08PSP_DEBUG_STAGE.txt contains "auto", returns
// PSP_CTRL_CROSS on a schedule (tap every ~2 s) until a game has started, so a
// PPSSPP run needs no host keyboard input.  0 otherwise.
unsigned int DebugAutoStartButtons();
} // namespace th08::psp
