#include "src/PspEffectOccupancy.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
struct Slot
{
    std::uint8_t active;
    std::uint8_t hasVertices;
    std::uint16_t visits;
};

bool EqualSlots(const Slot *left, const Slot *right)
{
    return std::memcmp(left, right,
                       sizeof(Slot) * th08::psp::kPspEffectSlotCount) == 0;
}

// Model the exact lifetime rule needed by EffectManager::OnUpdate: a callback
// that retires an Effect leaves its bit set until the following canonical
// visit frees optional vertices.  This is deliberately not an "active only"
// bitmap.
void CanonicalFrame(Slot *slots, std::uint32_t spawnFrom,
                    std::uint32_t spawnTo)
{
    for (std::uint32_t index = 0;
         index < th08::psp::kPspEffectSlotCount; ++index)
    {
        Slot &slot = slots[index];
        if (slot.active == 0U)
        {
            slot.hasVertices = 0U;
            continue;
        }
        ++slot.visits;
        if (index == spawnFrom)
            slots[spawnTo].active = 1U;
        if ((index % 11U) == 3U)
            slot.active = 0U;
    }
}

void OccupancyFrame(Slot *slots, th08::psp::PspEffectOccupancyBits *bits,
                    std::uint32_t spawnFrom, std::uint32_t spawnTo)
{
    for (std::uint32_t index = 0;
         index < th08::psp::kPspEffectSlotCount; ++index)
    {
        if (!bits->Test(index))
            continue;
        Slot &slot = slots[index];
        if (slot.active == 0U)
        {
            slot.hasVertices = 0U;
            bits->Forget(index);
            continue;
        }
        ++slot.visits;
        if (index == spawnFrom)
        {
            slots[spawnTo].active = 1U;
            bits->Mark(spawnTo);
        }
        if ((index % 11U) == 3U)
        {
            // Do not clear here: canonical cleanup happens on the next frame.
            slot.active = 0U;
        }
    }
}
} // namespace

int main()
{
    Slot canonical[th08::psp::kPspEffectSlotCount]{};
    Slot product[th08::psp::kPspEffectSlotCount]{};
    th08::psp::PspEffectOccupancyBits bits{};
    bits.Reset();

    const std::uint32_t seeds[] = {0U, 1U, 31U, 32U, 511U, 512U, 639U, 640U, 652U};
    for (std::uint32_t index : seeds)
    {
        canonical[index].active = 1U;
        canonical[index].hasVertices = 1U;
        product[index] = canonical[index];
        bits.Mark(index);
    }

    // A lower-index spawn must wait for the next frame; a higher-index spawn
    // must be observed later in the current ascending traversal.
    CanonicalFrame(canonical, 32U, 600U);
    OccupancyFrame(product, &bits, 32U, 600U);
    if (!EqualSlots(canonical, product) || product[600].visits != 1U)
        return 1;

    canonical[650].active = 1U;
    product[650].active = 1U;
    bits.Mark(650U);
    CanonicalFrame(canonical, 650U, 2U);
    OccupancyFrame(product, &bits, 650U, 2U);
    if (!EqualSlots(canonical, product) || product[2].visits != 0U)
        return 2;

    CanonicalFrame(canonical, 653U, 653U);
    OccupancyFrame(product, &bits, 653U, 653U);
    if (!EqualSlots(canonical, product))
        return 3;

    // The failure sentinel is outside the cache by construction.
    bits.Mark(653U);
    if (bits.Test(653U))
        return 4;

    std::printf("PSP_EFFECT_OCCUPANCY_HARNESS PASS slots=%u words=%u bytes=%zu\n",
                th08::psp::kPspEffectSlotCount,
                th08::psp::kPspEffectOccupancyWordCount, sizeof(bits));
    return 0;
}
