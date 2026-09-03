#pragma once

// Allocation-free conservative coverage for Player cancel regions. The grid
// never owns gameplay state and never answers "hit": an uncovered in-domain
// point is provably clear, while covered/out-of-domain points still execute
// Player's canonical ascending-slot floating-point tests.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace th08::psp
{
class PspBulletCancelSpatial
{
public:
    static constexpr std::int32_t kGridWidth = 12;
    static constexpr std::int32_t kGridHeight = 12;
    static constexpr std::int32_t kCellCount = kGridWidth * kGridHeight;
    static constexpr std::int32_t kWordCount = (kCellCount + 31) / 32;
    static constexpr float kOriginX = -128.0f;
    static constexpr float kOriginY = -128.0f;
    static constexpr float kCellSize = 64.0f;
    static constexpr float kLimitX =
        kOriginX + kCellSize * static_cast<float>(kGridWidth);
    static constexpr float kLimitY =
        kOriginY + kCellSize * static_cast<float>(kGridHeight);

    void Reset()
    {
        std::memset(covered_, 0, sizeof(covered_));
        valid_ = false;
    }

    void CoverEverything()
    {
        std::memset(covered_, 0xff, sizeof(covered_));
        valid_ = true;
    }

    bool AddCircle(float centerX, float centerY, float radius)
    {
        if (!Finite3(centerX, centerY, radius))
            return false;
        const float magnitude = std::fabs(radius);
        return AddAabb(centerX - magnitude, centerY - magnitude,
                       centerX + magnitude, centerY + magnitude);
    }

    bool AddAxisAlignedRect(float centerX, float centerY,
                            float width, float height)
    {
        if (!Finite4(centerX, centerY, width, height))
            return false;
        const float halfWidth = std::fabs(width) * 0.5f;
        const float halfHeight = std::fabs(height) * 0.5f;
        return AddAabb(centerX - halfWidth, centerY - halfHeight,
                       centerX + halfWidth, centerY + halfHeight);
    }

    bool AddRotatedRect(float centerX, float centerY,
                        float width, float height, float angle)
    {
        if (!Finite4(centerX, centerY, width, height) ||
            !std::isfinite(angle))
        {
            return false;
        }
        // |w|/2+|h|/2 is no smaller than the half diagonal. This avoids
        // gameplay trig and deliberately permits false-positive cells.
        const float conservativeRadius =
            (std::fabs(width) + std::fabs(height)) * 0.5f;
        return AddAabb(centerX - conservativeRadius,
                       centerY - conservativeRadius,
                       centerX + conservativeRadius,
                       centerY + conservativeRadius);
    }

    void Finalize() { valid_ = true; }
    void Invalidate() { valid_ = false; }
    bool IsValid() const { return valid_; }

    // Returns false when the query is not safe to index. In that case the
    // caller must execute the canonical full candidate scan.
    bool Query(float x, float y, bool *covered) const
    {
        if (!valid_ || covered == nullptr)
            return false;
        const std::int32_t cell = CellForPoint(x, y);
        if (cell < 0)
            return false;
        *covered =
            (covered_[cell >> 5] & (1U << (cell & 31))) != 0U;
        return true;
    }

    const std::uint32_t *CoverageWords() const { return covered_; }

private:
    static bool Finite3(float a, float b, float c)
    {
        return std::isfinite(a) && std::isfinite(b) && std::isfinite(c);
    }

    static bool Finite4(float a, float b, float c, float d)
    {
        return std::isfinite(a) && std::isfinite(b) &&
               std::isfinite(c) && std::isfinite(d);
    }

    static std::int32_t CellCoordinate(float value, float origin,
                                       float limit, std::int32_t count)
    {
        if (!std::isfinite(value) || value < origin || value >= limit)
            return -1;
        const std::int32_t cell =
            static_cast<std::int32_t>((value - origin) / kCellSize);
        return cell >= 0 && cell < count ? cell : -1;
    }

    static std::int32_t CellForPoint(float x, float y)
    {
        const std::int32_t cellX =
            CellCoordinate(x, kOriginX, kLimitX, kGridWidth);
        const std::int32_t cellY =
            CellCoordinate(y, kOriginY, kLimitY, kGridHeight);
        return cellX < 0 || cellY < 0
                   ? -1
                   : cellY * kGridWidth + cellX;
    }

    bool AddAabb(float minimumX, float minimumY,
                 float maximumX, float maximumY)
    {
        if (!Finite4(minimumX, minimumY, maximumX, maximumY))
            return false;
        if (maximumX < kOriginX || minimumX >= kLimitX ||
            maximumY < kOriginY || minimumY >= kLimitY)
        {
            return true;
        }

        const std::int32_t firstX = minimumX <= kOriginX
                                        ? 0
                                        : static_cast<std::int32_t>(
                                              (minimumX - kOriginX) / kCellSize);
        const std::int32_t firstY = minimumY <= kOriginY
                                        ? 0
                                        : static_cast<std::int32_t>(
                                              (minimumY - kOriginY) / kCellSize);
        // Inclusive maxima intentionally cover the neighbouring cell at an
        // exact boundary. The canonical strict circle/AABB test removes it.
        const std::int32_t lastX = maximumX >= kLimitX
                                       ? kGridWidth - 1
                                       : static_cast<std::int32_t>(
                                             (maximumX - kOriginX) / kCellSize);
        const std::int32_t lastY = maximumY >= kLimitY
                                       ? kGridHeight - 1
                                       : static_cast<std::int32_t>(
                                             (maximumY - kOriginY) / kCellSize);
        if (firstX < 0 || firstY < 0 || lastX < firstX || lastY < firstY ||
            firstX >= kGridWidth || firstY >= kGridHeight)
        {
            return false;
        }
        for (std::int32_t y = firstY; y <= lastY; ++y)
        {
            for (std::int32_t x = firstX; x <= lastX; ++x)
            {
                const std::int32_t cell = y * kGridWidth + x;
                covered_[cell >> 5] |= 1U << (cell & 31);
            }
        }
        return true;
    }

    std::uint32_t covered_[kWordCount];
    bool valid_;
};

static_assert(PspBulletCancelSpatial::kCellCount == 144,
              "TH08 cancel coverage grid geometry changed");
static_assert(PspBulletCancelSpatial::kWordCount == 5,
              "TH08 cancel coverage bitset geometry changed");
} // namespace th08::psp
