#pragma once

#include <cstdint>
#include <cstring>

namespace th08::psp
{
// IEEE-754 finite test chosen for Allegrex's integer datapath.  For every
// finite magnitude, adding one exponent unit leaves bit 31 clear.  Infinity
// and every NaN wrap into the range with bit 31 set.  Callers may OR several
// results and test bit 31 once.
inline std::uint32_t BulletOnePassFiniteCarry(float value)
{
    static_assert(sizeof(float) == sizeof(std::uint32_t),
                  "Bullet frontend requires binary32 floats");
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7fffffffU) + 0x00800000U;
}

template <typename Vertex>
inline void BulletOnePassTranslateRotation(Vertex *vertex, float localX,
                                            float localY, float sine,
                                            float cosine, float xOffset,
                                            float yOffset)
{
    vertex->pos.x =
        localX * cosine - localY * sine + xOffset;
    vertex->pos.y =
        localX * sine + localY * cosine + yOffset;
}

// Pure geometry shared by the PSP product path and its host harness.  The
// caller supplies a copy of the persistent quad scratch when it needs an
// independent candidate: Draw2D leaves W untouched, and a culled DrawInner
// also leaves diffuse untouched.  This helper therefore writes only the
// fields written by the canonical rotated Draw2D -> DrawInner path.
template <typename Vertex, typename Vm>
inline void BuildBulletOnePassRotatedQuad4V(Vertex *quad, const Vm &vm,
                                             float sine, float cosine,
                                             float shakeX, float shakeY)
{
    const float xOffset = vm.pos.x;
    const float yOffset = vm.pos.y;
    const float x = (vm.spriteSize.x * vm.scale.x) / 2.0f;
    const float y = (vm.spriteSize.y * vm.scale.y) / 2.0f;

    BulletOnePassTranslateRotation(&quad[0], -x, -y, sine, cosine,
                                   xOffset, yOffset);
    BulletOnePassTranslateRotation(&quad[1], x, -y, sine, cosine,
                                   xOffset, yOffset);
    BulletOnePassTranslateRotation(&quad[2], -x, y, sine, cosine,
                                   xOffset, yOffset);
    BulletOnePassTranslateRotation(&quad[3], x, y, sine, cosine,
                                   xOffset, yOffset);

    quad[0].pos.z = quad[1].pos.z =
        quad[2].pos.z = quad[3].pos.z = vm.pos.z;

    if (vm.anchor & 1U)
    {
        quad[0].pos.x += x;
        quad[1].pos.x += x;
        quad[2].pos.x += x;
        quad[3].pos.x += x;
    }
    if (vm.anchor & 2U)
    {
        quad[0].pos.y += y;
        quad[1].pos.y += y;
        quad[2].pos.y += y;
        quad[3].pos.y += y;
    }

    quad[0].pos.x += shakeX;
    quad[0].pos.y += shakeY;
    quad[1].pos.x += shakeX;
    quad[1].pos.y += shakeY;
    quad[2].pos.x += shakeX;
    quad[2].pos.y += shakeY;
    quad[3].pos.x += shakeX;
    quad[3].pos.y += shakeY;

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
inline T BulletOnePassMax(T left, T right)
{
    return left > right ? left : right;
}

template <typename T>
inline T BulletOnePassMin(T left, T right)
{
    return left < right ? left : right;
}

// Keep DrawInner's operand order and inclusive viewport edges.  Inputs are
// required to be finite by the product gate, so no alternate NaN policy can
// enter the fast path.
template <typename Vertex>
inline bool BulletOnePassQuad4VVisible(const Vertex *quad,
                                        float viewportLeft,
                                        float viewportTop,
                                        float viewportRight,
                                        float viewportBottom)
{
    float maxX = BulletOnePassMax(quad[0].pos.x, quad[1].pos.x);
    maxX = BulletOnePassMax(quad[2].pos.x, maxX);
    maxX = BulletOnePassMax(quad[3].pos.x, maxX);
    float maxY = BulletOnePassMax(quad[0].pos.y, quad[1].pos.y);
    maxY = BulletOnePassMax(quad[2].pos.y, maxY);
    maxY = BulletOnePassMax(quad[3].pos.y, maxY);
    float minX = BulletOnePassMin(quad[0].pos.x, quad[1].pos.x);
    minX = BulletOnePassMin(quad[2].pos.x, minX);
    minX = BulletOnePassMin(quad[3].pos.x, minX);
    float minY = BulletOnePassMin(quad[0].pos.y, quad[1].pos.y);
    minY = BulletOnePassMin(quad[2].pos.y, minY);
    minY = BulletOnePassMin(quad[3].pos.y, minY);
    return !(maxX < viewportLeft || maxY < viewportTop ||
             minX > viewportRight || minY > viewportBottom);
}
} // namespace th08::psp
