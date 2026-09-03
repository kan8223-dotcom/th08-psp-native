#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace th08::psp
{
// A GE_SPRITES submission can represent an axis-aligned quad by its upper-left
// and lower-right vertices only.  M0 and the default-off product share this
// vocabulary so the product cannot silently widen the hardware-proven set.
enum class EffectSpritePairQuadClass : std::uint32_t
{
    Accept = 0,
    Nonfinite,
    ZOrW,
    Diffuse,
    Axis,
    Uv,
    AreaOrMirror,
};

inline std::uint32_t EffectSpritePairFloatBits(float value)
{
    static_assert(sizeof(float) == sizeof(std::uint32_t),
                  "Effect sprite-pair audit requires binary32 floats");
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline bool EffectSpritePairFloatBitsEqual(float left, float right)
{
    return EffectSpritePairFloatBits(left) ==
           EffectSpritePairFloatBits(right);
}

inline bool EffectSpritePairFinite(float value)
{
    // Infinity and NaN are exactly the binary32 encodings with an all-ones
    // exponent.  Avoid a libm call in the PSP audit inner loop.
    return (EffectSpritePairFloatBits(value) & 0x7f800000U) != 0x7f800000U;
}

// Reproduce DrawNoRotation followed by DrawInner(flags=1), including the
// original expression/store order and half-pixel rounding.  The caller seeds
// quad from the persistent canonical scratch: DrawNoRotation never writes W,
// while a culled DrawInner deliberately leaves diffuse untouched.
template <typename Vertex, typename Vm>
inline void BuildEffectSpritePairCanonicalQuad(Vertex *quad, const Vm &vm,
                                                float shakeX, float shakeY)
{
    const float spriteHalfWidth =
        (vm.spriteSize.x * vm.scale.x) / 2.0f;
    const float spriteHalfHeight =
        (vm.spriteSize.y * vm.scale.y) / 2.0f;

    if ((vm.anchor & 1U) == 0U)
    {
        quad[0].pos.x = quad[2].pos.x = vm.pos.x - spriteHalfWidth;
        quad[1].pos.x = quad[3].pos.x = spriteHalfWidth + vm.pos.x;
    }
    else
    {
        quad[0].pos.x = quad[2].pos.x = vm.pos.x;
        quad[1].pos.x = quad[3].pos.x =
            spriteHalfWidth + vm.pos.x + spriteHalfWidth;
    }

    if ((vm.anchor & 2U) == 0U)
    {
        quad[0].pos.y = quad[1].pos.y = vm.pos.y - spriteHalfHeight;
        quad[2].pos.y = quad[3].pos.y = spriteHalfHeight + vm.pos.y;
    }
    else
    {
        quad[0].pos.y = quad[1].pos.y = vm.pos.y;
        quad[2].pos.y = quad[3].pos.y =
            spriteHalfHeight + vm.pos.y + spriteHalfHeight;
    }

    quad[0].pos.z = quad[1].pos.z =
        quad[2].pos.z = quad[3].pos.z = vm.pos.z;

    quad[0].pos.x += shakeX;
    quad[0].pos.y += shakeY;
    quad[1].pos.x += shakeX;
    quad[1].pos.y += shakeY;
    quad[2].pos.x += shakeX;
    quad[2].pos.y += shakeY;
    quad[3].pos.x += shakeX;
    quad[3].pos.y += shakeY;

    const float triangleX1 = std::nearbyint(quad[0].pos.x) - 0.5f;
    const float triangleX2 = std::nearbyint(quad[1].pos.x) - 0.5f;
    const float triangleY1 = std::nearbyint(quad[0].pos.y) - 0.5f;
    const float triangleY2 = std::nearbyint(quad[2].pos.y) - 0.5f;
    quad[2].pos.y = quad[3].pos.y = triangleY2;
    quad[0].pos.y = quad[1].pos.y = triangleY1;
    quad[1].pos.x = quad[3].pos.x = triangleX2;
    quad[0].pos.x = quad[2].pos.x = triangleX1;

    quad[0].textureUV.x = quad[2].textureUV.x =
        vm.loadedSprite->uvStart.x + vm.uvScrollPos.x;
    quad[1].textureUV.x = quad[3].textureUV.x =
        vm.loadedSprite->uvEnd.x + vm.uvScrollPos.x;
    quad[0].textureUV.y = quad[1].textureUV.y =
        vm.loadedSprite->uvStart.y + vm.uvScrollPos.y;
    quad[2].textureUV.y = quad[3].textureUV.y =
        vm.loadedSprite->uvEnd.y + vm.uvScrollPos.y;
}

template <typename T>
inline T EffectSpritePairMax(T left, T right)
{
    return left > right ? left : right;
}

template <typename T>
inline T EffectSpritePairMin(T left, T right)
{
    return left < right ? left : right;
}

template <typename Vertex>
inline bool EffectSpritePairCanonicalQuadVisible(const Vertex *quad,
                                                  float viewportLeft,
                                                  float viewportTop,
                                                  float viewportRight,
                                                  float viewportBottom)
{
    float maxX = EffectSpritePairMax(quad[0].pos.x, quad[1].pos.x);
    maxX = EffectSpritePairMax(quad[2].pos.x, maxX);
    maxX = EffectSpritePairMax(quad[3].pos.x, maxX);
    float maxY = EffectSpritePairMax(quad[0].pos.y, quad[1].pos.y);
    maxY = EffectSpritePairMax(quad[2].pos.y, maxY);
    maxY = EffectSpritePairMax(quad[3].pos.y, maxY);
    float minX = EffectSpritePairMin(quad[0].pos.x, quad[1].pos.x);
    minX = EffectSpritePairMin(quad[2].pos.x, minX);
    minX = EffectSpritePairMin(quad[3].pos.x, minX);
    float minY = EffectSpritePairMin(quad[0].pos.y, quad[1].pos.y);
    minY = EffectSpritePairMin(quad[2].pos.y, minY);
    minY = EffectSpritePairMin(quad[3].pos.y, minY);
    return !(maxX < viewportLeft || maxY < viewportTop ||
             minX > viewportRight || minY > viewportBottom);
}

template <typename Vertex>
inline bool EffectSpritePairQuadFinite(const Vertex *quad)
{
    for (std::uint32_t vertex = 0U; vertex < 4U; ++vertex)
    {
        if (!EffectSpritePairFinite(quad[vertex].pos.x) ||
            !EffectSpritePairFinite(quad[vertex].pos.y) ||
            !EffectSpritePairFinite(quad[vertex].pos.z) ||
            !EffectSpritePairFinite(quad[vertex].w) ||
            !EffectSpritePairFinite(quad[vertex].textureUV.x) ||
            !EffectSpritePairFinite(quad[vertex].textureUV.y))
        {
            return false;
        }
    }
    return true;
}

template <typename Vertex>
inline EffectSpritePairQuadClass ClassifyEffectSpritePairQuad(
    const Vertex *quad)
{
    if (quad == nullptr || !EffectSpritePairQuadFinite(quad))
        return EffectSpritePairQuadClass::Nonfinite;

    if (!EffectSpritePairFloatBitsEqual(quad[0].pos.z, quad[1].pos.z) ||
        !EffectSpritePairFloatBitsEqual(quad[0].pos.z, quad[2].pos.z) ||
        !EffectSpritePairFloatBitsEqual(quad[0].pos.z, quad[3].pos.z) ||
        !EffectSpritePairFloatBitsEqual(quad[0].w, quad[1].w) ||
        !EffectSpritePairFloatBitsEqual(quad[0].w, quad[2].w) ||
        !EffectSpritePairFloatBitsEqual(quad[0].w, quad[3].w) ||
        EffectSpritePairFloatBits(quad[0].w) !=
            EffectSpritePairFloatBits(1.0f))
    {
        return EffectSpritePairQuadClass::ZOrW;
    }
    if (quad[0].diffuse != quad[1].diffuse ||
        quad[0].diffuse != quad[2].diffuse ||
        quad[0].diffuse != quad[3].diffuse)
    {
        return EffectSpritePairQuadClass::Diffuse;
    }
    if (!EffectSpritePairFloatBitsEqual(quad[0].pos.x, quad[2].pos.x) ||
        !EffectSpritePairFloatBitsEqual(quad[1].pos.x, quad[3].pos.x) ||
        !EffectSpritePairFloatBitsEqual(quad[0].pos.y, quad[1].pos.y) ||
        !EffectSpritePairFloatBitsEqual(quad[2].pos.y, quad[3].pos.y))
    {
        return EffectSpritePairQuadClass::Axis;
    }
    if (!EffectSpritePairFloatBitsEqual(quad[0].textureUV.x,
                                        quad[2].textureUV.x) ||
        !EffectSpritePairFloatBitsEqual(quad[1].textureUV.x,
                                        quad[3].textureUV.x) ||
        !EffectSpritePairFloatBitsEqual(quad[0].textureUV.y,
                                        quad[1].textureUV.y) ||
        !EffectSpritePairFloatBitsEqual(quad[2].textureUV.y,
                                        quad[3].textureUV.y))
    {
        return EffectSpritePairQuadClass::Uv;
    }
    if (!(quad[1].pos.x > quad[0].pos.x) ||
        !(quad[2].pos.y > quad[0].pos.y) ||
        !(quad[1].textureUV.x > quad[0].textureUV.x) ||
        !(quad[2].textureUV.y > quad[0].textureUV.y))
    {
        return EffectSpritePairQuadClass::AreaOrMirror;
    }
    return EffectSpritePairQuadClass::Accept;
}

template <typename Vertex>
inline void BuildEffectSpritePair(const Vertex *quad, Vertex *pair)
{
    pair[0] = quad[0];
    pair[1] = quad[3];
}

template <typename Vertex>
inline void ReconstructEffectSpritePairQuad(const Vertex *pair, Vertex *quad)
{
    quad[0] = pair[0];
    quad[3] = pair[1];
    quad[1] = pair[0];
    quad[1].pos.x = pair[1].pos.x;
    quad[1].textureUV.x = pair[1].textureUV.x;
    quad[2] = pair[0];
    quad[2].pos.y = pair[1].pos.y;
    quad[2].textureUV.y = pair[1].textureUV.y;
}

struct EffectSpritePairOrderState
{
    std::uint32_t nextOrdinal = 0U;
    std::uint32_t lastSlot = 0U;
    bool hasLastSlot = false;
};

inline bool EffectSpritePairNoteOrder(EffectSpritePairOrderState *state,
                                      std::uint32_t slot,
                                      std::uint32_t ordinal)
{
    if (state == nullptr)
        return false;
    const bool matches = ordinal == state->nextOrdinal &&
                         (!state->hasLastSlot || slot > state->lastSlot);
    state->nextOrdinal = ordinal + 1U;
    state->lastSlot = slot;
    state->hasLastSlot = true;
    return matches;
}

// DrawInner flushes queued sprites before changing texture or blend mode.
// A live depth-test also makes a Z-write change a boundary: Supervisor's
// SetRenderState first flushes the canonical buffer.  When depth testing is
// disabled, VM-local Z-write is ignored and therefore must not split a run.
struct EffectSpritePairRunKey
{
    std::uintptr_t texture = 0U;
    std::uint32_t blendMode = 0U;
    std::uint32_t zWriteDisabled = 0U;
    std::uint32_t depthTestDisabled = 0U;
};

inline EffectSpritePairRunKey MakeEffectSpritePairRunKey(
    const void *texture, std::uint32_t blendMode,
    std::uint32_t zWriteDisabled, std::uint32_t depthTestDisabled)
{
    EffectSpritePairRunKey key{};
    key.texture = reinterpret_cast<std::uintptr_t>(texture);
    key.blendMode = blendMode;
    key.zWriteDisabled = zWriteDisabled;
    key.depthTestDisabled = depthTestDisabled;
    return key;
}

inline bool EffectSpritePairRunKeysEqual(
    const EffectSpritePairRunKey &left,
    const EffectSpritePairRunKey &right)
{
    return left.texture == right.texture &&
           left.blendMode == right.blendMode &&
           left.depthTestDisabled == right.depthTestDisabled &&
           (left.depthTestDisabled != 0U ||
            left.zWriteDisabled == right.zWriteDisabled);
}
} // namespace th08::psp
