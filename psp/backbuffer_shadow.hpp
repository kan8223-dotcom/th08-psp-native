#pragma once
// PSP: transient storage for the game's logical 640x480 backbuffer shadow.
//
// TH08 captures text (stage title, boss name, spell name) and the loading
// transition frame by reading the backbuffer back into a CPU copy.  That copy
// (614,400 B) used to be re-allocated from the 12 MiB renderer arena on every
// capture; once a stage's textures fill the arena the allocation fails and the
// process aborted (R-057 stage 4B).  During gameplay the ANM decode scratch is
// idle, so the shadow (plus the 480x272 native readback buffer) is borrowed
// from it for the duration of one capture and returned immediately after.
#include <cstddef>
namespace th08::psp
{
// Returns scratch memory for a shadow of `bytes`, or nullptr when the scratch
// is busy (ANM decode / dialogue transition) or already lent out.
void *BackbufferShadowAcquire(std::size_t bytes);
// Returns true (and releases the scratch) when `memory` is the lent shadow.
bool BackbufferShadowRelease(void *memory);
// Native 480x272 RGB565 readback buffer that accompanies a scratch-backed
// shadow; nullptr when the shadow is not scratch-backed.
void *BackbufferShadowNativeBuffer();
void *BackbufferShadowBase();
bool BackbufferShadowHeld();
// True when a scratch-backed shadow could be acquired right now.
bool BackbufferShadowAvailable();
} // namespace th08::psp
