#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace th08::psp
{
constexpr std::size_t kDrawPriorityNamedBinCount = 22U;
constexpr std::size_t kDrawPriorityOtherBin = kDrawPriorityNamedBinCount;
constexpr std::size_t kDrawPriorityBinCount =
    kDrawPriorityNamedBinCount + 1U;

struct DrawPriorityDurationStat
{
    std::uint64_t totalUs;
    std::uint64_t maxUs;
    std::uint32_t calls;
};

struct DrawPriorityDifference
{
    std::uint64_t value;
    bool underflow;
};

// One ordinal is selected from every aligned block of 16 presented frames.
// The selected slot is privately mixed per block rather than being a fixed
// ordinal residue, so a 48-tick animation cannot stay locked to one phase.
constexpr std::uint32_t DrawPriorityPrivateMix(std::uint32_t value)
{
    value += 0x9e3779b9U;
    value ^= value >> 16U;
    value *= 0x85ebca6bU;
    value ^= value >> 13U;
    value *= 0xc2b2ae35U;
    value ^= value >> 16U;
    return value;
}

constexpr std::uint32_t DrawPrioritySampleSlot(std::uint64_t block)
{
    const std::uint32_t folded =
        static_cast<std::uint32_t>(block) ^
        static_cast<std::uint32_t>(block >> 32U) * 0x7feb352dU;
    return DrawPriorityPrivateMix(folded) & 15U;
}

constexpr bool DrawPriorityShouldSampleOrdinal(std::uint64_t ordinal)
{
    return (ordinal & 15U) == DrawPrioritySampleSlot(ordinal >> 4U);
}

constexpr std::size_t DrawPriorityBinFor(int priority)
{
    return priority >= 0 &&
                   priority < static_cast<int>(kDrawPriorityNamedBinCount)
               ? static_cast<std::size_t>(priority)
               : kDrawPriorityOtherBin;
}

inline bool DrawPriorityAccumulate(DrawPriorityDurationStat &stat,
                                   std::uint64_t durationUs)
{
    const bool totalOverflow =
        durationUs > std::numeric_limits<std::uint64_t>::max() - stat.totalUs;
    stat.totalUs = totalOverflow
                       ? std::numeric_limits<std::uint64_t>::max()
                       : stat.totalUs + durationUs;
    if (durationUs > stat.maxUs)
        stat.maxUs = durationUs;
    if (stat.calls != std::numeric_limits<std::uint32_t>::max())
        ++stat.calls;
    return totalOverflow;
}

constexpr DrawPriorityDifference DrawPrioritySubtract(
    std::uint64_t inclusive, std::uint64_t nested)
{
    return {inclusive >= nested ? inclusive - nested : 0U,
            inclusive < nested};
}
} // namespace th08::psp
