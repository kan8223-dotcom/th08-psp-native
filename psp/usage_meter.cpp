#include "usage_meter.hpp"

namespace th08::psp
{
namespace
{
std::uint8_t gSc[kUsageMeterHistory];
std::uint8_t gMe[kUsageMeterHistory];
unsigned gHead = 0U;
std::uint64_t gWaitUs = 0U;
std::uint64_t gLastFrameEndUs = 0U;
std::uint32_t gMeCycles = 0U;
unsigned gLastSc = 0U;
unsigned gLastMe = 0U;
} // namespace

void UsageMeterNoteWait(std::uint64_t waitUs)
{
    gWaitUs += waitUs;
}

void UsageMeterAddMeCycles(std::uint32_t cycles)
{
    gMeCycles += cycles;
}

void UsageMeterEndFrame(std::uint64_t nowUs)
{
    if (gLastFrameEndUs != 0U && nowUs > gLastFrameEndUs)
    {
        const std::uint64_t period = nowUs - gLastFrameEndUs;
        const std::uint64_t busy = period > gWaitUs ? period - gWaitUs : 0U;
        unsigned sc = static_cast<unsigned>((busy * 100U) / period);
        // ME: busy cycles x 2 / 333 = us (CP0 Count runs at half clock).
        const std::uint64_t meUs = (static_cast<std::uint64_t>(gMeCycles) * 2U) / 333U;
        unsigned me = static_cast<unsigned>((meUs * 100U) / period);
        if (sc > 200U)
            sc = 200U;
        if (me > 200U)
            me = 200U;
        gHead = (gHead + 1U) % kUsageMeterHistory;
        gSc[gHead] = static_cast<std::uint8_t>(sc);
        gMe[gHead] = static_cast<std::uint8_t>(me);
        gLastSc = sc;
        gLastMe = me;
    }
    gLastFrameEndUs = nowUs;
    gWaitUs = 0U;
    gMeCycles = 0U;
}

const std::uint8_t *UsageMeterScHistory() { return gSc; }
const std::uint8_t *UsageMeterMeHistory() { return gMe; }
unsigned UsageMeterHead() { return gHead; }
unsigned UsageMeterLastSc() { return gLastSc; }
unsigned UsageMeterLastMe() { return gLastMe; }
} // namespace th08::psp
