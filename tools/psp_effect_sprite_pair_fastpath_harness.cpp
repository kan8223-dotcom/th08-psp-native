#include "psp/effect_sprite_pair_audit.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
enum class EventKind : std::uint32_t
{
    Pair,
    Cull,
    Canonical,
    Boundary,
};

struct Event
{
    EventKind kind;
    th08::psp::EffectSpritePairRunKey key;
    std::uint32_t ordinal;
    std::uint32_t zWrite;
    std::uint32_t depthTestDisabled;
    std::uint32_t mixColor;
};

std::uint32_t Next(std::uint32_t *state)
{
    std::uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

class ProductPlanner
{
public:
    void Consume(const Event &event)
    {
        switch (event.kind)
        {
        case EventKind::Pair:
            if (active_ &&
                !th08::psp::EffectSpritePairRunKeysEqual(key_, event.key))
            {
                Flush();
            }
            if (!active_)
            {
                active_ = true;
                key_ = event.key;
            }
            pending_.push_back(event.ordinal);
            // The key has already split an effective Z-write change. Mix color
            // is per vertex, while the last compatible VM is a valid state
            // representative for submission.
            lastZWrite_ = event.zWrite;
            lastMixColor_ = event.mixColor;
            break;
        case EventKind::Cull:
            ++culls_;
            break;
        case EventKind::Canonical:
            Flush();
            output_.push_back(event.ordinal);
            ++canonicalCalls_;
            break;
        case EventKind::Boundary:
            Flush();
            ++boundaries_;
            break;
        }
    }

    void Finish()
    {
        Flush();
    }

    const std::vector<std::uint32_t> &Output() const
    {
        return output_;
    }

    std::uint32_t CanonicalCalls() const
    {
        return canonicalCalls_;
    }

    std::uint32_t Culls() const
    {
        return culls_;
    }

    std::uint32_t Boundaries() const
    {
        return boundaries_;
    }

    std::uint32_t Runs() const
    {
        return runs_;
    }

private:
    void Flush()
    {
        if (!active_)
            return;
        output_.insert(output_.end(), pending_.begin(), pending_.end());
        pending_.clear();
        active_ = false;
        ++runs_;
    }

    bool active_ = false;
    th08::psp::EffectSpritePairRunKey key_{};
    std::vector<std::uint32_t> pending_;
    std::vector<std::uint32_t> output_;
    std::uint32_t canonicalCalls_ = 0U;
    std::uint32_t culls_ = 0U;
    std::uint32_t boundaries_ = 0U;
    std::uint32_t runs_ = 0U;
    std::uint32_t lastZWrite_ = 0U;
    std::uint32_t lastMixColor_ = 0U;
};

bool CheckKeyContract(std::uint32_t *random, std::uint32_t samples)
{
    for (std::uint32_t sample = 0U; sample < samples; ++sample)
    {
        const std::uintptr_t textureA =
            static_cast<std::uintptr_t>((Next(random) % 31U) + 1U) * 16U;
        const std::uint32_t blendA = Next(random) & 1U;
        const std::uint32_t zWriteA = Next(random) & 1U;
        const std::uint32_t depthA = Next(random) & 1U;
        const bool changeTexture = (Next(random) & 7U) == 0U;
        const bool changeBlend = (Next(random) & 7U) == 0U;
        const bool changeZWrite = (Next(random) & 7U) == 0U;
        const bool changeDepth = (Next(random) & 31U) == 0U;
        const std::uintptr_t textureB = changeTexture
            ? textureA + 16U
            : textureA;
        const std::uint32_t blendB = changeBlend
            ? (blendA ^ 1U)
            : blendA;
        const std::uint32_t zWriteB = changeZWrite
            ? (zWriteA ^ 1U)
            : zWriteA;
        const std::uint32_t depthB = changeDepth
            ? (depthA ^ 1U)
            : depthA;
        const auto keyA = th08::psp::MakeEffectSpritePairRunKey(
            reinterpret_cast<const void *>(textureA), blendA,
            zWriteA, depthA);
        const auto keyB = th08::psp::MakeEffectSpritePairRunKey(
            reinterpret_cast<const void *>(textureB), blendB,
            zWriteB, depthB);
        const bool expected = !changeTexture && !changeBlend &&
            !changeDepth && (depthA != 0U || !changeZWrite);
        if (th08::psp::EffectSpritePairRunKeysEqual(keyA, keyB) != expected)
            return false;

        // Mix color is deliberately absent: it is already baked into each
        // vertex. Z-write only matters while depth testing is live.
        const std::uint32_t ignoredMix = Next(random);
        (void)ignoredMix;
        if (!th08::psp::EffectSpritePairRunKeysEqual(keyA, keyA))
            return false;
    }
    return true;
}

bool CheckRandomOrder(std::uint32_t *random, std::uint32_t samples)
{
    ProductPlanner planner;
    std::vector<std::uint32_t> expected;
    std::uint32_t expectedCanonical = 0U;
    std::uint32_t expectedCulls = 0U;
    std::uint32_t expectedBoundaries = 0U;
    for (std::uint32_t ordinal = 0U; ordinal < samples; ++ordinal)
    {
        const std::uint32_t selector = Next(random) % 20U;
        Event event{};
        if (selector < 13U)
            event.kind = EventKind::Pair;
        else if (selector < 16U)
            event.kind = EventKind::Cull;
        else if (selector < 19U)
            event.kind = EventKind::Canonical;
        else
            event.kind = EventKind::Boundary;
        const std::uintptr_t texture =
            static_cast<std::uintptr_t>((Next(random) % 7U) + 1U) * 16U;
        event.key = th08::psp::MakeEffectSpritePairRunKey(
            reinterpret_cast<const void *>(texture), Next(random) & 1U,
            Next(random) & 1U, Next(random) & 1U);
        event.ordinal = ordinal;
        event.zWrite = event.key.zWriteDisabled;
        event.depthTestDisabled = event.key.depthTestDisabled;
        event.mixColor = Next(random);

        if (event.kind == EventKind::Pair ||
            event.kind == EventKind::Canonical)
        {
            expected.push_back(ordinal);
        }
        if (event.kind == EventKind::Canonical)
            ++expectedCanonical;
        else if (event.kind == EventKind::Cull)
            ++expectedCulls;
        else if (event.kind == EventKind::Boundary)
            ++expectedBoundaries;
        planner.Consume(event);
    }
    planner.Finish();
    if (planner.Output() != expected ||
        planner.CanonicalCalls() != expectedCanonical ||
        planner.Culls() != expectedCulls ||
        planner.Boundaries() != expectedBoundaries)
    {
        return false;
    }

    // Explicitly prove that culls neither split nor enter a visible run, while
    // one canonical fallback and one callback/projected boundary do split it.
    ProductPlanner directed;
    const auto key = th08::psp::MakeEffectSpritePairRunKey(
        reinterpret_cast<const void *>(0x1000U), 0U, 0U, 0U);
    directed.Consume({EventKind::Pair, key, 1U, 0U, 0U, 0x10U});
    directed.Consume({EventKind::Cull, key, 2U, 1U, 0U, 0x20U});
    directed.Consume({EventKind::Pair, key, 3U, 0U, 0U, 0x30U});
    directed.Consume({EventKind::Canonical, key, 4U, 0U, 0U, 0x40U});
    directed.Consume({EventKind::Pair, key, 5U, 0U, 0U, 0x50U});
    directed.Consume({EventKind::Boundary, key, 6U, 0U, 0U, 0x60U});
    directed.Finish();
    const std::vector<std::uint32_t> directedExpected{1U, 3U, 4U, 5U};
    return directed.Output() == directedExpected &&
           directed.CanonicalCalls() == 1U && directed.Culls() == 1U &&
           directed.Boundaries() == 1U && directed.Runs() == 2U;
}
} // namespace

int main()
{
    std::uint32_t random = 0x7f4a7c15U;
    constexpr std::uint32_t keySamples = 250000U;
    constexpr std::uint32_t orderSamples = 250000U;
    if (!CheckKeyContract(&random, keySamples) ||
        !CheckRandomOrder(&random, orderSamples))
    {
        std::fprintf(stderr, "effect-sprite-pair-fastpath: FAIL\n");
        return 1;
    }
    std::printf(
        "effect-sprite-pair-fastpath: PASS key_samples=%u order_samples=%u\n",
        keySamples, orderSamples);
    return 0;
}
