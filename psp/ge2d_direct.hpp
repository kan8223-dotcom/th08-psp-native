#pragma once
// TH08_PSP_GE_2D_DIRECT: pre-transformed (XYZRHW) draws bypass PSPGL's
// per-draw path (matrix reload, state walk, two syscalls) and are written
// straight into PSPGL's current command list as GE through-mode primitives.
// PSPGL keeps owning the list buffers, textures, framebuffers and the swap;
// its register shadow is updated so its own 3D draws stay consistent.
#if defined(PSP) && defined(TH08_PSP_GE_2D_DIRECT) && TH08_PSP_GE_2D_DIRECT
#define TH08_PSP_GE_2D_DIRECT_ENABLED 1
#else
#define TH08_PSP_GE_2D_DIRECT_ENABLED 0
#endif

// Same layout as d3d8_compat's PspClientVertex (u v | rgba | x y z), 24 bytes.
struct Th08Ge2dVertex
{
    float u, v;
    unsigned char r, g, b, a;
    float x, y, z;
};

extern "C"
{
// src: vertices in the 640x480 pre-transformed space (x,y already +0.5,
// z = 1 - 2*z').  dst: cached RAM the GE may read until the present fence
// (may alias src).  Returns 1 when submitted, 0 when the caller must use the
// PSPGL path (no context, display-list compile, or a CLUT texture without
// GE_DRAW_DIRECT's palette handling).
int th08_ge2d_direct_submit(const Th08Ge2dVertex *src, unsigned vertexCount, Th08Ge2dVertex *dst,
                            const unsigned short *indices, unsigned indexCount, float fitLeft,
                            float fitScaleX, float scaleY);
void th08_ge2d_direct_stats(unsigned long *submits, unsigned long *fallbacks, unsigned long *vertices);
// Single-pass variant: the caller converts straight into dst with these
// parameters (1 = direct path possible), then submits the prepared vertices.
struct Th08Ge2dParams
{
    float texW, texH; // 1 when untextured
    int flipped;      // v' = 1 - v
    float sz, tz;     // depth = sz * (-z3d) + tz
};
int th08_ge2d_direct_params(Th08Ge2dParams *params);
int th08_ge2d_direct_submit_prepared(const Th08Ge2dVertex *dst, unsigned vertexCount, const unsigned short *indices,
                                     unsigned indexCount);
}
