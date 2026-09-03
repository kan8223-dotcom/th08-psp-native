#include "enemy_active_bitmap_audit.hpp"

#if defined(PSP) && defined(TH08_PSP_ENEMY_ACTIVE_BITMAP_AUDIT) && \
    TH08_PSP_ENEMY_ACTIVE_BITMAP_AUDIT

#include "EnemyManager.hpp"
#include "fileio.hpp"

#include <stddef.h>
#include <stdint.h>

namespace th08
{
namespace psp
{
namespace
{

struct EnemyActiveBitmapAuditState
{
    EnemyManager *owner;
    EnemyActiveBitmapCore bitmap;
    int nextCandidate;
    int refreshAfterIndex;
    bool frameOpen;
    bool mismatchFlushed;
    uint32_t frameCanonicalActive;
    uint32_t frameCandidateVisits;
    uint64_t frames;
    uint64_t canonicalActive;
    uint64_t candidateVisits;
    uint64_t matchedActive;
    uint64_t stalePositives;
    uint64_t falseNegatives;
    uint64_t orderMismatches;
    uint64_t invalidPointers;
    uint32_t peakCanonicalActive;
    uint32_t periodicLogs;
};

static const uint32_t kMaxPeriodicLogsPerManagerLifetime = 12;

EnemyActiveBitmapAuditState gEnemyActiveBitmapAudit;

void ClearState(EnemyManager *manager)
{
    gEnemyActiveBitmapAudit.owner = manager;
    gEnemyActiveBitmapAudit.bitmap.Reset();
    gEnemyActiveBitmapAudit.nextCandidate = -1;
    gEnemyActiveBitmapAudit.refreshAfterIndex = -1;
    gEnemyActiveBitmapAudit.frameOpen = false;
    gEnemyActiveBitmapAudit.mismatchFlushed = false;
    gEnemyActiveBitmapAudit.frameCanonicalActive = 0;
    gEnemyActiveBitmapAudit.frameCandidateVisits = 0;
    gEnemyActiveBitmapAudit.frames = 0;
    gEnemyActiveBitmapAudit.canonicalActive = 0;
    gEnemyActiveBitmapAudit.candidateVisits = 0;
    gEnemyActiveBitmapAudit.matchedActive = 0;
    gEnemyActiveBitmapAudit.stalePositives = 0;
    gEnemyActiveBitmapAudit.falseNegatives = 0;
    gEnemyActiveBitmapAudit.orderMismatches = 0;
    gEnemyActiveBitmapAudit.invalidPointers = 0;
    gEnemyActiveBitmapAudit.peakCanonicalActive = 0;
    gEnemyActiveBitmapAudit.periodicLogs = 0;
}

void EnsureOwner(EnemyManager *manager)
{
    if (gEnemyActiveBitmapAudit.owner != manager)
        ClearState(manager);
}

void LogSummary(const char *phase)
{
    BootLog(
        "ENEMY_BITMAP_AUDIT phase=%s frames=%llu canonical=%llu "
        "candidate=%llu matched=%llu peak=%lu stale_positive=%llu "
        "false_negative=%llu order_mismatch=%llu invalid_pointer=%llu "
        "canonical_authoritative=1 product_skip=0\n",
        phase,
        static_cast<unsigned long long>(gEnemyActiveBitmapAudit.frames),
        static_cast<unsigned long long>(gEnemyActiveBitmapAudit.canonicalActive),
        static_cast<unsigned long long>(gEnemyActiveBitmapAudit.candidateVisits),
        static_cast<unsigned long long>(gEnemyActiveBitmapAudit.matchedActive),
        static_cast<unsigned long>(gEnemyActiveBitmapAudit.peakCanonicalActive),
        static_cast<unsigned long long>(gEnemyActiveBitmapAudit.stalePositives),
        static_cast<unsigned long long>(gEnemyActiveBitmapAudit.falseNegatives),
        static_cast<unsigned long long>(gEnemyActiveBitmapAudit.orderMismatches),
        static_cast<unsigned long long>(gEnemyActiveBitmapAudit.invalidPointers));
}

void FailLoudOnce(const char *reason, int index, int candidate)
{
    if (gEnemyActiveBitmapAudit.mismatchFlushed)
        return;
    gEnemyActiveBitmapAudit.mismatchFlushed = true;
    BootLog(
        "ENEMY_BITMAP_AUDIT FAIL-LOUD reason=%s index=%d candidate=%d "
        "frame=%llu canonical_authoritative=1 product_skip=0\n",
        reason, index, candidate,
        static_cast<unsigned long long>(gEnemyActiveBitmapAudit.frames + 1));
    FlushBootLog();
}

void RefreshCandidateAfterProcessedEnemy()
{
    if (gEnemyActiveBitmapAudit.refreshAfterIndex < 0)
        return;
    gEnemyActiveBitmapAudit.nextCandidate =
        gEnemyActiveBitmapAudit.bitmap.NextSetBitAfter(
            gEnemyActiveBitmapAudit.refreshAfterIndex);
    gEnemyActiveBitmapAudit.refreshAfterIndex = -1;
}

} // namespace

void EnemyActiveBitmapAuditReset(EnemyManager *manager)
{
    ClearState(manager);
}

void EnemyActiveBitmapAuditTeardown(EnemyManager *manager)
{
    if (gEnemyActiveBitmapAudit.owner != manager)
        return;
    if (gEnemyActiveBitmapAudit.frames != 0 ||
        gEnemyActiveBitmapAudit.falseNegatives != 0 ||
        gEnemyActiveBitmapAudit.stalePositives != 0)
    {
        LogSummary("teardown");
    }
    ClearState(NULL);
}

void EnemyActiveBitmapAuditTrack(EnemyManager *manager, int index)
{
    EnsureOwner(manager);
    gEnemyActiveBitmapAudit.bitmap.Track(index);
}

void EnemyActiveBitmapAuditUntrack(EnemyManager *manager, int index)
{
    EnsureOwner(manager);
    gEnemyActiveBitmapAudit.bitmap.Untrack(index);
}

void EnemyActiveBitmapAuditSyncEnemy(EnemyManager *manager, Enemy *enemy)
{
    EnsureOwner(manager);
    if (manager == NULL || enemy == NULL || manager->enemies == NULL)
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(&manager->enemies[0]);
    const uintptr_t address = reinterpret_cast<uintptr_t>(enemy);
    const uintptr_t extent =
        static_cast<uintptr_t>(sizeof(Enemy)) * kEnemyActiveBitmapCapacity;
    if (address < base || address >= base + extent ||
        ((address - base) % sizeof(Enemy)) != 0)
    {
        ++gEnemyActiveBitmapAudit.invalidPointers;
        FailLoudOnce("invalid_pointer", -1, -1);
        return;
    }

    const int index = static_cast<int>((address - base) / sizeof(Enemy));
    if ((enemy->flags1 & ENEMY_FLAG_ACTIVE) != 0)
        gEnemyActiveBitmapAudit.bitmap.Track(index);
    else
        gEnemyActiveBitmapAudit.bitmap.Untrack(index);
}

void EnemyActiveBitmapAuditBeginFrame(EnemyManager *manager)
{
    EnsureOwner(manager);
    gEnemyActiveBitmapAudit.nextCandidate =
        gEnemyActiveBitmapAudit.bitmap.NextSetBitAfter(-1);
    gEnemyActiveBitmapAudit.refreshAfterIndex = -1;
    gEnemyActiveBitmapAudit.frameCanonicalActive = 0;
    gEnemyActiveBitmapAudit.frameCandidateVisits = 0;
    gEnemyActiveBitmapAudit.frameOpen = true;
}

void EnemyActiveBitmapAuditObserveCanonicalSlot(EnemyManager *manager,
                                                int index, bool active)
{
    EnsureOwner(manager);
    if (!gEnemyActiveBitmapAudit.frameOpen || index < 0 ||
        index >= kEnemyActiveBitmapCapacity)
    {
        ++gEnemyActiveBitmapAudit.orderMismatches;
        FailLoudOnce("invalid_observe", index,
                     gEnemyActiveBitmapAudit.nextCandidate);
        return;
    }

    // RunEcl for the preceding active enemy may have recursively spawned an
    // enemy.  A future live ctz iterator would ask for its next bit only after
    // that callback returns, so refresh at this exact boundary.
    RefreshCandidateAfterProcessedEnemy();

    while (gEnemyActiveBitmapAudit.nextCandidate >= 0 &&
           gEnemyActiveBitmapAudit.nextCandidate < index)
    {
        const int staleIndex = gEnemyActiveBitmapAudit.nextCandidate;
        ++gEnemyActiveBitmapAudit.frameCandidateVisits;
        ++gEnemyActiveBitmapAudit.stalePositives;
        ++gEnemyActiveBitmapAudit.orderMismatches;
        gEnemyActiveBitmapAudit.bitmap.Untrack(staleIndex);
        FailLoudOnce("candidate_before_canonical", index, staleIndex);
        gEnemyActiveBitmapAudit.nextCandidate =
            gEnemyActiveBitmapAudit.bitmap.NextSetBitAfter(staleIndex);
    }

    const bool tracked = gEnemyActiveBitmapAudit.bitmap.Contains(index);
    if (!active)
    {
        if (tracked)
        {
            ++gEnemyActiveBitmapAudit.frameCandidateVisits;
            ++gEnemyActiveBitmapAudit.stalePositives;
            ++gEnemyActiveBitmapAudit.orderMismatches;
            gEnemyActiveBitmapAudit.bitmap.Untrack(index);
            FailLoudOnce("stale_positive", index,
                         gEnemyActiveBitmapAudit.nextCandidate);
            if (gEnemyActiveBitmapAudit.nextCandidate == index)
            {
                gEnemyActiveBitmapAudit.nextCandidate =
                    gEnemyActiveBitmapAudit.bitmap.NextSetBitAfter(index);
            }
        }
        return;
    }

    ++gEnemyActiveBitmapAudit.frameCanonicalActive;
    if (!tracked)
    {
        ++gEnemyActiveBitmapAudit.falseNegatives;
        ++gEnemyActiveBitmapAudit.orderMismatches;
        FailLoudOnce("false_negative", index,
                     gEnemyActiveBitmapAudit.nextCandidate);
        // Repair only after recording the miss.  The canonical 480-slot loop
        // remains the sole gameplay authority in this M0 generation.
        gEnemyActiveBitmapAudit.bitmap.Track(index);
    }

    if (gEnemyActiveBitmapAudit.nextCandidate == index)
    {
        ++gEnemyActiveBitmapAudit.frameCandidateVisits;
        ++gEnemyActiveBitmapAudit.matchedActive;
    }
    else
    {
        ++gEnemyActiveBitmapAudit.orderMismatches;
        FailLoudOnce("order_or_membership", index,
                     gEnemyActiveBitmapAudit.nextCandidate);
    }

    // Defer the next ctz query until the following canonical slot.  This is
    // what makes recursive spawns during the current Enemy::RunEcl visible.
    gEnemyActiveBitmapAudit.refreshAfterIndex = index;
}

void EnemyActiveBitmapAuditEndFrame(EnemyManager *manager)
{
    EnsureOwner(manager);
    if (!gEnemyActiveBitmapAudit.frameOpen)
        return;

    RefreshCandidateAfterProcessedEnemy();
    if (gEnemyActiveBitmapAudit.nextCandidate >= 0)
    {
        ++gEnemyActiveBitmapAudit.orderMismatches;
        FailLoudOnce("candidate_after_canonical",
                     kEnemyActiveBitmapCapacity,
                     gEnemyActiveBitmapAudit.nextCandidate);
    }

    ++gEnemyActiveBitmapAudit.frames;
    gEnemyActiveBitmapAudit.canonicalActive +=
        gEnemyActiveBitmapAudit.frameCanonicalActive;
    gEnemyActiveBitmapAudit.candidateVisits +=
        gEnemyActiveBitmapAudit.frameCandidateVisits;
    if (gEnemyActiveBitmapAudit.frameCanonicalActive >
        gEnemyActiveBitmapAudit.peakCanonicalActive)
    {
        gEnemyActiveBitmapAudit.peakCanonicalActive =
            gEnemyActiveBitmapAudit.frameCanonicalActive;
    }
    gEnemyActiveBitmapAudit.frameOpen = false;

    // Bound diagnostic I/O even on an all-stage replay: at most twelve
    // periodic lines, one fail-loud line, and one teardown line per manager
    // lifetime.  Logging can perturb timing, so M0 must remain opt-in.
    if ((gEnemyActiveBitmapAudit.frames % 600U) == 0U &&
        gEnemyActiveBitmapAudit.periodicLogs <
            kMaxPeriodicLogsPerManagerLifetime)
    {
        ++gEnemyActiveBitmapAudit.periodicLogs;
        LogSummary("periodic");
    }
}

} // namespace psp
} // namespace th08

#endif
