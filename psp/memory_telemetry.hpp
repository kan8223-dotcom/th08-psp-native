#pragma once

#include "runtime_telemetry_config.hpp"

#include <cstddef>

// Diagnostic-only scheduling gate.  Production builds retain the original
// global 300-game-frame sampler unless the PSP build explicitly enables the
// stage-relative A/B measurement policy.
#ifndef TH08_PSP_STAGE_RELATIVE_PERF_SAMPLING
#define TH08_PSP_STAGE_RELATIVE_PERF_SAMPLING 0
#endif

namespace th08::psp
{
#if TH08_PSP_RUNTIME_TELEMETRY
// The telemetry path is absolute so gameplay may chdir to the original-data
// directory without losing the log.  Heap capture and allocation-hook paths
// are allocation-free.  The current fopen/fwrite telemetry sink can allocate
// inside newlib and remains explicit pre-existing diagnostic debt.
void MemoryTelemetryInitialize(const char *gameDirectory);
void MemoryTelemetryMarkPhase(const char *phase);
void MemoryTelemetrySampleGameFrame();
// Called immediately after RenderPerfTelemetryEndFrame at the real PSP
// Present boundary.  Stage-relative perf windows are cut here so no partial
// current-frame counters can leak across their baseline.
void MemoryTelemetryAfterPresent();
void MemoryTelemetryShutdown();
#else
inline void MemoryTelemetryInitialize(const char *) {}
inline void MemoryTelemetryMarkPhase(const char *) {}
inline void MemoryTelemetrySampleGameFrame() {}
inline void MemoryTelemetryAfterPresent() {}
inline void MemoryTelemetryShutdown() {}
#endif
} // namespace th08::psp

// These wrappers cover the reconstructed engine's ZunMemory allocations and
// C++ new/delete without changing the allocation layout.  malloc_usable_size
// is sampled before free, so live/peak counters do not need an allocation map.
extern "C" void *th08_psp_tracked_malloc(std::size_t size, const char *owner);
extern "C" void *th08_psp_tracked_realloc(void *memory, std::size_t size, const char *owner);
extern "C" void th08_psp_tracked_free(void *memory);
