#pragma once
#include <d3d8.h>

#if defined(PSP) && defined(TH08_PSP_NATIVE_GE) && TH08_PSP_NATIVE_GE
#define TH08_PSP_DIALOGUE_TEXT_CACHE_ENABLED 1
namespace th08::psp
{
struct TextRowKey
{
    int x, y, width, height, fontHeight, fontWidth;
    unsigned color, outline, bold;
};
// The stage owns the optional cache. No interpreter/input/replay state is
// changed. The source is already relocated by Gui::LoadMsg.
void DialogueTextSource(const void *source, unsigned bytes, const unsigned *colors);
void ReleaseDialogueTextCache();
void PrewarmDialogueText();
bool DrawCachedText(const TextRowKey &, const char *, IDirect3DTexture8 *);
bool StorePrewarmedText(const TextRowKey &, const char *, IDirect3DTexture8 *,
                        const void *pixels, unsigned width, unsigned height,
                        unsigned pitch, D3DFORMAT format, const RECT &sourceRect);
}
// Native backend: copy an exact final row into the CPU master and update only
// that GE texture band. Other atlas rows and historical pixel packing survive.
bool th08_native_upload_text_row(IDirect3DTexture8 *, const RECT &, const void *,
                                  unsigned pitch, D3DFORMAT);
#else
#define TH08_PSP_DIALOGUE_TEXT_CACHE_ENABLED 0
#endif
