#pragma once

#include "fileio.hpp"
#include "platform.hpp"

namespace th08::psp
{
bool VideoInitialize();
void RenderBootstrapStatus(const DataDiscovery &data,
                           const MemorySnapshot &startupMemory,
                           const MemorySnapshot &postProbeMemory);
void RenderEngineFailureStatus(const char *phase, const char *state, int result,
                               const MemorySnapshot &memory);
void VideoShutdown();
} // namespace th08::psp
