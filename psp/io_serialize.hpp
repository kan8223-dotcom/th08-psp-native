#pragma once

// TH08_PSP_IO_SERIALIZE: every synchronous storage transaction bracketed by
// IoActivityScope (Win32/POSIX bridge, BOOT.LOG chunks, directory scans) and
// the SDL_ttf font stream take one recursive kernel mutex, so the main thread,
// the BGM streaming thread and glyph loads never overlap on the same device.
// Enabled at runtime only on PSP Go (ef0), where overlapping reads have shown
// 30 s stalls followed by failed reads (R-051).  Requires TH08_PSP_GO_IO_LAMP.
// Default OFF; PC oracle never sees it.
#if defined(PSP) && defined(TH08_PSP_IO_SERIALIZE) && TH08_PSP_IO_SERIALIZE && \
    defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP
#define TH08_PSP_IO_SERIALIZE_ENABLED 1
#else
#define TH08_PSP_IO_SERIALIZE_ENABLED 0
#endif
