#pragma once
// TH08_PSP_ANM_TEXTURE_16BIT: upload 32-bit (A8R8G8B8) ANM textures to the GE as
// 16-bit (5551 when alpha is binary, otherwise 4444).  Halves the renderer-arena
// footprint of the face/background sheets that fill the 12 MiB arena by stage 6
// (R-057).  Colour precision drops to 4-5 bits per channel.  Default OFF.
#if defined(PSP) && defined(TH08_PSP_ANM_TEXTURE_16BIT) && TH08_PSP_ANM_TEXTURE_16BIT
#define TH08_PSP_ANM_TEXTURE_16BIT_ENABLED 1
#else
#define TH08_PSP_ANM_TEXTURE_16BIT_ENABLED 0
#endif
