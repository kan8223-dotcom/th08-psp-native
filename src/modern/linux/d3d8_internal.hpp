#pragma once

#include <d3d8.h>

struct LinuxSurfaceAccess
{
    BYTE *pixels;
    UINT width;
    UINT height;
    UINT pitch;
    D3DFORMAT format;
};

struct LinuxTextureRegionStats
{
    UINT sampledPixels;
    UINT visiblePixels;
    UINT colorfulPixels;
    UINT nearWhitePixels;
    UINT visibleEdgePixels;
    UINT modulatedVisiblePixels;
    UINT modulatedColorfulPixels;
    UINT modulatedNearWhitePixels;
};

struct LinuxFramebufferDeltaStats
{
    UINT sampledPixels;
    UINT changedPixels;
    UINT colorfulChangedPixels;
    UINT chromaticChangedPixels;
    UINT nearWhiteChangedPixels;
    unsigned long long absoluteRgbDifference;
};

bool th08_linux_surface_access(IDirect3DSurface8 *surface, LinuxSurfaceAccess *access, bool readBackbuffer);
// Releases transient (scratch-backed) backbuffer shadow storage after a copy.
void th08_linux_surface_access_end(IDirect3DSurface8 *surface);
#if defined(PSP)
// Backbuffer-to-texture capture without a destination CPU copy (see d3d8_compat.cpp).
bool th08_linux_capture_direct_to_texture(IDirect3DSurface8 *destination, const RECT *destinationRect,
                                          const LinuxSurfaceAccess &source, const RECT *sourceRect,
                                          D3DCOLOR colorKey);
#endif
void th08_linux_surface_changed(IDirect3DSurface8 *surface);
void th08_linux_texture_mark_static(IDirect3DTexture8 *texture);
#if defined(PSP)
// The Bullet renderer brackets only its six enemy-bullet buckets with this
// token.  The D3D compatibility layer requires the token in addition to the
// exact indexed-quad shape before it may use PSPGL's native packed submit.
void th08_psp_bullet_direct_ge_set_batch(bool active);
// All-Item mixed batches use the Bullet arena but never borrow the Bullet
// owner token.  The separate token makes accidental overlap fail closed.
void th08_psp_item_mixed_ge_set_batch(bool active);
// ItemManager has a distinct presentation-only owner token.  Keeping it
// separate prevents the earlier Item pass from consuming or authorizing the
// enemy-bullet arena, while both owners may use the same immutable quad index
// authority and PSPGL submit ABI.
void th08_psp_item_direct_ge_set_batch(bool active);
// Item conversion storage is stage-scoped. The stage owner calls this only
// after every gameplay draw chain has been cut; the backend fences pending GE
// reads, releases the arena block, and rearms lazy allocation for the next
// stage after its texture uploads have completed.
bool th08_psp_item_direct_ge_release_stage(IDirect3DDevice8 *device);
#if defined(TH08_PSP_ASCII_POPUP_BATCH) && TH08_PSP_ASCII_POPUP_BATCH
// Reserve the compatibility renderer's existing converted-vertex workspace
// before AsciiManager mutates its shared popup VM.  Failure is therefore an
// atomic, no-render-state canonical fallback.
bool th08_psp_reserve_ascii_popup_sprite_pairs(IDirect3DDevice8 *device,
                                                UINT spriteCount);
// Submit an already validated array of two diagonal D3D XYZRHW vertices per
// axis-aligned popup glyph.  PSPGL maps GL_SPRITES_PSP to GE_SPRITES, while
// retaining ownership of its active list, texture, draw and depth buffers.
bool th08_psp_draw_ascii_popup_sprite_pairs(IDirect3DDevice8 *device,
                                             const void *vertices,
                                             UINT spriteCount, UINT stride);
#endif
#if defined(TH08_PSP_SCORE_POPUP_NATIVE_GE) && \
    TH08_PSP_SCORE_POPUP_NATIVE_GE
// Device-lifetime, presentation-only counters for the score-popup private
// PSPGL submit.  The query is a read-only snapshot: it neither resets an
// interval nor touches D3D/GL state, so telemetry MARKs cannot perturb the
// following replay or performance window.
struct PspScorePopupNativeGeStats
{
    unsigned long attempts;
    unsigned long submits;
    unsigned long clientFallbacks;
};
bool th08_psp_query_score_popup_native_ge_stats(
    IDirect3DDevice8 *device, PspScorePopupNativeGeStats *stats);
#endif
#if defined(TH08_PSP_BULLET_PACKED_VERTEX_AUDIT) && \
    TH08_PSP_BULLET_PACKED_VERTEX_AUDIT
// Device-lifetime, read-only snapshot for the M0 packed-once proof.  The
// candidate is a 96-byte stack shadow and never reaches PSPGL or the GE.
struct PspBulletPackedVertexAuditStats
{
    unsigned long attempts;
    unsigned long attemptedQuads;
    unsigned long eligibleBatches;
    unsigned long eligibleQuads;
    unsigned long comparedQuads;
    unsigned long matchedQuads;
    unsigned long mismatchQuads;
    unsigned long comparedVertices;
    unsigned long mismatchVertices;
    unsigned long mismatchBytes;
    unsigned long uniformDiffuseQuads;
    unsigned long perVertexDiffuseQuads;
    unsigned long canonicalFallbacks;
    unsigned long ownerFallbacks;
    unsigned long stateFallbacks;
    unsigned long indexFallbacks;
    unsigned long capacityFallbacks;
    unsigned long submitFallbacks;
    unsigned long canonicalNativeSubmits;
    unsigned long canonicalNativeSubmittedQuads;
    unsigned long mismatchUBytes;
    unsigned long mismatchVBytes;
    unsigned long mismatchColorBytes;
    unsigned long mismatchXBytes;
    unsigned long mismatchYBytes;
    unsigned long mismatchZBytes;
    unsigned long mismatchOtherBytes;
    unsigned long firstMismatchValid;
    unsigned long firstMismatchBatch;
    unsigned long firstMismatchQuad;
    unsigned long firstMismatchVertex;
    unsigned long firstMismatchByte;
};
bool th08_psp_query_bullet_packed_vertex_audit_stats(
    IDirect3DDevice8 *device, PspBulletPackedVertexAuditStats *stats);
#endif
#if defined(TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH) && \
    TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH
// M1 consumes the same final four-corner D3D quad that M0 proved, but writes
// only the final 24-byte PSP layout into the renderer-owned Present-fenced
// arena.  Begin/append/submit/end are presentation-only and never touch Bullet
// slots, VM order, RNG, collision state, or replay data.
struct PspBulletPackedVertexFastpathStats
{
    unsigned long beginAttempts;
    unsigned long acceptedBatches;
    unsigned long canonicalFallbackBatches;
    unsigned long appendAttempts;
    unsigned long appendedQuads;
    unsigned long packedVertices;
    unsigned long uniformDiffuseQuads;
    unsigned long perVertexDiffuseQuads;
    unsigned long submitAttempts;
    unsigned long submittedRuns;
    unsigned long submittedQuads;
    unsigned long nativeSubmits;
    unsigned long nativeSubmittedQuads;
    unsigned long clientFallbackSubmits;
    unsigned long clientFallbackQuads;
    unsigned long ownerFallbacks;
    unsigned long stateFallbacks;
    unsigned long indexFallbacks;
    unsigned long capacityFallbacks;
    unsigned long contractFallbacks;
    unsigned long abandonedRuns;
    unsigned long abandonedQuads;
    unsigned long recoverySplitRuns;
    unsigned long recoverySplitQuads;
    unsigned long maxRunQuads;
    unsigned long arenaHighWaterVertices;
    unsigned long arenaCapacityVertices;
};
bool th08_psp_bullet_packed_vertex_begin(IDirect3DDevice8 *device);
bool th08_psp_bullet_packed_vertex_append(IDirect3DDevice8 *device,
                                          const void *quad, UINT stride);
bool th08_psp_bullet_packed_vertex_submit(
    IDirect3DDevice8 *device, UINT quadCount,
    const unsigned short *indices, UINT indexCount);
void th08_psp_bullet_packed_vertex_end(IDirect3DDevice8 *device);
void th08_psp_bullet_packed_vertex_note_recovery_split(
    IDirect3DDevice8 *device, UINT submittedQuads);
bool th08_psp_query_bullet_packed_vertex_fastpath_stats(
    IDirect3DDevice8 *device, PspBulletPackedVertexFastpathStats *stats);
#endif
#if defined(TH08_PSP_ITEM_NATURAL_QUADS) && \
    TH08_PSP_ITEM_NATURAL_QUADS
// The frontend has already proved the untouched canonical 6V span and exact
// duplicate topology.  The backend packs corners 0/1/2/5 into its ordinary
// same-call client storage and emits one indexed draw at the existing Flush.
// A rejection occurs before PrepareState or PRIM, leaving the caller's 6V
// range authoritative for its original single DrawPrimitiveUP fallback.
enum PspItemNaturalQuadSubmitResult
{
    PSP_ITEM_NATURAL_SUBMIT_NATIVE = 0,
    PSP_ITEM_NATURAL_SUBMIT_CLIENT,
    PSP_ITEM_NATURAL_SUBMIT_CLIENT_AFTER_NATIVE_REJECT,
    PSP_ITEM_NATURAL_REJECT_DEVICE,
    PSP_ITEM_NATURAL_REJECT_STATE,
    PSP_ITEM_NATURAL_REJECT_CAPACITY,
    PSP_ITEM_NATURAL_REJECT_INDEX,
};
PspItemNaturalQuadSubmitResult th08_psp_draw_item_natural_quads(
    IDirect3DDevice8 *device, const void *canonicalVertices,
    UINT quadCount, UINT stride, const unsigned short *indices,
    UINT indexCount);
#endif
#if (defined(TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) && \
     TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) || \
    (defined(TH08_PSP_ITEM_MIXED_QUADS_FASTPATH) && \
     TH08_PSP_ITEM_MIXED_QUADS_FASTPATH)
// Device-lifetime accounting for the append-only Present-fenced arena shared
// by the Bullet and Item mixed submitters.  Owners remain separate even though
// the storage cursor/high-water are shared; the read-only query never resets
// counters or touches D3D/GL state.
struct PspMixedGeBackendStats
{
    unsigned long bulletAttempts;
    unsigned long bulletSubmittedBatches;
    unsigned long bulletSubmittedQuads;
    unsigned long bulletFallbacks;
    unsigned long bulletArenaExhaustions;
    unsigned long itemAttempts;
    unsigned long itemSubmittedBatches;
    unsigned long itemSubmittedQuads;
    unsigned long itemFallbacks;
    unsigned long itemArenaExhaustions;
    unsigned long sharedArenaHighWaterVertices;
    unsigned long sharedArenaCapacityVertices;
};
bool th08_psp_query_mixed_ge_backend_stats(
    IDirect3DDevice8 *device, PspMixedGeBackendStats *stats);
#endif
#if (defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH) && \
     TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH) || \
    (defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
     TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH)
// Generic PSPGL-owned GE_SPRITES bridge.  The mutable input is a validated
// run of two diagonal 28-byte D3D XYZRHW vertices.  D3D guards precede its
// forward in-place pack to PSPGL's 24-byte native format.  PSPGL then either
// owns one successful transient copy/submit or returns false before PRIM; the
// caller retains an independent canonical replay authority.  No game-side
// heap arena or raw sceGuStart is involved.
bool th08_psp_draw_sprite_pairs_in_place(IDirect3DDevice8 *device,
                                         void *vertices,
                                         UINT spriteCount, UINT stride);
#endif
#if defined(TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) && \
    TH08_PSP_BULLET_MIXED_QUADS_FASTPATH
// One enemy-bullet state run is represented as a two-corner prefix followed
// by a four-corner indexed suffix.  The backend validates and converts both
// immutable D3D ranges into the accepted Bullet direct-GE arena, then performs
// one atomic PSPGL mixed submit.  False means no primitive was emitted and the
// caller may replay its untouched canonical geometry.
bool th08_psp_draw_bullet_mixed_quads(
    IDirect3DDevice8 *device, const void *pairVertices, UINT pairCount,
    const void *quadVertices, UINT quadCount, UINT stride,
    const unsigned short *quadIndices, UINT quadIndexCount);
#endif
#if defined(TH08_PSP_ITEM_MIXED_QUADS_FASTPATH) && \
    TH08_PSP_ITEM_MIXED_QUADS_FASTPATH
// The Item traversal uses the same immutable prefix/suffix ABI and native
// arena as Bullet, under a disjoint presentation-only owner token.
bool th08_psp_draw_item_mixed_quads(
    IDirect3DDevice8 *device, const void *pairVertices, UINT pairCount,
    const void *quadVertices, UINT quadCount, UINT stride,
    const unsigned short *quadIndices, UINT quadIndexCount);
#endif
void th08_linux_surface_mark_static(IDirect3DSurface8 *surface);
void th08_linux_surface_discard_readback(IDirect3DSurface8 *surface);
bool th08_linux_surface_load_image_memory(IDirect3DDevice8 *device, const void *data,
                                          UINT size, IDirect3DSurface8 **surface,
                                          UINT *width, UINT *height);
void th08_linux_dialogue_snapshot_restore(IDirect3DDevice8 *device);
bool th08_linux_surface_capture_native(UINT logicalWidth, UINT logicalHeight,
                                       bool readDisplayedFrame,
                                       IDirect3DSurface8 **surface);
bool th08_linux_texture_upload_static(IDirect3DTexture8 *texture, const void *pixels,
                                      UINT sourcePitch, D3DFORMAT sourceFormat);
HRESULT th08_linux_surface_area_average_from_memory(
    IDirect3DSurface8 *destination, const RECT *destinationRect,
    const void *sourcePixels, UINT sourceWidth, UINT sourceHeight,
    UINT sourcePitch, D3DFORMAT sourceFormat, const RECT *sourceRect,
    D3DCOLOR colorKey);
#endif
bool th08_linux_texture_region_stats(IDirect3DTexture8 *texture, float u0, float v0,
                                     float u1, float v1, D3DCOLOR diffuse,
                                     LinuxTextureRegionStats *stats);
bool th08_linux_begin_framebuffer_probe(IDirect3DDevice8 *device, int left, int top,
                                        int right, int bottom);
bool th08_linux_end_framebuffer_probe(IDirect3DDevice8 *device,
                                      LinuxFramebufferDeltaStats *stats);
