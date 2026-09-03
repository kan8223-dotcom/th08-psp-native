#include "enemy_active_bitmap_audit.hpp"

#include <cassert>
#include <vector>

static void AssertMatches(const th08::psp::EnemyActiveBitmapCore &bitmap,
                          const bool *reference)
{
    int candidate = bitmap.NextSetBitAfter(-1);
    for (int i = 0; i < th08::psp::kEnemyActiveBitmapCapacity; ++i)
    {
        if (!reference[i])
            continue;
        assert(candidate == i);
        candidate = bitmap.NextSetBitAfter(candidate);
    }
    assert(candidate == -1);
}

int main()
{
    th08::psp::EnemyActiveBitmapCore bitmap;
    bitmap.Reset();
    assert(bitmap.NextSetBitAfter(-1) == -1);

    const int initial[] = {0, 31, 32, 127, 255, 479};
    for (unsigned int i = 0; i < sizeof(initial) / sizeof(initial[0]); ++i)
        bitmap.Track(initial[i]);

    std::vector<int> actual;
    for (int index = bitmap.NextSetBitAfter(-1); index >= 0;
         index = bitmap.NextSetBitAfter(index))
        actual.push_back(index);
    assert(actual.size() == sizeof(initial) / sizeof(initial[0]));
    for (unsigned int i = 0; i < actual.size(); ++i)
        assert(actual[i] == initial[i]);

    bitmap.Untrack(32);
    bitmap.Track(63); // Models a recursive spawn before the next ctz query.
    assert(bitmap.NextSetBitAfter(31) == 63);
    bitmap.Untrack(63); // Models stale-positive repair.
    assert(bitmap.NextSetBitAfter(31) == 127);

    for (int i = 0; i < th08::psp::kEnemyActiveBitmapCapacity; ++i)
        bitmap.Track(i);
    for (int i = 0; i < th08::psp::kEnemyActiveBitmapCapacity; ++i)
        assert(bitmap.NextSetBitAfter(i - 1) == i);
    assert(bitmap.NextSetBitAfter(479) == -1);

    // Deterministic differential churn: the sidecar must stay identical to a
    // canonical 480-boolean membership model through arbitrary slot reuse.
    bitmap.Reset();
    bool reference[th08::psp::kEnemyActiveBitmapCapacity] = {};
    unsigned int state = 0x13579bdfU;
    for (int operation = 0; operation < 20000; ++operation)
    {
        state = state * 1664525U + 1013904223U;
        const int index = static_cast<int>(state % 480U);
        if ((state & 0x80000000U) != 0)
        {
            bitmap.Track(index);
            reference[index] = true;
        }
        else
        {
            bitmap.Untrack(index);
            reference[index] = false;
        }
        if ((operation & 31) == 31)
            AssertMatches(bitmap, reference);
    }
    AssertMatches(bitmap, reference);
    return 0;
}
