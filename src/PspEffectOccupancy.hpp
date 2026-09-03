#pragma once

#include <cstdint>
#include <cstring>

namespace th08::psp
{
// TH08 owns 653 live/cleanup Effect slots plus slot 653 as the canonical
// allocation-failure sentinel.  The sidecar covers only the former and never
// changes Effect or EffectManager's replay/original ABI.
constexpr std::uint32_t kPspEffectSlotCount = 653U;
constexpr std::uint32_t kPspEffectOccupancyWordCount =
    (kPspEffectSlotCount + 31U) / 32U;

struct PspEffectOccupancyBits
{
    std::uint32_t words[kPspEffectOccupancyWordCount];

    void Reset()
    {
        std::memset(words, 0, sizeof(words));
    }

    bool Test(std::uint32_t index) const
    {
        return index < kPspEffectSlotCount &&
               (words[index >> 5U] &
                (std::uint32_t{1} << (index & 31U))) != 0U;
    }

    void Mark(std::uint32_t index)
    {
        if (index < kPspEffectSlotCount)
        {
            words[index >> 5U] |=
                std::uint32_t{1} << (index & 31U);
        }
    }

    void Forget(std::uint32_t index)
    {
        if (index < kPspEffectSlotCount)
        {
            words[index >> 5U] &=
                ~(std::uint32_t{1} << (index & 31U));
        }
    }
};

static_assert(kPspEffectOccupancyWordCount == 21U,
              "TH08 Effect occupancy geometry changed");
static_assert(sizeof(PspEffectOccupancyBits) == 84U,
              "TH08 Effect occupancy must stay a compact ABI-external cache");
} // namespace th08::psp
