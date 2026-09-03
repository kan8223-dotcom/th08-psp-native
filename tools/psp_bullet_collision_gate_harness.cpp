#include "src/PspBulletCollisionGate.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace
{
using th08::psp::PspBulletCollisionGateDecision;

struct Input
{
    float x;
    float y;
    float sizeX;
    float sizeY;
    bool grazePath;
    bool grazeSuppressed;
    bool cancelKnownEmpty;
    bool snapshotValid;
    float hurtLeft;
    float hurtTop;
    float hurtRight;
    float hurtBottom;
    float grazeLeft;
    float grazeTop;
    float grazeRight;
    float grazeBottom;
};

[[noreturn]] void Fail(const char *message)
{
    std::fprintf(stderr, "bullet-collision-gate: FAIL %s\n", message);
    std::exit(1);
}

PspBulletCollisionGateDecision Decide(const Input &input)
{
    return th08::psp::PspBulletCollisionDefinitelyClear(
        input.x, input.y, input.sizeX, input.sizeY, input.grazePath,
        input.grazeSuppressed, input.cancelKnownEmpty,
        input.snapshotValid, input.hurtLeft, input.hurtTop,
        input.hurtRight, input.hurtBottom, input.grazeLeft,
        input.grazeTop, input.grazeRight, input.grazeBottom);
}

// Independent model of BulletManager's first canonical Player call.  A return
// of -1 represents an input on which the proposed helper must fail closed; it
// is never accepted as proof of a canonical zero.
int CanonicalFirstCall(const Input &input)
{
    if (!input.cancelKnownEmpty || !input.snapshotValid ||
        input.sizeX < 0.0f || input.sizeY < 0.0f ||
        !std::isfinite(input.x) || !std::isfinite(input.y) ||
        !std::isfinite(input.sizeX) || !std::isfinite(input.sizeY))
    {
        return -1;
    }

    const float halfX = input.sizeX / 2.0f;
    const float halfY = input.sizeY / 2.0f;
    const float left = input.x - halfX;
    const float top = input.y - halfY;
    const float right = halfX + input.x;
    const float bottom = halfY + input.y;
    if (!std::isfinite(left) || !std::isfinite(top) ||
        !std::isfinite(right) || !std::isfinite(bottom))
    {
        return -1;
    }

    if (input.grazePath)
    {
        const float expandedLeft = left - 20.0f;
        const float expandedTop = top - 20.0f;
        const float expandedRight = right + 20.0f;
        const float expandedBottom = bottom + 20.0f;
        if (!std::isfinite(expandedLeft) ||
            !std::isfinite(expandedTop) ||
            !std::isfinite(expandedRight) ||
            !std::isfinite(expandedBottom))
        {
            return -1;
        }
        if (input.grazeSuppressed)
            return 0;
        return input.grazeLeft > expandedRight ||
                       input.grazeRight < expandedLeft ||
                       input.grazeTop > expandedBottom ||
                       input.grazeBottom < expandedTop
                   ? 0
                   : 1;
    }

    return input.hurtLeft > right || input.hurtTop > bottom ||
                   input.hurtRight < left || input.hurtBottom < top
               ? 0
               : 1;
}

void RequireSafe(const Input &input)
{
    const PspBulletCollisionGateDecision decision = Decide(input);
    if (th08::psp::PspBulletCollisionGateIsClear(decision) &&
        CanonicalFirstCall(input) != 0)
    {
        Fail("clear decision did not imply canonical zero");
    }
}

std::uint32_t Next(std::uint32_t &state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
} // namespace

int main()
{
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr float max = std::numeric_limits<float>::max();
    const Input base = {0.0f, 0.0f, 4.0f, 2.0f, true, false, true,
                        true, 100.0f, 100.0f, 110.0f, 110.0f,
                        100.0f, 100.0f, 110.0f, 110.0f};

    struct Directed
    {
        const char *name;
        Input input;
        PspBulletCollisionGateDecision expected;
    };
    std::array<Directed, 13> directed{};
    directed[0] = {"far graze", base,
                   th08::psp::PSP_BULLET_COLLISION_GATE_CLEAR_GRAZE_SEPARATE};
    directed[1] = directed[0];
    directed[1].name = "far lethal";
    directed[1].input.grazePath = false;
    directed[1].expected =
        th08::psp::PSP_BULLET_COLLISION_GATE_CLEAR_LETHAL_SEPARATE;
    directed[2] = directed[0];
    directed[2].name = "suppressed graze";
    directed[2].input.grazeSuppressed = true;
    directed[2].input.grazeLeft = -1.0f;
    directed[2].input.grazeTop = -1.0f;
    directed[2].input.grazeRight = 1.0f;
    directed[2].input.grazeBottom = 1.0f;
    directed[2].expected =
        th08::psp::PSP_BULLET_COLLISION_GATE_CLEAR_GRAZE_SUPPRESSED;
    directed[3] = directed[0];
    directed[3].name = "graze equality";
    directed[3].input.grazeLeft = 22.0f;
    directed[3].input.grazeTop = -1.0f;
    directed[3].input.grazeRight = 24.0f;
    directed[3].input.grazeBottom = 1.0f;
    directed[3].expected =
        th08::psp::PSP_BULLET_COLLISION_GATE_FALLBACK_TOUCH_OR_OVERLAP;
    directed[4] = directed[3];
    directed[4].name = "graze strict separation";
    directed[4].input.grazeLeft = 22.001f;
    directed[4].expected =
        th08::psp::PSP_BULLET_COLLISION_GATE_CLEAR_GRAZE_SEPARATE;
    directed[5] = directed[1];
    directed[5].name = "lethal equality";
    directed[5].input.hurtLeft = 2.0f;
    directed[5].input.hurtTop = -1.0f;
    directed[5].input.hurtRight = 4.0f;
    directed[5].input.hurtBottom = 1.0f;
    directed[5].expected =
        th08::psp::PSP_BULLET_COLLISION_GATE_FALLBACK_TOUCH_OR_OVERLAP;
    directed[6] = directed[0];
    directed[6].name = "cancel unknown";
    directed[6].input.cancelKnownEmpty = false;
    directed[6].expected =
        th08::psp::PSP_BULLET_COLLISION_GATE_FALLBACK_CANCEL_UNKNOWN;
    directed[7] = directed[0];
    directed[7].name = "snapshot invalid";
    directed[7].input.snapshotValid = false;
    directed[7].expected =
        th08::psp::PSP_BULLET_COLLISION_GATE_FALLBACK_SNAPSHOT_INVALID;
    directed[8] = directed[0];
    directed[8].name = "negative width";
    directed[8].input.sizeX = -1.0f;
    directed[8].expected =
        th08::psp::PSP_BULLET_COLLISION_GATE_FALLBACK_BULLET_INVALID;
    directed[9] = directed[0];
    directed[9].name = "nan position";
    directed[9].input.x = nan;
    directed[9].expected =
        th08::psp::PSP_BULLET_COLLISION_GATE_FALLBACK_BULLET_INVALID;
    directed[10] = directed[0];
    directed[10].name = "infinite size";
    directed[10].input.sizeY = inf;
    directed[10].expected =
        th08::psp::PSP_BULLET_COLLISION_GATE_FALLBACK_BULLET_INVALID;
    directed[11] = directed[0];
    directed[11].name = "finite overflow";
    directed[11].input.x = max;
    directed[11].input.sizeX = max;
    directed[11].expected =
        th08::psp::PSP_BULLET_COLLISION_GATE_FALLBACK_BULLET_INVALID;
    directed[12] = directed[0];
    directed[12].name = "negative zero valid";
    directed[12].input.x = -0.0f;
    directed[12].input.y = -0.0f;
    directed[12].input.sizeX = -0.0f;
    directed[12].input.sizeY = -0.0f;
    directed[12].expected =
        th08::psp::PSP_BULLET_COLLISION_GATE_CLEAR_GRAZE_SEPARATE;

    for (const Directed &test : directed)
    {
        if (Decide(test.input) != test.expected)
        {
            std::fprintf(stderr, "directed case failed: %s\n", test.name);
            return 2;
        }
        RequireSafe(test.input);
    }

    if (!th08::psp::PspBulletCollisionSnapshotBoundsValid(
            -1.0f, -1.0f, 1.0f, 1.0f,
            -20.0f, -20.0f, 20.0f, 20.0f) ||
        th08::psp::PspBulletCollisionSnapshotBoundsValid(
            1.0f, -1.0f, -1.0f, 1.0f,
            -20.0f, -20.0f, 20.0f, 20.0f) ||
        th08::psp::PspBulletCollisionSnapshotBoundsValid(
            nan, -1.0f, 1.0f, 1.0f,
            -20.0f, -20.0f, 20.0f, 20.0f))
    {
        Fail("snapshot validity gate");
    }

    constexpr std::array<float, 9> values = {
        -512.0f, -64.0f, -22.001f, -22.0f, 0.0f,
        22.0f, 22.001f, 64.0f, 512.0f};
    constexpr std::array<float, 5> sizes = {0.0f, 2.0f, 4.0f, 8.0f, 32.0f};
    std::uint32_t state = 0x8e17a5c3U;
    std::uint32_t checked = 0;
    std::uint32_t clears = 0;
    for (std::uint32_t iteration = 0; iteration < 200000U; ++iteration)
    {
        Input input = base;
        input.x = values[Next(state) % values.size()];
        input.y = values[Next(state) % values.size()];
        input.sizeX = sizes[Next(state) % sizes.size()];
        input.sizeY = sizes[Next(state) % sizes.size()];
        input.grazePath = (Next(state) & 1U) != 0U;
        input.grazeSuppressed = (Next(state) & 7U) == 0U;
        input.cancelKnownEmpty = (Next(state) & 15U) != 0U;

        const float hurtX = values[Next(state) % values.size()];
        const float hurtY = values[Next(state) % values.size()];
        const float hurtW = sizes[Next(state) % sizes.size()];
        const float hurtH = sizes[Next(state) % sizes.size()];
        input.hurtLeft = hurtX - hurtW / 2.0f;
        input.hurtTop = hurtY - hurtH / 2.0f;
        input.hurtRight = hurtW / 2.0f + hurtX;
        input.hurtBottom = hurtH / 2.0f + hurtY;

        const float grazeX = values[Next(state) % values.size()];
        const float grazeY = values[Next(state) % values.size()];
        const float grazeW = sizes[Next(state) % sizes.size()];
        const float grazeH = sizes[Next(state) % sizes.size()];
        input.grazeLeft = grazeX - grazeW / 2.0f;
        input.grazeTop = grazeY - grazeH / 2.0f;
        input.grazeRight = grazeW / 2.0f + grazeX;
        input.grazeBottom = grazeH / 2.0f + grazeY;
        input.snapshotValid =
            th08::psp::PspBulletCollisionSnapshotBoundsValid(
                input.hurtLeft, input.hurtTop, input.hurtRight,
                input.hurtBottom, input.grazeLeft, input.grazeTop,
                input.grazeRight, input.grazeBottom);

        RequireSafe(input);
        clears += th08::psp::PspBulletCollisionGateIsClear(Decide(input))
                      ? 1U
                      : 0U;
        ++checked;
    }

    std::printf("bullet-collision-gate: PASS directed=%zu random=%u clear=%u\n",
                directed.size(), checked, clears);
    return 0;
}
