#pragma once

// TH08_PSP_TICK_GATE_BYPASS: the retail main loop runs one simulation tick
// only when the wall clock has crossed the next 1/60 s boundary and otherwise
// yields (Sleep(0)) and re-enters through the message pump.  On PSP the render
// cadence (WaitForPspRenderCadence, VBlank based) already paces the
// simulation to 60 Hz, so the wall-clock gate is a second pacer with a
// different phase: between the two the main thread only spins in the
// PeekMessage/SDL pump (r121 hardware: 1.3 + 0.6 ms per tick, up to 10 gated
// Render calls per tick even when the loop is behind schedule).  With the
// switch every Render call runs a tick; the cadence wait keeps 60 Hz when the
// loop is ahead.  lastFrameTime bookkeeping is unchanged.  Input, RNG and
// replay bytes are untouched (timing only).  Default OFF; PC oracle never
// sees it.
#if defined(PSP) && defined(TH08_PSP_TICK_GATE_BYPASS) && TH08_PSP_TICK_GATE_BYPASS
#define TH08_PSP_TICK_GATE_BYPASS_ENABLED 1
#else
#define TH08_PSP_TICK_GATE_BYPASS_ENABLED 0
#endif
