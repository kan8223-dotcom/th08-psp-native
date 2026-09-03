#pragma once

#include <stdint.h>

namespace th08
{
struct Enemy;
struct EnemyManager;

namespace psp
{

static const int kEnemyActiveBitmapCapacity = 480;
static const int kEnemyActiveBitmapWordCount =
    (kEnemyActiveBitmapCapacity + 31) / 32;

// Integer-only storage shared by the M0 observer and a possible later product
// iterator.  It deliberately lives outside EnemyManager so OFF, AUDIT, and a
// future PRODUCT build keep the same game/replay ABI.
class EnemyActiveBitmapCore
{
public:
    void Reset()
    {
        for (int i = 0; i < kEnemyActiveBitmapWordCount; ++i)
            words_[i] = 0;
    }

    bool Contains(int index) const
    {
        if (index < 0 || index >= kEnemyActiveBitmapCapacity)
            return false;
        return (words_[index >> 5] & (uint32_t(1) << (index & 31))) != 0;
    }

    void Track(int index)
    {
        if (index < 0 || index >= kEnemyActiveBitmapCapacity)
            return;
        words_[index >> 5] |= uint32_t(1) << (index & 31);
    }

    void Untrack(int index)
    {
        if (index < 0 || index >= kEnemyActiveBitmapCapacity)
            return;
        words_[index >> 5] &= ~(uint32_t(1) << (index & 31));
    }

    int NextSetBitAfter(int previousIndex) const
    {
        int index = previousIndex + 1;
        int wordIndex = index >> 5;
        if (index < 0)
        {
            index = 0;
            wordIndex = 0;
        }
        if (wordIndex >= kEnemyActiveBitmapWordCount)
            return -1;

        uint32_t word = words_[wordIndex] & (~uint32_t(0) << (index & 31));
        while (true)
        {
            if (word != 0)
                return (wordIndex << 5) + CountTrailingZeros(word);
            if (++wordIndex >= kEnemyActiveBitmapWordCount)
                return -1;
            word = words_[wordIndex];
        }
    }

private:
    static int CountTrailingZeros(uint32_t value)
    {
#if defined(__GNUC__)
        return __builtin_ctz(value);
#else
        int count = 0;
        while ((value & 1U) == 0U)
        {
            value >>= 1;
            ++count;
        }
        return count;
#endif
    }

    uint32_t words_[kEnemyActiveBitmapWordCount];
};

#if defined(PSP) && defined(TH08_PSP_ENEMY_ACTIVE_BITMAP_AUDIT) && \
    TH08_PSP_ENEMY_ACTIVE_BITMAP_AUDIT
void EnemyActiveBitmapAuditReset(EnemyManager *manager);
void EnemyActiveBitmapAuditTeardown(EnemyManager *manager);
void EnemyActiveBitmapAuditTrack(EnemyManager *manager, int index);
void EnemyActiveBitmapAuditUntrack(EnemyManager *manager, int index);
void EnemyActiveBitmapAuditSyncEnemy(EnemyManager *manager, Enemy *enemy);
void EnemyActiveBitmapAuditBeginFrame(EnemyManager *manager);
void EnemyActiveBitmapAuditObserveCanonicalSlot(EnemyManager *manager,
                                                int index, bool active);
void EnemyActiveBitmapAuditEndFrame(EnemyManager *manager);

#define TH08_PSP_ENEMY_BITMAP_RESET(manager) \
    ::th08::psp::EnemyActiveBitmapAuditReset((manager))
#define TH08_PSP_ENEMY_BITMAP_TEARDOWN(manager) \
    ::th08::psp::EnemyActiveBitmapAuditTeardown((manager))
#define TH08_PSP_ENEMY_BITMAP_TRACK(manager, index) \
    ::th08::psp::EnemyActiveBitmapAuditTrack((manager), (index))
#define TH08_PSP_ENEMY_BITMAP_UNTRACK(manager, index) \
    ::th08::psp::EnemyActiveBitmapAuditUntrack((manager), (index))
#define TH08_PSP_ENEMY_BITMAP_SYNC(manager, enemy) \
    ::th08::psp::EnemyActiveBitmapAuditSyncEnemy((manager), (enemy))
#define TH08_PSP_ENEMY_BITMAP_BEGIN_FRAME(manager) \
    ::th08::psp::EnemyActiveBitmapAuditBeginFrame((manager))
#define TH08_PSP_ENEMY_BITMAP_OBSERVE(manager, index, active) \
    ::th08::psp::EnemyActiveBitmapAuditObserveCanonicalSlot( \
        (manager), (index), (active))
#define TH08_PSP_ENEMY_BITMAP_END_FRAME(manager) \
    ::th08::psp::EnemyActiveBitmapAuditEndFrame((manager))
#else
#define TH08_PSP_ENEMY_BITMAP_RESET(manager) ((void)0)
#define TH08_PSP_ENEMY_BITMAP_TEARDOWN(manager) ((void)0)
#define TH08_PSP_ENEMY_BITMAP_TRACK(manager, index) ((void)0)
#define TH08_PSP_ENEMY_BITMAP_UNTRACK(manager, index) ((void)0)
#define TH08_PSP_ENEMY_BITMAP_SYNC(manager, enemy) ((void)0)
#define TH08_PSP_ENEMY_BITMAP_BEGIN_FRAME(manager) ((void)0)
#define TH08_PSP_ENEMY_BITMAP_OBSERVE(manager, index, active) ((void)0)
#define TH08_PSP_ENEMY_BITMAP_END_FRAME(manager) ((void)0)
#endif

} // namespace psp
} // namespace th08
