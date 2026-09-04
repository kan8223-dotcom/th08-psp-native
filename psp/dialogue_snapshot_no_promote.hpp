#pragma once

// TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE: the dialogue background snapshot
// (a 512x512 texture captured when a conversation starts and drawn behind it
// every frame) is excluded from the GE4 upper-tier promotion.  On hardware
// every capture was followed by a promotion of that texture and the
// background showed black during dialogue (R-045); PPSSPP has no upper tier
// and never reproduced it.  Pixels of every other texture are unchanged.
// Default OFF; PC oracle never sees it.
#if defined(PSP) && defined(TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE) && TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE
#define TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE_ENABLED 1
#else
#define TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE_ENABLED 0
#endif
