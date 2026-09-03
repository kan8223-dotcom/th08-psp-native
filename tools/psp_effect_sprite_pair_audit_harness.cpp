#include "psp/effect_sprite_pair_audit.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
struct Vec2
{
    float x;
    float y;
};

struct Vec3
{
    float x;
    float y;
    float z;
};

struct Sprite
{
    Vec2 uvStart;
    Vec2 uvEnd;
};

struct Vm
{
    Vec3 pos;
    Vec2 spriteSize;
    Vec2 scale;
    Vec2 uvScrollPos;
    std::uint32_t anchor;
    const Sprite *loadedSprite;
};

struct Vertex
{
    Vec3 pos;
    float w;
    std::uint32_t diffuse;
    Vec2 textureUV;
};

static_assert(sizeof(Vertex) == 28U,
              "TH08 Effect frontend vertex stride changed");

void CanonicalBuild(Vertex *quad, const Vm &vm, float shakeX, float shakeY)
{
    const float halfWidth = (vm.spriteSize.x * vm.scale.x) / 2.0f;
    const float halfHeight = (vm.spriteSize.y * vm.scale.y) / 2.0f;
    if ((vm.anchor & 1U) == 0U)
    {
        quad[0].pos.x = quad[2].pos.x = vm.pos.x - halfWidth;
        quad[1].pos.x = quad[3].pos.x = halfWidth + vm.pos.x;
    }
    else
    {
        quad[0].pos.x = quad[2].pos.x = vm.pos.x;
        quad[1].pos.x = quad[3].pos.x =
            halfWidth + vm.pos.x + halfWidth;
    }
    if ((vm.anchor & 2U) == 0U)
    {
        quad[0].pos.y = quad[1].pos.y = vm.pos.y - halfHeight;
        quad[2].pos.y = quad[3].pos.y = halfHeight + vm.pos.y;
    }
    else
    {
        quad[0].pos.y = quad[1].pos.y = vm.pos.y;
        quad[2].pos.y = quad[3].pos.y =
            halfHeight + vm.pos.y + halfHeight;
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
    const float x0 = std::nearbyint(quad[0].pos.x) - 0.5f;
    const float x1 = std::nearbyint(quad[1].pos.x) - 0.5f;
    const float y0 = std::nearbyint(quad[0].pos.y) - 0.5f;
    const float y1 = std::nearbyint(quad[2].pos.y) - 0.5f;
    quad[2].pos.y = quad[3].pos.y = y1;
    quad[0].pos.y = quad[1].pos.y = y0;
    quad[1].pos.x = quad[3].pos.x = x1;
    quad[0].pos.x = quad[2].pos.x = x0;
    quad[0].textureUV.x = quad[2].textureUV.x =
        vm.loadedSprite->uvStart.x + vm.uvScrollPos.x;
    quad[1].textureUV.x = quad[3].textureUV.x =
        vm.loadedSprite->uvEnd.x + vm.uvScrollPos.x;
    quad[0].textureUV.y = quad[1].textureUV.y =
        vm.loadedSprite->uvStart.y + vm.uvScrollPos.y;
    quad[2].textureUV.y = quad[3].textureUV.y =
        vm.loadedSprite->uvEnd.y + vm.uvScrollPos.y;
}

float CanonicalMax(float left, float right)
{
    return left > right ? left : right;
}

float CanonicalMin(float left, float right)
{
    return left < right ? left : right;
}

bool CanonicalVisible(const Vertex *quad)
{
    float maxX = CanonicalMax(quad[0].pos.x, quad[1].pos.x);
    maxX = CanonicalMax(quad[2].pos.x, maxX);
    maxX = CanonicalMax(quad[3].pos.x, maxX);
    float maxY = CanonicalMax(quad[0].pos.y, quad[1].pos.y);
    maxY = CanonicalMax(quad[2].pos.y, maxY);
    maxY = CanonicalMax(quad[3].pos.y, maxY);
    float minX = CanonicalMin(quad[0].pos.x, quad[1].pos.x);
    minX = CanonicalMin(quad[2].pos.x, minX);
    minX = CanonicalMin(quad[3].pos.x, minX);
    float minY = CanonicalMin(quad[0].pos.y, quad[1].pos.y);
    minY = CanonicalMin(quad[2].pos.y, minY);
    minY = CanonicalMin(quad[3].pos.y, minY);
    return !(maxX < 32.0f || maxY < 16.0f ||
             minX > 416.0f || minY > 464.0f);
}

std::uint32_t Next(std::uint32_t *state)
{
    std::uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

float Range(std::uint32_t *state, float magnitude)
{
    return static_cast<float>(static_cast<std::int32_t>(Next(state))) *
           (magnitude / 2147483648.0f);
}

bool CheckGeometry(const Vm &vm, float shakeX, float shakeY,
                   std::uint32_t ordinal)
{
    Vertex seed[4]{};
    for (std::uint32_t index = 0U; index < 4U; ++index)
    {
        seed[index].pos = {900.0f + index, -900.0f - index,
                           20.0f + index};
        seed[index].w = 1.0f;
        seed[index].diffuse = 0xa0503010U;
        seed[index].textureUV = {600.0f + index, -600.0f - index};
    }
    Vertex canonical[4];
    Vertex candidate[4];
    std::memcpy(canonical, seed, sizeof(seed));
    std::memcpy(candidate, seed, sizeof(seed));
    CanonicalBuild(canonical, vm, shakeX, shakeY);
    th08::psp::BuildEffectSpritePairCanonicalQuad(
        candidate, vm, shakeX, shakeY);
    if (std::memcmp(canonical, candidate, sizeof(canonical)) != 0)
    {
        std::fprintf(stderr,
                     "effect-sprite-pair: geometry mismatch sample=%u\n",
                     ordinal);
        return false;
    }
    const bool expectedVisible = CanonicalVisible(canonical);
    const bool actualVisible =
        th08::psp::EffectSpritePairCanonicalQuadVisible(
            candidate, 32.0f, 16.0f, 416.0f, 464.0f);
    if (expectedVisible != actualVisible)
    {
        std::fprintf(stderr,
                     "effect-sprite-pair: cull mismatch sample=%u\n",
                     ordinal);
        return false;
    }
    return true;
}

bool CheckPairRoundTrip()
{
    Vertex quad[4]{};
    quad[0] = {{10.5f, 20.5f, 0.07f}, 1.0f, 0x90abcdefU,
               {0.125f, 0.25f}};
    quad[1] = quad[0];
    quad[1].pos.x = 42.5f;
    quad[1].textureUV.x = 0.875f;
    quad[2] = quad[0];
    quad[2].pos.y = 76.5f;
    quad[2].textureUV.y = 0.75f;
    quad[3] = quad[2];
    quad[3].pos.x = quad[1].pos.x;
    quad[3].textureUV.x = quad[1].textureUV.x;
    if (th08::psp::ClassifyEffectSpritePairQuad(quad) !=
        th08::psp::EffectSpritePairQuadClass::Accept)
    {
        return false;
    }
    Vertex pair[2];
    Vertex reconstructed[4];
    th08::psp::BuildEffectSpritePair(quad, pair);
    th08::psp::ReconstructEffectSpritePairQuad(pair, reconstructed);
    return std::memcmp(quad, reconstructed, sizeof(quad)) == 0;
}

bool CheckRejectClasses()
{
    Vertex quad[4]{};
    quad[0] = {{10.5f, 20.5f, 0.07f}, 1.0f, 0xffffffffU,
               {0.125f, 0.25f}};
    quad[1] = quad[0];
    quad[1].pos.x = 42.5f;
    quad[1].textureUV.x = 0.875f;
    quad[2] = quad[0];
    quad[2].pos.y = 76.5f;
    quad[2].textureUV.y = 0.75f;
    quad[3] = quad[2];
    quad[3].pos.x = quad[1].pos.x;
    quad[3].textureUV.x = quad[1].textureUV.x;

    Vertex changed[4];
    std::memcpy(changed, quad, sizeof(changed));
    changed[2].pos.z = 0.08f;
    if (th08::psp::ClassifyEffectSpritePairQuad(changed) !=
        th08::psp::EffectSpritePairQuadClass::ZOrW)
        return false;
    std::memcpy(changed, quad, sizeof(changed));
    changed[2].diffuse ^= 1U;
    if (th08::psp::ClassifyEffectSpritePairQuad(changed) !=
        th08::psp::EffectSpritePairQuadClass::Diffuse)
        return false;
    std::memcpy(changed, quad, sizeof(changed));
    changed[2].pos.x += 1.0f;
    if (th08::psp::ClassifyEffectSpritePairQuad(changed) !=
        th08::psp::EffectSpritePairQuadClass::Axis)
        return false;
    std::memcpy(changed, quad, sizeof(changed));
    changed[2].textureUV.x += 0.125f;
    if (th08::psp::ClassifyEffectSpritePairQuad(changed) !=
        th08::psp::EffectSpritePairQuadClass::Uv)
        return false;
    std::memcpy(changed, quad, sizeof(changed));
    changed[1].pos.x = changed[0].pos.x;
    changed[3].pos.x = changed[2].pos.x;
    if (th08::psp::ClassifyEffectSpritePairQuad(changed) !=
        th08::psp::EffectSpritePairQuadClass::AreaOrMirror)
        return false;
    std::memcpy(changed, quad, sizeof(changed));
    const std::uint32_t nanBits = 0x7fc01234U;
    std::memcpy(&changed[0].pos.x, &nanBits, sizeof(nanBits));
    return th08::psp::ClassifyEffectSpritePairQuad(changed) ==
           th08::psp::EffectSpritePairQuadClass::Nonfinite;
}
} // namespace

int main()
{
    if (!CheckPairRoundTrip() || !CheckRejectClasses())
    {
        std::fprintf(stderr,
                     "effect-sprite-pair: pair/classification failed\n");
        return 1;
    }

    const std::uint32_t finiteBits[] = {
        0x00000000U, 0x80000000U, 0x00000001U, 0x807fffffU,
        0x3f800000U, 0xbf800000U, 0x7f7fffffU, 0xff7fffffU,
    };
    const std::uint32_t nonfiniteBits[] = {
        0x7f800000U, 0xff800000U, 0x7fc00000U, 0xffc00000U,
        0x7f800001U, 0xff800001U, 0x7fffffffU, 0xffffffffU,
    };
    for (const std::uint32_t bits : finiteBits)
    {
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        if (!th08::psp::EffectSpritePairFinite(value))
            return 1;
    }
    for (const std::uint32_t bits : nonfiniteBits)
    {
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        if (th08::psp::EffectSpritePairFinite(value))
            return 1;
    }

    th08::psp::EffectSpritePairOrderState order{};
    if (!th08::psp::EffectSpritePairNoteOrder(&order, 3U, 0U) ||
        !th08::psp::EffectSpritePairNoteOrder(&order, 8U, 1U) ||
        !th08::psp::EffectSpritePairNoteOrder(&order, 20U, 2U) ||
        th08::psp::EffectSpritePairNoteOrder(&order, 20U, 3U))
    {
        std::fprintf(stderr, "effect-sprite-pair: order gate failed\n");
        return 1;
    }

    Sprite sprite{{0.125f, 0.25f}, {0.875f, 0.75f}};
    std::uint32_t random = 0xd1b54a35U;
    std::uint32_t ordinal = 0U;
    for (std::uint32_t sample = 0U; sample < 250000U; ++sample)
    {
        sprite.uvStart = {Range(&random, 4.0f), Range(&random, 4.0f)};
        sprite.uvEnd = {Range(&random, 4.0f), Range(&random, 4.0f)};
        Vm vm{{Range(&random, 1024.0f), Range(&random, 1024.0f),
               Range(&random, 4.0f)},
              {Range(&random, 256.0f), Range(&random, 256.0f)},
              {Range(&random, 8.0f), Range(&random, 8.0f)},
              {Range(&random, 8.0f), Range(&random, 8.0f)},
              Next(&random) & 3U, &sprite};
        if (!CheckGeometry(vm, Range(&random, 32.0f),
                           Range(&random, 32.0f), ordinal++))
        {
            return 1;
        }
    }

    std::printf("effect-sprite-pair: PASS samples=%u\n", ordinal);
    return 0;
}
