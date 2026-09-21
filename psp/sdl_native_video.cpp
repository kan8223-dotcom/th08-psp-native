#include <SDL.h>
#include <cstddef>

// SDL 2.32.8 src/video/SDL_sysvideo.h bootstrap ABI. Audio, events, image
// decoding and fonts remain SDL-owned; native GE owns the display. Reject
// SDL video initialization and cut SDL_pspvideo -> SDL_pspgl -> EGL linkage.
struct SDL_VideoDevice;
struct NativeOnlyVideoBootstrap
{
    const char *name;
    const char *description;
    SDL_VideoDevice *(*create)();
    int (*showMessageBox)(const SDL_MessageBoxData *, int *);
};
static_assert(SDL_MAJOR_VERSION == 2 && SDL_MINOR_VERSION == 32 && SDL_PATCHLEVEL == 8,
              "Reaudit SDL video bootstrap ABI after SDK upgrade");
static_assert(sizeof(NativeOnlyVideoBootstrap) == 16 &&
              offsetof(NativeOnlyVideoBootstrap, create) == 8 &&
              offsetof(NativeOnlyVideoBootstrap, showMessageBox) == 12,
              "SDL PSP video bootstrap ABI");
static SDL_VideoDevice *NoSdlVideo()
{
    SDL_SetError("TH08 native GE owns the display; SDL video is unavailable");
    return nullptr;
}
extern "C"
{
NativeOnlyVideoBootstrap PSP_bootstrap = {"psp", "TH08 native GE display owner", NoSdlVideo, nullptr};
}
