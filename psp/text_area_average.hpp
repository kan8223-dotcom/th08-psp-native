#pragma once
#include <cstdint>
#include <cstring>

namespace th08::psp
{
// TH07's exact horizontal-2x box filter, specialized for the existing TH08
// 1555 DIB and 1555/4444 atlas. Expand to 8 bits BEFORE averaging, like D3DX;
// averaging the packed 5-bit channels instead changes the rounding.
template<unsigned Rows, bool Output4444>
inline void AverageText5551Row(const unsigned char *source, unsigned sourcePitch,
                              unsigned char *destination, unsigned width)
{
    constexpr unsigned count = Rows * 2;
    for (unsigned x = 0; x < width; ++x)
    {
        unsigned r = 0, g = 0, b = 0, a = 0;
        for (unsigned y = 0; y < Rows; ++y)
            for (unsigned dx = 0; dx < 2; ++dx)
            {
                std::uint16_t p;
                std::memcpy(&p, source + y * sourcePitch + (x * 2 + dx) * 2, 2);
                r += ((p >> 10) & 31) * 255 / 31;
                g += ((p >> 5) & 31) * 255 / 31;
                b += (p & 31) * 255 / 31;
                a += (p & 0x8000) ? 255 : 0;
            }
        const std::uint16_t out = static_cast<std::uint16_t>(Output4444 ?
            (((a / count) >> 4) << 12) | (((r / count) >> 4) << 8) |
            (((g / count) >> 4) << 4) | ((b / count) >> 4) :
            ((a / count >= 128) ? 0x8000 : 0) | (((r / count) * 31 / 255) << 10) |
            (((g / count) * 31 / 255) << 5) | ((b / count) * 31 / 255));
        std::memcpy(destination + x * 2, &out, 2);
    }
}

template<bool Output4444 = false>
inline bool AverageText5551_2x(const unsigned char *source, unsigned sourcePitch,
                              unsigned sourceHeight, unsigned char *destination,
                              unsigned destinationPitch, unsigned width, unsigned height)
{
    // The caller has validated both clipped rectangles and exact 2x width.
    // The narrow ratio covers stock text and bounds every sum. Unknown sizes
    // return before any write, leaving the original generic filter available.
    if (!source || !destination || !width || !height || width > 512 || height > 64 ||
        sourceHeight < height || sourceHeight > height * 3 ||
        sourcePitch < width * 4 || destinationPitch < width * 2) return false;
    for (unsigned y = 0; y < height; ++y)
    {
        const unsigned first = y * sourceHeight / height;
        const unsigned end = ((y + 1) * sourceHeight + height - 1) / height;
        const auto *row = source + first * sourcePitch;
        auto *out = destination + y * destinationPitch;
        switch (end - first)
        {
        case 1: AverageText5551Row<1, Output4444>(row, sourcePitch, out, width); break;
        case 2: AverageText5551Row<2, Output4444>(row, sourcePitch, out, width); break;
        case 3: AverageText5551Row<3, Output4444>(row, sourcePitch, out, width); break;
        case 4: AverageText5551Row<4, Output4444>(row, sourcePitch, out, width); break;
        }
    }
    return true;
}
}
