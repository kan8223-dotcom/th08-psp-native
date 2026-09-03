#include "PspBulletCancelSpatial.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

using th08::psp::PspBulletCancelSpatial;

namespace
{
struct Rng
{
    std::uint32_t state = 0x08c0ffeeU;
    std::uint32_t Next()
    {
        state = state * 1664525U + 1013904223U;
        return state;
    }
    float Range(float lo, float hi)
    {
        return lo + (hi - lo) *
            static_cast<float>(Next() & 0xffffU) / 65535.0f;
    }
};

bool ExactCircle(float px, float py, float cx, float cy, float radius)
{
    const float dx = px - cx;
    const float dy = py - cy;
    return dx * dx + dy * dy < radius * radius;
}

bool ExactRect(float px, float py, float cx, float cy,
               float width, float height, float angle)
{
    const float dx = px - cx;
    const float dy = py - cy;
    const float cosine = std::cos(-angle);
    const float sine = std::sin(-angle);
    const float rx = dx * cosine - dy * sine;
    const float ry = dx * sine + dy * cosine;
    return -width * 0.5f <= rx && rx <= width * 0.5f &&
           -height * 0.5f <= ry && ry <= height * 0.5f;
}

bool CoveredOrFallback(const PspBulletCancelSpatial &grid, float x, float y)
{
    bool covered = false;
    return !grid.Query(x, y, &covered) || covered;
}

int Fail(const char *message)
{
    std::fprintf(stderr, "FAIL %s\n", message);
    return 1;
}
} // namespace

int main()
{
    PspBulletCancelSpatial grid;
    grid.Reset();
    grid.Finalize();
    bool covered = true;
    if (!grid.Query(0.0f, 0.0f, &covered) || covered)
        return Fail("empty in-domain cell must be safely rejected");
    if (grid.Query(-128.01f, 0.0f, &covered))
        return Fail("out-of-domain point must request canonical fallback");

    grid.Reset();
    if (!grid.AddCircle(64.0f, 64.0f, 64.0f))
        return Fail("finite circle refused");
    grid.Finalize();
    const float circleBoundary[][2] = {
        {64.0f, 64.0f}, {127.999f, 64.0f},
        {64.0f, 127.999f}, {0.001f, 64.0f},
    };
    for (const auto &point : circleBoundary)
    {
        if (ExactCircle(point[0], point[1], 64.0f, 64.0f, 64.0f) &&
            !CoveredOrFallback(grid, point[0], point[1]))
        {
            return Fail("circle boundary false negative");
        }
    }

    Rng rng;
    for (int shape = 0; shape < 2000; ++shape)
    {
        const float cx = rng.Range(-180.0f, 690.0f);
        const float cy = rng.Range(-180.0f, 690.0f);
        const float width = rng.Range(0.01f, 320.0f);
        const float height = rng.Range(0.01f, 320.0f);
        const float angle = rng.Range(-3.1415927f, 3.1415927f);
        const bool circle = (rng.Next() & 1U) != 0U;
        grid.Reset();
        const bool added = circle
            ? grid.AddCircle(cx, cy, width * 0.5f)
            : grid.AddRotatedRect(cx, cy, width, height, angle);
        if (!added)
            return Fail("finite randomized shape refused");
        grid.Finalize();

        for (int pointIndex = 0; pointIndex < 200; ++pointIndex)
        {
            const float x = rng.Range(-200.0f, 720.0f);
            const float y = rng.Range(-200.0f, 720.0f);
            const bool exact = circle
                ? ExactCircle(x, y, cx, cy, width * 0.5f)
                : ExactRect(x, y, cx, cy, width, height, angle);
            if (exact && !CoveredOrFallback(grid, x, y))
                return Fail("randomized conservative-coverage false negative");
        }
    }

    grid.Reset();
    if (grid.AddCircle(std::numeric_limits<float>::quiet_NaN(),
                       0.0f, 1.0f))
    {
        return Fail("nonfinite geometry must force caller fallback");
    }
    std::puts("PASS psp_bullet_cancel_spatial");
    return 0;
}
