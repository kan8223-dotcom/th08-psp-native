#pragma once

// TH08_PSP_FONT_STREAM_CACHE: the SDL_ttf/FreeType font stream is served from
// a small block cache (4 KiB blocks) inside the bridge, so the storage driver
// only ever sees block-aligned 4 KiB reads and one seek per block.  On the PSP
// Go the FreeType access pattern (tell/seek/tiny-read bursts) precedes every
// 30 s ef0 stall (R-051, IO_TRACE evidence).  Requires TH08_PSP_IO_SERIALIZE
// (the stream itself is routed through the bridge by that switch).
// Default OFF; PC oracle never sees it.
#if defined(PSP) && defined(TH08_PSP_FONT_STREAM_CACHE) && TH08_PSP_FONT_STREAM_CACHE && \
    defined(TH08_PSP_IO_SERIALIZE) && TH08_PSP_IO_SERIALIZE
#define TH08_PSP_FONT_STREAM_CACHE_ENABLED 1
#else
#define TH08_PSP_FONT_STREAM_CACHE_ENABLED 0
#endif
