#include "softfloat_census.hpp"

#if TH08_PSP_SOFTFLOAT_CENSUS_ENABLED

#include "fileio.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace th08::psp
{
volatile std::uint8_t gSoftfloatCurrentPhase = kSoftfloatPhaseNone;

enum OpClass : std::uint8_t
{
    kDfAdd = 0,
    kDfSub,
    kDfMul,
    kDfDiv,
    kDfCmp,
    kDfExtend,
    kDfTrunc,
    kDfFix,
    kDfFloat,
    kLibmSin,
    kLibmCos,
    kLibmAtan2,
    kLibmAtan,
    kLibmSqrt,
    kLibmFloorCeil,
    kLibmFmod,
    kLibmOther,
    kLibmfSin,
    kLibmfCos,
    kLibmfAtan2,
    kLibmfSqrt,
    kLibmfFloor,
    kLibmfFmod,
    kOpClassCount,
};
constexpr std::uint8_t kFirstLibm = kLibmSin;
constexpr std::uint8_t kFirstLibmf = kLibmfSin;
std::uint32_t gLibmDepth = 0U;

namespace
{

// Per phase: [0] binary64 helper calls issued directly by game/port code,
// [1] libm binary64 entry calls, [2] libm binary32 entry calls.  Binary64
// helper calls made from inside a wrapped libm routine are counted in
// gInternalDf only (they are the libm routine's own cost).
std::uint32_t gPhase[kSoftfloatPhaseBuckets][3]{};
std::uint32_t gOps[kOpClassCount]{};
std::uint32_t gInternalDf = 0U;
bool gWindowActive = false;
} // namespace

void Note(OpClass op)
{
    if (!gWindowActive)
        return;
    ++gOps[op];
    std::uint8_t phase = gSoftfloatCurrentPhase;
    if (phase >= kSoftfloatPhaseBuckets)
        phase = kSoftfloatPhaseNone;
    if (op < kFirstLibm)
    {
        if (gLibmDepth != 0U)
            ++gInternalDf;
        else
            ++gPhase[phase][0];
    }
    else if (op < kFirstLibmf)
    {
        ++gPhase[phase][1];
    }
    else
    {
        ++gPhase[phase][2];
    }
}

namespace
{
void ClearWindowCounters()
{
    std::memset(gPhase, 0, sizeof(gPhase));
    std::memset(gOps, 0, sizeof(gOps));
    gInternalDf = 0U;
}
} // namespace

void SoftfloatCensusResetWindow(bool active)
{
    ClearWindowCounters();
    gWindowActive = active;
}

void SoftfloatCensusCancelWindow()
{
    if (gWindowActive)
        ClearWindowCounters();
    gWindowActive = false;
}

void SoftfloatCensusEmitWindow(std::int32_t stage,
                               std::uint32_t baselineStageFrame,
                               std::uint32_t stageFrame)
{
    if (!gWindowActive)
        return;
    std::uint64_t directDf = 0U;
    std::uint64_t libm = 0U;
    std::uint64_t libmf = 0U;
    for (std::size_t i = 0U; i < kSoftfloatPhaseBuckets; ++i)
    {
        directDf += gPhase[i][0];
        libm += gPhase[i][1];
        libmf += gPhase[i][2];
    }
    // One bounded record at the parent PERF_ATTR 600-tick boundary.  Phase
    // indices follow PerfAttributionPhase; ph22 is outside every scope.
    BootLog(
        "SOFTFLOAT V1 st=%ld sf=%lu-%lu df_direct=%llu df_internal=%lu "
        "libm=%llu libmf=%llu "
        "ph0=%lu/%lu/%lu ph1=%lu/%lu/%lu ph2=%lu/%lu/%lu ph3=%lu/%lu/%lu "
        "ph4=%lu/%lu/%lu ph5=%lu/%lu/%lu ph6=%lu/%lu/%lu ph7=%lu/%lu/%lu "
        "ph8=%lu/%lu/%lu ph9=%lu/%lu/%lu ph10=%lu/%lu/%lu ph11=%lu/%lu/%lu "
        "ph12=%lu/%lu/%lu ph13=%lu/%lu/%lu ph14=%lu/%lu/%lu ph15=%lu/%lu/%lu "
        "ph16=%lu/%lu/%lu ph17=%lu/%lu/%lu ph18=%lu/%lu/%lu ph19=%lu/%lu/%lu "
        "ph20=%lu/%lu/%lu ph21=%lu/%lu/%lu ph22=%lu/%lu/%lu "
        "add=%lu sub=%lu mul=%lu div=%lu cmp=%lu ext=%lu trunc=%lu fix=%lu "
        "flt=%lu sin=%lu cos=%lu atan2=%lu atan=%lu sqrt=%lu floor=%lu "
        "fmod=%lu other=%lu sinf=%lu cosf=%lu atan2f=%lu sqrtf=%lu "
        "floorf=%lu fmodf=%lu\n",
        static_cast<long>(stage),
        static_cast<unsigned long>(baselineStageFrame),
        static_cast<unsigned long>(stageFrame),
        static_cast<unsigned long long>(directDf),
        static_cast<unsigned long>(gInternalDf),
        static_cast<unsigned long long>(libm),
        static_cast<unsigned long long>(libmf),
#define TH08_SOFTFLOAT_PHASE_ARGS(index)                                       \
        static_cast<unsigned long>(gPhase[index][0]),                          \
        static_cast<unsigned long>(gPhase[index][1]),                          \
        static_cast<unsigned long>(gPhase[index][2])
        TH08_SOFTFLOAT_PHASE_ARGS(0), TH08_SOFTFLOAT_PHASE_ARGS(1),
        TH08_SOFTFLOAT_PHASE_ARGS(2), TH08_SOFTFLOAT_PHASE_ARGS(3),
        TH08_SOFTFLOAT_PHASE_ARGS(4), TH08_SOFTFLOAT_PHASE_ARGS(5),
        TH08_SOFTFLOAT_PHASE_ARGS(6), TH08_SOFTFLOAT_PHASE_ARGS(7),
        TH08_SOFTFLOAT_PHASE_ARGS(8), TH08_SOFTFLOAT_PHASE_ARGS(9),
        TH08_SOFTFLOAT_PHASE_ARGS(10), TH08_SOFTFLOAT_PHASE_ARGS(11),
        TH08_SOFTFLOAT_PHASE_ARGS(12), TH08_SOFTFLOAT_PHASE_ARGS(13),
        TH08_SOFTFLOAT_PHASE_ARGS(14), TH08_SOFTFLOAT_PHASE_ARGS(15),
        TH08_SOFTFLOAT_PHASE_ARGS(16), TH08_SOFTFLOAT_PHASE_ARGS(17),
        TH08_SOFTFLOAT_PHASE_ARGS(18), TH08_SOFTFLOAT_PHASE_ARGS(19),
        TH08_SOFTFLOAT_PHASE_ARGS(20), TH08_SOFTFLOAT_PHASE_ARGS(21),
        TH08_SOFTFLOAT_PHASE_ARGS(22),
#undef TH08_SOFTFLOAT_PHASE_ARGS
        static_cast<unsigned long>(gOps[kDfAdd]),
        static_cast<unsigned long>(gOps[kDfSub]),
        static_cast<unsigned long>(gOps[kDfMul]),
        static_cast<unsigned long>(gOps[kDfDiv]),
        static_cast<unsigned long>(gOps[kDfCmp]),
        static_cast<unsigned long>(gOps[kDfExtend]),
        static_cast<unsigned long>(gOps[kDfTrunc]),
        static_cast<unsigned long>(gOps[kDfFix]),
        static_cast<unsigned long>(gOps[kDfFloat]),
        static_cast<unsigned long>(gOps[kLibmSin]),
        static_cast<unsigned long>(gOps[kLibmCos]),
        static_cast<unsigned long>(gOps[kLibmAtan2]),
        static_cast<unsigned long>(gOps[kLibmAtan]),
        static_cast<unsigned long>(gOps[kLibmSqrt]),
        static_cast<unsigned long>(gOps[kLibmFloorCeil]),
        static_cast<unsigned long>(gOps[kLibmFmod]),
        static_cast<unsigned long>(gOps[kLibmOther]),
        static_cast<unsigned long>(gOps[kLibmfSin]),
        static_cast<unsigned long>(gOps[kLibmfCos]),
        static_cast<unsigned long>(gOps[kLibmfAtan2]),
        static_cast<unsigned long>(gOps[kLibmfSqrt]),
        static_cast<unsigned long>(gOps[kLibmfFloor]),
        static_cast<unsigned long>(gOps[kLibmfFmod]));
}
} // namespace th08::psp

// Linker wrappers.  Each performs only integer bookkeeping and forwards to
// the real routine; no binary64 arithmetic is evaluated here, so the
// wrappers cannot recurse into themselves.
using th08::psp::Note;
using th08::psp::OpClass;
#define TH08_WRAP2(name, type, cls)                                            \
    extern "C" type __real_##name(type, type);                                 \
    extern "C" type __wrap_##name(type a, type b)                              \
    {                                                                          \
        Note(cls);                                                             \
        return __real_##name(a, b);                                            \
    }
#define TH08_WRAP_CMP(name)                                                    \
    extern "C" int __real_##name(double, double);                              \
    extern "C" int __wrap_##name(double a, double b)                           \
    {                                                                          \
        Note(th08::psp::kDfCmp);                                               \
        return __real_##name(a, b);                                            \
    }
#define TH08_WRAP_CONV(name, rtype, atype, cls)                                \
    extern "C" rtype __real_##name(atype);                                     \
    extern "C" rtype __wrap_##name(atype a)                                    \
    {                                                                          \
        Note(cls);                                                             \
        return __real_##name(a);                                               \
    }
#define TH08_WRAP_LIBM1(name, type, cls)                                       \
    extern "C" type __real_##name(type);                                       \
    extern "C" type __wrap_##name(type a)                                      \
    {                                                                          \
        Note(cls);                                                             \
        ++th08::psp::gLibmDepth;                                               \
        const type r = __real_##name(a);                                       \
        --th08::psp::gLibmDepth;                                               \
        return r;                                                              \
    }
#define TH08_WRAP_LIBM2(name, type, cls)                                       \
    extern "C" type __real_##name(type, type);                                 \
    extern "C" type __wrap_##name(type a, type b)                              \
    {                                                                          \
        Note(cls);                                                             \
        ++th08::psp::gLibmDepth;                                               \
        const type r = __real_##name(a, b);                                    \
        --th08::psp::gLibmDepth;                                               \
        return r;                                                              \
    }

TH08_WRAP2(__adddf3, double, th08::psp::kDfAdd)
TH08_WRAP2(__subdf3, double, th08::psp::kDfSub)
TH08_WRAP2(__muldf3, double, th08::psp::kDfMul)
TH08_WRAP2(__divdf3, double, th08::psp::kDfDiv)
TH08_WRAP_CMP(__eqdf2)
TH08_WRAP_CMP(__nedf2)
TH08_WRAP_CMP(__gtdf2)
TH08_WRAP_CMP(__gedf2)
TH08_WRAP_CMP(__ltdf2)
TH08_WRAP_CMP(__ledf2)
TH08_WRAP_CMP(__unorddf2)
TH08_WRAP_CONV(__extendsfdf2, double, float, th08::psp::kDfExtend)
TH08_WRAP_CONV(__truncdfsf2, float, double, th08::psp::kDfTrunc)
TH08_WRAP_CONV(__fixdfsi, int, double, th08::psp::kDfFix)
TH08_WRAP_CONV(__fixunsdfsi, unsigned int, double, th08::psp::kDfFix)
TH08_WRAP_CONV(__fixdfdi, long long, double, th08::psp::kDfFix)
TH08_WRAP_CONV(__fixunsdfdi, unsigned long long, double, th08::psp::kDfFix)
TH08_WRAP_CONV(__floatsidf, double, int, th08::psp::kDfFloat)
TH08_WRAP_CONV(__floatunsidf, double, unsigned int, th08::psp::kDfFloat)
TH08_WRAP_CONV(__floatdidf, double, long long, th08::psp::kDfFloat)
TH08_WRAP_LIBM1(sin, double, th08::psp::kLibmSin)
TH08_WRAP_LIBM1(cos, double, th08::psp::kLibmCos)
TH08_WRAP_LIBM2(atan2, double, th08::psp::kLibmAtan2)
TH08_WRAP_LIBM1(atan, double, th08::psp::kLibmAtan)
TH08_WRAP_LIBM1(sqrt, double, th08::psp::kLibmSqrt)
TH08_WRAP_LIBM1(floor, double, th08::psp::kLibmFloorCeil)
TH08_WRAP_LIBM1(ceil, double, th08::psp::kLibmFloorCeil)
TH08_WRAP_LIBM2(fmod, double, th08::psp::kLibmFmod)
TH08_WRAP_LIBM2(pow, double, th08::psp::kLibmOther)
TH08_WRAP_LIBM1(exp, double, th08::psp::kLibmOther)
TH08_WRAP_LIBM1(log, double, th08::psp::kLibmOther)
TH08_WRAP_LIBM1(tan, double, th08::psp::kLibmOther)
TH08_WRAP_LIBM1(sinf, float, th08::psp::kLibmfSin)
TH08_WRAP_LIBM1(cosf, float, th08::psp::kLibmfCos)
TH08_WRAP_LIBM2(atan2f, float, th08::psp::kLibmfAtan2)
TH08_WRAP_LIBM1(sqrtf, float, th08::psp::kLibmfSqrt)
TH08_WRAP_LIBM1(floorf, float, th08::psp::kLibmfFloor)
TH08_WRAP_LIBM2(fmodf, float, th08::psp::kLibmfFmod)

#endif // TH08_PSP_SOFTFLOAT_CENSUS_ENABLED
