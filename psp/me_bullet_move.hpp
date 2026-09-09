#pragma once
// TH08_PSP_ME_BULLET_MOVE: the Media Engine advances the fired bullets whose
// transform program is terminal and idle (position += velocity, off-screen
// bookkeeping, one ANM script step) between the spell-card update and the
// bullet update, while the SC runs the effect and item updates.  The SC loop
// then skips those steps and only runs collision and the side effects.
// ANM scripts that draw a random number are not stepped on the ME (the vm is
// restored) so the RNG sequence stays canonical.
#include <cstdint>

struct __attribute__((aligned(64))) PspMeBulletMoveJob
{
    std::uint32_t first, last;         // bullet slot range [first, last)
    std::uint32_t bulletsAddr;         // Bullet array (plain address)
    std::uint32_t strideBytes;         // sizeof(Bullet)
    std::uint32_t flagsAddr;           // unsigned char[0x600]: 1 moved, 2 deactivate, 4 anm stepped
    std::uint32_t freeze;              // g_GameManager.scriptedUpdateFreeze
    std::uint32_t processed, moved, deactivated, anmDone, anmAborted;
    std::uint32_t addrMask;            // OR-ed into every RAM pointer (0x80000000 on the ME: kseg0 alias)
    std::uint32_t onMe;                // 1: running on the ME (per-bullet cache invalidate/writeback)
    std::uint32_t activeBitsAddr;      // u32[48] occupancy bitmap copy (bit i = slot i), or 0
    std::uint32_t probe;               // 1: count fired bullets through the uncached alias (reachability)
    std::uint32_t scFired;             // SC count at kick (probe frames)
    std::uint32_t meFired;             // ME count (probe frames)
    std::uint32_t meLast;              // header echo from the ME
    // Field offsets inside Bullet (cache maintenance on the ME).
    std::uint32_t offA, lenA;          // position .. offscreenFrames (state, flags, counters)
    std::uint32_t offTransformIndex;
    std::uint32_t offTransforms, transformRecordBytes, offKindInRecord;
    std::uint32_t offLoadedSprite, offCurrentInstruction;
    std::uint32_t offState;
    std::uint32_t pad[6];
};

extern "C"
{
// Runs on the ME (through me_worker) or on the SC when the ME is unavailable.
void th08_me_bullet_move_kernel(PspMeBulletMoveJob *job);
// SC side.
void th08_psp_me_bullet_move_kick(void);               // EffectManager::OnUpdate start
void th08_psp_me_bullet_move_wait(void);               // BulletManager::OnUpdate after the item update
const unsigned char *th08_psp_me_bullet_move_flags(void); // NULL when no job covers this frame
}
