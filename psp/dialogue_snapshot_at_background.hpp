#pragma once

// TH08_PSP_DIALOGUE_SNAPSHOT_AT_BACKGROUND: the dialogue background snapshot
// is redrawn from Background::OnDraw, at the exact point where the original
// game skips the stage geometry while a conversation is shown (and relies on
// the previous frame's pixels surviving in the D3D backbuffer), instead of at
// BeginScene.  On hardware the BeginScene draw never reached the screen
// (R-045: readback proven non-black, magenta probe invisible).  Default OFF;
// PC oracle never sees it.
#if defined(PSP) && defined(TH08_PSP_DIALOGUE_SNAPSHOT_AT_BACKGROUND) && TH08_PSP_DIALOGUE_SNAPSHOT_AT_BACKGROUND
#define TH08_PSP_DIALOGUE_SNAPSHOT_AT_BACKGROUND_ENABLED 1
#else
#define TH08_PSP_DIALOGUE_SNAPSHOT_AT_BACKGROUND_ENABLED 0
#endif
