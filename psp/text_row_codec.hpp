#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace th08::psp
{
// Lossless byte RLE: high bit means zero run; otherwise literal run.
// Count is low seven bits + 1. No unaligned integer loads or target pixel math.
inline std::size_t EncodeTextRow(const unsigned char *src, std::size_t bytes,
                                 unsigned char *dst, std::size_t capacity)
{
    std::size_t in = 0, out = 0;
    while (in < bytes)
    {
        const bool zero = src[in] == 0;
        std::size_t count = 1;
        while (count < 128 && in + count < bytes && (src[in + count] == 0) == zero) ++count;
        const std::size_t needed = 1 + (zero ? 0 : count);
        if (needed > capacity - out) return 0;
        dst[out++] = static_cast<unsigned char>((zero ? 128 : 0) | (count - 1));
        if (!zero) { std::memcpy(dst + out, src + in, count); out += count; }
        in += count;
    }
    return out;
}
inline bool DecodeTextRow(const unsigned char *src, std::size_t bytes,
                           unsigned char *dst, std::size_t rawBytes)
{
    std::size_t in = 0, out = 0;
    while (in < bytes)
    {
        const unsigned token = src[in++];
        const std::size_t count = (token & 127U) + 1U;
        if (count > rawBytes - out) return false;
        if (token & 128U) std::memset(dst + out, 0, count);
        else
        {
            if (count > bytes - in) return false;
            std::memcpy(dst + out, src + in, count); in += count;
        }
        out += count;
    }
    return out == rawBytes;
}
}
