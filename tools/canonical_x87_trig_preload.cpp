// Diagnostic-only Linux replay oracle for TH08 1.00d float trigonometry.
//
// The canonical executable (SHA-256 330fbdbf...213d924) implements _cosf at
// 0x00408d40 and _sinf at 0x00409060 as float wrappers around x87 FCOS/FSIN
// cores at 0x004a3ef0 and 0x004a3fa0.  RunEcl opcodes 33 and 32 call those
// wrappers at 0x004193e7 and 0x0041937f respectively.  Some Linux libm
// implementations round cosf(0x4006b668) one ULP differently.  Preload this
// file only into a frozen Linux replay-audit executable; it is not production
// PSP code and must never be linked into EBOOT.PBP.

#include <cfloat>
#include <cstdint>
#include <cstring>

#if defined(TH08_CANONICAL_X87_TRIG_TRACE)
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

#if !defined(TH08_CANONICAL_X87_COMPLETED_FRAMES_OFFSET)
#error Define the frozen oracle gCompletedFrames image offset for trace builds.
#endif
#endif

#if !defined(__i386__) && !defined(__x86_64__)
#error This diagnostic oracle requires an x87-capable host.
#endif

static_assert(FLT_RADIX == 2, "TH08 requires binary floating point");
static_assert(sizeof(float) == sizeof(std::uint32_t),
              "TH08 requires IEEE binary32 float");
static_assert(LDBL_MANT_DIG == 64,
              "The target reduction constant requires x87 extended precision");

namespace
{

constexpr std::uint16_t kRetailControlWord = 0x027fU;

// Exact 80-bit value stored at canonical target address 0x004bda2a:
// 0x4000c90fdaa22168c235 (pi).  The reduction path is irrelevant to normal
// ECL angles, but retaining it mirrors the target core when FCOS/FSIN sets C2.
const long double kTargetPi =
    3.141592653589793238462643383279502884L;

float CanonicalCosf(float angle)
{
    std::uint16_t savedControlWord;
    float result;
    __asm__ volatile(
        "fnstcw %[saved]\n\t"
        "fldcw %[retail]\n\t"
        "flds %[angle]\n\t"
        "1: fcos\n\t"
        "fnstsw %%ax\n\t"
        "testw $0x0400, %%ax\n\t"
        "jz 3f\n\t"
        "fldt %[pi]\n\t"
        "fxch %%st(1)\n\t"
        "2: fprem1\n\t"
        "fnstsw %%ax\n\t"
        "testw $0x0400, %%ax\n\t"
        "jnz 2b\n\t"
        "fstp %%st(1)\n\t"
        "jmp 1b\n\t"
        "3: fstps %[result]\n\t"
        "fldcw %[saved]"
        : [saved] "=&m"(savedControlWord), [result] "=m"(result)
        : [retail] "m"(kRetailControlWord), [angle] "m"(angle),
          [pi] "m"(kTargetPi)
        : "ax", "cc", "st", "memory");
    return result;
}

float CanonicalSinf(float angle)
{
    std::uint16_t savedControlWord;
    float result;
    __asm__ volatile(
        "fnstcw %[saved]\n\t"
        "fldcw %[retail]\n\t"
        "flds %[angle]\n\t"
        "1: fsin\n\t"
        "fnstsw %%ax\n\t"
        "testw $0x0400, %%ax\n\t"
        "jz 3f\n\t"
        "fldt %[pi]\n\t"
        "fxch %%st(1)\n\t"
        "2: fprem1\n\t"
        "fnstsw %%ax\n\t"
        "testw $0x0400, %%ax\n\t"
        "jnz 2b\n\t"
        "fstp %%st(1)\n\t"
        "jmp 1b\n\t"
        "3: fstps %[result]\n\t"
        "fldcw %[saved]"
        : [saved] "=&m"(savedControlWord), [result] "=m"(result)
        : [retail] "m"(kRetailControlWord), [angle] "m"(angle),
          [pi] "m"(kTargetPi)
        : "ax", "cc", "st", "memory");
    return result;
}

#if defined(TH08_CANONICAL_X87_TRIG_TRACE)
FILE *gTraceOutput = nullptr;
std::uint32_t gTraceFirstFrame = 0;
std::uint32_t gTraceLastFrame = UINT32_MAX;

std::uint32_t TraceBits(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::uint32_t ParseTraceFrame(const char *name, std::uint32_t fallback)
{
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
        return fallback;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 0);
    return end != value && *end == '\0'
               ? static_cast<std::uint32_t>(parsed)
               : fallback;
}

__attribute__((constructor)) void InitializeTrace()
{
    const char *path = std::getenv("TH08_CANONICAL_X87_TRIG_TRACE_PATH");
    if (path == nullptr || path[0] == '\0')
        return;
    gTraceOutput = std::fopen(path, "wb");
    if (gTraceOutput == nullptr)
        return;
    gTraceFirstFrame = ParseTraceFrame(
        "TH08_CANONICAL_X87_TRIG_TRACE_FIRST_FRAME", 0);
    gTraceLastFrame = ParseTraceFrame(
        "TH08_CANONICAL_X87_TRIG_TRACE_LAST_FRAME", UINT32_MAX);
    std::setvbuf(gTraceOutput, nullptr, _IOLBF, 0);
}

__attribute__((destructor)) void CloseTrace()
{
    if (gTraceOutput != nullptr)
        std::fclose(gTraceOutput);
}

void TraceCall(char operation, float angle, float result, void *caller)
{
    if (gTraceOutput == nullptr)
        return;

    Dl_info callerInfo{};
    if (dladdr(caller, &callerInfo) == 0 || callerInfo.dli_fbase == nullptr)
        return;
    const auto base = reinterpret_cast<std::uintptr_t>(callerInfo.dli_fbase);
    const auto callerValue = reinterpret_cast<std::uintptr_t>(caller);
    const auto frameAddress = base +
        static_cast<std::uintptr_t>(TH08_CANONICAL_X87_COMPLETED_FRAMES_OFFSET);
    const std::uint32_t completedFrames =
        *reinterpret_cast<volatile const std::uint32_t *>(frameAddress);
    if (completedFrames < gTraceFirstFrame ||
        completedFrames > gTraceLastFrame)
        return;

    std::fprintf(gTraceOutput,
                 "completed=%u caller=%zx op=%c angle=%08x x87=%08x\n",
                 completedFrames, static_cast<std::size_t>(callerValue - base),
                 operation, TraceBits(angle), TraceBits(result));
}
#endif

} // namespace

extern "C" __attribute__((visibility("default")))
float cosf(float angle) noexcept
{
    const float result = CanonicalCosf(angle);
#if defined(TH08_CANONICAL_X87_TRIG_TRACE)
    TraceCall('c', angle, result, __builtin_return_address(0));
#endif
    return result;
}

extern "C" __attribute__((visibility("default")))
float sinf(float angle) noexcept
{
    const float result = CanonicalSinf(angle);
#if defined(TH08_CANONICAL_X87_TRIG_TRACE)
    TraceCall('s', angle, result, __builtin_return_address(0));
#endif
    return result;
}

extern "C" __attribute__((visibility("default")))
unsigned int th08_canonical_x87_trig_oracle_version() noexcept
{
    return 1U;
}

#if defined(TH08_CANONICAL_X87_TRIG_SELF_TEST)
#include <cstdio>

namespace
{

std::uint32_t FloatBits(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float FloatFromBits(std::uint32_t bits)
{
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

} // namespace

int main()
{
    const float frame4667Witness = FloatFromBits(0x4006b668U);
    const std::uint32_t frame4667Cosine =
        FloatBits(CanonicalCosf(frame4667Witness));
    const std::uint32_t frame4667Sine =
        FloatBits(CanonicalSinf(frame4667Witness));
    const float frame4674Witness = FloatFromBits(0xc033725cU);
    const std::uint32_t frame4674Cosine =
        FloatBits(CanonicalCosf(frame4674Witness));
    const std::uint32_t frame4674Sine =
        FloatBits(CanonicalSinf(frame4674Witness));

    // These are target-wrapper results on the current x87 host.  In
    // particular, canonical cosf is ...173, whereas this host's dynamically
    // called glibc cosf is ...172.  The second witness is the first remaining
    // PSP Stage-5 ECL divergence after correcting the first one.
    if (frame4667Cosine != 0xbf025173U ||
        frame4667Sine != 0x3f5c590dU ||
        frame4674Cosine != 0xbf7189a8U ||
        frame4674Sine != 0xbea9a729U)
    {
        std::fprintf(stderr,
                     "canonical-x87-trig: FAIL "
                     "f4667=%08x/%08x/%08x f4674=%08x/%08x/%08x\n",
                     FloatBits(frame4667Witness), frame4667Sine,
                     frame4667Cosine, FloatBits(frame4674Witness),
                     frame4674Sine, frame4674Cosine);
        return 1;
    }

    std::printf("canonical-x87-trig: PASS "
                "f4667=%08x/%08x/%08x f4674=%08x/%08x/%08x\n",
                FloatBits(frame4667Witness), frame4667Sine,
                frame4667Cosine, FloatBits(frame4674Witness),
                frame4674Sine, frame4674Cosine);
    return 0;
}
#endif
