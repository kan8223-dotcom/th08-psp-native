#pragma once

// TH08_PSP_IO_BOUNCE_HIGH: on the PSP Go (ef0), reads and writes whose user
// buffer lies in the extended memory above 0x0A000000 are staged through a
// 64 KiB buffer inside the module image (low memory).  Suspected cause of the
// 30 s ef0 stalls (R-051): the internal-storage driver's DMA never reaches the
// extended region, so the request times out.  Runtime-enabled together with
// TH08_PSP_IO_SERIALIZE (the stage buffer relies on that serialization).
// Default OFF; PC oracle never sees it.
#if defined(PSP) && defined(TH08_PSP_IO_BOUNCE_HIGH) && TH08_PSP_IO_BOUNCE_HIGH && \
    defined(TH08_PSP_IO_SERIALIZE) && TH08_PSP_IO_SERIALIZE
#define TH08_PSP_IO_BOUNCE_HIGH_ENABLED 1
#else
#define TH08_PSP_IO_BOUNCE_HIGH_ENABLED 0
#endif
