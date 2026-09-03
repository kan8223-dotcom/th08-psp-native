#include "psp/bullet_onepass_4v.hpp"

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
    unsigned int anchor;
    const Sprite *loadedSprite;
};

struct Vertex
{
    Vec3 pos;
    float w;
    std::uint32_t diffuse;
    Vec2 textureUV;
};

static_assert(sizeof(Vertex) == 28, "TH08 frontend vertex stride changed");

void CanonicalTranslate(Vertex *vertex, float x, float y, float sine,
                        float cosine, float xOffset, float yOffset)
{
    vertex->pos.x = x * cosine - y * sine + xOffset;
    vertex->pos.y = x * sine + y * cosine + yOffset;
}

void CanonicalBuild(Vertex *quad, const Vm &vm, float sine, float cosine,
                    float shakeX, float shakeY)
{
    const float xOffset = vm.pos.x;
    const float yOffset = vm.pos.y;
    const float x = (vm.spriteSize.x * vm.scale.x) / 2.0f;
    const float y = (vm.spriteSize.y * vm.scale.y) / 2.0f;
    CanonicalTranslate(&quad[0], -x, -y, sine, cosine, xOffset, yOffset);
    CanonicalTranslate(&quad[1], x, -y, sine, cosine, xOffset, yOffset);
    CanonicalTranslate(&quad[2], -x, y, sine, cosine, xOffset, yOffset);
    CanonicalTranslate(&quad[3], x, y, sine, cosine, xOffset, yOffset);
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

float CanonicalMax(float left, float right)
{
    return left > right ? left : right;
}

float CanonicalMin(float left, float right)
{
    return left < right ? left : right;
}

bool CanonicalVisible(const Vertex *quad, float left, float top, float right,
                      float bottom)
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
    return !(maxX < left || maxY < top || minX > right || minY > bottom);
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
    const std::int32_t signedValue = static_cast<std::int32_t>(Next(state));
    return static_cast<float>(signedValue) *
           (magnitude / 2147483648.0f);
}

bool Check(const Vm &vm, float sine, float cosine, float shakeX,
           float shakeY, std::uint32_t ordinal)
{
    Vertex seed[4]{};
    for (unsigned int index = 0; index < 4U; ++index)
    {
        seed[index].pos = {1000.0f + index, -1000.0f - index,
                           50.0f + index};
        seed[index].w = index == 0U ? -0.0f : 1.0f + index;
        seed[index].diffuse = 0x10203040U + index;
        seed[index].textureUV = {2000.0f + index, -2000.0f - index};
    }
    Vertex canonical[4];
    Vertex candidate[4];
    std::memcpy(canonical, seed, sizeof(seed));
    std::memcpy(candidate, seed, sizeof(seed));
    CanonicalBuild(canonical, vm, sine, cosine, shakeX, shakeY);
    th08::psp::BuildBulletOnePassRotatedQuad4V(
        candidate, vm, sine, cosine, shakeX, shakeY);
    if (std::memcmp(canonical, candidate, sizeof(canonical)) != 0)
    {
        const auto *expected =
            reinterpret_cast<const unsigned char *>(canonical);
        const auto *actual =
            reinterpret_cast<const unsigned char *>(candidate);
        for (std::size_t byte = 0; byte < sizeof(canonical); ++byte)
        {
            if (expected[byte] != actual[byte])
            {
                std::fprintf(stderr,
                             "bullet-onepass-4v: geometry mismatch "
                             "sample=%u byte=%zu expected=%02x actual=%02x\n",
                             ordinal, byte, expected[byte], actual[byte]);
                break;
            }
        }
        return false;
    }
    for (unsigned int index = 0; index < 4U; ++index)
    {
        if (std::memcmp(&candidate[index].w, &seed[index].w,
                        sizeof(candidate[index].w)) != 0 ||
            candidate[index].diffuse != seed[index].diffuse)
        {
            std::fprintf(stderr,
                         "bullet-onepass-4v: persistent scratch changed "
                         "sample=%u vertex=%u\n", ordinal, index);
            return false;
        }
    }
    const bool expectedVisible =
        CanonicalVisible(canonical, 32.0f, 16.0f, 416.0f, 464.0f);
    const bool actualVisible = th08::psp::BulletOnePassQuad4VVisible(
        candidate, 32.0f, 16.0f, 416.0f, 464.0f);
    if (expectedVisible != actualVisible)
    {
        std::fprintf(stderr,
                     "bullet-onepass-4v: cull mismatch sample=%u\n",
                     ordinal);
        return false;
    }
    return true;
}

bool CheckFiniteCarry(std::uint32_t bits, bool expectedFinite)
{
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    const bool actualFinite =
        (th08::psp::BulletOnePassFiniteCarry(value) & 0x80000000U) == 0U;
    if (actualFinite == expectedFinite)
        return true;
    std::fprintf(stderr,
                 "bullet-onepass-4v: finite carry mismatch bits=%08x "
                 "expected=%u actual=%u\n",
                 bits, expectedFinite ? 1U : 0U,
                 actualFinite ? 1U : 0U);
    return false;
}
} // namespace

int main()
{
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
        if (!CheckFiniteCarry(bits, true))
            return 1;
    }
    for (const std::uint32_t bits : nonfiniteBits)
    {
        if (!CheckFiniteCarry(bits, false))
            return 1;
    }

    Sprite sprite{{0.125f, 0.25f}, {0.875f, 0.75f}};
    const float fixed[][8] = {
        {100.25f, 80.75f, 0.05f, 16.0f, 16.0f, 1.0f, 1.0f, 0.0f},
        {-0.0f, 0.0f, 0.05f, -0.0f, 0.0f, -1.0f, 1.0f, 3.0f},
        {32.0f, 16.0f, 0.05f, 64.0f, 128.0f, 0.5f, -2.0f, 1.0f},
        {416.0f, 464.0f, 0.05f, 1.0f, 1.0f, 1.0f, 1.0f, 2.0f},
    };
    std::uint32_t ordinal = 0U;
    for (const auto &values : fixed)
    {
        Vm vm{{values[0], values[1], values[2]},
              {values[3], values[4]},
              {values[5], values[6]},
              {0.03125f, -0.0625f},
              static_cast<unsigned int>(values[7]), &sprite};
        for (unsigned int angle = 0; angle < 8U; ++angle)
        {
            const float radians =
                -3.14159265358979323846f + angle * 0.8975979f;
            if (!Check(vm, std::sin(radians), std::cos(radians),
                       -0.25f, 0.5f, ordinal++))
                return 1;
        }
    }

    std::uint32_t random = 0x6d2b79f5U;
    for (unsigned int sample = 0; sample < 250000U; ++sample)
    {
        sprite.uvStart = {Range(&random, 4.0f), Range(&random, 4.0f)};
        sprite.uvEnd = {Range(&random, 4.0f), Range(&random, 4.0f)};
        Vm vm{{Range(&random, 1024.0f), Range(&random, 1024.0f),
               Range(&random, 4.0f)},
              {Range(&random, 256.0f), Range(&random, 256.0f)},
              {Range(&random, 8.0f), Range(&random, 8.0f)},
              {Range(&random, 8.0f), Range(&random, 8.0f)},
              Next(&random) & 15U, &sprite};
        const float angle = Range(&random, 16.0f);
        if (!Check(vm, std::sin(angle), std::cos(angle),
                   Range(&random, 32.0f), Range(&random, 32.0f), ordinal++))
            return 1;
    }

    Vertex edge[4]{};
    edge[0].pos = edge[1].pos = edge[2].pos = edge[3].pos =
        {31.0f, 16.0f, 0.05f};
    if (th08::psp::BulletOnePassQuad4VVisible(
            edge, 32.0f, 16.0f, 416.0f, 464.0f))
    {
        std::fprintf(stderr,
                     "bullet-onepass-4v: strict outside edge was visible\n");
        return 1;
    }
    edge[0].pos.x = edge[1].pos.x = edge[2].pos.x = edge[3].pos.x = 32.0f;
    if (!th08::psp::BulletOnePassQuad4VVisible(
            edge, 32.0f, 16.0f, 416.0f, 464.0f))
    {
        std::fprintf(stderr,
                     "bullet-onepass-4v: inclusive viewport edge was culled\n");
        return 1;
    }

    std::printf("bullet-onepass-4v: PASS samples=%u\n", ordinal);
    return 0;
}
