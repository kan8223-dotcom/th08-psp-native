#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
uint32_t gState = 0x4d595df4U;

uint32_t NextU32()
{
    uint32_t value = gState;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    gState = value;
    return value;
}

float UnitSigned()
{
    return static_cast<float>(static_cast<int32_t>(NextU32())) /
           2147483648.0f;
}

uint32_t Bits(float value)
{
    uint32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

float CompatibleAtan2(float y, float x)
{
    return static_cast<float>(
        std::atan2(static_cast<double>(y), static_cast<double>(x)));
}

float CompatibleMulSub(float a, float b, float c, float d)
{
    return static_cast<float>(static_cast<double>(a) * static_cast<double>(b) -
                              static_cast<double>(c) * static_cast<double>(d));
}

float CompatibleMulAdd(float a, float b, float c, float d)
{
    return static_cast<float>(static_cast<double>(a) * static_cast<double>(b) +
                              static_cast<double>(c) * static_cast<double>(d));
}
} // namespace

int main()
{
    const uint32_t cases = 1000000U;
    uint32_t atan2fDifferences = 0;
    uint32_t rotateHelperMismatches = 0;
    uint32_t rotateStepwiseDifferences = 0;

    for (uint32_t i = 0; i < cases; ++i)
    {
        const float y = UnitSigned() * 1024.0f;
        const float x = UnitSigned() * 1024.0f;
        const float retailAngle = static_cast<float>(
            std::atan2(static_cast<double>(y), static_cast<double>(x)));
        if (Bits(retailAngle) != Bits(std::atan2(y, x)))
            ++atan2fDifferences;

        const float angle = UnitSigned() * 3.1415927410125732421875f;
        const float pointX = UnitSigned() * 2048.0f;
        const float pointY = UnitSigned() * 2048.0f;
        const float sine = std::sin(angle);
        const float cosine = std::cos(angle);

        const float oracleX = static_cast<float>(
            static_cast<long double>(cosine) * pointX -
            static_cast<long double>(sine) * pointY);
        const float oracleY = static_cast<float>(
            static_cast<long double>(cosine) * pointY +
            static_cast<long double>(sine) * pointX);
        const float helperX = CompatibleMulSub(cosine, pointX, sine, pointY);
        const float helperY = CompatibleMulAdd(cosine, pointY, sine, pointX);
        if (Bits(oracleX) != Bits(helperX) || Bits(oracleY) != Bits(helperY))
            ++rotateHelperMismatches;

        const float stepwiseX = cosine * pointX - sine * pointY;
        const float stepwiseY = cosine * pointY + sine * pointX;
        if (Bits(oracleX) != Bits(stepwiseX) || Bits(oracleY) != Bits(stepwiseY))
            ++rotateStepwiseDifferences;

        if (Bits(retailAngle) != Bits(CompatibleAtan2(y, x)))
        {
            std::fprintf(stderr, "explicit atan2 helper mismatch at case %u\n", i);
            return 1;
        }
    }

    if (rotateHelperMismatches != 0 || atan2fDifferences == 0 ||
        rotateStepwiseDifferences == 0)
    {
        std::fprintf(stderr,
                     "x87 boundary FAIL atan2f=%u rotate_helper=%u rotate_step=%u\n",
                     atan2fDifferences, rotateHelperMismatches,
                     rotateStepwiseDifferences);
        return 1;
    }

    std::printf(
        "x87-angle-rotate: PASS cases=%u atan2f_differences=%u "
        "rotate_helper_mismatches=%u rotate_stepwise_differences=%u\n",
        cases, atan2fDifferences, rotateHelperMismatches,
        rotateStepwiseDifferences);
    return 0;
}
