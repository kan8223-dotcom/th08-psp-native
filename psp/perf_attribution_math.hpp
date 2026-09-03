#pragma once

#include <cstdint>

namespace th08::psp
{
struct PerfAttributionDifference
{
    std::uint64_t value;
    bool underflow;
};

// Inclusive callback timers deliberately preserve the original call graph.
// Convert them to disjoint reporting buckets only after the 600-frame window;
// never insert a second traversal or change the measured gameplay path.
constexpr PerfAttributionDifference PerfAttributionSubtract(
    std::uint64_t inclusive, std::uint64_t nested0,
    std::uint64_t nested1 = 0U)
{
    const bool nestedOverflow = nested0 > UINT64_MAX - nested1;
    const std::uint64_t nested = nestedOverflow ? UINT64_MAX
                                                : nested0 + nested1;
    return {inclusive >= nested ? inclusive - nested : 0U,
            nestedOverflow || inclusive < nested};
}
} // namespace th08::psp
