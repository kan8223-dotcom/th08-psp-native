#pragma once

#if defined(PSP) && defined(TH08_PSP_GE_DRAW_DIRECT) && TH08_PSP_GE_DRAW_DIRECT
#define TH08_PSP_GE_DRAW_DIRECT_ENABLED 1
#else
#define TH08_PSP_GE_DRAW_DIRECT_ENABLED 0
#endif

// Rendering only. PSPGL still owns textures, command-list storage and the
// present fence; this backend owns packed draw submission and render state.
// Function/factor values use the existing GL enum mapping at the D3D edge.
struct Th08GeDrawState
{
    unsigned blend, blendSource, blendDestination;
    unsigned alpha, alphaFunction, alphaReference;
    unsigned depth, depthFunction, depthWrite;
    unsigned fog, fogColor;
    float fogStart, fogEnd;
};

extern "C"
{
int th08_ge_draw_direct_state(const Th08GeDrawState *state);
// Caller owns both immutable ranges until the existing Present fence, and
// has validated the immutable index table against the vertex count.
int th08_ge_draw_direct_triangles(const void *vertices, unsigned vertexBytes,
                                 const unsigned short *indices, unsigned indexBytes,
                                 unsigned indexCount);
// Flush state and, after it, the current palette. Also used by through-mode
// submissions: the T8 image/mode registers already belong to the bound texture.
void th08_ge_draw_direct_flush_state();
void th08_ge_draw_direct_stats(unsigned long *submits, unsigned long *fallbacks,
                              unsigned long *palettes, unsigned long *states);
}
