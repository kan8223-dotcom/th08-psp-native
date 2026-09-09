#pragma once
// TH08_PSP_BG_FAST_PROJECT: Background::RenderObjects projects each type-2
// quad's translation twice through D3DXVec3Project (two 4x4 matrix products
// per call).  Replace with a bit-identical origin projection: the same
// summation order as D3DXMatrixMultiply/TransformCoord, but only the
// translation row is computed (2 vec4 x mat4 instead of 2 mat4 x mat4).
// Default OFF.
#if defined(PSP) && defined(TH08_PSP_BG_FAST_PROJECT) && TH08_PSP_BG_FAST_PROJECT
#define TH08_PSP_BG_FAST_PROJECT_ENABLED 1
#else
#define TH08_PSP_BG_FAST_PROJECT_ENABLED 0
#endif
