#pragma once
#include <cstdint>

#if defined(PSP) && defined(TH08_PSP_WAIT_PROBE) && TH08_PSP_WAIT_PROBE
#define TH08_PSP_WAIT_PROBE_ENABLED 1
namespace th08::psp
{
enum class WaitProbeApi : std::uint32_t { None, GeListSync, GeDrawSync, VblankStart };
struct WaitProbeSnapshot
{
    std::uint32_t sequence = 0, active = 0, api = 0, caller = 0, startedUs = 0;
    std::uint32_t observedContext = 0xffffffffU, phase = 0xffffffffU;
    int mainThread = -1, qid = -1, result = 0;
    bool consistent = false;
};
// Main is the sole writer. Readers never wait for an interrupted writer.
void WaitProbeInitialize(int mainThread);
void WaitProbeObserve(std::int32_t stage, std::uint32_t frame);
bool WaitProbeBegin(WaitProbeApi api, int qid, int mode, std::uintptr_t caller,
                    std::uint32_t phase);
void WaitProbeEnd(bool tracked, int result);
WaitProbeSnapshot WaitProbeRead();
}
#else
#define TH08_PSP_WAIT_PROBE_ENABLED 0
#endif
