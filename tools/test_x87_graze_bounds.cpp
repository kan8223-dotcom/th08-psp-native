#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if !defined(__i386__) && !defined(__x86_64__)
#error This fingerprint requires an x87-capable host.
#endif

namespace
{

uint32_t FloatBits(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

uint32_t gState = 0x54483038U;

uint32_t NextU32()
{
    gState = gState * 1664525U + 1013904223U;
    return gState;
}

float PlayfieldPosition()
{
    const int32_t signedValue = static_cast<int32_t>(NextU32());
    return static_cast<float>(
        static_cast<double>(signedValue) * (768.0 / 2147483648.0));
}

float X87GrazeMin(float position, float size)
{
    static const float two = 2.0f;
    static const float margin = 20.0f;
    float result;
    __asm__ volatile(
        "flds %[size]\n\t"
        "fdivs %[two]\n\t"
        "fsubrs %[position]\n\t"
        "fsubs %[margin]\n\t"
        "fstps %[result]"
        : [result] "=m"(result)
        : [position] "m"(position), [size] "m"(size),
          [two] "m"(two), [margin] "m"(margin)
        : "st");
    return result;
}

float X87GrazeMax(float position, float size)
{
    static const float two = 2.0f;
    static const float margin = 20.0f;
    float result;
    __asm__ volatile(
        "flds %[size]\n\t"
        "fdivs %[two]\n\t"
        "fadds %[position]\n\t"
        "fadds %[margin]\n\t"
        "fstps %[result]"
        : [result] "=m"(result)
        : [position] "m"(position), [size] "m"(size),
          [two] "m"(two), [margin] "m"(margin)
        : "st");
    return result;
}

// This deliberately models Allegrex's current mul.s -> add/sub.s ->
// add/sub.s sequence.  Volatile stores make both binary32 rounding points
// explicit even when the host compiler is allowed to contract expressions.
float StepwiseGrazeMin(float position, float size)
{
    volatile float half = size * 0.5f;
    volatile float first = position - half;
    volatile float result = first - 20.0f;
    return result;
}

float StepwiseGrazeMax(float position, float size)
{
    volatile float half = size * 0.5f;
    volatile float first = position + half;
    volatile float result = first + 20.0f;
    return result;
}

float DoubleGrazeMin(float position, float size)
{
    return static_cast<float>(static_cast<double>(position) -
                              static_cast<double>(size) / 2.0 - 20.0);
}

float DoubleGrazeMax(float position, float size)
{
    return static_cast<float>(static_cast<double>(size) / 2.0 +
                              static_cast<double>(position) + 20.0);
}

// BulletManager initializes every enemy-bullet collision extent to one of
// these integer-valued sizes.  For this domain, (size / 2 + 20) is exact in
// binary32, leaving just the retail-equivalent final add/sub rounding.
float GroupedGrazeMin(float position, float size)
{
    volatile float extent = size * 0.5f + 20.0f;
    volatile float result = position - extent;
    return result;
}

float GroupedGrazeMax(float position, float size)
{
    volatile float extent = size * 0.5f + 20.0f;
    volatile float result = position + extent;
    return result;
}

bool AxisOverlaps(float playerMin, float playerMax,
                  float incomingMin, float incomingMax)
{
    return !(playerMin > incomingMax || playerMax < incomingMin);
}

} // namespace

int main()
{
    const float bulletSizes[] = {4.0f, 5.0f, 6.0f, 8.0f, 10.0f, 24.0f};
    constexpr uint32_t sampleCount = 1000000U;

    uint16_t savedControlWord;
    const uint16_t retailControlWord = 0x027f;
    __asm__ volatile("fnstcw %0" : "=m"(savedControlWord));
    __asm__ volatile("fldcw %0" : : "m"(retailControlWord));

    uint32_t minStepwiseDifferences = 0;
    uint32_t maxStepwiseDifferences = 0;
    uint32_t boundaryDecisionDifferences = 0;
    uint32_t doubleMismatches = 0;
    uint32_t groupedMismatches = 0;

    for (uint32_t sample = 0; sample < sampleCount; ++sample)
    {
        const float position = PlayfieldPosition();
        const float size = bulletSizes[NextU32() %
                                       (sizeof(bulletSizes) /
                                        sizeof(bulletSizes[0]))];

        const float retailMin = X87GrazeMin(position, size);
        const float retailMax = X87GrazeMax(position, size);
        const float stepwiseMin = StepwiseGrazeMin(position, size);
        const float stepwiseMax = StepwiseGrazeMax(position, size);
        const float doubleMin = DoubleGrazeMin(position, size);
        const float doubleMax = DoubleGrazeMax(position, size);
        const float groupedMin = GroupedGrazeMin(position, size);
        const float groupedMax = GroupedGrazeMax(position, size);

        if (FloatBits(retailMin) != FloatBits(stepwiseMin))
            ++minStepwiseDifferences;
        if (FloatBits(retailMax) != FloatBits(stepwiseMax))
            ++maxStepwiseDifferences;
        if (FloatBits(retailMin) != FloatBits(doubleMin) ||
            FloatBits(retailMax) != FloatBits(doubleMax))
            ++doubleMismatches;
        if (FloatBits(retailMin) != FloatBits(groupedMin) ||
            FloatBits(retailMax) != FloatBits(groupedMax))
            ++groupedMismatches;

        // Exercise the inclusive overlap decision at both changed endpoints.
        // A one-ULP bound change is enough to invert one platform's graze.
        if (FloatBits(retailMax) != FloatBits(stepwiseMax))
        {
            const float witness = retailMax > stepwiseMax
                                      ? retailMax
                                      : stepwiseMax;
            const bool retail = AxisOverlaps(witness, witness,
                                             retailMin, retailMax);
            const bool stepwise = AxisOverlaps(witness, witness,
                                               stepwiseMin, stepwiseMax);
            if (retail != stepwise)
                ++boundaryDecisionDifferences;
        }
        if (FloatBits(retailMin) != FloatBits(stepwiseMin))
        {
            const float witness = retailMin < stepwiseMin
                                      ? retailMin
                                      : stepwiseMin;
            const bool retail = AxisOverlaps(witness, witness,
                                             retailMin, retailMax);
            const bool stepwise = AxisOverlaps(witness, witness,
                                               stepwiseMin, stepwiseMax);
            if (retail != stepwise)
                ++boundaryDecisionDifferences;
        }
    }

    __asm__ volatile("fldcw %0" : : "m"(savedControlWord));

    if (minStepwiseDifferences == 0 || maxStepwiseDifferences == 0 ||
        boundaryDecisionDifferences == 0 || doubleMismatches != 0 ||
        groupedMismatches != 0)
    {
        std::fprintf(
            stderr,
            "x87-graze-bounds: FAIL samples=%u min_step=%u max_step=%u "
            "decision=%u double_mismatch=%u grouped_mismatch=%u\n",
            sampleCount, minStepwiseDifferences, maxStepwiseDifferences,
            boundaryDecisionDifferences, doubleMismatches,
            groupedMismatches);
        return 1;
    }

    std::printf(
        "x87-graze-bounds: PASS samples=%u min_step_differences=%u "
        "max_step_differences=%u boundary_decision_differences=%u "
        "double_mismatches=%u grouped_mismatches=%u\n",
        sampleCount, minStepwiseDifferences, maxStepwiseDifferences,
        boundaryDecisionDifferences, doubleMismatches, groupedMismatches);
    return 0;
}
