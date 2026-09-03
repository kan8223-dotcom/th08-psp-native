#pragma once

// Pure, allocation-free PSP Bullet occupancy enumerator.  This header has no
// engine dependencies so the exact traversal and mutation contract can be
// exercised by a host differential harness.
namespace th08::psp
{

class PspBulletLiveEnumerator
{
public:
    static constexpr unsigned int kSlotCount = 0x600U;
    static constexpr unsigned int kWordBits = 32U;
    static constexpr unsigned int kWordCount = kSlotCount / kWordBits;

    PspBulletLiveEnumerator(const volatile unsigned int *activeWords,
                            bool activeWordsValid)
        : words(activeWords), valid(activeWords != nullptr && activeWordsValid),
          slotZeroPending(true), descendingExclusive(kSlotCount)
    {
    }

    bool IsUsable() const
    {
        return valid;
    }

    // Yield the canonical TH08 BulletManager order: slot 0 first, followed by
    // slots 1535..1.  The bitset is deliberately re-read on every call.  A
    // bullet spawned into a not-yet-visited slot is therefore observed in this
    // frame, while a slot activated behind the cursor is not revisited, exactly
    // like the original full scan.
    //
    // slotProbes counts individual occupancy candidates (the special slot-0
    // test plus each yielded set bit).  wordProbes counts volatile bitset word
    // loads.  BulletManager separately counts authoritative live bodies.
    bool Next(unsigned int *outSlot, unsigned int *slotProbes,
              unsigned int *wordProbes)
    {
        if (!valid || outSlot == nullptr)
            return false;

        if (slotZeroPending)
        {
            slotZeroPending = false;
            if (slotProbes != nullptr)
                ++*slotProbes;
            if (wordProbes != nullptr)
                ++*wordProbes;
            if ((words[0] & 1U) != 0U)
            {
                *outSlot = 0U;
                return true;
            }
        }

        if (descendingExclusive <= 1U)
            return false;

        unsigned int candidate = descendingExclusive - 1U;
        unsigned int wordIndex = candidate / kWordBits;
        unsigned int highestBit = candidate % kWordBits;

        for (;;)
        {
            if (wordProbes != nullptr)
                ++*wordProbes;
            unsigned int word = words[wordIndex];
            if (highestBit != kWordBits - 1U)
                word &= (1U << (highestBit + 1U)) - 1U;
            if (wordIndex == 0U)
                word &= ~1U; // slot 0 belongs only to the first special step.

            if (word != 0U)
            {
                const unsigned int bit = MostSignificantSetBit(word);
                const unsigned int slot = wordIndex * kWordBits + bit;
                descendingExclusive = slot;
                if (slotProbes != nullptr)
                    ++*slotProbes;
                *outSlot = slot;
                return true;
            }

            if (wordIndex == 0U)
                break;
            --wordIndex;
            highestBit = kWordBits - 1U;
        }

        descendingExclusive = 1U;
        return false;
    }

private:
    static unsigned int MostSignificantSetBit(unsigned int word)
    {
        // The caller proves word != 0, so __builtin_clz never receives its
        // undefined zero input.  Allegrex GCC lowers this without a slot walk.
        return 31U - static_cast<unsigned int>(__builtin_clz(word));
    }

    const volatile unsigned int *words;
    bool valid;
    bool slotZeroPending;
    unsigned int descendingExclusive;
};

static_assert(sizeof(unsigned int) == 4U,
              "Bullet live enumeration requires 32-bit occupancy words");
static_assert(PspBulletLiveEnumerator::kWordCount == 48U,
              "The 1536-slot Bullet pool must occupy exactly 48 words");

} // namespace th08::psp
