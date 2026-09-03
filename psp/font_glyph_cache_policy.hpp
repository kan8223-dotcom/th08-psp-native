#pragma once

namespace th08::psp
{
// PSP model 0 is the 32 MiB PSP-1000.  A failed model query is deliberately
// ineligible as well, so the established deterministic flush remains the
// fail-safe behavior.
constexpr bool IsPspFontGlyphCacheRetainModel(int hardwareModel)
{
    return hardwareModel > 0;
}

// Retention is valid only while the same live SDL_ttf face remains the owner
// and its configured point size still matches the descriptor being released.
// A caller must flush/reconfigure when this returns false.
constexpr bool ShouldRetainPspFontGlyphCache(
    bool featureEnabled, int hardwareModel, const void *runtimeOwner,
    int runtimePointSize, const void *descriptorOwner, int descriptorPointSize)
{
    return featureEnabled && IsPspFontGlyphCacheRetainModel(hardwareModel) &&
           runtimeOwner != nullptr && runtimeOwner == descriptorOwner &&
           runtimePointSize > 0 && runtimePointSize == descriptorPointSize;
}
} // namespace th08::psp
