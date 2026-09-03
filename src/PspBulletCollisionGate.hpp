#pragma once

#include <cmath>

namespace th08::psp
{

// This helper is deliberately independent of Player/Bullet layout so the
// proposed PSP collision gate can be proved with a host differential test.
// It models only a canonical negative result.  Every uncertain input returns
// a fallback decision; no positive collision is ever predicted here.
enum PspBulletCollisionGateDecision
{
    PSP_BULLET_COLLISION_GATE_FALLBACK_CANCEL_UNKNOWN = 0,
    PSP_BULLET_COLLISION_GATE_FALLBACK_SNAPSHOT_INVALID,
    PSP_BULLET_COLLISION_GATE_FALLBACK_BULLET_INVALID,
    PSP_BULLET_COLLISION_GATE_FALLBACK_TOUCH_OR_OVERLAP,
    PSP_BULLET_COLLISION_GATE_CLEAR_GRAZE_SUPPRESSED,
    PSP_BULLET_COLLISION_GATE_CLEAR_GRAZE_SEPARATE,
    PSP_BULLET_COLLISION_GATE_CLEAR_LETHAL_SEPARATE,
};

inline bool PspBulletCollisionSnapshotBoundsValid(
    float hurtLeft, float hurtTop, float hurtRight, float hurtBottom,
    float grazeLeft, float grazeTop, float grazeRight, float grazeBottom)
{
    return std::isfinite(hurtLeft) && std::isfinite(hurtTop) &&
           std::isfinite(hurtRight) && std::isfinite(hurtBottom) &&
           std::isfinite(grazeLeft) && std::isfinite(grazeTop) &&
           std::isfinite(grazeRight) && std::isfinite(grazeBottom) &&
           hurtLeft <= hurtRight && hurtTop <= hurtBottom &&
           grazeLeft <= grazeRight && grazeTop <= grazeBottom;
}

inline bool PspBulletCollisionGateIsClear(
    PspBulletCollisionGateDecision decision)
{
    return decision == PSP_BULLET_COLLISION_GATE_CLEAR_GRAZE_SUPPRESSED ||
           decision == PSP_BULLET_COLLISION_GATE_CLEAR_GRAZE_SEPARATE ||
           decision == PSP_BULLET_COLLISION_GATE_CLEAR_LETHAL_SEPARATE;
}

// `grazePath` is the exact BulletManager branch
// (!isGrazed && activeTimer >= 16).  The two paths are mutually exclusive:
// a zero from CheckGrazeCollision jumps directly to the ANM update, while the
// other branch calls CheckBulletCollision.  Consequently only the bounds used
// by that canonical first call need to prove separation.
inline PspBulletCollisionGateDecision
PspBulletCollisionDefinitelyClear(
    float positionX, float positionY, float sizeX, float sizeY,
    bool grazePath, bool grazeSuppressedByPlayerState,
    bool cancelSetKnownEmpty, bool snapshotBoundsValid,
    float hurtLeft, float hurtTop, float hurtRight, float hurtBottom,
    float grazeLeft, float grazeTop, float grazeRight, float grazeBottom)
{
    if (!cancelSetKnownEmpty)
        return PSP_BULLET_COLLISION_GATE_FALLBACK_CANCEL_UNKNOWN;
    if (!snapshotBoundsValid)
        return PSP_BULLET_COLLISION_GATE_FALLBACK_SNAPSHOT_INVALID;
    if (sizeX < 0.0f || sizeY < 0.0f ||
        !std::isfinite(positionX) || !std::isfinite(positionY) ||
        !std::isfinite(sizeX) || !std::isfinite(sizeY))
    {
        return PSP_BULLET_COLLISION_GATE_FALLBACK_BULLET_INVALID;
    }

    // CheckGrazeCollision computes these values before its player-state test.
    // Reject an overflowing intermediate even though the retail PSP FPU would
    // normally continue, keeping the proposed shortcut strictly fail-closed.
    const float halfX = sizeX / 2.0f;
    const float halfY = sizeY / 2.0f;
    const float bulletLeft = positionX - halfX;
    const float bulletTop = positionY - halfY;
    const float bulletRight = halfX + positionX;
    const float bulletBottom = halfY + positionY;
    if (!std::isfinite(bulletLeft) || !std::isfinite(bulletTop) ||
        !std::isfinite(bulletRight) || !std::isfinite(bulletBottom))
    {
        return PSP_BULLET_COLLISION_GATE_FALLBACK_BULLET_INVALID;
    }

    if (grazePath)
    {
        // Preserve Player::CheckGrazeCollision's source operation order:
        // position - size/2 - 20 and size/2 + position + 20.  Equality remains
        // an overlap because every separation comparison is strict.
        const float expandedLeft = bulletLeft - 20.0f;
        const float expandedTop = bulletTop - 20.0f;
        const float expandedRight = bulletRight + 20.0f;
        const float expandedBottom = bulletBottom + 20.0f;
        if (!std::isfinite(expandedLeft) || !std::isfinite(expandedTop) ||
            !std::isfinite(expandedRight) ||
            !std::isfinite(expandedBottom))
        {
            return PSP_BULLET_COLLISION_GATE_FALLBACK_BULLET_INVALID;
        }

        if (grazeSuppressedByPlayerState)
            return PSP_BULLET_COLLISION_GATE_CLEAR_GRAZE_SUPPRESSED;

        if (grazeLeft > expandedRight || grazeRight < expandedLeft ||
            grazeTop > expandedBottom || grazeBottom < expandedTop)
        {
            return PSP_BULLET_COLLISION_GATE_CLEAR_GRAZE_SEPARATE;
        }
        return PSP_BULLET_COLLISION_GATE_FALLBACK_TOUCH_OR_OVERLAP;
    }

    // Match CheckBulletCollision's x-min, y-min, x-max, y-max comparison
    // order as well as its strict operators.
    if (hurtLeft > bulletRight || hurtTop > bulletBottom ||
        hurtRight < bulletLeft || hurtBottom < bulletTop)
    {
        return PSP_BULLET_COLLISION_GATE_CLEAR_LETHAL_SEPARATE;
    }
    return PSP_BULLET_COLLISION_GATE_FALLBACK_TOUCH_OR_OVERLAP;
}

} // namespace th08::psp
