#pragma once

// TH08_PSP_DIALOGUE_LIVE_BACKGROUND: keep drawing the stage background (sky
// layers, stage effect, stage objects) while a conversation is shown, instead
// of freezing the frame captured at the start of the dialogue.  The original
// skips the stage draw during dialogue and relies on the previous frame
// surviving in the D3D backbuffer; on PSP that snapshot also freezes the
// defeat effects captured with it.  The background update never stops during
// dialogue, so drawing it live simply scrolls.  Deliberate visual deviation,
// requested by the maintainer.  Default OFF; PC oracle never sees it.
#if defined(PSP) && defined(TH08_PSP_DIALOGUE_LIVE_BACKGROUND) && TH08_PSP_DIALOGUE_LIVE_BACKGROUND
#define TH08_PSP_DIALOGUE_LIVE_BACKGROUND_ENABLED 1
#else
#define TH08_PSP_DIALOGUE_LIVE_BACKGROUND_ENABLED 0
#endif
