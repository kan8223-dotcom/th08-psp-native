#pragma once

#include "fileio.hpp"
#include "platform.hpp"

namespace th08::psp
{
bool VideoInitialize();
void RenderBootstrapStatus(const DataDiscovery &data,
                           const MemorySnapshot &startupMemory,
                           const MemorySnapshot &postProbeMemory);
void VideoShutdown();
} // namespace th08::psp
