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
// Returns a TH_BUTTON_* mask to OR into the frame input (0 = none).
// "auto": tap SHOOT until a game starts.  "menu=replay": walk the title
// menu into Replay -> 4th list entry -> stage select -> mode -> start.
unsigned int DebugAutoStartButtons();
// Debug replay playback: when TH08PSP_DEBUG_STAGE.txt contains
// "replay=<file in ./replay>", the title screen starts that replay once, from
// its first recorded stage, without any input.  NULL otherwise.
const char *DebugReplayAutoStart();
// "replay_stage=N" in the same file: start the auto-started replay at stage N (0-based, 7 = 6B); -1 = first stage.
int DebugReplayAutoStartStage();
// Test async lease memory pressure even when PPSSPP cannot enable GE4 triple.
bool DebugRetainStreamLeaseForTest();
} // namespace th08::psp
