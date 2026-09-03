#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace th08
{
namespace psp
{

// Sum four bytes per load without ever forming an unaligned uint32_t lvalue.
// The byte prefix and tail make the helper correct for every alignment/size;
// the five retail anti-tamper ranges are all aligned and multiples of four.
inline std::uint32_t AntiTamperSwarByteSum(const std::uint8_t *address,
                                          std::size_t size)
{
    std::uint32_t sum = 0;

    while (size != 0U &&
           (reinterpret_cast<std::uintptr_t>(address) & 3U) != 0U)
    {
        sum += *address++;
        --size;
    }

    while (size >= sizeof(std::uint32_t))
    {
        std::uint32_t word;
        std::memcpy(&word, address, sizeof(word));
        const std::uint32_t pairSums =
            (word & 0x00ff00ffU) + ((word >> 8U) & 0x00ff00ffU);
        sum += (pairSums & 0x0000ffffU) + (pairSums >> 16U);
        address += sizeof(word);
        size -= sizeof(word);
    }

    while (size != 0U)
    {
        sum += *address++;
        --size;
    }

    return sum;
}

// CalcChecksum's unsigned destination makes every signed rng8[2] increment
// undergo uint32_t conversion. Repeated addition is therefore exactly this
// multiplication/addition modulo 2^32.
inline std::uint32_t AntiTamperAdvanceValue(std::uint32_t value,
                                            std::int32_t increment,
                                            std::uint32_t byteCount)
{
    return value + static_cast<std::uint32_t>(increment) * byteCount;
}

} // namespace psp
} // namespace th08
