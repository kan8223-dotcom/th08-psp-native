#pragma once
#include <cstddef>
#include <cstring>

#if defined(PSP) && defined(TH08_PSP_PORTRAIT_LOWER_VRAM) && TH08_PSP_PORTRAIT_LOWER_VRAM
#define TH08_PSP_PORTRAIT_LOWER_VRAM_ENABLED 1
#else
#define TH08_PSP_PORTRAIT_LOWER_VRAM_ENABLED 0
#endif

namespace th08 { namespace psp {
// Only the tall stage-boss body, not expressions, names, shared cut-in art,
// player portraits, stage backgrounds or general ANM textures.
inline std::size_t PortraitLowerBytes(const char *owner, unsigned width,
                                      unsigned height, std::size_t available)
{
    if (owner == NULL || std::strncmp(owner, "face_st", 7) != 0 ||
        owner[7] != '0' || owner[8] < '1' || owner[8] > '8')
        return 0;
    const char *suffix = owner + 9;
    if (std::strcmp(suffix, ".anm") != 0 &&
        std::strcmp(suffix, "a.anm") != 0 && std::strcmp(suffix, "b.anm") != 0 &&
        std::strcmp(suffix, "m.anm") != 0 && std::strcmp(suffix, "sp.anm") != 0 &&
        std::strcmp(suffix, "asp.anm") != 0 && std::strcmp(suffix, "bsp.anm") != 0)
        return 0;
    if ((width != 256U && width != 512U) || height != 512U)
        return 0;
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 2U;
    const std::size_t reserve = 256U * 1024U;
    // Avoid an overflow in bytes + reserve / available - reserve.
    return available >= reserve && bytes <= available - reserve ? bytes : 0;
}
} }
