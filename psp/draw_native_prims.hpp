#pragma once
// TH08_PSP_DRAW_NATIVE_PRIMS: submit 3D/2D triangle batches from the generic
// D3D draw path as packed 24-byte PSP native vertices through the existing
// no-copy indexed GE hook (__pspgl_th08_draw_native_indexed_triangles) instead
// of PSPGL client arrays.  Removes PSPGL's per-vertex client-array conversion
// for stage background effects and objects.  Vertices live in a small
// renderer-arena partition recycled per Present like the Item direct path.
// Falls back to the client-array path when the partition is full or the hook
// rejects the batch.  Default OFF.
#if defined(PSP) && defined(TH08_PSP_DRAW_NATIVE_PRIMS) && TH08_PSP_DRAW_NATIVE_PRIMS
#define TH08_PSP_DRAW_NATIVE_PRIMS_ENABLED 1
#else
#define TH08_PSP_DRAW_NATIVE_PRIMS_ENABLED 0
#endif
