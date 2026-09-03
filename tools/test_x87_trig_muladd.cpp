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

uint32_t gState = 0x584d4138U;

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

float X87SinMulAdd(float angle, float magnitude, float addend)
{
    float result;
    __asm__ volatile(
        "flds %[angle]\n\t"
        "fsin\n\t"
        "fmuls %[magnitude]\n\t"
        "fadds %[addend]\n\t"
        "fstps %[result]"
        : [result] "=m"(result)
        : [angle] "m"(angle), [magnitude] "m"(magnitude),
          [addend] "m"(addend)
        : "st");
    return result;
}

float X87CosMulAdd(float angle, float magnitude, float addend)
{
    float result;
    __asm__ volatile(
        "flds %[angle]\n\t"
        "fcos\n\t"
        "fmuls %[magnitude]\n\t"
        "fadds %[addend]\n\t"
        "fstps %[result]"
        : [result] "=m"(result)
        : [angle] "m"(angle), [magnitude] "m"(magnitude),
          [addend] "m"(addend)
        : "st");
    return result;
}

float X87StoredCos(float angle)
{
    float result;
    __asm__ volatile(
        "flds %[angle]\n\t"
        "fcos\n\t"
        "fstps %[result]"
        : [result] "=m"(result)
        : [angle] "m"(angle)
        : "st");
    return result;
}

float X87StoredSin(float angle)
{
    float result;
    __asm__ volatile(
        "flds %[angle]\n\t"
        "fsin\n\t"
        "fstps %[result]"
        : [result] "=m"(result)
        : [angle] "m"(angle)
        : "st");
    return result;
}

float StagedX87SinMulAdd(float angle, float magnitude, float addend)
{
    volatile float trig = X87StoredSin(angle);
    volatile float product = trig * magnitude;
    volatile float result = product + addend;
    return result;
}

float StagedX87CosMulAdd(float angle, float magnitude, float addend)
{
    volatile float trig = X87StoredCos(angle);
    volatile float product = trig * magnitude;
    volatile float result = product + addend;
    return result;
}

float DoubleSinMulAdd(float angle, float magnitude, float addend)
{
    return static_cast<float>(
        std::sin(static_cast<double>(angle)) * static_cast<double>(magnitude) +
        static_cast<double>(addend));
}

float DoubleCosMulAdd(float angle, float magnitude, float addend)
{
    return static_cast<float>(
        std::cos(static_cast<double>(angle)) * static_cast<double>(magnitude) +
        static_cast<double>(addend));
}

float StepwiseSinMulAdd(float angle, float magnitude, float addend)
{
    volatile float trig = std::sin(angle);
    volatile float product = trig * magnitude;
    volatile float result = product + addend;
    return result;
}

float StepwiseCosMulAdd(float angle, float magnitude, float addend)
{
    volatile float trig = std::cos(angle);
    volatile float product = trig * magnitude;
    volatile float result = product + addend;
    return result;
}

} // namespace

int main()
{
    constexpr uint32_t sampleCount = 1000000U;
    constexpr float pi = 3.1415927410125732421875f;

    uint16_t savedControlWord;
    const uint16_t retailControlWord = 0x027f;
    __asm__ volatile("fnstcw %0" : "=m"(savedControlWord));
    __asm__ volatile("fldcw %0" : : "m"(retailControlWord));

    uint32_t sinMismatches = 0;
    uint32_t cosMismatches = 0;
    uint32_t stepwiseSinDifferences = 0;
    uint32_t stepwiseCosDifferences = 0;
    uint32_t firstSinCase = UINT32_MAX;
    uint32_t firstCosCase = UINT32_MAX;
    uint32_t firstSinExpected = 0;
    uint32_t firstSinActual = 0;
    uint32_t firstCosExpected = 0;
    uint32_t firstCosActual = 0;

    struct ExactFixture
    {
        char operation;
        uint32_t angle;
        uint32_t magnitude;
        uint32_t addend;
        uint32_t fullX87Expected;
    };
    const ExactFixture exactFixtures[] = {
        // The target's unusual Fantasy Seal axis order: X uses sine and Y
        // uses cosine.  These are work slots 7 and 15 at frame 5207.
        {'s', 0xbcd67770U, 0x40e66666U, 0x434e48c8U, 0x434e1888U},
        {'c', 0xbcd67770U, 0x40e66666U, 0x43d50555U, 0x43d89e9eU},
        {'s', 0x404762eaU, 0x40e66666U, 0x434e48c8U, 0x434e7908U},
        {'c', 0x404762eaU, 0x40e66666U, 0x43d50555U, 0x43d16c0cU},
        // Work slot 0 at frame 5219, using the same target axis order.
        {'s', 0x40096474U, 0x422cccceU, 0x434e48c8U, 0x437283cdU},
        {'c', 0x40096474U, 0x422cccceU, 0x43d50555U, 0x43c941b1U},
    };

    for (const ExactFixture &fixture : exactFixtures)
    {
        float angle;
        float magnitude;
        float addend;
        std::memcpy(&angle, &fixture.angle, sizeof(angle));
        std::memcpy(&magnitude, &fixture.magnitude, sizeof(magnitude));
        std::memcpy(&addend, &fixture.addend, sizeof(addend));
        const bool sine = fixture.operation == 's';
        const uint32_t x87 = FloatBits(
            sine ? X87SinMulAdd(angle, magnitude, addend)
                 : X87CosMulAdd(angle, magnitude, addend));
        const uint32_t helper = FloatBits(
            sine ? DoubleSinMulAdd(angle, magnitude, addend)
                 : DoubleCosMulAdd(angle, magnitude, addend));
        const uint32_t stepwise = FloatBits(
            sine ? StepwiseSinMulAdd(angle, magnitude, addend)
                 : StepwiseCosMulAdd(angle, magnitude, addend));
        const uint32_t stagedX87 = FloatBits(
            sine ? StagedX87SinMulAdd(angle, magnitude, addend)
                 : StagedX87CosMulAdd(angle, magnitude, addend));
        std::printf("exact-%c angle=%08x magnitude=%08x addend=%08x "
                    "x87=%08x helper=%08x staged_x87=%08x host_step=%08x "
                    "full_expected=%08x\n",
                    fixture.operation, fixture.angle, fixture.magnitude,
                    fixture.addend,
                    x87, helper, stagedX87, stepwise,
                    fixture.fullX87Expected);
        if (x87 != fixture.fullX87Expected ||
            helper != fixture.fullX87Expected)
        {
            std::fprintf(stderr,
                         "x87-trig-muladd: FAIL exact boundary fixture\n");
            __asm__ volatile("fldcw %0" : : "m"(savedControlWord));
            return 1;
        }
    }

    for (uint32_t sample = 0; sample < sampleCount; ++sample)
    {
        const float angle = UnitSigned() * pi;
        const float magnitude = UnitSigned() * 2048.0f;
        const float addend = UnitSigned() * 2048.0f;

        const uint32_t x87Sin = FloatBits(
            X87SinMulAdd(angle, magnitude, addend));
        const uint32_t doubleSin = FloatBits(
            DoubleSinMulAdd(angle, magnitude, addend));
        if (x87Sin != doubleSin)
        {
            ++sinMismatches;
            if (firstSinCase == UINT32_MAX)
            {
                firstSinCase = sample;
                firstSinExpected = x87Sin;
                firstSinActual = doubleSin;
            }
        }
        if (x87Sin != FloatBits(
                          StepwiseSinMulAdd(angle, magnitude, addend)))
            ++stepwiseSinDifferences;

        const uint32_t x87Cos = FloatBits(
            X87CosMulAdd(angle, magnitude, addend));
        const uint32_t doubleCos = FloatBits(
            DoubleCosMulAdd(angle, magnitude, addend));
        if (x87Cos != doubleCos)
        {
            ++cosMismatches;
            if (firstCosCase == UINT32_MAX)
            {
                firstCosCase = sample;
                firstCosExpected = x87Cos;
                firstCosActual = doubleCos;
            }
        }
        if (x87Cos != FloatBits(
                          StepwiseCosMulAdd(angle, magnitude, addend)))
            ++stepwiseCosDifferences;
    }

    __asm__ volatile("fldcw %0" : : "m"(savedControlWord));

    std::printf(
        "x87-trig-muladd: samples=%u helper_sin_mismatches=%u "
        "helper_cos_mismatches=%u stepwise_sin_differences=%u "
        "stepwise_cos_differences=%u first_sin=%u/%08x/%08x "
        "first_cos=%u/%08x/%08x\n",
        sampleCount, sinMismatches, cosMismatches,
        stepwiseSinDifferences, stepwiseCosDifferences,
        firstSinCase, firstSinExpected, firstSinActual,
        firstCosCase, firstCosExpected, firstCosActual);

    if (sinMismatches != 0 || cosMismatches != 0 ||
        stepwiseSinDifferences == 0 || stepwiseCosDifferences == 0)
    {
        std::fprintf(stderr,
                     "x87-trig-muladd: FAIL helper mismatch or missing "
                     "stepwise-rounding witness\n");
        return 1;
    }
    return 0;
}
