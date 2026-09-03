#pragma once

// Runtime performance/memory/audio instrumentation is enabled for diagnostic
// builds unless the PSP product build explicitly compiles it out.  This gate
// never controls allocation ownership, arena guards, crash breadcrumbs, audio
// mixing, rendering, gameplay, RNG, or replay state.
#ifndef TH08_PSP_RUNTIME_TELEMETRY
#define TH08_PSP_RUNTIME_TELEMETRY 1
#endif

#if TH08_PSP_RUNTIME_TELEMETRY != 0 && TH08_PSP_RUNTIME_TELEMETRY != 1
#error TH08_PSP_RUNTIME_TELEMETRY must be 0 or 1
#endif

