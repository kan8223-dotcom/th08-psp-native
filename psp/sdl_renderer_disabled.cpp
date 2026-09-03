#include <SDL.h>

// TH08 uses SDL only for its PSP OpenGL window, input, image decoding, text
// rasterization and SC audio. It never creates an SDL_Renderer. The stock PSP
// SDL video object nevertheless references the optional window-surface
// renderer, pulling SDL_render_psp.o and its unused 1 MiB static GE list into
// every executable. Provide the official disabled-renderer behavior at the
// archive boundary so the PSPGL path remains unchanged and that object is not
// linked.

namespace
{
int RendererUnavailable()
{
    return SDL_SetError("SDL_Renderer is disabled in the TH08 PSPGL build");
}
} // namespace

extern "C"
{
int SDL_GetNumRenderDrivers(void)
{
    return 0;
}

int SDL_GetRenderDriverInfo(int, SDL_RendererInfo *)
{
    return RendererUnavailable();
}

SDL_Renderer *SDL_CreateRenderer(SDL_Window *, int, Uint32)
{
    RendererUnavailable();
    return nullptr;
}

SDL_Renderer *SDL_GetRenderer(SDL_Window *)
{
    return nullptr;
}

int SDL_GetRendererInfo(SDL_Renderer *, SDL_RendererInfo *)
{
    return RendererUnavailable();
}

SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *, SDL_Surface *)
{
    RendererUnavailable();
    return nullptr;
}

SDL_Texture *SDL_CreateTexture(SDL_Renderer *, Uint32, int, int, int)
{
    RendererUnavailable();
    return nullptr;
}

int SDL_UpdateTexture(SDL_Texture *, const SDL_Rect *, const void *, int)
{
    return RendererUnavailable();
}

void SDL_DestroyTexture(SDL_Texture *)
{
}

int SDL_RenderSetViewport(SDL_Renderer *, const SDL_Rect *)
{
    return RendererUnavailable();
}

int SDL_RenderCopy(SDL_Renderer *, SDL_Texture *, const SDL_Rect *, const SDL_Rect *)
{
    return RendererUnavailable();
}

void SDL_RenderPresent(SDL_Renderer *)
{
    RendererUnavailable();
}

void SDL_DestroyRenderer(SDL_Renderer *)
{
}

void SDL_DestroyRendererWithoutFreeing(SDL_Renderer *)
{
}

int SDL_PSP_RenderGetProp(SDL_Renderer *, int, void **out)
{
    if (out != nullptr)
        *out = nullptr;
    return RendererUnavailable();
}
} // extern "C"
