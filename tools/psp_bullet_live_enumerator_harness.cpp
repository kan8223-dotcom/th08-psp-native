#include "PspBulletLiveEnumerator.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <random>
#include <vector>

namespace
{

constexpr unsigned int kSlots =
    th08::psp::PspBulletLiveEnumerator::kSlotCount;
constexpr unsigned int kWords =
    th08::psp::PspBulletLiveEnumerator::kWordCount;

struct Pool
{
    std::array<unsigned int, kWords + 1U> words{};
    std::array<bool, kSlots + 1U> active{};

    void Set(unsigned int slot, bool value)
    {
        assert(slot < kSlots);
        active[slot] = value;
        const unsigned int mask = 1U << (slot & 31U);
        if (value)
            words[slot >> 5U] |= mask;
        else
            words[slot >> 5U] &= ~mask;
    }
};

struct MutationStream
{
    explicit MutationStream(std::uint32_t seed) : rng(seed) {}

    void Apply(Pool &pool, unsigned int current)
    {
        // Include current-slot deactivation and arbitrary ahead/behind writes.
        const unsigned int actionCount = rng() % 5U;
        for (unsigned int action = 0U; action < actionCount; ++action)
            pool.Set(rng() % kSlots, (rng() & 1U) != 0U);
        if ((rng() & 7U) == 0U)
            pool.Set(current, false);
    }

    std::mt19937 rng;
};

std::vector<unsigned int> CanonicalRun(Pool &pool, std::uint32_t mutationSeed)
{
    MutationStream mutations(mutationSeed);
    std::vector<unsigned int> visited;
    for (unsigned int ordinal = 0U; ordinal < kSlots; ++ordinal)
    {
        const unsigned int slot = ordinal == 0U ? 0U : kSlots - ordinal;
        if (!pool.active[slot])
            continue;
        visited.push_back(slot);
        mutations.Apply(pool, slot);
    }
    return visited;
}

std::vector<unsigned int> OptimizedRun(Pool &pool, std::uint32_t mutationSeed,
                                       unsigned int *slotProbes = nullptr,
                                       unsigned int *wordProbes = nullptr)
{
    MutationStream mutations(mutationSeed);
    th08::psp::PspBulletLiveEnumerator enumerator(pool.words.data(), true);
    assert(enumerator.IsUsable());
    std::vector<unsigned int> visited;
    unsigned int localSlotProbes = 0U;
    unsigned int localWordProbes = 0U;
    unsigned int slot = 0U;
    while (enumerator.Next(&slot, &localSlotProbes, &localWordProbes))
    {
        // This is BulletManager's authoritative false-positive repair.  A stale
        // set bit may cost one candidate but may never create a gameplay visit.
        if (!pool.active[slot])
        {
            pool.Set(slot, false);
            continue;
        }
        visited.push_back(slot);
        mutations.Apply(pool, slot);
    }
    if (slotProbes != nullptr)
        *slotProbes = localSlotProbes;
    if (wordProbes != nullptr)
        *wordProbes = localWordProbes;
    return visited;
}

void TestOrderAndBoundaries()
{
    Pool pool;
    for (unsigned int slot : {0U, 1U, 31U, 32U, 33U, 1535U})
        pool.Set(slot, true);
    pool.active[kSlots] = true; // The physical sentinel is not a logical bit.
    pool.words[kWords] = 0xffffffffU; // Out-of-range canary word.

    const std::vector<unsigned int> expected{0U, 1535U, 33U, 32U, 31U, 1U};
    th08::psp::PspBulletLiveEnumerator enumerator(pool.words.data(), true);
    std::vector<unsigned int> actual;
    unsigned int slot = 0U;
    while (enumerator.Next(&slot, nullptr, nullptr))
        actual.push_back(slot);
    assert(actual == expected);
    assert(pool.words[kWords] == 0xffffffffU);
    assert(actual.end() == std::find(actual.begin(), actual.end(), kSlots));
}

void TestSameFrameMutations()
{
    Pool fromZero;
    fromZero.Set(0U, true);
    th08::psp::PspBulletLiveEnumerator zeroEnum(fromZero.words.data(), true);
    unsigned int slot = 0U;
    assert(zeroEnum.Next(&slot, nullptr, nullptr) && slot == 0U);
    fromZero.Set(1535U, true);
    assert(zeroEnum.Next(&slot, nullptr, nullptr) && slot == 1535U);
    assert(!zeroEnum.Next(&slot, nullptr, nullptr));

    Pool aroundCurrent;
    aroundCurrent.Set(100U, true);
    th08::psp::PspBulletLiveEnumerator currentEnum(aroundCurrent.words.data(),
                                                    true);
    assert(currentEnum.Next(&slot, nullptr, nullptr) && slot == 100U);
    aroundCurrent.Set(100U, false); // current deactivation
    aroundCurrent.Set(101U, true);  // behind cursor: canonical already passed it
    aroundCurrent.Set(99U, true);   // ahead of cursor: canonical will see it
    assert(currentEnum.Next(&slot, nullptr, nullptr) && slot == 99U);
    assert(!currentEnum.Next(&slot, nullptr, nullptr));

    Pool lateZero;
    lateZero.Set(1535U, true);
    th08::psp::PspBulletLiveEnumerator lateZeroEnum(lateZero.words.data(),
                                                     true);
    assert(lateZeroEnum.Next(&slot, nullptr, nullptr) && slot == 1535U);
    lateZero.Set(0U, true); // slot 0 was already inspected and is not revisited
    assert(!lateZeroEnum.Next(&slot, nullptr, nullptr));

    Pool boundary;
    boundary.Set(32U, true);
    th08::psp::PspBulletLiveEnumerator boundaryEnum(boundary.words.data(), true);
    assert(boundaryEnum.Next(&slot, nullptr, nullptr) && slot == 32U);
    boundary.Set(33U, true); // behind
    boundary.Set(31U, true); // ahead, across the word boundary
    assert(boundaryEnum.Next(&slot, nullptr, nullptr) && slot == 31U);
    assert(!boundaryEnum.Next(&slot, nullptr, nullptr));
}

void TestFallbackAndProbeBound()
{
    std::array<unsigned int, kWords> words{};
    th08::psp::PspBulletLiveEnumerator unavailable(nullptr, true);
    th08::psp::PspBulletLiveEnumerator invalid(words.data(), false);
    assert(!unavailable.IsUsable());
    assert(!invalid.IsUsable());

    Pool stalePositive;
    stalePositive.words[77U >> 5U] |= 1U << (77U & 31U);
    const auto staleVisited = OptimizedRun(stalePositive, 0x77U);
    assert(staleVisited.empty());
    assert((stalePositive.words[77U >> 5U] & (1U << (77U & 31U))) == 0U);

    Pool empty;
    unsigned int slotProbes = 0U;
    unsigned int wordProbes = 0U;
    const auto visited =
        OptimizedRun(empty, 0x0U, &slotProbes, &wordProbes);
    assert(visited.empty());
    assert(slotProbes == 1U);
    // slot 0 has its own authoritative words[0] load, then the descending
    // search probes all 48 words with bit 0 masked out.
    assert(wordProbes == kWords + 1U);

    Pool dense;
    for (unsigned int slot = 0U; slot < kSlots; ++slot)
        dense.Set(slot, true);
    const auto denseVisited =
        OptimizedRun(dense, 0x13579bdfU, &slotProbes, &wordProbes);
    assert(slotProbes <= kSlots);
    assert(wordProbes <= slotProbes + kWords);
    (void)denseVisited;
}

void TestRandomDifferential()
{
    std::mt19937 setup(0x086024U);
    for (unsigned int trial = 0U; trial < 20000U; ++trial)
    {
        Pool canonical;
        const unsigned int threshold = setup() & 1023U;
        for (unsigned int slot = 0U; slot < kSlots; ++slot)
        {
            if ((setup() & 1023U) < threshold)
                canonical.Set(slot, true);
        }
        Pool optimized = canonical;
        const std::uint32_t mutationSeed = setup();
        const auto canonicalVisited = CanonicalRun(canonical, mutationSeed);
        const auto optimizedVisited = OptimizedRun(optimized, mutationSeed);
        assert(optimizedVisited == canonicalVisited);
        assert(optimized.active == canonical.active);
        assert(optimized.words == canonical.words);
    }
}

} // namespace

int main()
{
    TestOrderAndBoundaries();
    TestSameFrameMutations();
    TestFallbackAndProbeBound();
    TestRandomDifferential();
    return 0;
}
