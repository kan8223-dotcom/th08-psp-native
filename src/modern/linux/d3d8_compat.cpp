#include "d3d8_internal.hpp"
#include "Gui.hpp"

#include <SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <math.h>
#if defined(PSP)
#include <malloc.h>
#include "anm_scratch.hpp"
#include "boot_checkpoint.hpp"
#include "fileio.hpp"
#include "ge4_bridge.hpp"
#include "memory_telemetry.hpp"
#include "perf_attribution.hpp"
#include "render_perf_telemetry.hpp"
#include "draw_priority_subprofile.hpp"
#include "swap_nowait.hpp"
#include "swap_async.hpp"
#include "swap_triple.hpp"
#include "pspgl_stream_arena.hpp"
#include "dialogue_snapshot_no_promote.hpp"
#include "dialogue_snapshot_diag.hpp"
#include "dialogue_snapshot_at_background.hpp"
#include "dialogue_live_background.hpp"
#if TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE_ENABLED
// Set by RestoreDialogueSnapshot around its surface-cache draw so the texture
// upload below never arms the GE4 static-upload (promotion) hint for it.
static bool pspSuppressStaticUploadPromotion = false;
#endif
#if TH08_PSP_DIALOGUE_SNAPSHOT_DIAG_ENABLED
// R-045 diagnostic: while set, DrawPspSurfaceCache draws a flat magenta quad.
static bool pspDialogueRestoreDiagFlash = false;
#endif
#include "perf_env.hpp"
#include <pspdisplay.h>
// pspthreadman.h drags psptypes.h (u32 etc.) into this TU; declare the one
// clock entry point the flip guard needs instead.
extern "C" long long sceKernelGetSystemTimeWide(void);
#include "render_resource_arena.hpp"
#include "backbuffer_shadow.hpp"
#include "anm_texture_16bit.hpp"

// Import the one GE query used here without pulling PSPSDK's legacy u32
// typedefs into TH08's reconstruction typedef namespace.
extern "C" unsigned int sceGeEdramGetSize(void);
#else
#define TH08_PSP_BOOT_CHECKPOINT(phase, state, result) ((void)0)
#endif
#ifndef TH08_PSP_SWAP_NOWAIT_ENABLED
#define TH08_PSP_SWAP_NOWAIT_ENABLED 0
#endif
#ifndef TH08_PSP_SWAP_ASYNC_ENABLED
#define TH08_PSP_SWAP_ASYNC_ENABLED 0
#endif
#ifndef TH08_PSP_PERF_ENV_ENABLED
#define TH08_PSP_PERF_ENV_ENABLED 0
#endif
#include <new>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#if defined(PSP) && defined(TH08_PSP_PREPARE_STATE_CACHE) && \
    TH08_PSP_PREPARE_STATE_CACHE
#define TH08_PSP_PREPARE_STATE_CACHE_ENABLED 1
#else
#define TH08_PSP_PREPARE_STATE_CACHE_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_BULLET_UNIFIED_QUADS) && \
    TH08_PSP_BULLET_UNIFIED_QUADS
#define TH08_PSP_BULLET_UNIFIED_QUADS_ENABLED 1
#else
#define TH08_PSP_BULLET_UNIFIED_QUADS_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_BULLET_DIRECT_GE) && \
    TH08_PSP_BULLET_DIRECT_GE
#define TH08_PSP_BULLET_DIRECT_GE_ENABLED 1
#else
#define TH08_PSP_BULLET_DIRECT_GE_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_BULLET_PACKED_VERTEX_AUDIT) && \
    TH08_PSP_BULLET_PACKED_VERTEX_AUDIT
#define TH08_PSP_BULLET_PACKED_VERTEX_AUDIT_ENABLED 1
#else
#define TH08_PSP_BULLET_PACKED_VERTEX_AUDIT_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH) && \
    TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH
#define TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH_ENABLED 1
#else
#define TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) && \
    TH08_PSP_BULLET_MIXED_QUADS_FASTPATH
#define TH08_PSP_BULLET_MIXED_QUADS_FASTPATH_ENABLED 1
#else
#define TH08_PSP_BULLET_MIXED_QUADS_FASTPATH_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_MIXED_QUADS_FASTPATH) && \
    TH08_PSP_ITEM_MIXED_QUADS_FASTPATH
#define TH08_PSP_ITEM_MIXED_QUADS_FASTPATH_ENABLED 1
#else
#define TH08_PSP_ITEM_MIXED_QUADS_FASTPATH_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_DIRECT_GE) && \
    TH08_PSP_ITEM_DIRECT_GE
#define TH08_PSP_ITEM_DIRECT_GE_ENABLED 1
#else
#define TH08_PSP_ITEM_DIRECT_GE_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_NATURAL_QUADS) && \
    TH08_PSP_ITEM_NATURAL_QUADS
#define TH08_PSP_ITEM_NATURAL_QUADS_ENABLED 1
#else
#define TH08_PSP_ITEM_NATURAL_QUADS_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_NATURAL_NATIVE_COPY) && \
    TH08_PSP_ITEM_NATURAL_NATIVE_COPY
#define TH08_PSP_ITEM_NATURAL_NATIVE_COPY_ENABLED 1
#else
#define TH08_PSP_ITEM_NATURAL_NATIVE_COPY_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_BATCH) && \
    TH08_PSP_ASCII_POPUP_BATCH
#define TH08_PSP_ASCII_POPUP_BATCH_ENABLED 1
#else
#define TH08_PSP_ASCII_POPUP_BATCH_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH) && \
    TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH
#define TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH_ENABLED 1
#else
#define TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH) && \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH
#define TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH_ENABLED 1
#else
#define TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH_ENABLED 0
#endif

#if TH08_PSP_ASCII_POPUP_BATCH_ENABLED || \
    TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH_ENABLED || \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH_ENABLED || \
    TH08_PSP_BULLET_MIXED_QUADS_FASTPATH_ENABLED || \
    TH08_PSP_ITEM_MIXED_QUADS_FASTPATH_ENABLED
#define TH08_PSP_SPRITE_PAIR_BATCH_ENABLED 1
#else
#define TH08_PSP_SPRITE_PAIR_BATCH_ENABLED 0
#endif

#if defined(PSP) && defined(TH08_PSP_SCORE_POPUP_NATIVE_GE) && \
    TH08_PSP_SCORE_POPUP_NATIVE_GE
#define TH08_PSP_SCORE_POPUP_NATIVE_GE_ENABLED 1
#else
#define TH08_PSP_SCORE_POPUP_NATIVE_GE_ENABLED 0
#endif

#if TH08_PSP_BULLET_DIRECT_GE_ENABLED && \
    !TH08_PSP_BULLET_UNIFIED_QUADS_ENABLED
#error TH08_PSP_BULLET_DIRECT_GE requires TH08_PSP_BULLET_UNIFIED_QUADS
#endif

#if TH08_PSP_BULLET_PACKED_VERTEX_AUDIT_ENABLED && \
    !TH08_PSP_BULLET_DIRECT_GE_ENABLED
#error TH08_PSP_BULLET_PACKED_VERTEX_AUDIT requires Bullet direct GE
#endif

#if TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH_ENABLED && \
    !TH08_PSP_BULLET_DIRECT_GE_ENABLED
#error TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH requires Bullet direct GE
#endif

#if TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH_ENABLED && \
    TH08_PSP_BULLET_PACKED_VERTEX_AUDIT_ENABLED
#error Bullet packed-vertex audit and product are mutually exclusive
#endif

#if TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH_ENABLED && \
    (TH08_PSP_BULLET_MIXED_QUADS_FASTPATH_ENABLED || \
     TH08_PSP_ITEM_MIXED_QUADS_FASTPATH_ENABLED)
#error Bullet packed-vertex product is isolated from mixed 2V products
#endif

#if TH08_PSP_BULLET_MIXED_QUADS_FASTPATH_ENABLED && \
    (!TH08_PSP_BULLET_UNIFIED_QUADS_ENABLED || \
     !TH08_PSP_BULLET_DIRECT_GE_ENABLED)
#error TH08_PSP_BULLET_MIXED_QUADS_FASTPATH requires Bullet unified/direct GE
#endif

#if TH08_PSP_ITEM_MIXED_QUADS_FASTPATH_ENABLED && \
    (!TH08_PSP_BULLET_UNIFIED_QUADS_ENABLED || \
     !TH08_PSP_BULLET_DIRECT_GE_ENABLED)
#error TH08_PSP_ITEM_MIXED_QUADS_FASTPATH requires Bullet unified/direct GE
#endif

#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED && \
    !TH08_PSP_BULLET_UNIFIED_QUADS_ENABLED
#error TH08_PSP_ITEM_NATURAL_QUADS requires Bullet unified quads
#endif

#if TH08_PSP_ITEM_NATURAL_NATIVE_COPY_ENABLED && \
    !TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
#error TH08_PSP_ITEM_NATURAL_NATIVE_COPY requires Item natural quads
#endif

#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED && \
    (TH08_PSP_ITEM_DIRECT_GE_ENABLED || \
     TH08_PSP_ITEM_MIXED_QUADS_FASTPATH_ENABLED || \
     TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH_ENABLED)
#error ITEM natural quads are isolated from prior Item topology products
#endif

#if TH08_PSP_ITEM_DIRECT_GE_ENABLED && \
    !TH08_PSP_BULLET_UNIFIED_QUADS_ENABLED
#error TH08_PSP_ITEM_DIRECT_GE requires TH08_PSP_BULLET_UNIFIED_QUADS
#endif

#if TH08_PSP_SCORE_POPUP_NATIVE_GE_ENABLED && \
    !TH08_PSP_ASCII_POPUP_BATCH_ENABLED
#error TH08_PSP_SCORE_POPUP_NATIVE_GE requires TH08_PSP_ASCII_POPUP_BATCH
#endif

#if TH08_PSP_BULLET_DIRECT_GE_ENABLED || TH08_PSP_ITEM_DIRECT_GE_ENABLED
#define TH08_PSP_ANY_DIRECT_GE_ENABLED 1
#else
#define TH08_PSP_ANY_DIRECT_GE_ENABLED 0
#endif

#if TH08_PSP_PSPGL_STREAM_ARENA_ENABLED
extern "C" int __pspgl_th08_stream_arena_install(void *base, size_t halfBytes);
extern "C" void __pspgl_th08_stream_arena_begin_frame(unsigned parity);
extern "C" void __pspgl_th08_stream_arena_stats(unsigned long *allocs,
                                                 unsigned long *overflows,
                                                 unsigned long *peakBytes);
constexpr size_t kPspglStreamArenaHalfBytes = 256U * 1024U;
constexpr size_t kPspglStreamArenaLeaseBytes =
    kPspglStreamArenaHalfBytes;
#endif

#if TH08_PSP_ANY_DIRECT_GE_ENABLED
extern "C" int __pspgl_th08_draw_native_indexed_triangles(
    const void *vertices, unsigned vertexBytes,
    const unsigned short *indices, unsigned indexBytes,
    unsigned indexCount);
#endif

#if TH08_PSP_BULLET_MIXED_QUADS_FASTPATH_ENABLED || \
    TH08_PSP_ITEM_MIXED_QUADS_FASTPATH_ENABLED
extern "C" int __pspgl_th08_draw_native_mixed_quads(
    const void *pairVertices, unsigned pairVertexBytes,
    const void *quadVertices, unsigned quadVertexBytes,
    unsigned quadCount, const unsigned short *quadIndices,
    unsigned quadIndexBytes, unsigned quadIndexCount);
#endif

#if TH08_PSP_SCORE_POPUP_NATIVE_GE_ENABLED || \
    TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH_ENABLED || \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH_ENABLED
extern "C" int __pspgl_th08_draw_native_sprite_pairs_copy(
    const void *vertices, unsigned vertexBytes);
#endif

#if TH08_PSP_ITEM_NATURAL_NATIVE_COPY_ENABLED
extern "C" int __pspgl_th08_draw_native_indexed_quads_copy(
    const void *vertices, unsigned vertexBytes,
    const unsigned short *indices, unsigned indexBytes,
    unsigned quadCount);
#endif

namespace
{
class LinuxTexture;
#if defined(PSP)
// Diagnostics: who forced the 640x480 backbuffer shadow allocation.
static const char *gPspShadowReason = "?";
static const char *gPspSurfaceOp = "?";
extern "C" void th08_linux_note_surface_op(const char *op) { gPspSurfaceOp = op != NULL ? op : "?"; }
#endif
class LinuxSurface;

#if TH08_PSP_ANY_DIRECT_GE_ENABLED
// These mutually exclusive tokens are presentation-only. They never enter
// simulation state, and the native submit path rejects an accidental overlap.
bool g_PspBulletDirectGeBatchActive = false;
bool g_PspItemDirectGeBatchActive = false;
#if TH08_PSP_BULLET_MIXED_QUADS_FASTPATH_ENABLED || \
    TH08_PSP_ITEM_MIXED_QUADS_FASTPATH_ENABLED
bool g_PspItemMixedGeBatchActive = false;
#endif
// The immutable shared quad table has 0x600 entries. Larger Item passes are
// split at this boundary by AnmManager while retaining their original order.
constexpr UINT kPspBulletDirectGeMaxQuads = 0x600U;
constexpr UINT kPspBulletDirectGeVertexCapacity =
    kPspBulletDirectGeMaxQuads * 4U;

// Fixed in every Bullet direct-GE build so M1 OFF/ON preserves sizeof the
// renderer object and its heap placement.  Only behavior/query exposure is
// feature-gated; the storage contract itself never moves later members.
struct PspBulletPackedVertexFastpathStorageStats
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
#if TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH_ENABLED
static_assert(
    sizeof(PspBulletPackedVertexFastpathStorageStats) ==
        sizeof(PspBulletPackedVertexFastpathStats),
    "M1 fixed renderer reservation diverged from its public telemetry ABI");
#endif
#endif

#if TH08_PSP_ITEM_DIRECT_GE_ENABLED
// Do not reserve the 2,096-slot logical Item ceiling as permanent renderer
// storage.  The Stage 5 load needs one last 256 KiB surface and the original
// 201,216-byte reservation left too small a contiguous renderer block for it.
// 1,280 packed quads consume 120 KiB, fit below the measured post-load
// 129,664-byte largest block, and cover the measured Stage 5 peak of 964 with
// 316 slots of headroom.  A larger or more fragmented pass remains correct:
// the existing generic indexed path draws every overflowing batch in the
// same order without changing Item state or the logical pool limit.
constexpr UINT kPspItemDirectGeArenaQuads = 1280U;
constexpr UINT kPspItemDirectGeVertexCapacity =
    kPspItemDirectGeArenaQuads * 4U;
constexpr size_t kPspItemDirectGeArenaBytes =
    static_cast<size_t>(kPspItemDirectGeVertexCapacity) * 24U;
static_assert(kPspItemDirectGeArenaBytes == 122880U,
              "Bounded Item native arena payload must remain exactly 122880 bytes");
#endif

typedef void (APIENTRY *GenFramebuffersFunction)(GLsizei, GLuint *);
typedef void (APIENTRY *BindFramebufferFunction)(GLenum, GLuint);
typedef void (APIENTRY *FramebufferTexture2DFunction)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *CheckFramebufferStatusFunction)(GLenum);
typedef void (APIENTRY *DeleteFramebuffersFunction)(GLsizei, const GLuint *);
typedef void (APIENTRY *GenRenderbuffersFunction)(GLsizei, GLuint *);
typedef void (APIENTRY *BindRenderbufferFunction)(GLenum, GLuint);
typedef void (APIENTRY *RenderbufferStorageFunction)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *FramebufferRenderbufferFunction)(GLenum, GLenum, GLenum, GLuint);
typedef void (APIENTRY *DeleteRenderbuffersFunction)(GLsizei, const GLuint *);
typedef void (APIENTRY *FogCoordfFunction)(GLfloat);

struct FramebufferApi
{
    FramebufferApi()
        : genFramebuffers(NULL), bindFramebuffer(NULL), framebufferTexture2D(NULL),
          checkFramebufferStatus(NULL), deleteFramebuffers(NULL), genRenderbuffers(NULL),
          bindRenderbuffer(NULL), renderbufferStorage(NULL), framebufferRenderbuffer(NULL),
          deleteRenderbuffers(NULL)
    {
    }

    void *Load(const char *coreName, const char *extensionName)
    {
        void *procedure = SDL_GL_GetProcAddress(coreName);
        return procedure != NULL ? procedure : SDL_GL_GetProcAddress(extensionName);
    }

    bool Initialize()
    {
        genFramebuffers = reinterpret_cast<GenFramebuffersFunction>(
            Load("glGenFramebuffers", "glGenFramebuffersEXT"));
        bindFramebuffer = reinterpret_cast<BindFramebufferFunction>(
            Load("glBindFramebuffer", "glBindFramebufferEXT"));
        framebufferTexture2D = reinterpret_cast<FramebufferTexture2DFunction>(
            Load("glFramebufferTexture2D", "glFramebufferTexture2DEXT"));
        checkFramebufferStatus = reinterpret_cast<CheckFramebufferStatusFunction>(
            Load("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT"));
        deleteFramebuffers = reinterpret_cast<DeleteFramebuffersFunction>(
            Load("glDeleteFramebuffers", "glDeleteFramebuffersEXT"));
        genRenderbuffers = reinterpret_cast<GenRenderbuffersFunction>(
            Load("glGenRenderbuffers", "glGenRenderbuffersEXT"));
        bindRenderbuffer = reinterpret_cast<BindRenderbufferFunction>(
            Load("glBindRenderbuffer", "glBindRenderbufferEXT"));
        renderbufferStorage = reinterpret_cast<RenderbufferStorageFunction>(
            Load("glRenderbufferStorage", "glRenderbufferStorageEXT"));
        framebufferRenderbuffer = reinterpret_cast<FramebufferRenderbufferFunction>(
            Load("glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT"));
        deleteRenderbuffers = reinterpret_cast<DeleteRenderbuffersFunction>(
            Load("glDeleteRenderbuffers", "glDeleteRenderbuffersEXT"));
        return genFramebuffers != NULL && bindFramebuffer != NULL && framebufferTexture2D != NULL &&
               checkFramebufferStatus != NULL && deleteFramebuffers != NULL &&
               genRenderbuffers != NULL && bindRenderbuffer != NULL &&
               renderbufferStorage != NULL && framebufferRenderbuffer != NULL &&
               deleteRenderbuffers != NULL;
    }

    GenFramebuffersFunction genFramebuffers;
    BindFramebufferFunction bindFramebuffer;
    FramebufferTexture2DFunction framebufferTexture2D;
    CheckFramebufferStatusFunction checkFramebufferStatus;
    DeleteFramebuffersFunction deleteFramebuffers;
    GenRenderbuffersFunction genRenderbuffers;
    BindRenderbufferFunction bindRenderbuffer;
    RenderbufferStorageFunction renderbufferStorage;
    FramebufferRenderbufferFunction framebufferRenderbuffer;
    DeleteRenderbuffersFunction deleteRenderbuffers;
};

FramebufferApi g_framebufferApi;
FogCoordfFunction g_fogCoordf;

#if defined(PSP)
constexpr int kPspScreenWidth = 480;
constexpr int kPspScreenHeight = 272;
constexpr int kPspFitWidth = kPspScreenWidth;
constexpr int kPspFitLeft = 0;
constexpr UINT kPspNativeCaptureTextureWidth = 512;
constexpr UINT kPspNativeCaptureTextureHeight = 512;
constexpr size_t kPspNativeCaptureNativeBytes =
    static_cast<size_t>(kPspFitWidth) * kPspScreenHeight * sizeof(WORD);
constexpr size_t kPspNativeCaptureTextureBytes =
    static_cast<size_t>(kPspNativeCaptureTextureWidth) *
    kPspNativeCaptureTextureHeight * sizeof(WORD);
constexpr size_t kPspNativeCaptureWorkspaceBytes =
    kPspNativeCaptureNativeBytes + kPspNativeCaptureTextureBytes;

class PspGe4StaticUploadScope
{
  public:
    explicit PspGe4StaticUploadScope(bool immutable)
        : armed(immutable && th08_psp_ge4_active() != 0), finalized(false)
    {
        if (armed)
            th08_psp_ge4_static_upload_hint_begin();
    }

    ~PspGe4StaticUploadScope()
    {
        if (armed)
            th08_psp_ge4_static_upload_hint_end();
    }

    void Finalize(GLuint textureName)
    {
        if (!armed || finalized || textureName == 0)
            return;
        const GLclampf priority = 1.0f;
        glPrioritizeTextures(1, &textureName, &priority);
        finalized = true;
    }

  private:
    bool armed;
    bool finalized;
};

struct PspPhysicalRect
{
    int left;
    int bottom;
    int width;
    int height;
};

int PspScaleFloor(int value, int physicalExtent, int logicalExtent)
{
    return logicalExtent > 0 ? value * physicalExtent / logicalExtent : 0;
}

int PspScaleCeil(int value, int physicalExtent, int logicalExtent)
{
    return logicalExtent > 0
               ? (value * physicalExtent + logicalExtent - 1) / logicalExtent
               : 0;
}

PspPhysicalRect MakePspPhysicalRect(int left, int top, int right, int bottom,
                                    int logicalWidth, int logicalHeight)
{
    const int physicalLeft = kPspFitLeft +
                             PspScaleFloor(left, kPspFitWidth, logicalWidth);
    const int physicalRight = kPspFitLeft +
                              PspScaleCeil(right, kPspFitWidth, logicalWidth);
    const int physicalTop = PspScaleFloor(top, kPspScreenHeight, logicalHeight);
    const int physicalBottom = PspScaleCeil(bottom, kPspScreenHeight, logicalHeight);
    PspPhysicalRect result = {
        physicalLeft,
        kPspScreenHeight - physicalBottom,
        physicalRight - physicalLeft,
        physicalBottom - physicalTop,
    };
    return result;
}

void ApplyPspViewport()
{
    glViewport(kPspFitLeft, 0, kPspFitWidth, kPspScreenHeight);
}

void ClearPspFrameBands(int logicalWidth, int logicalHeight)
{
    if (logicalWidth <= 0 || logicalHeight <= 0)
        return;

    // TH07's stable PSP backend clears only the four HUD-frame bands.  TH08
    // also deliberately preserves the playfield between some frames, so a
    // whole-backbuffer clear would break fades and capture effects.  Clearing
    // these bands prevents pre-SDL/bootstrap pixels from surviving wherever a
    // transition does not redraw the HUD.
    const int topBandBottom = 16 * logicalHeight / 480;
    const int bottomBandTop = 464 * logicalHeight / 480;
    const int playfieldLeft = 32 * logicalWidth / 640;
    const int playfieldRight = 416 * logicalWidth / 640;
    const int bands[4][4] = {
        {0, 0, logicalWidth, topBandBottom},
        {0, bottomBandTop, logicalWidth, logicalHeight},
        {0, topBandBottom, playfieldLeft, bottomBandTop},
        {playfieldRight, topBandBottom, logicalWidth, bottomBandTop},
    };

    ApplyPspViewport();
    glEnable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    for (int index = 0; index < 4; ++index)
    {
        const PspPhysicalRect rect = MakePspPhysicalRect(
            bands[index][0], bands[index][1], bands[index][2], bands[index][3],
            logicalWidth, logicalHeight);
        if (rect.width > 0 && rect.height > 0)
        {
            glScissor(rect.left, rect.bottom, rect.width, rect.height);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }
    glDisable(GL_SCISSOR_TEST);
}

struct PspClientVertex
{
    GLfloat u;
    GLfloat v;
    GLubyte r;
    GLubyte g;
    GLubyte b;
    GLubyte a;
    GLfloat x;
    GLfloat y;
    GLfloat z;
};

static_assert(sizeof(PspClientVertex) == 24U,
              "PSPGL native quad submit requires the proven 24-byte layout");
static_assert(offsetof(PspClientVertex, u) == 0U &&
                  offsetof(PspClientVertex, v) == 4U &&
                  offsetof(PspClientVertex, r) == 8U &&
                  offsetof(PspClientVertex, x) == 12U &&
                  offsetof(PspClientVertex, y) == 16U &&
                  offsetof(PspClientVertex, z) == 20U,
              "packed Bullet audit requires exact UV/RGBA/XYZ byte offsets");
#if TH08_PSP_ITEM_DIRECT_GE_ENABLED
static_assert(kPspItemDirectGeArenaBytes ==
                  static_cast<size_t>(kPspItemDirectGeVertexCapacity) *
                      sizeof(PspClientVertex),
              "Item native arena must cover every packed Item vertex");
#endif

#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
// This cache is deliberately limited to value-like fixed-function state.
// Texture binding, uploads, client arrays and draw submission stay on the
// proven path until a later, separately measured change.
struct PspPrepareStateCache
{
    unsigned int epoch = 0;

    bool viewportValid = false;
    GLint viewportLeft = 0;
    GLint viewportBottom = 0;
    GLsizei viewportWidth = 0;
    GLsizei viewportHeight = 0;

    bool scissorEnableValid = false;
    bool scissorEnabled = false;
    bool scissorRectValid = false;
    GLint scissorLeft = 0;
    GLint scissorBottom = 0;
    GLsizei scissorWidth = 0;
    GLsizei scissorHeight = 0;

    bool matrixModeValid = false;
    GLenum matrixMode = GL_MODELVIEW;
    bool projectionValid = false;
    bool projectionTransformed = false;
    UINT projectionWidth = 0;
    UINT projectionHeight = 0;
    unsigned int projectionRawVersion = 0;
    bool modelViewValid = false;
    bool modelViewTransformed = false;
    unsigned int worldRawVersion = 0;
    unsigned int viewRawVersion = 0;

    bool blendEnableValid = false;
    bool blendEnabled = false;
    bool blendFunctionValid = false;
    GLenum blendSource = GL_ONE;
    GLenum blendDestination = GL_ZERO;

    bool alphaEnableValid = false;
    bool alphaEnabled = false;
    bool alphaFunctionValid = false;
    GLenum alphaFunction = GL_ALWAYS;
    DWORD alphaReference = 0;

    bool depthEnableValid = false;
    bool depthEnabled = false;
    bool depthFunctionValid = false;
    GLenum depthFunction = GL_LEQUAL;
    bool depthMaskValid = false;
    bool depthMask = true;

    bool fogEnableValid = false;
    bool fogEnabled = false;
    bool fogModeValid = false;
    DWORD fogColor = 0;
    DWORD fogStart = 0;
    DWORD fogEnd = 0;
    bool fogColorValid = false;
    bool fogStartValid = false;
    bool fogEndValid = false;

    bool textureEnableValid = false;
    bool textureEnabled = false;
    bool textureEnvironmentValid = false;
    GLint textureEnvironment = GL_MODULATE;

    void Invalidate()
    {
        // The epoch lazily invalidates sampler values stored with each GL
        // texture name.  A 32-bit wrap cannot occur during a practical run.
        ++epoch;
        viewportValid = false;
        scissorEnableValid = false;
        scissorRectValid = false;
        matrixModeValid = false;
        projectionValid = false;
        modelViewValid = false;
        blendEnableValid = false;
        blendFunctionValid = false;
        alphaEnableValid = false;
        alphaFunctionValid = false;
        depthEnableValid = false;
        depthFunctionValid = false;
        depthMaskValid = false;
        fogEnableValid = false;
        fogModeValid = false;
        fogColorValid = false;
        fogStartValid = false;
        fogEndValid = false;
        textureEnableValid = false;
        textureEnvironmentValid = false;
    }
};

PspPrepareStateCache *g_pspPrepareStateCache = NULL;

class PspPrepareStateBoundary
{
  public:
    explicit PspPrepareStateBoundary(PspPrepareStateCache *cache_)
        : cache(cache_)
    {
        if (cache != NULL)
            cache->Invalidate();
    }

    ~PspPrepareStateBoundary()
    {
        if (cache != NULL)
            cache->Invalidate();
    }

  private:
    PspPrepareStateCache *cache;
};

void ApplyPspCachedCapability(GLenum capability, bool enabled,
                              bool *valid, bool *applied,
                              UINT *emitted)
{
    if (!*valid || *applied != enabled)
    {
        if (enabled)
            glEnable(capability);
        else
            glDisable(capability);
        *valid = true;
        *applied = enabled;
        ++*emitted;
    }
}

void ApplyPspCachedViewport(PspPrepareStateCache *cache, GLint left,
                            GLint bottom, GLsizei width, GLsizei height,
                            UINT *emitted)
{
    if (!cache->viewportValid || cache->viewportLeft != left ||
        cache->viewportBottom != bottom || cache->viewportWidth != width ||
        cache->viewportHeight != height)
    {
        glViewport(left, bottom, width, height);
        cache->viewportValid = true;
        cache->viewportLeft = left;
        cache->viewportBottom = bottom;
        cache->viewportWidth = width;
        cache->viewportHeight = height;
        ++*emitted;
    }
}

void ApplyPspCachedScissor(PspPrepareStateCache *cache, GLint left,
                           GLint bottom, GLsizei width, GLsizei height,
                           UINT *emitted)
{
    if (!cache->scissorRectValid || cache->scissorLeft != left ||
        cache->scissorBottom != bottom || cache->scissorWidth != width ||
        cache->scissorHeight != height)
    {
        glScissor(left, bottom, width, height);
        cache->scissorRectValid = true;
        cache->scissorLeft = left;
        cache->scissorBottom = bottom;
        cache->scissorWidth = width;
        cache->scissorHeight = height;
        ++*emitted;
    }
}

void ApplyPspCachedMatrixMode(PspPrepareStateCache *cache, GLenum mode,
                              UINT *emitted)
{
    if (!cache->matrixModeValid || cache->matrixMode != mode)
    {
        glMatrixMode(mode);
        cache->matrixModeValid = true;
        cache->matrixMode = mode;
        ++*emitted;
    }
}
#endif
#endif

UINT BytesPerPixel(D3DFORMAT format)
{
    switch (format)
    {
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: return 2;
    default: return 4;
    }
}

void DecodePixel(const BYTE *source, D3DFORMAT format, BYTE *rgba)
{
    WORD pixel;
    switch (format)
    {
    case D3DFMT_R8G8B8:
        rgba[0] = source[2]; rgba[1] = source[1]; rgba[2] = source[0]; rgba[3] = 255; break;
    case D3DFMT_R5G6B5:
        memcpy(&pixel, source, sizeof(pixel));
        rgba[0] = static_cast<BYTE>(((pixel >> 11) & 31) * 255 / 31);
        rgba[1] = static_cast<BYTE>(((pixel >> 5) & 63) * 255 / 63);
        rgba[2] = static_cast<BYTE>((pixel & 31) * 255 / 31); rgba[3] = 255; break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        memcpy(&pixel, source, sizeof(pixel));
        rgba[0] = static_cast<BYTE>(((pixel >> 10) & 31) * 255 / 31);
        rgba[1] = static_cast<BYTE>(((pixel >> 5) & 31) * 255 / 31);
        rgba[2] = static_cast<BYTE>((pixel & 31) * 255 / 31);
        rgba[3] = format == D3DFMT_A1R5G5B5 && !(pixel & 0x8000) ? 0 : 255; break;
    case D3DFMT_A4R4G4B4:
        memcpy(&pixel, source, sizeof(pixel));
        rgba[0] = static_cast<BYTE>(((pixel >> 8) & 15) * 17);
        rgba[1] = static_cast<BYTE>(((pixel >> 4) & 15) * 17);
        rgba[2] = static_cast<BYTE>((pixel & 15) * 17);
        rgba[3] = static_cast<BYTE>(((pixel >> 12) & 15) * 17); break;
    default:
        rgba[0] = source[2]; rgba[1] = source[1]; rgba[2] = source[0];
        rgba[3] = format == D3DFMT_X8R8G8B8 ? 255 : source[3]; break;
    }
}

void EncodePixel(BYTE *destination, D3DFORMAT format, const BYTE *rgba)
{
    WORD pixel;
    switch (format)
    {
    case D3DFMT_R8G8B8:
        destination[0] = rgba[2]; destination[1] = rgba[1]; destination[2] = rgba[0]; break;
    case D3DFMT_R5G6B5:
        pixel = static_cast<WORD>(((rgba[0] * 31 / 255) << 11) |
                                  ((rgba[1] * 63 / 255) << 5) | (rgba[2] * 31 / 255));
        memcpy(destination, &pixel, sizeof(pixel)); break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        pixel = static_cast<WORD>(((format == D3DFMT_X1R5G5B5 || rgba[3] >= 128) ? 0x8000 : 0) |
                                  ((rgba[0] * 31 / 255) << 10) |
                                  ((rgba[1] * 31 / 255) << 5) | (rgba[2] * 31 / 255));
        memcpy(destination, &pixel, sizeof(pixel)); break;
    case D3DFMT_A4R4G4B4:
        pixel = static_cast<WORD>(((rgba[3] >> 4) << 12) | ((rgba[0] >> 4) << 8) |
                                  ((rgba[1] >> 4) << 4) | (rgba[2] >> 4));
        memcpy(destination, &pixel, sizeof(pixel)); break;
    default:
        destination[0] = rgba[2]; destination[1] = rgba[1]; destination[2] = rgba[0];
        destination[3] = format == D3DFMT_X8R8G8B8 ? 255 : rgba[3]; break;
    }
}

void Identity(D3DMATRIX *matrix)
{
    memset(matrix, 0, sizeof(*matrix));
    matrix->_11 = matrix->_22 = matrix->_33 = matrix->_44 = 1.0f;
}

class LinuxSurface : public IDirect3DSurface8
{
  public:
    LinuxSurface(UINT width_, UINT height_, D3DFORMAT format_, bool backbuffer_,
                 LinuxTexture *owner_, bool allocatePixels_ = true)
        : refs(1), width(width_), height(height_), format(format_), backbuffer(backbuffer_), owner(owner_),
          dirty(!backbuffer_), pspBlitTexture(0), pspBlitTextureDirty(true),
          pspDiscardCpuCopy(false), pspBlitProxy(NULL)
    {
        pitch = width * BytesPerPixel(format);
        // Texture storage is allocated on first Lock/access. Static PSP ANM
        // textures can stream straight from the archive into PSPGL and never
        // need a second 512 KiB CPU copy.
#if defined(PSP)
        if (owner == NULL && allocatePixels_)
#else
        // The host oracle keeps the original CPU-backed texture surface.
        // PSP alone may defer that copy because its upload path can stream
        // static ANM pixels directly into PSPGL.
        if (allocatePixels_)
#endif
        {
#if defined(PSP)
            const std::size_t needed = static_cast<std::size_t>(pitch) * height;
            if (!th08::psp::RenderResourceArenaCanAllocate(needed))
            {
                th08::psp::BootLog("SURFACE_PIXELS skip owner=%s bytes=%lu largest=%lu\n",
                                   backbuffer ? "backbuffer shadow init" : "surface pixels",
                                   static_cast<unsigned long>(needed),
                                   static_cast<unsigned long>(th08::psp::RenderResourceArenaLargestFree()));
                th08::psp::FlushBootLog();
            }
            else
            {
                th08::psp::RenderResourceAllocationScope arenaScope(
                    backbuffer ? "backbuffer shadow init" : "surface pixels");
                pixels.resize(pitch * height);
            }
#else
            pixels.resize(pitch * height);
#endif
        }
    }
#if defined(PSP)
    bool CreateNativeFramebufferTexture(const WORD *texturePixels,
                                        const char **failureReason)
    {
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(g_pspPrepareStateCache);
#endif
        if (failureReason != NULL)
            *failureReason = NULL;
        if (texturePixels == NULL)
        {
            if (failureReason != NULL)
                *failureReason = "texture_source_missing";
            return false;
        }

        if (pspBlitTexture != 0)
        {
            if (failureReason != NULL)
                *failureReason = "backing_already_exists";
            return false;
        }

        th08::psp::RenderPerfNoteUploadAttempt();
        th08::psp::RenderResourceAllocationScope arenaScope(
            "native transition texture");
        // This texture is a read-only transition/dialogue snapshot after the
        // initial upload.  Mark only that proven lifetime as eligible for the
        // explicit GE4 upper-aperture promotion path.
#if TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE_ENABLED
        // Except the dialogue snapshot: its promotion right after a mid-frame
        // upload showed a black background on hardware (R-045).
        PspGe4StaticUploadScope staticUpload(!pspSuppressStaticUploadPromotion);
#else
        PspGe4StaticUploadScope staticUpload(true);
#endif
        // Scope contention must fail closed.  PSPGL's memalign wrapper falls
        // back to newlib when no renderer scope is active; sending a 512 KiB
        // dialogue backing there would recreate the fragmented-heap failure
        // that this capture path is intended to avoid.
        if (!th08::psp::RenderResourceAllocationScopeActive())
        {
            if (failureReason != NULL)
                *failureReason = "render_scope_busy";
            return false;
        }
        while (glGetError() != GL_NO_ERROR) {}
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glEnable(GL_TEXTURE_2D);
        glGenTextures(1, &pspBlitTexture);
        if (pspBlitTexture == 0)
        {
            glPopAttrib();
            if (failureReason != NULL)
                *failureReason = "texture_name_failed";
            return false;
        }

        glBindTexture(GL_TEXTURE_2D, pspBlitTexture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        // PSPGL r12 requires a data-backed first definition. An empty texture
        // followed by a sub-image produced a valid-but-black full-screen cache.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                     kPspNativeCaptureTextureWidth,
                     kPspNativeCaptureTextureHeight, 0,
                     GL_RGB, GL_UNSIGNED_SHORT_5_6_5_REV, texturePixels);
        th08::psp::RenderPerfNoteActualUpload(
            kPspNativeCaptureTextureWidth *
            kPspNativeCaptureTextureHeight *
            static_cast<UINT>(sizeof(WORD)));
        const GLenum error = glGetError();
        if (error == GL_NO_ERROR)
            staticUpload.Finalize(pspBlitTexture);
        // Force PSPGL to snapshot the live GE texture registers before the
        // saved attribute binding is restored.
        glBindTexture(GL_TEXTURE_2D, 0);
        glPopAttrib();
        if (error != GL_NO_ERROR)
        {
            fprintf(stderr,
                    "TH08PSP NATIVE_CAPTURE create_error=0x%04x\n",
                    static_cast<unsigned int>(error));
            glDeleteTextures(1, &pspBlitTexture);
            pspBlitTexture = 0;
            if (failureReason != NULL)
                *failureReason = "texture_create_failed";
            return false;
        }

        pspBlitTextureDirty = false;
        pspDiscardCpuCopy = true;
        dirty = false;
        return true;
    }

    bool CaptureNativeFramebufferFromWorkspace(
        bool readDisplayedFrame, void *workspace, size_t workspaceBytes,
        const char **failureReason = NULL)
    {
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(g_pspPrepareStateCache);
#endif
        if (failureReason != NULL)
            *failureReason = NULL;
        if (backbuffer || width == 0 || height == 0)
        {
            if (failureReason != NULL)
                *failureReason = "invalid_surface";
            return false;
        }
        if (workspace == NULL || workspaceBytes < kPspNativeCaptureWorkspaceBytes)
        {
            if (failureReason != NULL)
                *failureReason = "workspace_capacity";
            return false;
        }

        unsigned char *scratch = static_cast<unsigned char *>(workspace);
        WORD *nativePixels = reinterpret_cast<WORD *>(scratch);
        WORD *texturePixels = reinterpret_cast<WORD *>(
            scratch + kPspNativeCaptureNativeBytes);
        memset(texturePixels, 0, kPspNativeCaptureTextureBytes);

        while (glGetError() != GL_NO_ERROR) {}
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        // A transition requested late in the draw chain captures the current
        // BACK buffer.  A dialogue snapshot is requested at the next
        // BeginScene and must capture the last displayed FRONT buffer instead
        // of the alternating, stale BACK buffer.  Always restore BACK so
        // ordinary render-target reads retain PSPGL's window default.
        glReadBuffer(readDisplayedFrame ? GL_FRONT : GL_BACK);
        glReadPixels(kPspFitLeft, 0, kPspFitWidth, kPspScreenHeight,
                     GL_RGB, GL_UNSIGNED_SHORT_5_6_5_REV, nativePixels);
        glReadBuffer(GL_BACK);
        GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            fprintf(stderr, "TH08PSP NATIVE_CAPTURE read_error=0x%04x\n",
                    static_cast<unsigned int>(error));
            if (failureReason != NULL)
                *failureReason = "framebuffer_read_failed";
            return false;
        }
#if TH08_PSP_DIALOGUE_SNAPSHOT_DIAG_ENABLED
        {
            const size_t nativeCount =
                static_cast<size_t>(kPspFitWidth) * kPspScreenHeight;
            size_t nonzero = 0;
            unsigned long checksum = 0;
            for (size_t i = 0; i < nativeCount; ++i)
            {
                const WORD value = nativePixels[i];
                if (value != 0)
                    ++nonzero;
                checksum = checksum * 31u + value;
            }
            void *displayTop = NULL;
            int displayWidth = 0;
            int displayFormat = 0;
            sceDisplayGetFrameBuf(&displayTop, &displayWidth, &displayFormat,
                                  PSP_DISPLAY_SETBUF_IMMEDIATE);
            const size_t center = static_cast<size_t>(kPspScreenHeight / 2) * kPspFitWidth +
                                  kPspFitWidth / 2;
            th08::psp::BootLog(
                "NATIVE_CAPTURE_PIXELS read_front=%d nonzero=%lu/%lu checksum=%08lx "
                "center=%04x row10=%04x,%04x row262=%04x,%04x dst=%p "
                "display=%p width=%d fmt=%d\n",
                readDisplayedFrame ? 1 : 0,
                static_cast<unsigned long>(nonzero),
                static_cast<unsigned long>(nativeCount), checksum,
                static_cast<unsigned int>(nativePixels[center]),
                static_cast<unsigned int>(nativePixels[10 * kPspFitWidth + 10]),
                static_cast<unsigned int>(nativePixels[10 * kPspFitWidth + kPspFitWidth - 10]),
                static_cast<unsigned int>(nativePixels[262 * kPspFitWidth + 10]),
                static_cast<unsigned int>(nativePixels[262 * kPspFitWidth + kPspFitWidth - 10]),
                static_cast<void *>(nativePixels), displayTop, displayWidth,
                displayFormat);
        }
#endif

        // glReadPixels starts at the physical framebuffer's bottom row. Keep
        // the D3D-facing top-left convention while preserving every native
        // 480x272 RGB565 pixel exactly; the unused texture area stays clear.
        for (UINT y = 0; y < static_cast<UINT>(kPspScreenHeight); ++y)
        {
            memcpy(texturePixels +
                       static_cast<size_t>(y) *
                           kPspNativeCaptureTextureWidth,
                   nativePixels + static_cast<size_t>(kPspScreenHeight - 1 - y) *
                                      kPspFitWidth,
                   static_cast<size_t>(kPspFitWidth) * sizeof(WORD));
        }

        if (!CreateNativeFramebufferTexture(texturePixels, failureReason))
            return false;
#if TH08_PSP_DIALOGUE_SNAPSHOT_DIAG_ENABLED
        th08::psp::BootLog("NATIVE_CAPTURE_TEXTURE glname=%lu source=%p\n",
                           static_cast<unsigned long>(pspBlitTexture),
                           static_cast<const void *>(texturePixels));
#endif
        fprintf(stderr,
                "TH08PSP NATIVE_CAPTURE ready logical=%lux%lu physical=%dx%d "
                "borrowed=%lu texture=%lu backing=%s\n",
                static_cast<unsigned long>(width),
                static_cast<unsigned long>(height), kPspFitWidth,
                kPspScreenHeight,
                static_cast<unsigned long>(kPspNativeCaptureWorkspaceBytes),
                static_cast<unsigned long>(kPspNativeCaptureTextureBytes),
                "created");
        return true;
    }

    bool CaptureNativeFramebuffer(bool readDisplayedFrame,
                                  const char **failureReason = NULL)
    {
        if (failureReason != NULL)
            *failureReason = NULL;
        if (!th08::psp::AnmScratchTransitionActive())
        {
            if (failureReason != NULL)
                *failureReason = "scratch_not_reserved";
            return false;
        }

        void *scratch = th08::psp::AnmScratchTransitionBase();
        if (scratch == NULL ||
            !th08::psp::AnmScratchTransitionSetActiveBytes(
                kPspNativeCaptureWorkspaceBytes))
        {
            if (failureReason != NULL)
                *failureReason = "scratch_capacity";
            return false;
        }
        return CaptureNativeFramebufferFromWorkspace(
            readDisplayedFrame, scratch, kPspNativeCaptureWorkspaceBytes,
            failureReason);
    }
    void DestroyNativeFramebufferBacking();
#endif
    ~LinuxSurface();
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG value = --refs; if (value == 0) delete this; return value; }
    HRESULT GetDesc(D3DSURFACE_DESC *description)
    {
        if (description == NULL) return E_INVALIDARG;
        memset(description, 0, sizeof(*description));
        description->Format = format; description->Type = D3DRTYPE_SURFACE;
        description->Pool = backbuffer ? D3DPOOL_DEFAULT : D3DPOOL_SYSTEMMEM;
        description->Size = pitch * height;
        description->Width = width; description->Height = height; return S_OK;
    }
    HRESULT LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
    {
        if (locked == NULL) return E_INVALIDARG;
#if defined(PSP)
        if (backbuffer) gPspShadowReason = (flags & D3DLOCK_READONLY) ? "LockRect(readonly)" : "LockRect";
#endif
        if (!EnsurePixels()) return E_OUTOFMEMORY;
        if (backbuffer && (flags & D3DLOCK_READONLY)) ReadBackbuffer();
        UINT left = rect != NULL && rect->left > 0 ? static_cast<UINT>(rect->left) : 0;
        UINT top = rect != NULL && rect->top > 0 ? static_cast<UINT>(rect->top) : 0;
        if (left >= width || top >= height) return E_INVALIDARG;
        locked->Pitch = pitch;
        locked->pBits = &pixels[top * pitch + left * BytesPerPixel(format)]; return S_OK;
    }
    HRESULT UnlockRect()
    {
        dirty = true;
        pspBlitTextureDirty = true;
#if defined(PSP)
        DropScratchShadow();
#endif
        return S_OK;
    }
#if defined(PSP)
    // Return a scratch-backed backbuffer shadow as soon as its capture is done.
    void DropScratchShadow()
    {
        if (backbuffer && !pixels.empty() && th08::psp::BackbufferShadowBase() == &pixels[0])
            std::vector<BYTE>().swap(pixels);
    }
#endif
    bool EnsurePixels()
    {
        if (pixels.empty() && pitch != 0 && height != 0)
        {
#if defined(PSP)
            if (backbuffer)
            {
                const std::size_t needed = static_cast<std::size_t>(pitch) * height;
                // Never let a capture abort the game: skip it when neither the
                // idle scratch nor the renderer arena can hold the shadow.
                if (!th08::psp::BackbufferShadowAvailable() &&
                    th08::psp::RenderResourceArenaLargestFree() < needed)
                {
                    th08::psp::BootLog("BACKBUFFER_SHADOW skip bytes=%lu reason=%s op=%s arena_largest=%lu\n",
                                       static_cast<unsigned long>(needed), gPspShadowReason, gPspSurfaceOp,
                                       static_cast<unsigned long>(th08::psp::RenderResourceArenaLargestFree()));
                    th08::psp::FlushBootLog();
                    return false;
                }
            }
            if (!backbuffer && !th08::psp::RenderResourceArenaCanAllocate(static_cast<std::size_t>(pitch) * height))
            {
                th08::psp::BootLog("SURFACE_PIXELS skip owner=surface pixels lazy bytes=%lu largest=%lu\n",
                                   static_cast<unsigned long>(static_cast<std::size_t>(pitch) * height),
                                   static_cast<unsigned long>(th08::psp::RenderResourceArenaLargestFree()));
                th08::psp::FlushBootLog();
                return false;
            }
            th08::psp::RenderResourceAllocationScope arenaScope(
                backbuffer ? "backbuffer shadow" : "surface pixels lazy");
#endif
            pixels.resize(pitch * height);
        }
        return !pixels.empty() || pitch == 0 || height == 0;
    }
    HRESULT GetDC(HDC *dc)
    { if (dc == NULL) return E_INVALIDARG; *dc = CreateCompatibleDC(NULL); return *dc ? S_OK : E_FAIL; }
    HRESULT ReleaseDC(HDC dc) { return DeleteDC(dc) ? S_OK : E_FAIL; }
    void ReadBackbuffer()
    {
        if (!backbuffer || width == 0 || height == 0) return;
        if (!EnsurePixels()) return;
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(g_pspPrepareStateCache);
#endif
#if defined(PSP)
        th08::psp::RenderResourceAllocationScope arenaScope("backbuffer native readback");
        fprintf(stderr, "TH08PSP READBACK phase=begin size=%lux%lu\n",
                static_cast<unsigned long>(width), static_cast<unsigned long>(height));
        const UINT bytes = BytesPerPixel(format);
        const size_t nativePixelCount = static_cast<size_t>(kPspFitWidth) *
                                        kPspScreenHeight;
        WORD *nativePixels = static_cast<WORD *>(th08::psp::BackbufferShadowNativeBuffer());
        const bool ownsNative = nativePixels == NULL;
        if (ownsNative)
            nativePixels = static_cast<WORD *>(memalign(16, nativePixelCount * sizeof(WORD)));
        if (nativePixels == NULL)
        {
            fprintf(stderr, "TH08PSP READBACK native_alloc_failed bytes=%lu\n",
                    static_cast<unsigned long>(nativePixelCount * sizeof(WORD)));
            return;
        }

        // PSPGL only accepts the framebuffer's native pixel type here.  The
        // display surface is 480x272 RGB565 in GE order (BBBBBGGGGGGRRRRR),
        // while the D3D-facing backbuffer remains the game's logical 640x480.
        // Read the physical buffer once, then expand it back to logical space.
        while (glGetError() != GL_NO_ERROR) {}
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(kPspFitLeft, 0, kPspFitWidth, kPspScreenHeight,
                     GL_RGB, GL_UNSIGNED_SHORT_5_6_5_REV, nativePixels);
        const GLenum readError = glGetError();
        if (readError != GL_NO_ERROR)
        {
            fprintf(stderr, "TH08PSP READBACK read_error=0x%04x\n",
                    static_cast<unsigned int>(readError));
            if (ownsNative) free(nativePixels);
            return;
        }

        for (UINT y = 0; y < height; ++y)
        {
            UINT sourceY = static_cast<UINT>(
                (static_cast<unsigned long long>(2 * y + 1) * kPspScreenHeight) /
                (2 * height));
            if (sourceY >= static_cast<UINT>(kPspScreenHeight))
                sourceY = kPspScreenHeight - 1;
            const size_t sourceRow = static_cast<size_t>(kPspScreenHeight - 1 - sourceY) *
                                     kPspFitWidth;
            BYTE *destinationRow = &pixels[y * pitch];
            for (UINT x = 0; x < width; ++x)
            {
                UINT sourceX = static_cast<UINT>(
                    (static_cast<unsigned long long>(2 * x + 1) * kPspFitWidth) /
                    (2 * width));
                if (sourceX >= static_cast<UINT>(kPspFitWidth))
                    sourceX = kPspFitWidth - 1;
                const WORD pixel = nativePixels[sourceRow + sourceX];
                BYTE rgba[4];
                rgba[0] = static_cast<BYTE>((pixel & 31) * 255 / 31);
                rgba[1] = static_cast<BYTE>(((pixel >> 5) & 63) * 255 / 63);
                rgba[2] = static_cast<BYTE>(((pixel >> 11) & 31) * 255 / 31);
                rgba[3] = 255;
                EncodePixel(destinationRow + x * bytes, format, rgba);
            }
        }
        if (ownsNative) free(nativePixels);
#else
        std::vector<BYTE> rgba(width * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, &rgba[0]);
        const UINT bytes = BytesPerPixel(format);
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
                EncodePixel(&pixels[y * pitch + x * bytes], format,
                            &rgba[((height - 1 - y) * width + x) * 4]);
#endif
        dirty = false;
#if defined(PSP)
        fprintf(stderr, "TH08PSP READBACK phase=done\n");
#endif
    }
    void FlushBackbuffer()
    {
#if defined(PSP)
        th08::psp::RenderPerfNoteUploadAttempt();
#endif
        if (!backbuffer || !dirty || width == 0 || height == 0) return;
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(g_pspPrepareStateCache);
#endif
#if defined(PSP)
        th08::psp::RenderResourceAllocationScope arenaScope("backbuffer flush");
        constexpr UINT outputWidth = kPspFitWidth;
        constexpr UINT outputHeight = kPspScreenHeight;
        constexpr UINT textureWidth = 512;
        constexpr UINT textureHeight = 512;
        fprintf(stderr, "TH08PSP BACKBUFFER phase=begin size=%lux%lu\n",
                static_cast<unsigned long>(width), static_cast<unsigned long>(height));
        std::vector<BYTE> rgba(textureWidth * textureHeight * 4, 0);
        fprintf(stderr, "TH08PSP BACKBUFFER phase=allocated bytes=%lu\n",
                static_cast<unsigned long>(rgba.size()));
        const UINT bytes = BytesPerPixel(format);
        for (UINT y = 0; y < outputHeight; ++y)
        {
            float sourceY = (static_cast<float>(y) + 0.5f) * height / outputHeight - 0.5f;
            int sourceY0 = static_cast<int>(floorf(sourceY));
            float yFraction = sourceY - sourceY0;
            if (sourceY0 < 0) { sourceY0 = 0; yFraction = 0.0f; }
            UINT sourceY1 = static_cast<UINT>(sourceY0 + 1);
            if (sourceY1 >= height) { sourceY1 = height - 1; yFraction = 0.0f; }
            for (UINT x = 0; x < outputWidth; ++x)
            {
                float sourceX = (static_cast<float>(x) + 0.5f) * width / outputWidth - 0.5f;
                int sourceX0 = static_cast<int>(floorf(sourceX));
                float xFraction = sourceX - sourceX0;
                if (sourceX0 < 0) { sourceX0 = 0; xFraction = 0.0f; }
                UINT sourceX1 = static_cast<UINT>(sourceX0 + 1);
                if (sourceX1 >= width) { sourceX1 = width - 1; xFraction = 0.0f; }

                BYTE samples[4][4];
                DecodePixel(&pixels[static_cast<UINT>(sourceY0) * pitch +
                                    static_cast<UINT>(sourceX0) * bytes], format, samples[0]);
                DecodePixel(&pixels[static_cast<UINT>(sourceY0) * pitch + sourceX1 * bytes],
                            format, samples[1]);
                DecodePixel(&pixels[sourceY1 * pitch + static_cast<UINT>(sourceX0) * bytes],
                            format, samples[2]);
                DecodePixel(&pixels[sourceY1 * pitch + sourceX1 * bytes], format, samples[3]);

                BYTE *destination = &rgba[(y * textureWidth + x) * 4];
                for (UINT component = 0; component < 4; ++component)
                {
                    const float top = samples[0][component] +
                        (samples[1][component] - samples[0][component]) * xFraction;
                    const float bottom = samples[2][component] +
                        (samples[3][component] - samples[2][component]) * xFraction;
                    destination[component] = static_cast<BYTE>(
                        top + (bottom - top) * yFraction + 0.5f);
                }
            }
        }
#else
        std::vector<BYTE> rgba(width * height * 4);
        const UINT bytes = BytesPerPixel(format);
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
                DecodePixel(&pixels[y * pitch + x * bytes], format, &rgba[(y * width + x) * 4]);
#endif

        GLuint name = 0;
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glDisable(GL_ALPHA_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_TEXTURE_2D);
        glGenTextures(1, &name); glBindTexture(GL_TEXTURE_2D, name);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
#if defined(PSP)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
#else
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
#endif
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
#if defined(PSP)
                     textureWidth, textureHeight,
#else
                     width, height,
#endif
                     0, GL_RGBA, GL_UNSIGNED_BYTE, &rgba[0]);
#if defined(PSP)
        th08::psp::RenderPerfNoteActualUpload(textureWidth * textureHeight * 4U);
        fprintf(stderr, "TH08PSP BACKBUFFER phase=uploaded\n");
#endif
#if defined(PSP)
        ApplyPspViewport();
#endif
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0.0, width, height, 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glColor4ub(255, 255, 255, 255);
        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, 0.0f);
#if defined(PSP)
        glTexCoord2f(static_cast<float>(outputWidth) / textureWidth, 0.0f);
        glVertex2f(static_cast<float>(width), 0.0f);
        glTexCoord2f(0.0f, static_cast<float>(outputHeight) / textureHeight);
        glVertex2f(0.0f, static_cast<float>(height));
        glTexCoord2f(static_cast<float>(outputWidth) / textureWidth,
                     static_cast<float>(outputHeight) / textureHeight);
        glVertex2f(static_cast<float>(width), static_cast<float>(height));
#else
        glTexCoord2f(1.0f, 0.0f); glVertex2f(static_cast<float>(width), 0.0f);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f, static_cast<float>(height));
        glTexCoord2f(1.0f, 1.0f); glVertex2f(static_cast<float>(width), static_cast<float>(height));
#endif
        glEnd();
        glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW);
        glDeleteTextures(1, &name);
        glPopAttrib();
        dirty = false;
    }
#if defined(PSP)
    bool BlitToBackbuffer(LinuxSurface *destination, const RECT &sourceRect,
                          const POINT &destinationPoint, bool submitDraw = true)
    {
        th08::psp::RenderPerfNoteUploadAttempt();
        // The cached full-screen texture and its one-frame conversion buffer
        // have the same renderer lifetime.  Keep both out of the fragmented
        // game heap; the staging allocation is returned immediately after GE
        // upload while PSPGL retains only the texture backing it owns.
        th08::psp::RenderResourceAllocationScope arenaScope("surface cache blit");
        const bool cacheReady = pspBlitTexture != 0 && !pspBlitTextureDirty;
        if (destination == NULL || !destination->backbuffer || backbuffer ||
            (pixels.empty() && !cacheReady) ||
            sourceRect.left < 0 || sourceRect.top < 0 ||
            sourceRect.right <= sourceRect.left || sourceRect.bottom <= sourceRect.top ||
            sourceRect.right > static_cast<LONG>(width) ||
            sourceRect.bottom > static_cast<LONG>(height))
        {
            return false;
        }
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(g_pspPrepareStateCache);
#endif

        constexpr UINT textureWidth = 512;
        constexpr UINT textureHeight = 512;
        const UINT contentWidth = static_cast<UINT>(
            (static_cast<unsigned long long>(width) * kPspFitWidth + 639u) / 640u);
        const UINT contentHeight = static_cast<UINT>(
            (static_cast<unsigned long long>(height) * kPspScreenHeight + 479u) / 480u);
        if (contentWidth == 0 || contentHeight == 0 || contentWidth > textureWidth ||
            contentHeight > textureHeight)
        {
            return false;
        }

        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glEnable(GL_TEXTURE_2D);
        if (pspBlitTexture == 0)
            glGenTextures(1, &pspBlitTexture);
        if (pspBlitTexture == 0)
        {
            glPopAttrib();
            return false;
        }
        glBindTexture(GL_TEXTURE_2D, pspBlitTexture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

        if (pspBlitTextureDirty)
        {
            // A discarded source copy proves this cache immutable after its
            // first complete definition.  Mutable software surfaces stay out
            // of upper GE memory because later CPU writes must remain legal.
            PspGe4StaticUploadScope staticUpload(pspDiscardCpuCopy);
            // Keep a persistent RGB565 full-screen surface texture instead of
            // reading the framebuffer back every frame.  PSPGL reliably
            // uploads ANM textures through one data-backed glTexImage2D call;
            // allocating an empty texture and filling it with sub-images left
            // this surface black even though no GL error was reported.
            const size_t imageBytes = static_cast<size_t>(textureWidth) *
                                      textureHeight * sizeof(WORD);
            WORD *rgb565Image = static_cast<WORD *>(
                th08::psp::RenderResourceArenaAllocate(
                    imageBytes, 64, "surface cache staging"));
            if (rgb565Image == NULL)
            {
                fprintf(stderr, "TH08PSP SURFACE_CACHE staging_alloc_failed bytes=%lu\n",
                        static_cast<unsigned long>(imageBytes));
                glPopAttrib();
                return false;
            }
            memset(rgb565Image, 0, imageBytes);
            const UINT bytes = BytesPerPixel(format);
            while (glGetError() != GL_NO_ERROR) {}
            for (UINT y = 0; y < contentHeight; ++y)
            {
                float sourceY = (static_cast<float>(y) + 0.5f) * height /
                                    contentHeight -
                                0.5f;
                int sourceY0 = static_cast<int>(floorf(sourceY));
                float yFraction = sourceY - sourceY0;
                if (sourceY0 < 0) { sourceY0 = 0; yFraction = 0.0f; }
                UINT sourceY1 = static_cast<UINT>(sourceY0 + 1);
                if (sourceY1 >= height) { sourceY1 = height - 1; yFraction = 0.0f; }
                for (UINT x = 0; x < contentWidth; ++x)
                {
                    float sourceX = (static_cast<float>(x) + 0.5f) * width /
                                        contentWidth -
                                    0.5f;
                    int sourceX0 = static_cast<int>(floorf(sourceX));
                    float xFraction = sourceX - sourceX0;
                    if (sourceX0 < 0) { sourceX0 = 0; xFraction = 0.0f; }
                    UINT sourceX1 = static_cast<UINT>(sourceX0 + 1);
                    if (sourceX1 >= width) { sourceX1 = width - 1; xFraction = 0.0f; }

                    BYTE samples[4][4];
                    DecodePixel(&pixels[static_cast<UINT>(sourceY0) * pitch +
                                        static_cast<UINT>(sourceX0) * bytes], format,
                                samples[0]);
                    DecodePixel(&pixels[static_cast<UINT>(sourceY0) * pitch +
                                        sourceX1 * bytes],
                                format, samples[1]);
                    DecodePixel(&pixels[sourceY1 * pitch +
                                        static_cast<UINT>(sourceX0) * bytes],
                                format, samples[2]);
                    DecodePixel(&pixels[sourceY1 * pitch + sourceX1 * bytes], format,
                                samples[3]);

                    BYTE interpolated[3];
                    for (UINT component = 0; component < 3; ++component)
                    {
                        const float top = samples[0][component] +
                            (samples[1][component] - samples[0][component]) * xFraction;
                        const float bottom = samples[2][component] +
                            (samples[3][component] - samples[2][component]) * xFraction;
                        interpolated[component] = static_cast<BYTE>(
                            top + (bottom - top) * yFraction + 0.5f);
                    }
                    // GL_UNSIGNED_SHORT_5_6_5_REV is PSPGL's native GE 5650
                    // layout (BBBBBGGGGGGRRRRR).  Supplying it natively avoids
                    // PSPGL's extra full-image conversion and the associated
                    // peak allocation at the title/stage transition.
                    rgb565Image[y * textureWidth + x] = static_cast<WORD>(
                        ((interpolated[2] >> 3) << 11) |
                        ((interpolated[1] >> 2) << 5) |
                        (interpolated[0] >> 3));
                }
            }
            glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, textureWidth, textureHeight, 0,
                         GL_RGB, GL_UNSIGNED_SHORT_5_6_5_REV, rgb565Image);
            th08::psp::RenderPerfNoteActualUpload(
                textureWidth * textureHeight * static_cast<UINT>(sizeof(WORD)));
            free(rgb565Image);
            const GLenum uploadError = glGetError();
            if (uploadError != GL_NO_ERROR)
            {
                fprintf(stderr, "TH08PSP SURFACE_CACHE upload_error=0x%04x\n",
                        static_cast<unsigned int>(uploadError));
                glPopAttrib();
                return false;
            }
            if (pspDiscardCpuCopy)
            {
                // glTexImage2D is the commit point. Keep the sole decoded
                // source intact until PSPGL confirms the persistent cache, so
                // a transient upload failure can retry instead of replacing
                // the title image with a newly allocated black CPU surface.
                std::vector<BYTE>().swap(pixels);
                fprintf(stderr, "TH08PSP SURFACE_CACHE cpu_copy_released source=%lux%lu\n",
                        static_cast<unsigned long>(width), static_cast<unsigned long>(height));
            }
            staticUpload.Finalize(pspBlitTexture);
            pspBlitTextureDirty = false;
            fprintf(stderr, "TH08PSP SURFACE_CACHE uploaded source=%lux%lu content=%lux%lu\n",
                    static_cast<unsigned long>(width), static_cast<unsigned long>(height),
                    static_cast<unsigned long>(contentWidth),
                    static_cast<unsigned long>(contentHeight));
        }

        // CopyRects submits cached surfaces through LinuxDevice::Draw, the
        // same path used by ANM sprites.  In that case this function only
        // prepares the cache; submitting its raw PSPGL quad as well would draw
        // every loading/dialogue snapshot twice and perturb following state.
        if (!submitDraw)
        {
            glBindTexture(GL_TEXTURE_2D, 0);
            glPopAttrib();
            return true;
        }

        ApplyPspViewport();
        glDisable(GL_ALPHA_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
        glDisable(GL_FOG); glDisable(GL_LIGHTING);
        glDepthMask(GL_FALSE);
        // PSPGL's immediate-mode path is not reliable for this full-screen
        // upload.  Use the same client-array path as the working ANM sprite
        // renderer, and modulate by opaque white so the texture is unchanged.
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(0.0, destination->width, destination->height, 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glColor4ub(255, 255, 255, 255);

        const float uScale = static_cast<float>(contentWidth) /
                             (static_cast<float>(width) * textureWidth);
        const float vScale = static_cast<float>(contentHeight) /
                             (static_cast<float>(height) * textureHeight);
        const float u0 = sourceRect.left * uScale;
        const float v0 = sourceRect.top * vScale;
        const float u1 = sourceRect.right * uScale;
        const float v1 = sourceRect.bottom * vScale;
        const float x0 = static_cast<float>(destinationPoint.x);
        const float y0 = static_cast<float>(destinationPoint.y);
        const float x1 = x0 + static_cast<float>(sourceRect.right - sourceRect.left);
        const float y1 = y0 + static_cast<float>(sourceRect.bottom - sourceRect.top);

        const PspClientVertex quad[4] = {
            {u0, v0, 255, 255, 255, 255, x0, y0, 0.0f},
            {u1, v0, 255, 255, 255, 255, x1, y0, 0.0f},
            {u0, v1, 255, 255, 255, 255, x0, y1, 0.0f},
            {u1, v1, 255, 255, 255, 255, x1, y1, 0.0f},
        };
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(PspClientVertex), &quad[0].x);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(PspClientVertex), &quad[0].r);
        glTexCoordPointer(2, GL_FLOAT, sizeof(PspClientVertex), &quad[0].u);
#if TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED
        th08::psp::DrawPrioritySubprofileNoteDraw(4U);
#endif
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        const GLenum drawError = glGetError();
        if (drawError != GL_NO_ERROR)
            fprintf(stderr, "TH08PSP SURFACE_CACHE draw_error=0x%04x\n",
                    static_cast<unsigned int>(drawError));
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        // PSPGL r12 only snapshots a texture object's live GE address/size/
        // stride registers when glBindTexture switches away from it.
        // glPopAttrib(GL_TEXTURE_BIT) alone drops the current binding without
        // performing that snapshot, so the following normal D3D draw would
        // rebind this object with zeroed registers and paint black.  Force the
        // regular B -> 0 bind edge before restoring the saved attributes.
        glBindTexture(GL_TEXTURE_2D, 0);
        glPopAttrib();
        destination->dirty = false;
        return true;
    }
#endif
    ULONG refs;
    UINT width, height, pitch;
    D3DFORMAT format;
    bool backbuffer;
    LinuxTexture *owner;
    bool dirty;
    GLuint pspBlitTexture;
    bool pspBlitTextureDirty;
    bool pspDiscardCpuCopy;
    LinuxTexture *pspBlitProxy;
    std::vector<BYTE> pixels;
};

class LinuxTexture : public IDirect3DTexture8
{
  public:
    LinuxTexture(UINT width, UINT height, D3DFORMAT format)
        : refs(1), priority(0), glName(0), uploaded(false), discardCpuCopy(false), pspGlFormat(0), pspGlType(0)
    { surface = new LinuxSurface(width, height, format, false, this); }
    ~LinuxTexture()
    {
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(g_pspPrepareStateCache);
#endif
        surface->owner = NULL; surface->Release();
        if (glName != 0) glDeleteTextures(1, &glName);
    }
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG value = --refs; if (value == 0) delete this; return value; }
    DWORD SetPriority(DWORD value) { DWORD old = priority; priority = value; return old; }
    void PreLoad() { Upload(); }
    HRESULT GetLevelDesc(UINT level, D3DSURFACE_DESC *description)
    { return level == 0 ? surface->GetDesc(description) : E_INVALIDARG; }
    HRESULT GetSurfaceLevel(UINT level, IDirect3DSurface8 **result)
    {
        if (level != 0 || result == NULL) return E_INVALIDARG;
        surface->AddRef(); *result = surface; return S_OK;
    }
    HRESULT LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
    { return level == 0 ? surface->LockRect(locked, rect, flags) : E_INVALIDARG; }
    HRESULT UnlockRect(UINT level)
    { if (level != 0) return E_INVALIDARG; uploaded = false; return surface->UnlockRect(); }
    void Upload()
    {
#if defined(PSP)
        th08::psp::RenderPerfNoteUploadAttempt();
#endif
        if (uploaded && !surface->dirty) return;
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(g_pspPrepareStateCache);
        // A storage redefinition normally preserves sampler state, but PSPGL's
        // object commit happens on bind-away.  Reapply all four parameters on
        // the next draw instead of relying on that implementation detail.
        pspSamplerValidMask = 0;
#endif
#if defined(PSP)
        // PSPGL allocates immutable texture backing inside glTexImage2D.  Its
        // eDRAM spill must come from the pre-reserved renderer arena, not from
        // the already fragmented gameplay heap.
        th08::psp::RenderResourceAllocationScope arenaScope("texture upload");
        PspGe4StaticUploadScope staticUpload(
            discardCpuCopy && !surface->pixels.empty()
#if TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE_ENABLED
            && !pspSuppressStaticUploadPromotion
#endif
        );
        if (surface->pixels.empty())
        {
            fprintf(stderr,
                    "TH08PSP TEXTURE_UPLOAD empty size=%lux%lu fmt=%lu uploaded=%d dirty=%d static=%d\n",
                    static_cast<unsigned long>(surface->width),
                    static_cast<unsigned long>(surface->height),
                    static_cast<unsigned long>(surface->format), uploaded ? 1 : 0,
                    surface->dirty ? 1 : 0, discardCpuCopy ? 1 : 0);
        }
#endif
        if (glName == 0) glGenTextures(1, &glName);
        glBindTexture(GL_TEXTURE_2D, glName);
#if defined(PSP)
        if (surface->pixels.empty())
        {
            // '@' ANM entries are render/capture targets with no initial
            // payload. Allocate only their GPU backing; a later LockRect will
            // materialize the CPU copy if the game actually writes one.
            GLenum emptyFormat = GL_RGBA;
            GLenum emptyType = GL_UNSIGNED_BYTE;
            switch (surface->format)
            {
            case D3DFMT_A4R4G4B4:
                emptyType = GL_UNSIGNED_SHORT_4_4_4_4;
                break;
            case D3DFMT_A1R5G5B5:
            case D3DFMT_X1R5G5B5:
                emptyType = GL_UNSIGNED_SHORT_5_5_5_1;
                break;
            case D3DFMT_R5G6B5:
                emptyFormat = GL_RGB;
                emptyType = GL_UNSIGNED_SHORT_5_6_5;
                break;
            case D3DFMT_R8G8B8:
                emptyFormat = GL_RGB;
                break;
            default:
                break;
            }
            glPixelStorei(GL_UNPACK_ALIGNMENT, emptyType == GL_UNSIGNED_BYTE ? 1 : 2);
            while (glGetError() != GL_NO_ERROR) {}
            glTexImage2D(GL_TEXTURE_2D, 0, emptyFormat, surface->width, surface->height,
                         0, emptyFormat, emptyType, NULL);
            th08::psp::RenderPerfNoteActualUpload(0);
            const GLenum emptyError = glGetError();
            // PSPGL r12 commits the live GE texture address/size/stride to the
            // texture object only on a real bind-away edge.  PrepareState would
            // otherwise bind this same name again (a no-op) and could draw with
            // stale registers.
            glBindTexture(GL_TEXTURE_2D, 0);
            if (emptyError != GL_NO_ERROR)
            {
                fprintf(stderr,
                        "TH08PSP TEXTURE_UPLOAD empty_error size=%lux%lu fmt=%lu error=0x%04x\n",
                        static_cast<unsigned long>(surface->width),
                        static_cast<unsigned long>(surface->height),
                        static_cast<unsigned long>(surface->format),
                        static_cast<unsigned int>(emptyError));
                return;
            }
            uploaded = true;
            surface->dirty = false;
            pspGlFormat = emptyFormat;
            pspGlType = emptyType;
            return;
        }

        // PSPGL accepts every packed 16-bit D3D texture without first
        // expanding it to RGBA. Dynamic text/effect textures must retain their
        // CPU copy, so rotate the packed alpha field in place, upload, then
        // perform the exact inverse rotation. Static ANM textures simply
        // discard the transformed source after a successful upload. This
        // removes a width*height*4 contiguous temporary while preserving every
        // channel and all later LockRect/UnlockRect behavior.
        if (!surface->pixels.empty() && BytesPerPixel(surface->format) == 2)
        {
            GLenum uploadFormat = GL_RGBA;
            GLenum uploadType = GL_UNSIGNED_SHORT_4_4_4_4;
            bool native16 = true;
            bool rotate4 = false;
            bool rotate1 = false;
            bool forceOpaqueBit = false;
            WORD *pixels = reinterpret_cast<WORD *>(&surface->pixels[0]);
            const UINT pixelCount = surface->width * surface->height;

            switch (surface->format)
            {
            case D3DFMT_A4R4G4B4:
                rotate4 = true;
                for (UINT i = 0; i < pixelCount; ++i)
                    pixels[i] = static_cast<WORD>((pixels[i] << 4) | (pixels[i] >> 12));
                break;
            case D3DFMT_A1R5G5B5:
                uploadType = GL_UNSIGNED_SHORT_5_5_5_1;
                rotate1 = true;
                for (UINT i = 0; i < pixelCount; ++i)
                    pixels[i] = static_cast<WORD>((pixels[i] << 1) | (pixels[i] >> 15));
                break;
            case D3DFMT_X1R5G5B5:
                uploadType = GL_UNSIGNED_SHORT_5_5_5_1;
                rotate1 = true;
                forceOpaqueBit = true;
                for (UINT i = 0; i < pixelCount; ++i)
                    pixels[i] = static_cast<WORD>((pixels[i] << 1) | 1U);
                break;
            case D3DFMT_R5G6B5:
                uploadFormat = GL_RGB;
                uploadType = GL_UNSIGNED_SHORT_5_6_5;
                break;
            default:
                native16 = false;
                break;
            }

            if (native16)
            {
                while (glGetError() != GL_NO_ERROR) {}
                glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
                glTexImage2D(GL_TEXTURE_2D, 0, uploadFormat, surface->width, surface->height, 0,
                             uploadFormat, uploadType, pixels);
                th08::psp::RenderPerfNoteActualUpload(pixelCount * 2U);
                const GLenum nativeError = glGetError();
                if (!discardCpuCopy || nativeError != GL_NO_ERROR)
                {
                    if (rotate4)
                    {
                        for (UINT i = 0; i < pixelCount; ++i)
                            pixels[i] = static_cast<WORD>((pixels[i] >> 4) |
                                                         (pixels[i] << 12));
                    }
                    else if (rotate1)
                    {
                        for (UINT i = 0; i < pixelCount; ++i)
                        {
                            pixels[i] = static_cast<WORD>((pixels[i] >> 1) |
                                (forceOpaqueBit ? 0x8000U : (pixels[i] << 15)));
                        }
                    }
                }
#if defined(TH08_PSP_VERBOSE_RENDER_DIAGNOSTICS)
                fprintf(stderr,
                        "TH08PSP TEXTURE_UPLOAD mode=native16 size=%lux%lu fmt=%lu dynamic=%d error=0x%04x\n",
                        static_cast<unsigned long>(surface->width),
                        static_cast<unsigned long>(surface->height),
                        static_cast<unsigned long>(surface->format),
                        discardCpuCopy ? 0 : 1,
                        static_cast<unsigned int>(nativeError));
#else
                if (nativeError != GL_NO_ERROR)
                {
                    fprintf(stderr,
                            "TH08PSP TEXTURE_UPLOAD native16_error size=%lux%lu fmt=%lu error=0x%04x\n",
                            static_cast<unsigned long>(surface->width),
                            static_cast<unsigned long>(surface->height),
                            static_cast<unsigned long>(surface->format),
                            static_cast<unsigned int>(nativeError));
                }
#endif
                if (nativeError == GL_NO_ERROR)
                    staticUpload.Finalize(glName);
                glBindTexture(GL_TEXTURE_2D, 0);
                if (nativeError != GL_NO_ERROR)
                    return;
                uploaded = true;
                surface->dirty = false;
                if (discardCpuCopy)
                    std::vector<BYTE>().swap(surface->pixels);
                return;
            }
        }
#endif
        std::vector<BYTE> rgba(surface->width * surface->height * 4);
        UINT bytes = BytesPerPixel(surface->format);
        for (UINT y = 0; y < surface->height; ++y)
            for (UINT x = 0; x < surface->width; ++x)
                DecodePixel(&surface->pixels[y * surface->pitch + x * bytes], surface->format,
                            &rgba[(y * surface->width + x) * 4]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surface->width, surface->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba.empty() ? NULL : &rgba[0]);
#if defined(PSP)
        th08::psp::RenderPerfNoteActualUpload(surface->width * surface->height * 4U);
        const GLenum rgbaError = glGetError();
        if (rgbaError == GL_NO_ERROR)
            staticUpload.Finalize(glName);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (rgbaError != GL_NO_ERROR)
        {
            fprintf(stderr,
                    "TH08PSP TEXTURE_UPLOAD rgba_error size=%lux%lu fmt=%lu error=0x%04x\n",
                    static_cast<unsigned long>(surface->width),
                    static_cast<unsigned long>(surface->height),
                    static_cast<unsigned long>(surface->format),
                    static_cast<unsigned int>(rgbaError));
            return;
        }
#endif
        uploaded = true; surface->dirty = false;
#if defined(PSP)
        if (discardCpuCopy)
        {
            std::vector<BYTE>().swap(surface->pixels);
        }
#endif
    }
    ULONG refs;
    DWORD priority;
    GLuint glName;
    bool uploaded;
    bool discardCpuCopy;
    // GL client format/type the live GE image was created with (0 = unknown).
    GLenum pspGlFormat;
    GLenum pspGlType;
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
    // Sampler parameters are texture-object state.  Keep their applied values
    // beside the owning GL name so deletion/name reuse cannot inherit a stale
    // cache entry.  Device-wide raw-state boundaries invalidate them by epoch.
    unsigned int pspSamplerEpoch = 0;
    GLuint pspSamplerGlName = 0;
    unsigned int pspSamplerValidMask = 0;
    GLint pspSamplerMinFilter = GL_NEAREST;
    GLint pspSamplerMagFilter = GL_NEAREST;
    GLint pspSamplerWrapS = GL_REPEAT;
    GLint pspSamplerWrapT = GL_REPEAT;
#endif
    LinuxSurface *surface;
};

#if defined(PSP)
void LinuxSurface::DestroyNativeFramebufferBacking()
{
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
    PspPrepareStateBoundary stateBoundary(g_pspPrepareStateCache);
#endif
    if (pspBlitProxy != NULL)
    {
        // The proxy owns the same GL object used by the surface cache.
        pspBlitTexture = 0;
        pspBlitProxy->Release();
        pspBlitProxy = NULL;
    }
    if (pspBlitTexture != 0)
    {
        glDeleteTextures(1, &pspBlitTexture);
        pspBlitTexture = 0;
    }
    pspBlitTextureDirty = true;
}
#endif

LinuxSurface::~LinuxSurface()
{
#if defined(PSP)
    DestroyNativeFramebufferBacking();
#else
    if (pspBlitTexture != 0)
        glDeleteTextures(1, &pspBlitTexture);
#endif
}

class LinuxVertexBuffer : public IDirect3DVertexBuffer8
{
  public:
    explicit LinuxVertexBuffer(UINT size) : refs(1), bytes(size) {}
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG value = --refs; if (value == 0) delete this; return value; }
    HRESULT Lock(UINT offset, UINT size, BYTE **data, DWORD)
    {
        if (data == NULL || offset > bytes.size()) return E_INVALIDARG;
        if (size == 0) size = static_cast<UINT>(bytes.size() - offset);
        if (offset + size > bytes.size()) return E_INVALIDARG;
        *data = bytes.empty() ? NULL : &bytes[offset]; return S_OK;
    }
    HRESULT Unlock() { return S_OK; }
    ULONG refs;
    std::vector<BYTE> bytes;
};

GLenum PrimitiveMode(D3DPRIMITIVETYPE type)
{
    switch (type)
    {
    case D3DPT_POINTLIST: return GL_POINTS;
    case D3DPT_LINELIST: return GL_LINES;
    case D3DPT_LINESTRIP: return GL_LINE_STRIP;
    case D3DPT_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
    case D3DPT_TRIANGLEFAN: return GL_TRIANGLE_FAN;
    default: return GL_TRIANGLES;
    }
}

UINT VertexCount(D3DPRIMITIVETYPE type, UINT primitiveCount)
{
    if (type == D3DPT_POINTLIST) return primitiveCount;
    if (type == D3DPT_LINELIST) return primitiveCount * 2;
    if (type == D3DPT_LINESTRIP) return primitiveCount + 1;
    if (type == D3DPT_TRIANGLELIST) return primitiveCount * 3;
    return primitiveCount + 2;
}

GLenum CompareFunction(DWORD function)
{
    switch (function)
    {
    case D3DCMP_NEVER: return GL_NEVER;
    case D3DCMP_LESS: return GL_LESS;
    case D3DCMP_EQUAL: return GL_EQUAL;
    case D3DCMP_LESSEQUAL: return GL_LEQUAL;
    case D3DCMP_GREATER: return GL_GREATER;
    case D3DCMP_NOTEQUAL: return GL_NOTEQUAL;
    case D3DCMP_GREATEREQUAL: return GL_GEQUAL;
    default: return GL_ALWAYS;
    }
}

GLenum BlendFunction(DWORD function)
{
    switch (function)
    {
    case D3DBLEND_ZERO: return GL_ZERO;
    case D3DBLEND_ONE: return GL_ONE;
    case D3DBLEND_SRCCOLOR: return GL_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR: return GL_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA: return GL_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA: return GL_DST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case D3DBLEND_DESTCOLOR: return GL_DST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
    case D3DBLEND_SRCALPHASAT: return GL_SRC_ALPHA_SATURATE;
    default: return GL_ONE;
    }
}

bool TextureOperationUsesTexture(DWORD operation, DWORD argument1, DWORD argument2)
{
    if (operation == D3DTOP_DISABLE) return false;
    if (operation == D3DTOP_SELECTARG1)
        return (argument1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
    return (argument1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE ||
           (argument2 & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
}

GLenum TextureArgumentSource(DWORD argument)
{
    switch (argument & D3DTA_SELECTMASK)
    {
    case D3DTA_TEXTURE: return GL_TEXTURE;
    case D3DTA_TFACTOR: return GL_CONSTANT;
    default: return GL_PRIMARY_COLOR;
    }
}

void ConfigureTextureComponent(GLenum combineParameter, GLenum source0Parameter,
                               GLenum source1Parameter, GLenum operand0Parameter,
                               GLenum operand1Parameter, DWORD operation,
                               DWORD argument1, DWORD argument2, GLenum operand)
{
    glTexEnvi(GL_TEXTURE_ENV, combineParameter,
              operation == D3DTOP_SELECTARG1 ? GL_REPLACE : GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV, source0Parameter, TextureArgumentSource(argument1));
    glTexEnvi(GL_TEXTURE_ENV, operand0Parameter, operand);
    glTexEnvi(GL_TEXTURE_ENV, source1Parameter, TextureArgumentSource(argument2));
    glTexEnvi(GL_TEXTURE_ENV, operand1Parameter, operand);
}

class LinuxDevice : public IDirect3DDevice8
{
  public:
#if TH08_PSP_DIALOGUE_SNAPSHOT_AT_BACKGROUND_ENABLED
    // Entry for Background::OnDraw (th08_linux_dialogue_snapshot_restore).
    void RestoreDialogueSnapshotExternal()
    {
#if TH08_PSP_DIALOGUE_SNAPSHOT_DIAG_ENABLED
        static unsigned long diagCalls = 0;
        if (diagCalls < 3UL)
        {
            ++diagCalls;
            th08::psp::BootLog("DIALOGUE_BG_HOOK entry=%lu ready=%d surface=%p backbuffer=%p\n",
                               diagCalls, dialogueSnapshotReady ? 1 : 0,
                               static_cast<void *>(dialogueSnapshotSurface),
                               static_cast<void *>(backbuffer));
        }
#endif
        RestoreDialogueSnapshot();
    }
#endif
    LinuxDevice(SDL_Window *window_, const D3DPRESENT_PARAMETERS &parameters)
        : refs(1), window(window_), context(NULL), backbuffer(NULL), texture(NULL), vertexBuffer(NULL),
          fvf(0), streamStride(0), renderFramebuffer(0), renderColorTexture(0), renderDepthBuffer(0),
          dialogueSnapshotTexture(0), dialogueSnapshotSurface(NULL), framebufferReady(false),
          dialogueSnapshotReady(false),
          dialogueRestoreFailureLogged(false), wasDialogPresent(false), presentCount(0),
          dialogueCaptureAttempts(0), dialogueCaptureSuccesses(0),
          dialogueCaptureFailures(0), dialogueRestoreCount(0),
          dialogueRestoreFailures(0), dialogueRestoresThisCapture(0),
          dialogueLateAllocationCount(0), dialogueLateHeapBytes(0),
          dialogueCapturePresent(0), dialogueCaptureUsedArenaFallback(false),
          probeLeft(0), probeBottom(0),
          probeWidth(0), probeHeight(0)
    {
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        // PSP owns one D3D device/context.  Raw surface helpers use this active
        // cache pointer only to fence value-state around their private GL work.
        g_pspPrepareStateCache = &pspPrepareStateCache;
#endif
        memset(renderStates, 0, sizeof(renderStates)); memset(textureStates, 0, sizeof(textureStates));
        Identity(&world); Identity(&view); Identity(&projection); Identity(&textureTransform);
        TH08_PSP_BOOT_CHECKPOINT("gl_context", "before_create", 0);
        context = SDL_GL_CreateContext(window);
        TH08_PSP_BOOT_CHECKPOINT("gl_context", "after_create",
                                 context != NULL ? 1 : 0);
        if (context == NULL) return;
        TH08_PSP_BOOT_CHECKPOINT("gl_context", "before_make_current", 0);
        const int makeCurrentResult = SDL_GL_MakeCurrent(window, context);
        TH08_PSP_BOOT_CHECKPOINT("gl_context", "after_make_current",
                                 makeCurrentResult);
#if defined(PSP)
        // SDL/PSPGL owns the lower front/back/depth surfaces by this point.
        // Drain that initialization before widening the aperture; ResetInternal
        // and all later immutable texture finalization may then see 4 MiB.
        TH08_PSP_BOOT_CHECKPOINT("ge4_pre_enable_finish", "before_gl_finish", 0);
        glFinish();
        TH08_PSP_BOOT_CHECKPOINT("ge4_pre_enable_finish", "after_gl_finish", 0);
        TH08_PSP_BOOT_CHECKPOINT("ge4_enable", "before", 0);
        const int ge4Active = th08_psp_ge4_enable_after_gu_idle();
        th08::psp::BootLog(
            "GE4 renderer_gate active=%d visible_bytes=%lu\n", ge4Active,
            static_cast<unsigned long>(sceGeEdramGetSize()));
        TH08_PSP_BOOT_CHECKPOINT("ge4_enable", "after", ge4Active);
#endif
        TH08_PSP_BOOT_CHECKPOINT("gl_entrypoints", "before", 0);
        g_fogCoordf = reinterpret_cast<FogCoordfFunction>(SDL_GL_GetProcAddress("glFogCoordf"));
        if (g_fogCoordf == NULL)
            g_fogCoordf = reinterpret_cast<FogCoordfFunction>(SDL_GL_GetProcAddress("glFogCoordfEXT"));
        TH08_PSP_BOOT_CHECKPOINT("gl_entrypoints", "after",
                                 g_fogCoordf != NULL ? 1 : 0);
        TH08_PSP_BOOT_CHECKPOINT("swap_interval", "before", 0);
#if defined(PSP)
        // A copied PC th08.cfg may request D3DPRESENT_INTERVAL_IMMEDIATE.
        // PSP has no useful tear-enabled presentation mode: keep one and only
        // one VBlank wait inside PSPGL, matching the proven TH07 pacing
        // contract.  Do not add a second sceDisplayWaitVblankStart here.
#if TH08_PSP_SWAP_NOWAIT_ENABLED
        // TH08_PSP_SWAP_NOWAIT: PSPGL issues sceDisplaySetFrameBuf(NEXTFRAME)
        // and returns.  WaitForPspRenderCadence owns every 60 Hz interval and
        // PspWaitForPendingFlip() guards the next frame's first GE write.
        const int requestedSwapInterval = 0;
#else
        const int requestedSwapInterval = 1;
#endif
#else
        const int requestedSwapInterval =
            parameters.FullScreen_PresentationInterval ==
                    D3DPRESENT_INTERVAL_IMMEDIATE
                ? 0
                : 1;
#endif
        const int swapIntervalResult =
            SDL_GL_SetSwapInterval(requestedSwapInterval);
        const int effectiveSwapInterval = SDL_GL_GetSwapInterval();
#if defined(PSP)
        th08::psp::BootLog(
            "PACING requested=%d set_result=%d effective=%d "
            "source=%s sc_only=1\n",
            requestedSwapInterval, swapIntervalResult, effectiveSwapInterval,
            TH08_PSP_SWAP_NOWAIT_ENABLED ? "cadence_nowait" : "pspgl_vblank");
#endif
        TH08_PSP_BOOT_CHECKPOINT("swap_interval", "after", swapIntervalResult);
#if TH08_PSP_SWAP_ASYNC_ENABLED
        TH08_PSP_BOOT_CHECKPOINT("swap_async", "before", 0);
        TH08_PSP_BOOT_CHECKPOINT("swap_async", "after",
                                 th08::psp::SwapAsyncInitialize() ? 1 : 0);
#endif
#if TH08_PSP_SWAP_TRIPLE_ENABLED
        // Before the device allocates its own VRAM objects so the third
        // colour buffer is guaranteed a place in the lower eDRAM tier.
        TH08_PSP_BOOT_CHECKPOINT("swap_triple", "before", 0);
        TH08_PSP_BOOT_CHECKPOINT("swap_triple", "after",
                                 th08::psp::SwapTripleInitialize() ? 1 : 0);
#endif
#if TH08_PSP_PSPGL_STREAM_ARENA_ENABLED
        pspStreamArenaLease = NULL;
        pspStreamArenaPresent = ~0UL;
        pspStreamArenaLeaseFrames = 0UL;
        pspStreamArenaAllocationFailures = 0UL;
        pspStreamArenaReleaseFailures = 0UL;
        pspStreamArenaAllocs = 0UL;
        pspStreamArenaOverflows = 0UL;
        pspStreamArenaPeakBytes = 0UL;
        pspStreamArenaOrphanAddress = 0UL;
        pspStreamArenaFaulted = false;
        __pspgl_th08_stream_arena_install(NULL, 0U);
        th08::psp::BootLog(
            "PSPGL_STREAM_ARENA mode=frame_lease resident_bytes=0 "
            "lease_bytes=%lu half_bytes=%lu storage=render_arena "
            "fallback=canonical_memalign fence=present_sync\n",
            static_cast<unsigned long>(kPspglStreamArenaLeaseBytes),
            static_cast<unsigned long>(kPspglStreamArenaHalfBytes));
#endif
        TH08_PSP_BOOT_CHECKPOINT("device_reset", "before", 0);
        framebufferReady = ResetInternal(parameters);
        TH08_PSP_BOOT_CHECKPOINT("device_reset", "after",
                                 framebufferReady ? 1 : 0);
        renderStates[D3DRS_TEXTUREFACTOR] = 0xffffffffu;
        renderStates[D3DRS_SRCBLEND] = D3DBLEND_SRCALPHA;
        renderStates[D3DRS_DESTBLEND] = D3DBLEND_INVSRCALPHA;
        renderStates[D3DRS_ZWRITEENABLE] = TRUE;
        renderStates[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL; renderStates[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
        textureStates[D3DTSS_COLOROP] = D3DTOP_MODULATE;
        textureStates[D3DTSS_COLORARG1] = D3DTA_TEXTURE; textureStates[D3DTSS_COLORARG2] = D3DTA_DIFFUSE;
        textureStates[D3DTSS_ALPHAOP] = D3DTOP_MODULATE;
        textureStates[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE; textureStates[D3DTSS_ALPHAARG2] = D3DTA_DIFFUSE;
        textureStates[D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP; textureStates[D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
        textureStates[D3DTSS_MINFILTER] = D3DTEXF_POINT; textureStates[D3DTSS_MAGFILTER] = D3DTEXF_POINT;
        glDisable(GL_CULL_FACE); glDisable(GL_LIGHTING);
#if defined(PSP)
#if defined(TH08_PSP_RENDER_PERF_DIAG)
        perfFrequency = SDL_GetPerformanceFrequency();
        perfLastPresent = perfDrawTicks = perfPresentTicks = perfSwapTicks = perfFrameTicks = 0;
        perfDrawCalls = perfVertices = perfFrames = 0;
#endif
        pspDrawVertices = NULL;
        pspDrawVertexCapacity = 0;
#if TH08_PSP_SCORE_POPUP_NATIVE_GE_ENABLED
        pspScorePopupNativeAttempts = 0U;
        pspScorePopupNativeSubmits = 0U;
        pspScorePopupNativeClientFallbacks = 0U;
        th08::psp::BootLog(
            "SCORE_POPUP_NATIVE_GE path=pspgl_stream_copy immediate=1 "
            "delayed_arena=0 score_only=1 time_popups=0 item_time=0 sc_only=1\n");
#endif
#if TH08_PSP_BULLET_DIRECT_GE_ENABLED
        const size_t directGeArenaBytes = static_cast<size_t>(
            kPspBulletDirectGeVertexCapacity) * sizeof(PspClientVertex);
        // This storage is renderer-owned for the device lifetime, so take it
        // from the run-lifetime render arena reserved before frontend heap
        // fragmentation begins.  A heap fallback would consume more than the
        // accepted Stage 5 minimum free heap and can turn this optional path
        // into a later gameplay OOM; failure must instead retain the generic
        // PSPGL path without adding Main-RAM heap pressure.
        pspBulletDirectGeVertices = static_cast<PspClientVertex *>(
            th08::psp::RenderResourceArenaAllocate(
                directGeArenaBytes, 64U, "bullet direct GE vertices"));
        pspBulletDirectGeVerticesBase = pspBulletDirectGeVertices;
        pspBulletDirectGeVertexCursor = 0U;
        pspBulletDirectGeArenaHighWater = 0U;
        pspBulletDirectGeArenaPresent = ~0UL;
        pspBulletDirectGeSubmittedBatches = 0U;
        pspBulletDirectGeSubmittedQuads = 0U;
        pspBulletDirectGeFallbacks = 0U;
        memset(&pspBulletPackedVertexFastpath, 0,
               sizeof(pspBulletPackedVertexFastpath));
        pspBulletPackedVertexFastpath.arenaCapacityVertices =
            kPspBulletDirectGeVertexCapacity;
        pspBulletPackedVertexBatchActive = false;
        pspBulletPackedVertexRunActive = false;
        pspBulletPackedVertexRunStart = 0U;
        pspBulletPackedVertexRunQuads = 0U;
        pspBulletPackedVertexRunPresent = ~0UL;
#if TH08_PSP_BULLET_PACKED_VERTEX_AUDIT_ENABLED
        memset(&pspBulletPackedVertexAudit, 0,
               sizeof(pspBulletPackedVertexAudit));
        th08::psp::BootLog(
            "BULLET_PACKED_VERTEX_AUDIT enabled=1 mode=shadow_only "
            "canonical_authority=direct_ge_4v_indexed candidate_submit=0 "
            "candidate_storage=stack_96B sc_only=1 me=0 topology=4v\n");
#endif
#if TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH_ENABLED
        th08::psp::BootLog(
            "BULLET_PACKED_VERTEX_FASTPATH enabled=1 mode=product "
            "writer=frontend_final_quad storage=direct_ge_present_fenced "
            "source_stride=28 packed_stride=24 effective_color=uniform_once "
            "native_fallback=same_24B_client_array topology=4v_indexed "
            "extra_flush=0 extra_draw=0 sc_only=1 me=0\n");
#endif
#if TH08_PSP_BULLET_MIXED_QUADS_FASTPATH_ENABLED || \
    TH08_PSP_ITEM_MIXED_QUADS_FASTPATH_ENABLED
        pspBulletMixedGeAttempts = 0U;
        pspBulletMixedGeSubmittedBatches = 0U;
        pspBulletMixedGeSubmittedQuads = 0U;
        pspBulletMixedGeFallbacks = 0U;
        pspBulletMixedGeArenaExhaustions = 0U;
        pspItemMixedGeAttempts = 0U;
        pspItemMixedGeSubmittedBatches = 0U;
        pspItemMixedGeSubmittedQuads = 0U;
        pspItemMixedGeFallbacks = 0U;
        pspItemMixedGeArenaExhaustions = 0U;
#endif
        th08::psp::BootLog(
            "BULLET_DIRECT_GE arena_ready=%d bytes=%lu alignment=64 "
            "capacity_vertices=%lu storage=render_arena heap_fallback=0 "
            "sc_only=1\n",
            pspBulletDirectGeVertices != NULL ? 1 : 0,
            static_cast<unsigned long>(
                kPspBulletDirectGeVertexCapacity *
                sizeof(PspClientVertex)),
            static_cast<unsigned long>(
                kPspBulletDirectGeVertexCapacity));
#endif
#if TH08_PSP_ITEM_DIRECT_GE_ENABLED
        // Item storage is deliberately lazy and stage-scoped. Reserving it here stole the
        // contiguous render-arena window used by the first Stage texture's
        // 256 KiB conversion workspace.  The first real Item batch runs only
        // after its texture Upload() has returned and that workspace has been
        // released, so it is the safe lifetime boundary for this stage's
        // working set. GameManager releases it after cutting every stage draw
        // chain, before the next stage loads. Allocation failure retains the
        // complete generic path and is rearmed at that same boundary.
        pspItemDirectGeVertices = NULL;
        pspItemDirectGeAllocationAttempted = false;
        pspItemDirectGeFaulted = false;
        pspItemDirectGeVertexCursor = 0U;
        pspItemDirectGeArenaHighWater = 0U;
        pspItemDirectGeArenaPresent = ~0UL;
        pspItemDirectGeSubmittedBatches = 0U;
        pspItemDirectGeSubmittedQuads = 0U;
        pspItemDirectGeFallbacks = 0U;
        th08::psp::BootLog(
            "ITEM_DIRECT_GE arena_ready=0 allocation=lazy bytes=%lu alignment=64 "
            "capacity_quads=%lu capacity_vertices=%lu "
            "storage=render_arena heap_fallback=0 sc_only=1\n",
            static_cast<unsigned long>(kPspItemDirectGeArenaBytes),
            static_cast<unsigned long>(kPspItemDirectGeArenaQuads),
            static_cast<unsigned long>(kPspItemDirectGeVertexCapacity));
#endif
#if TH08_PSP_ANY_DIRECT_GE_ENABLED
        // Both owners use the same immutable 0,1,2/1,2,3 u16 table. The first
        // accepted prefix establishes its exact address and validation state.
        pspBulletDirectGeIndexAuthority = NULL;
        pspBulletDirectGeValidatedIndexCount = 0U;
        pspBulletDirectGeIndexAuthorityRejected = false;
#endif
#endif
    }
    void BeginPspglStreamArenaFrame()
    {
#if TH08_PSP_PSPGL_STREAM_ARENA_ENABLED
        if (pspStreamArenaPresent == presentCount)
            return;
        pspStreamArenaPresent = presentCount;
        if (pspStreamArenaFaulted)
            return;

        pspStreamArenaLease = th08::psp::RenderResourceArenaAllocate(
            kPspglStreamArenaLeaseBytes, 64U, "pspgl stream frame lease");
        if (pspStreamArenaLease == NULL)
        {
            ++pspStreamArenaAllocationFailures;
            __pspgl_th08_stream_arena_install(NULL, 0U);
            return;
        }
        if (__pspgl_th08_stream_arena_install(
                pspStreamArenaLease, kPspglStreamArenaHalfBytes) == 0)
        {
            ++pspStreamArenaAllocationFailures;
            __pspgl_th08_stream_arena_install(NULL, 0U);
            const th08::psp::RenderResourceArenaFreeResult freeResult =
                th08::psp::RenderResourceArenaTryFree(pspStreamArenaLease);
            if (freeResult != th08::psp::RenderResourceArenaFreeResult::Freed)
            {
                ++pspStreamArenaReleaseFailures;
                pspStreamArenaFaulted = true;
            }
            if (freeResult ==
                th08::psp::RenderResourceArenaFreeResult::NotOwned)
            {
                pspStreamArenaOrphanAddress = static_cast<unsigned long>(
                    reinterpret_cast<std::uintptr_t>(pspStreamArenaLease));
            }
            pspStreamArenaLease = NULL;
            pspStreamArenaPresent = ~0UL;
            return;
        }

        // The synchronous PSPGL Present below fences every display list that
        // can reference this frame's transient vertices.  The frozen archive
        // computes its unused parity-1 pointer at the one-past address; this
        // synchronous path is contractually fixed to parity 0, so one 256 KiB
        // half is the complete live allocation.
        __pspgl_th08_stream_arena_begin_frame(0U);
        ++pspStreamArenaLeaseFrames;
#endif
    }

    void ReleasePspglStreamArenaFrame(bool report)
    {
#if TH08_PSP_PSPGL_STREAM_ARENA_ENABLED
        if (pspStreamArenaLease != NULL)
        {
            unsigned long allocs = 0UL;
            unsigned long overflows = 0UL;
            unsigned long peakBytes = 0UL;
            __pspgl_th08_stream_arena_stats(&allocs, &overflows, &peakBytes);
            pspStreamArenaAllocs += allocs;
            pspStreamArenaOverflows += overflows;
            if (peakBytes > pspStreamArenaPeakBytes)
                pspStreamArenaPeakBytes = peakBytes;

            // Disable the allocator before returning its backing block.  The
            // caller must have fenced the frame first; Present and teardown do.
            __pspgl_th08_stream_arena_install(NULL, 0U);
            const th08::psp::RenderResourceArenaFreeResult freeResult =
                th08::psp::RenderResourceArenaTryFree(pspStreamArenaLease);
            if (freeResult != th08::psp::RenderResourceArenaFreeResult::Freed)
            {
                ++pspStreamArenaReleaseFailures;
                pspStreamArenaFaulted = true;
            }
            // Quarantined pointers are consumed by the arena.  A logically
            // impossible NotOwned result is fail-closed, with its address kept
            // as inert diagnostics so Present never retries it every frame.
            if (freeResult ==
                th08::psp::RenderResourceArenaFreeResult::NotOwned)
            {
                pspStreamArenaOrphanAddress = static_cast<unsigned long>(
                    reinterpret_cast<std::uintptr_t>(pspStreamArenaLease));
            }
            pspStreamArenaLease = NULL;
        }
        pspStreamArenaPresent = ~0UL;

        if (report)
        {
            th08::psp::BootLog(
                "PSPGL_STREAM_ARENA stats presents=%lu lease_frames=%lu "
                "allocs=%lu overflows=%lu peak_bytes=%lu half_bytes=%lu "
                "resident_bytes=0 allocation_failures=%lu "
                "release_failures=%lu orphan=0x%08lx faulted=%d\n",
                presentCount, pspStreamArenaLeaseFrames,
                pspStreamArenaAllocs, pspStreamArenaOverflows,
                pspStreamArenaPeakBytes,
                static_cast<unsigned long>(kPspglStreamArenaHalfBytes),
                pspStreamArenaAllocationFailures,
                pspStreamArenaReleaseFailures,
                pspStreamArenaOrphanAddress,
                pspStreamArenaFaulted ? 1 : 0);
        }
#else
        (void)report;
#endif
    }

    ~LinuxDevice()
    {
        if (context != NULL)
        {
            SDL_GL_MakeCurrent(window, context);
            // Commit the live PSPGL texture registers, then wait until GE no
            // longer references any lower/upper eDRAM owner before freeing it.
            glBindTexture(GL_TEXTURE_2D, 0);
            glFinish();
#if TH08_PSP_PSPGL_STREAM_ARENA_ENABLED
            ReleasePspglStreamArenaFrame(false);
#endif
        }
        if (texture != NULL) texture->Release();
        if (vertexBuffer != NULL) vertexBuffer->Release();
        if (backbuffer != NULL) backbuffer->Release();
        DestroyRenderTarget();
        if (context != NULL)
        {
            // Context teardown releases PSPGL's remaining fixed allocations.
            // The outer PSP host restores the 2 MiB aperture only afterwards.
            glFinish();
            SDL_GL_DeleteContext(context);
        }
#if defined(PSP)
#if TH08_PSP_SCORE_POPUP_NATIVE_GE_ENABLED
        th08::psp::BootLog(
            "SCORE_POPUP_NATIVE_GE attempts=%lu submitted=%lu "
            "client_fallbacks=%lu path=pspgl_stream_copy score_only=1 "
            "time_popups=0 item_time=0\n",
            pspScorePopupNativeAttempts,
            pspScorePopupNativeSubmits,
            pspScorePopupNativeClientFallbacks);
#endif
#if TH08_PSP_BULLET_DIRECT_GE_ENABLED
#if TH08_PSP_BULLET_MIXED_QUADS_FASTPATH_ENABLED || \
    TH08_PSP_ITEM_MIXED_QUADS_FASTPATH_ENABLED
        th08::psp::BootLog(
            "MIXED_GE_BACKEND bullet_attempts=%lu "
            "bullet_submitted_batches=%lu bullet_submitted_quads=%lu "
            "bullet_fallbacks=%lu bullet_arena_exhaustions=%lu "
            "item_attempts=%lu item_submitted_batches=%lu "
            "item_submitted_quads=%lu item_fallbacks=%lu "
            "item_arena_exhaustions=%lu shared_arena_high_water_vertices=%lu "
            "shared_arena_capacity_vertices=%lu\n",
            pspBulletMixedGeAttempts, pspBulletMixedGeSubmittedBatches,
            pspBulletMixedGeSubmittedQuads, pspBulletMixedGeFallbacks,
            pspBulletMixedGeArenaExhaustions, pspItemMixedGeAttempts,
            pspItemMixedGeSubmittedBatches, pspItemMixedGeSubmittedQuads,
            pspItemMixedGeFallbacks, pspItemMixedGeArenaExhaustions,
            static_cast<unsigned long>(pspBulletDirectGeArenaHighWater),
            static_cast<unsigned long>(kPspBulletDirectGeVertexCapacity));
#endif
#if TH08_PSP_BULLET_PACKED_VERTEX_AUDIT_ENABLED
        th08::psp::BootLog(
            "BULLET_PACKED_VERTEX_AUDIT attempts=%lu attempted_quads=%lu "
            "eligible_batches=%lu eligible_quads=%lu compared_quads=%lu "
            "matched_quads=%lu mismatch_quads=%lu compared_vertices=%lu "
            "mismatch_vertices=%lu mismatch_bytes=%lu "
            "uniform_diffuse_quads=%lu per_vertex_diffuse_quads=%lu "
            "canonical_fallbacks=%lu owner_fallbacks=%lu "
            "state_fallbacks=%lu index_fallbacks=%lu "
            "capacity_fallbacks=%lu submit_fallbacks=%lu "
            "canonical_native_submits=%lu canonical_native_submitted_quads=%lu "
            "mismatch_u_bytes=%lu mismatch_v_bytes=%lu "
            "mismatch_color_bytes=%lu mismatch_x_bytes=%lu "
            "mismatch_y_bytes=%lu mismatch_z_bytes=%lu "
            "mismatch_other_bytes=%lu first_mismatch_valid=%lu "
            "first_mismatch_batch=%lu first_mismatch_quad=%lu "
            "first_mismatch_vertex=%lu first_mismatch_byte=%lu "
            "candidate_submits=0 canonical_authority=1 product_enabled=0\n",
            pspBulletPackedVertexAudit.attempts,
            pspBulletPackedVertexAudit.attemptedQuads,
            pspBulletPackedVertexAudit.eligibleBatches,
            pspBulletPackedVertexAudit.eligibleQuads,
            pspBulletPackedVertexAudit.comparedQuads,
            pspBulletPackedVertexAudit.matchedQuads,
            pspBulletPackedVertexAudit.mismatchQuads,
            pspBulletPackedVertexAudit.comparedVertices,
            pspBulletPackedVertexAudit.mismatchVertices,
            pspBulletPackedVertexAudit.mismatchBytes,
            pspBulletPackedVertexAudit.uniformDiffuseQuads,
            pspBulletPackedVertexAudit.perVertexDiffuseQuads,
            pspBulletPackedVertexAudit.canonicalFallbacks,
            pspBulletPackedVertexAudit.ownerFallbacks,
            pspBulletPackedVertexAudit.stateFallbacks,
            pspBulletPackedVertexAudit.indexFallbacks,
            pspBulletPackedVertexAudit.capacityFallbacks,
            pspBulletPackedVertexAudit.submitFallbacks,
            pspBulletPackedVertexAudit.canonicalNativeSubmits,
            pspBulletPackedVertexAudit.canonicalNativeSubmittedQuads,
            pspBulletPackedVertexAudit.mismatchUBytes,
            pspBulletPackedVertexAudit.mismatchVBytes,
            pspBulletPackedVertexAudit.mismatchColorBytes,
            pspBulletPackedVertexAudit.mismatchXBytes,
            pspBulletPackedVertexAudit.mismatchYBytes,
            pspBulletPackedVertexAudit.mismatchZBytes,
            pspBulletPackedVertexAudit.mismatchOtherBytes,
            pspBulletPackedVertexAudit.firstMismatchValid,
            pspBulletPackedVertexAudit.firstMismatchBatch,
            pspBulletPackedVertexAudit.firstMismatchQuad,
            pspBulletPackedVertexAudit.firstMismatchVertex,
            pspBulletPackedVertexAudit.firstMismatchByte);
#endif
#if TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH_ENABLED
        th08::psp::BootLog(
            "BULLET_PACKED_VERTEX_FASTPATH begin_attempts=%lu "
            "accepted_batches=%lu canonical_fallback_batches=%lu "
            "append_attempts=%lu appended_quads=%lu packed_vertices=%lu "
            "uniform_diffuse_quads=%lu per_vertex_diffuse_quads=%lu "
            "submit_attempts=%lu submitted_runs=%lu submitted_quads=%lu "
            "native_submits=%lu native_submitted_quads=%lu "
            "client_fallback_submits=%lu client_fallback_quads=%lu "
            "owner_fallbacks=%lu state_fallbacks=%lu index_fallbacks=%lu "
            "capacity_fallbacks=%lu contract_fallbacks=%lu "
            "abandoned_runs=%lu abandoned_quads=%lu "
            "recovery_split_runs=%lu recovery_split_quads=%lu "
            "max_run_quads=%lu "
            "arena_high_water_vertices=%lu arena_capacity_vertices=%lu "
            "manager_28B_staging_writes=0 packed_generation_passes=1 "
            "extra_flush=0 topology=4v_indexed product_enabled=1\n",
            pspBulletPackedVertexFastpath.beginAttempts,
            pspBulletPackedVertexFastpath.acceptedBatches,
            pspBulletPackedVertexFastpath.canonicalFallbackBatches,
            pspBulletPackedVertexFastpath.appendAttempts,
            pspBulletPackedVertexFastpath.appendedQuads,
            pspBulletPackedVertexFastpath.packedVertices,
            pspBulletPackedVertexFastpath.uniformDiffuseQuads,
            pspBulletPackedVertexFastpath.perVertexDiffuseQuads,
            pspBulletPackedVertexFastpath.submitAttempts,
            pspBulletPackedVertexFastpath.submittedRuns,
            pspBulletPackedVertexFastpath.submittedQuads,
            pspBulletPackedVertexFastpath.nativeSubmits,
            pspBulletPackedVertexFastpath.nativeSubmittedQuads,
            pspBulletPackedVertexFastpath.clientFallbackSubmits,
            pspBulletPackedVertexFastpath.clientFallbackQuads,
            pspBulletPackedVertexFastpath.ownerFallbacks,
            pspBulletPackedVertexFastpath.stateFallbacks,
            pspBulletPackedVertexFastpath.indexFallbacks,
            pspBulletPackedVertexFastpath.capacityFallbacks,
            pspBulletPackedVertexFastpath.contractFallbacks,
            pspBulletPackedVertexFastpath.abandonedRuns,
            pspBulletPackedVertexFastpath.abandonedQuads,
            pspBulletPackedVertexFastpath.recoverySplitRuns,
            pspBulletPackedVertexFastpath.recoverySplitQuads,
            pspBulletPackedVertexFastpath.maxRunQuads,
            pspBulletDirectGeArenaHighWater,
            static_cast<unsigned long>(kPspBulletDirectGeVertexCapacity));
#endif
        th08::psp::BootLog(
            "BULLET_DIRECT_GE submitted_batches=%lu submitted_quads=%lu "
            "fallbacks=%lu arena_high_water_vertices=%lu "
            "arena_high_water_bytes=%lu\n",
            pspBulletDirectGeSubmittedBatches,
            pspBulletDirectGeSubmittedQuads,
            pspBulletDirectGeFallbacks,
            static_cast<unsigned long>(pspBulletDirectGeArenaHighWater),
            static_cast<unsigned long>(pspBulletDirectGeArenaHighWater *
                                       sizeof(PspClientVertex)));
        if (pspBulletDirectGeVertices != NULL)
        {
            const th08::psp::RenderResourceArenaFreeResult freeResult =
                th08::psp::RenderResourceArenaTryFree(
                    pspBulletDirectGeVerticesBase);
            pspBulletDirectGeVerticesBase = NULL;
            th08::psp::BootLog(
                "BULLET_DIRECT_GE arena_release=%lu storage=render_arena\n",
                static_cast<unsigned long>(freeResult));
            pspBulletDirectGeVertices = NULL;
        }
#endif
#if TH08_PSP_ITEM_DIRECT_GE_ENABLED
        th08::psp::BootLog(
            "ITEM_DIRECT_GE submitted_batches=%lu submitted_quads=%lu "
            "fallbacks=%lu arena_high_water_vertices=%lu "
            "arena_high_water_bytes=%lu capacity_bytes=%lu\n",
            pspItemDirectGeSubmittedBatches,
            pspItemDirectGeSubmittedQuads,
            pspItemDirectGeFallbacks,
            static_cast<unsigned long>(pspItemDirectGeArenaHighWater),
            static_cast<unsigned long>(pspItemDirectGeArenaHighWater *
                                       sizeof(PspClientVertex)),
            static_cast<unsigned long>(kPspItemDirectGeArenaBytes));
        if (pspItemDirectGeVertices != NULL)
        {
            const th08::psp::RenderResourceArenaFreeResult freeResult =
                th08::psp::RenderResourceArenaTryFree(
                    pspItemDirectGeVertices);
            th08::psp::BootLog(
                "ITEM_DIRECT_GE arena_release=%lu storage=render_arena\n",
                static_cast<unsigned long>(freeResult));
            pspItemDirectGeVertices = NULL;
        }
#endif
        free(pspDrawVertices);
#endif
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        if (g_pspPrepareStateCache == &pspPrepareStateCache)
            g_pspPrepareStateCache = NULL;
#endif
    }
    bool Ready() const { return context != NULL && backbuffer != NULL && framebufferReady; }
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG value = --refs; if (value == 0) delete this; return value; }
    HRESULT TestCooperativeLevel() { return S_OK; }
    HRESULT Reset(D3DPRESENT_PARAMETERS *parameters)
    {
        if (parameters == NULL) return E_INVALIDARG;
#if TH08_PSP_PSPGL_STREAM_ARENA_ENABLED
        if (pspStreamArenaLease != NULL)
        {
            glFinish();
            ReleasePspglStreamArenaFrame(false);
        }
#endif
#if TH08_PSP_SWAP_TRIPLE_ENABLED
        th08::psp::SwapTripleDrain();
#endif
        framebufferReady = ResetInternal(*parameters);
        return framebufferReady ? S_OK : E_FAIL;
    }
    HRESULT Present(const RECT *, const RECT *, HWND, const RGNDATA *)
    {
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(&pspPrepareStateCache);
#endif
#if defined(PSP) && defined(TH08_PSP_RENDER_PERF_DIAG)
        const Uint64 presentStart = SDL_GetPerformanceCounter();
#endif
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
        {
            th08::psp::PerfAttributionScope preSwapScope(
                th08::psp::PerfAttributionPhase::PresentPreSwap);
#endif
        backbuffer->FlushBackbuffer();
        presentCount++;

#if defined(PSP)
        glFlush();
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
        }
#endif
#if defined(TH08_PSP_RENDER_PERF_DIAG)
        const Uint64 swapStart = SDL_GetPerformanceCounter();
#endif
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
        {
            th08::psp::PerfAttributionScope swapScope(
                th08::psp::PerfAttributionPhase::PresentSwap);
            th08::psp::PerfAttributionWaitContextScope waitContext(
                th08::psp::PerfAttributionWaitContext::Swap);
#endif
#if TH08_PSP_SWAP_TRIPLE_ENABLED
        if (th08::psp::SwapTripleActive())
        {
            // No GE wait, no flip guard: the next frame draws into a buffer
            // that is neither displayed nor pending.
            std::uint64_t tripleWaitUs = 0U;
            th08::psp::SwapTriplePresent(&tripleWaitUs);
            (void)tripleWaitUs;
            pspSwapFlipPending = false;
#if TH08_PSP_PERF_ENV_ENABLED
            th08::psp::PerfEnvNoteGeFrameEnd(
                static_cast<std::uint64_t>(sceKernelGetSystemTimeWide()));
#endif
        }
        else
#endif
#if TH08_PSP_SWAP_ASYNC_ENABLED
        if (th08::psp::SwapAsyncActive())
        {
            th08::psp::SwapAsyncPresent();
            pspSwapFlipPending = true;
        }
        else
#endif
        {
            SDL_GL_SwapWindow(window);
#if TH08_PSP_PERF_ENV_ENABLED
            th08::psp::PerfEnvNoteGeFrameEnd(
                static_cast<std::uint64_t>(sceKernelGetSystemTimeWide()));
#endif
#if TH08_PSP_SWAP_NOWAIT_ENABLED
            pspSwapVcount = sceDisplayGetVcount();
            pspSwapFlipPending = true;
#endif
        }
#if TH08_PSP_PSPGL_STREAM_ARENA_ENABLED
        // eglSwapBuffers synchronizes the just-rendered colour target, which
        // also retires every transient stream buffer pinned to those lists.
        ReleasePspglStreamArenaFrame(
            presentCount == 1UL || (presentCount % 600UL) == 0UL);
#endif
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
        }
#endif
#if defined(TH08_PSP_RENDER_PERF_DIAG)
        const Uint64 presentEnd = SDL_GetPerformanceCounter();
#endif
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
        {
            th08::psp::PerfAttributionScope postSwapScope(
                th08::psp::PerfAttributionPhase::PresentPostSwap);
#endif
        ApplyPspViewport();
#if defined(TH08_PSP_RENDER_PERF_DIAG)
        perfPresentTicks += presentEnd - presentStart;
        perfSwapTicks += presentEnd - swapStart;
        if (perfLastPresent != 0)
            perfFrameTicks += presentEnd - perfLastPresent;
        perfLastPresent = presentEnd;
        ++perfFrames;
        if (perfFrames >= 8 && perfFrequency != 0)
        {
            const double ticksToMs = 1000.0 / static_cast<double>(perfFrequency);
            const double measuredFrames = perfFrames > 1 ? perfFrames - 1 : 1;
            fprintf(stderr,
                    "TH08PSP PERF frames=%lu full_ms=%.2f present_ms=%.2f swap_ms=%.2f "
                    "draw_ms=%.2f draws=%.2f vertices=%.2f\n",
                    static_cast<unsigned long>(perfFrames),
                    perfFrameTicks * ticksToMs / measuredFrames,
                    perfPresentTicks * ticksToMs / perfFrames,
                    perfSwapTicks * ticksToMs / perfFrames,
                    perfDrawTicks * ticksToMs / perfFrames,
                    static_cast<double>(perfDrawCalls) / perfFrames,
                    static_cast<double>(perfVertices) / perfFrames);
            perfDrawTicks = perfPresentTicks = perfSwapTicks = perfFrameTicks = 0;
            perfDrawCalls = perfVertices = perfFrames = 0;
            perfLastPresent = 0;
        }
#endif
        th08::psp::RenderPerfTelemetryEndFrame();
        // Stage-relative A/B windows must start and end only after an actual
        // completed Present.  This keeps any pre-draw/shared-batch partial
        // counters out of the next 300-Present interval without clearing
        // render ownership state mid-batch.
        th08::psp::MemoryTelemetryAfterPresent();
#if TH08_PSP_PERF_ATTRIBUTION_ENABLED
        }
#endif
#else
        int drawableWidth, drawableHeight;
        SDL_GL_GetDrawableSize(window, &drawableWidth, &drawableHeight);
        g_framebufferApi.bindFramebuffer(GL_FRAMEBUFFER, 0);
        glDrawBuffer(GL_BACK);
        glViewport(0, 0, drawableWidth, drawableHeight);

        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glDisable(GL_ALPHA_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING); glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, renderColorTexture);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(0.0, drawableWidth, drawableHeight, 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glColor4ub(255, 255, 255, 255);
        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f, 0.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(static_cast<float>(drawableWidth), 0.0f);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, static_cast<float>(drawableHeight));
        glTexCoord2f(1.0f, 0.0f); glVertex2f(static_cast<float>(drawableWidth),
                                            static_cast<float>(drawableHeight));
        glEnd();
        glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW);
        glPopAttrib();

        glFlush();
        SDL_GL_SwapWindow(window);

        g_framebufferApi.bindFramebuffer(GL_FRAMEBUFFER, renderFramebuffer);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glViewport(0, 0, backbuffer->width, backbuffer->height);
#endif
        return S_OK;
    }
    HRESULT GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE, IDirect3DSurface8 **result)
    {
        if (index != 0 || result == NULL || backbuffer == NULL) return E_INVALIDARG;
        backbuffer->AddRef(); *result = backbuffer; return S_OK;
    }
    HRESULT CreateTexture(UINT width, UINT height, UINT, DWORD, D3DFORMAT format, D3DPOOL,
                          IDirect3DTexture8 **result)
    {
        if (result == NULL || width == 0 || height == 0) return E_INVALIDARG;
        if (format == D3DFMT_UNKNOWN) format = D3DFMT_A8R8G8B8;
        *result = new(std::nothrow) LinuxTexture(width, height, format);
        return *result != NULL ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT CreateVertexBuffer(UINT size, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer8 **result)
    {
        if (result == NULL) return E_INVALIDARG;
        *result = new(std::nothrow) LinuxVertexBuffer(size); return *result != NULL ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE, BOOL,
                               IDirect3DSurface8 **result)
    { return CreateSurface(width, height, format, result); }
    HRESULT CreateImageSurface(UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8 **result)
    { return CreateSurface(width, height, format, result); }
#if defined(PSP)
    bool CreatePspSurfaceCacheProxy(LinuxSurface *source)
    {
        if (source == NULL || source->pspBlitTexture == 0)
            return false;
        if (source->pspBlitProxy != NULL)
            return true;

        source->pspBlitProxy = new(std::nothrow) LinuxTexture(1, 1, D3DFMT_R5G6B5);
        if (source->pspBlitProxy == NULL)
            return false;
        source->pspBlitProxy->glName = source->pspBlitTexture;
        source->pspBlitProxy->uploaded = true;
        source->pspBlitProxy->surface->dirty = false;
        return true;
    }

    bool EnsurePspSurfaceCacheProxy(LinuxSurface *source, const char *owner)
    {
        th08::psp::RenderResourceAllocationScope arenaScope(owner);
        return CreatePspSurfaceCacheProxy(source);
    }

    bool DrawPspSurfaceCache(LinuxSurface *source, const RECT &sourceRect,
                             const POINT &destinationPoint)
    {
        if (source == NULL || source->pspBlitTexture == 0 || source->width == 0 ||
            source->height == 0)
            return false;

        if (!EnsurePspSurfaceCacheProxy(source, "surface cache proxy"))
            return false;

        // Submit the surface through the exact D3D-compatible draw path used
        // by visible ANM sprites.  Raw PSPGL draws made from CopyRects were
        // accepted without errors but never reached the displayed buffer.
        DWORD savedRenderStates[256];
        DWORD savedTextureStates[32];
        memcpy(savedRenderStates, renderStates, sizeof(savedRenderStates));
        memcpy(savedTextureStates, textureStates, sizeof(savedTextureStates));
        const DWORD savedFvf = fvf;
        LinuxTexture *savedTexture = texture;
        if (savedTexture != NULL)
            savedTexture->AddRef();
        source->pspBlitProxy->AddRef();
        if (texture != NULL)
            texture->Release();
        texture = source->pspBlitProxy;

        renderStates[D3DRS_ALPHABLENDENABLE] = FALSE;
        renderStates[D3DRS_ALPHATESTENABLE] = FALSE;
        renderStates[D3DRS_ZENABLE] = FALSE;
        renderStates[D3DRS_ZWRITEENABLE] = FALSE;
        renderStates[D3DRS_FOGENABLE] = FALSE;
        textureStates[D3DTSS_COLOROP] = D3DTOP_MODULATE;
        textureStates[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        textureStates[D3DTSS_COLORARG2] = D3DTA_DIFFUSE;
        textureStates[D3DTSS_ALPHAOP] = D3DTOP_MODULATE;
        textureStates[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        textureStates[D3DTSS_ALPHAARG2] = D3DTA_DIFFUSE;
        textureStates[D3DTSS_ADDRESSU] = D3DTADDRESS_CLAMP;
        textureStates[D3DTSS_ADDRESSV] = D3DTADDRESS_CLAMP;
        textureStates[D3DTSS_MINFILTER] = D3DTEXF_LINEAR;
        textureStates[D3DTSS_MAGFILTER] = D3DTEXF_LINEAR;
        fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
#if TH08_PSP_DIALOGUE_SNAPSHOT_DIAG_ENABLED
        if (pspDialogueRestoreDiagFlash)
        {
            textureStates[D3DTSS_COLOROP] = D3DTOP_SELECTARG1;
            textureStates[D3DTSS_COLORARG1] = D3DTA_DIFFUSE;
        }
        const DWORD quadDiffuse = pspDialogueRestoreDiagFlash ? 0xffff00ffu : 0xffffffffu;
#else
        const DWORD quadDiffuse = 0xffffffffu;
#endif

        constexpr float textureWidth = 512.0f;
        constexpr float textureHeight = 512.0f;
        const float contentWidth = static_cast<float>(
            (static_cast<unsigned long long>(source->width) * kPspFitWidth + 639u) /
            640u);
        const float contentHeight = static_cast<float>(
            (static_cast<unsigned long long>(source->height) * kPspScreenHeight + 479u) /
            480u);
        const float uScale = contentWidth /
                             (static_cast<float>(source->width) * textureWidth);
        const float vScale = contentHeight /
                             (static_cast<float>(source->height) * textureHeight);
        const float u0 = static_cast<float>(sourceRect.left) * uScale;
        const float v0 = static_cast<float>(sourceRect.top) * vScale;
        const float u1 = static_cast<float>(sourceRect.right) * uScale;
        const float v1 = static_cast<float>(sourceRect.bottom) * vScale;
        const float x0 = static_cast<float>(destinationPoint.x);
        const float y0 = static_cast<float>(destinationPoint.y);
        const float x1 = x0 + static_cast<float>(sourceRect.right - sourceRect.left);
        const float y1 = y0 + static_cast<float>(sourceRect.bottom - sourceRect.top);

        th08::VertexTex1DiffuseXyzrhw quad[6];
        const float xs[6] = {x0, x1, x0, x1, x0, x1};
        const float ys[6] = {y0, y0, y1, y0, y1, y1};
        const float us[6] = {u0, u1, u0, u1, u0, u1};
        const float vs[6] = {v0, v0, v1, v0, v1, v1};
        for (UINT index = 0; index < 6; ++index)
        {
            quad[index].pos.x = xs[index];
            quad[index].pos.y = ys[index];
            quad[index].pos.z = 0.0f;
            quad[index].w = 1.0f;
            quad[index].diffuse = quadDiffuse;
            quad[index].textureUV.x = us[index];
            quad[index].textureUV.y = vs[index];
        }
        const HRESULT drawStatus = Draw(D3DPT_TRIANGLELIST, 2,
                                        reinterpret_cast<const BYTE *>(quad),
                                        sizeof(quad[0]));

        memcpy(renderStates, savedRenderStates, sizeof(savedRenderStates));
        memcpy(textureStates, savedTextureStates, sizeof(savedTextureStates));
        fvf = savedFvf;
        texture->Release();
        texture = savedTexture;
        return SUCCEEDED(drawStatus);
    }
#endif
    HRESULT CopyRects(IDirect3DSurface8 *sourceRaw, const RECT *sourceRects, UINT count,
                      IDirect3DSurface8 *destinationRaw, const POINT *destinationPoints)
    {
#if defined(PSP)
        gPspSurfaceOp = "CopyRects";
#endif
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(&pspPrepareStateCache);
#endif
#if defined(PSP)
        LinuxSurface *sourceSurface = static_cast<LinuxSurface *>(sourceRaw);
        LinuxSurface *destinationSurface = static_cast<LinuxSurface *>(destinationRaw);
        if (sourceSurface != NULL && destinationSurface != NULL &&
            !sourceSurface->backbuffer && destinationSurface->backbuffer &&
            (count == 0 || count == 1))
        {
            RECT rect;
            if (sourceRects != NULL)
                rect = sourceRects[0];
            else
            {
                rect.left = 0; rect.top = 0;
                rect.right = sourceSurface->width; rect.bottom = sourceSurface->height;
            }
            POINT point;
            point.x = destinationPoints != NULL ? destinationPoints[0].x : 0;
            point.y = destinationPoints != NULL ? destinationPoints[0].y : 0;
            if (sourceSurface->BlitToBackbuffer(destinationSurface, rect, point,
                                                false))
            {
                if (DrawPspSurfaceCache(sourceSurface, rect, point))
                {
                    destinationSurface->dirty = false;
                    return S_OK;
                }
            }
        }
#endif
        LinuxSurfaceAccess source, destination;
        if (!th08_linux_surface_access(sourceRaw, &source, true) ||
            !th08_linux_surface_access(destinationRaw, &destination, false)) return E_INVALIDARG;
        if (source.format != destination.format) return E_NOTIMPL;
        if (count == 0) count = 1;
        UINT bytes = BytesPerPixel(source.format);
        for (UINT index = 0; index < count; ++index)
        {
            RECT rect;
            if (sourceRects != NULL) rect = sourceRects[index];
            else { rect.left = 0; rect.top = 0; rect.right = source.width; rect.bottom = source.height; }
            POINT point; point.x = destinationPoints != NULL ? destinationPoints[index].x : 0;
            point.y = destinationPoints != NULL ? destinationPoints[index].y : 0;
            UINT copyWidth = rect.right > rect.left ? rect.right - rect.left : 0;
            UINT copyHeight = rect.bottom > rect.top ? rect.bottom - rect.top : 0;
            if (point.x < 0 || point.y < 0 || rect.left < 0 || rect.top < 0) continue;
            if (static_cast<UINT>(point.x) + copyWidth > destination.width) copyWidth = destination.width - point.x;
            if (static_cast<UINT>(point.y) + copyHeight > destination.height) copyHeight = destination.height - point.y;
            for (UINT y = 0; y < copyHeight; ++y)
                memcpy(destination.pixels + (point.y + y) * destination.pitch + point.x * bytes,
                       source.pixels + (rect.top + y) * source.pitch + rect.left * bytes, copyWidth * bytes);
        }
        th08_linux_surface_changed(destinationRaw); return S_OK;
    }
#if TH08_PSP_SWAP_NOWAIT_ENABLED
    // The flip requested by the previous Present (NEXTFRAME) takes effect at
    // the next VBlank.  Never let the GE write into the buffer still on
    // screen: wait for that VBlank if the covered simulation ticks did not
    // already cross it (the usual case at 20/30 draws).
    void PspWaitForPendingFlip()
    {
        if (!pspSwapFlipPending)
            return;
#if TH08_PSP_SWAP_ASYNC_ENABLED
        if (th08::psp::SwapAsyncActive())
        {
            std::uint64_t waitedUs = 0U;
            th08::psp::SwapAsyncWaitFlipComplete(&waitedUs);
#if TH08_PSP_PERF_ENV_ENABLED
            th08::psp::PerfEnvNoteFlipWait(waitedUs);
#endif
            pspSwapFlipPending = false;
            return;
        }
#endif
        // No wait context: the time stays inside the enclosing DrawFrame
        // phase (draw frame other) and is reported by PERF_ENV as fw=, so
        // the main record never double counts it.
        const std::uint64_t waitStartUs =
            static_cast<std::uint64_t>(sceKernelGetSystemTimeWide());
        // Draw only once the display scans the buffer PSPGL asked it to show
        // (the VBlank counter alone let the GE write the on-screen buffer).
        {
            const void *const frontBase = th08::psp::SwapFrontBufferBase();
            while (!th08::psp::SwapDisplayShows(frontBase))
                sceDisplayWaitVblankStart();
            (void)pspSwapVcount;
        }
        pspSwapFlipPending = false;
#if TH08_PSP_PERF_ENV_ENABLED
        const std::uint64_t waitEndUs =
            static_cast<std::uint64_t>(sceKernelGetSystemTimeWide());
        th08::psp::PerfEnvNoteFlipWait(
            waitEndUs >= waitStartUs ? waitEndUs - waitStartUs : 0U);
#else
        (void)waitStartUs;
#endif
    }
#endif
    HRESULT BeginScene()
    {
#if TH08_PSP_SWAP_NOWAIT_ENABLED
        PspWaitForPendingFlip();
#endif
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(&pspPrepareStateCache);
#endif
#if defined(PSP)
#if TH08_PSP_BULLET_DIRECT_GE_ENABLED
        // Present waits for the displayed color target's PSPGL lists before it
        // returns.  Reset only after observing that fence; a repeated
        // BeginScene without Present must keep the append-only arena intact.
        if (pspBulletDirectGeArenaPresent != presentCount)
        {
#if TH08_PSP_SWAP_TRIPLE_ENABLED
            // Triple buffering removed the GE sync from Present: fence the
            // previous frame's lists here before its vertices are recycled.
            th08::psp::SwapTripleWaitPendingDone();
#endif
#if TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH_ENABLED
            if (pspBulletPackedVertexRunActive)
            {
                ++pspBulletPackedVertexFastpath.contractFallbacks;
                ++pspBulletPackedVertexFastpath.abandonedRuns;
                pspBulletPackedVertexFastpath.abandonedQuads +=
                    pspBulletPackedVertexRunQuads;
            }
            pspBulletPackedVertexBatchActive = false;
            pspBulletPackedVertexRunActive = false;
            pspBulletPackedVertexRunStart = 0U;
            pspBulletPackedVertexRunQuads = 0U;
            pspBulletPackedVertexRunPresent = presentCount;
#endif
            pspBulletDirectGeVertexCursor = 0U;
            pspBulletDirectGeArenaPresent = presentCount;
        }
#endif
#if TH08_PSP_PSPGL_STREAM_ARENA_ENABLED
        BeginPspglStreamArenaFrame();
#endif
#if TH08_PSP_ITEM_DIRECT_GE_ENABLED
        // The Item partition follows the same Present/swap fence contract but
        // has its own append cursor. BeginScene may be called repeatedly by
        // frontend/dialogue paths, so it must not recycle storage by itself.
        if (pspItemDirectGeArenaPresent != presentCount)
        {
            pspItemDirectGeVertexCursor = 0U;
            pspItemDirectGeArenaPresent = presentCount;
        }
#endif
        if (backbuffer != NULL)
            ClearPspFrameBands(static_cast<int>(backbuffer->width),
                               static_cast<int>(backbuffer->height));
#endif
        const bool dialogPresent = th08::g_Gui.IsDialoguePresent() != 0;
#if !TH08_PSP_DIALOGUE_LIVE_BACKGROUND_ENABLED
        if (dialogPresent && !wasDialogPresent)
            CaptureDialogueSnapshot();
#if !TH08_PSP_DIALOGUE_SNAPSHOT_AT_BACKGROUND_ENABLED
        if (dialogPresent && dialogueSnapshotReady)
            RestoreDialogueSnapshot();
#endif
        if (!dialogPresent && wasDialogPresent)
            ReleaseDialogueSnapshot();
#endif
        wasDialogPresent = dialogPresent;
        return S_OK;
    }
    HRESULT EndScene() { return S_OK; }
    HRESULT Clear(DWORD, const D3DRECT *, DWORD flags, D3DCOLOR color, float depth, DWORD)
    {
#if TH08_PSP_SWAP_NOWAIT_ENABLED
#if TH08_PSP_FLIP_GUARD_COLOR_ONLY_ENABLED
        // A depth-only clear never writes the colour buffer (PSPGL glClear:
        // GE clear mode with GU_DEPTH_BUFFER_BIT alone), so it may run while
        // the previous frame is still being shown.  The PSP stencil lives in
        // the colour buffer's alpha and keeps the guard.
        if ((flags & (D3DCLEAR_TARGET | D3DCLEAR_STENCIL)) != 0)
            PspWaitForPendingFlip();
#else
        PspWaitForPendingFlip();
#endif
#endif
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(&pspPrepareStateCache);
#endif
        if ((flags & D3DCLEAR_TARGET) && getenv("TH08_LINUX_RENDER_TRACE") != NULL)
        {
            FILE *trace = fopen("modern-render.txt", "ab");
            if (trace != NULL)
            {
                fprintf(trace,
                        "frame=%lu flags=%08lx color=%08lx viewport=%lu,%lu,%lu,%lu caller=%p\n",
                        presentCount, static_cast<unsigned long>(flags), static_cast<unsigned long>(color),
                        static_cast<unsigned long>(viewport.X), static_cast<unsigned long>(viewport.Y),
                        static_cast<unsigned long>(viewport.Width), static_cast<unsigned long>(viewport.Height),
                        __builtin_return_address(0));
                fclose(trace);
            }
        }

        GLbitfield mask = 0;
        if (flags & D3DCLEAR_TARGET)
        {
            glClearColor(((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f,
                         (color & 255) / 255.0f, ((color >> 24) & 255) / 255.0f);
            mask |= GL_COLOR_BUFFER_BIT;
        }
        if (flags & D3DCLEAR_ZBUFFER) { glClearDepth(depth); mask |= GL_DEPTH_BUFFER_BIT; }
        if (flags & D3DCLEAR_STENCIL) mask |= GL_STENCIL_BUFFER_BIT;
        const int targetWidth = backbuffer != NULL ? backbuffer->width : viewport.Width;
        const int targetHeight = backbuffer != NULL ? backbuffer->height : viewport.Height;
        const int left = static_cast<int>(viewport.X);
        const int bottom = targetHeight - static_cast<int>(viewport.Y + viewport.Height);
        const int width = static_cast<int>(viewport.Width);
        const int height = static_cast<int>(viewport.Height);
#if defined(PSP)
        // The D3D-facing viewport remains 640x480, but PSPGL rejects any
        // viewport larger than the physical 480x272 draw surface.  Scale the
        // clear rectangle just like PrepareState does for drawing; the old
        // logical glViewport call failed every frame and left stage pixels
        // uncleared, producing permanent bullet trails.
        ApplyPspViewport();
        const PspPhysicalRect physicalRect = MakePspPhysicalRect(
            left, static_cast<int>(viewport.Y), left + width,
            static_cast<int>(viewport.Y) + height, targetWidth, targetHeight);
        glEnable(GL_SCISSOR_TEST);
        glScissor(physicalRect.left, physicalRect.bottom,
                  physicalRect.width, physicalRect.height);
#else
        glViewport(0, 0, targetWidth, targetHeight);
        glEnable(GL_SCISSOR_TEST);
        glScissor(left, bottom, width, height);
#endif
        if (flags & D3DCLEAR_ZBUFFER) glDepthMask(GL_TRUE);
        glClear(mask);
        if (flags & D3DCLEAR_ZBUFFER)
            glDepthMask(renderStates[D3DRS_ZWRITEENABLE] ? GL_TRUE : GL_FALSE);
        glDisable(GL_SCISSOR_TEST);
        return S_OK;
    }
    HRESULT SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix)
    {
#if defined(PSP)
        th08::psp::RenderPerfNoteStateRequested();
#endif
        if (matrix == NULL) return E_INVALIDARG;
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        // Version changes are based on the 16 raw float words, not numerical
        // equality.  This preserves signed zero/NaN distinctions while making
        // repeated identical SetTransform calls free at PrepareState time.
        if (state == D3DTS_WORLD && memcmp(&world, matrix, sizeof(world)) != 0)
            ++pspWorldRawVersion;
        else if (state == D3DTS_VIEW && memcmp(&view, matrix, sizeof(view)) != 0)
            ++pspViewRawVersion;
        else if (state == D3DTS_PROJECTION &&
                 memcmp(&projection, matrix, sizeof(projection)) != 0)
            ++pspProjectionRawVersion;
#endif
        if (state == D3DTS_WORLD) world = *matrix;
        else if (state == D3DTS_VIEW) view = *matrix;
        else if (state == D3DTS_PROJECTION) projection = *matrix;
        else if (state == D3DTS_TEXTURE0) textureTransform = *matrix;
        return S_OK;
    }
    HRESULT SetViewport(const D3DVIEWPORT8 *value)
    {
#if defined(PSP)
        th08::psp::RenderPerfNoteStateRequested();
#endif
        if (value == NULL)
            return E_INVALIDARG;
        viewport = *value;
        return S_OK;
    }
    HRESULT GetViewport(D3DVIEWPORT8 *value)
    { if (value == NULL) return E_INVALIDARG; *value = viewport; return S_OK; }
    HRESULT SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
    {
#if defined(PSP)
        th08::psp::RenderPerfNoteStateRequested();
#endif
        if (static_cast<UINT>(state) < 256)
            renderStates[state] = value;
        return S_OK;
    }
    HRESULT SetTexture(DWORD stage, IDirect3DTexture8 *value)
    {
#if defined(PSP)
        th08::psp::RenderPerfNoteStateRequested();
#endif
        if (stage != 0) return S_OK;
        LinuxTexture *next = static_cast<LinuxTexture *>(value);
        if (next != NULL) next->AddRef();
        if (texture != NULL)
            texture->Release();
        texture = next;
        return S_OK;
    }
    HRESULT SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE state, DWORD value)
    {
#if defined(PSP)
        th08::psp::RenderPerfNoteStateRequested();
#endif
        if (stage == 0 && static_cast<UINT>(state) < 32)
            textureStates[state] = value;
        return S_OK;
    }
    HRESULT SetVertexShader(DWORD value)
    {
#if defined(PSP)
        th08::psp::RenderPerfNoteStateRequested();
#endif
        fvf = value; return S_OK;
    }
    HRESULT SetStreamSource(UINT stream, IDirect3DVertexBuffer8 *value, UINT stride)
    {
#if defined(PSP)
        th08::psp::RenderPerfNoteStateRequested();
#endif
        if (stream != 0) return E_INVALIDARG;
        LinuxVertexBuffer *next = static_cast<LinuxVertexBuffer *>(value);
        if (next != NULL) next->AddRef();
        if (vertexBuffer != NULL) vertexBuffer->Release();
        vertexBuffer = next; streamStride = stride; return S_OK;
    }
    HRESULT DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount)
    {
        if (vertexBuffer == NULL || streamStride == 0) return E_FAIL;
        UINT offset = startVertex * streamStride, count = VertexCount(type, primitiveCount);
        if (offset + count * streamStride > vertexBuffer->bytes.size()) return E_INVALIDARG;
        return Draw(type, primitiveCount, &vertexBuffer->bytes[offset], streamStride);
    }
    HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primitiveCount, const void *vertices, UINT stride)
    { return vertices != NULL ? Draw(type, primitiveCount, static_cast<const BYTE *>(vertices), stride) : E_INVALIDARG; }
    HRESULT DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minVertexIndex,
                                   UINT numVertexIndices, UINT primitiveCount,
                                   const void *indices, D3DFORMAT indexFormat,
                                   const void *vertices, UINT stride)
    {
        if (indices == NULL || vertices == NULL || stride == 0)
            return E_INVALIDARG;
        return DrawIndexed(type, minVertexIndex, numVertexIndices,
                           primitiveCount, indices, indexFormat,
                           static_cast<const BYTE *>(vertices), stride);
    }
    HRESULT GetDeviceCaps(D3DCAPS8 *caps)
    {
        if (caps == NULL) return E_INVALIDARG;
        memset(caps, 0, sizeof(*caps)); caps->DeviceType = D3DDEVTYPE_HAL;
        caps->Caps2 = D3DCAPS2_CANRENDERWINDOWED;
        caps->PresentationIntervals = D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_IMMEDIATE;
        caps->DevCaps = D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_HWRASTERIZATION |
                        D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                        D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TLVERTEXVIDEOMEMORY;
        caps->MaxTextureWidth = caps->MaxTextureHeight = 4096;
        caps->MaxTextureBlendStages = 1; caps->MaxSimultaneousTextures = 1;
        caps->MaxPrimitiveCount = 0x100000; caps->MaxStreams = 1; caps->MaxStreamStride = 256;
        caps->TextureOpCaps = D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_SELECTARG1;
        return S_OK;
    }
    HRESULT ResourceManagerDiscardBytes(DWORD) { return S_OK; }

    bool BeginFramebufferProbe(int left, int top, int right, int bottom)
    {
#if defined(PSP)
        (void)left; (void)top; (void)right; (void)bottom;
        return false;
#else
        if (backbuffer == NULL || left >= right || top >= bottom)
            return false;
        if (left < 0) left = 0;
        if (top < 0) top = 0;
        if (right > static_cast<int>(backbuffer->width)) right = backbuffer->width;
        if (bottom > static_cast<int>(backbuffer->height)) bottom = backbuffer->height;
        if (left >= right || top >= bottom)
            return false;

        probeLeft = left;
        probeBottom = static_cast<int>(backbuffer->height) - bottom;
        probeWidth = right - left;
        probeHeight = bottom - top;
        probePixels.resize(probeWidth * probeHeight * 4);
        glReadPixels(probeLeft, probeBottom, probeWidth, probeHeight, GL_RGBA,
                     GL_UNSIGNED_BYTE, probePixels.empty() ? NULL : &probePixels[0]);
        return !probePixels.empty();
#endif
    }

    bool EndFramebufferProbe(LinuxFramebufferDeltaStats *stats)
    {
#if defined(PSP)
        (void)stats;
        return false;
#else
        if (stats == NULL || probePixels.empty() || probeWidth <= 0 || probeHeight <= 0)
            return false;

        std::vector<BYTE> after(probePixels.size());
        glReadPixels(probeLeft, probeBottom, probeWidth, probeHeight, GL_RGBA,
                     GL_UNSIGNED_BYTE, after.empty() ? NULL : &after[0]);
        memset(stats, 0, sizeof(*stats));
        stats->sampledPixels = probeWidth * probeHeight;
        for (UINT index = 0; index < stats->sampledPixels; ++index)
        {
            const BYTE *beforePixel = &probePixels[index * 4];
            const BYTE *afterPixel = &after[index * 4];
            const int redDifference = abs(static_cast<int>(afterPixel[0]) - beforePixel[0]);
            const int greenDifference = abs(static_cast<int>(afterPixel[1]) - beforePixel[1]);
            const int blueDifference = abs(static_cast<int>(afterPixel[2]) - beforePixel[2]);
            const int alphaDifference = abs(static_cast<int>(afterPixel[3]) - beforePixel[3]);
            if (redDifference == 0 && greenDifference == 0 && blueDifference == 0 && alphaDifference == 0)
                continue;

            ++stats->changedPixels;
            stats->absoluteRgbDifference += redDifference + greenDifference + blueDifference;
            const BYTE maximum = afterPixel[0] > afterPixel[1]
                ? (afterPixel[0] > afterPixel[2] ? afterPixel[0] : afterPixel[2])
                : (afterPixel[1] > afterPixel[2] ? afterPixel[1] : afterPixel[2]);
            const BYTE minimum = afterPixel[0] < afterPixel[1]
                ? (afterPixel[0] < afterPixel[2] ? afterPixel[0] : afterPixel[2])
                : (afterPixel[1] < afterPixel[2] ? afterPixel[1] : afterPixel[2]);
            if (maximum - minimum >= 32)
                ++stats->colorfulChangedPixels;
            const int maximumDifference = redDifference > greenDifference
                ? (redDifference > blueDifference ? redDifference : blueDifference)
                : (greenDifference > blueDifference ? greenDifference : blueDifference);
            const int minimumDifference = redDifference < greenDifference
                ? (redDifference < blueDifference ? redDifference : blueDifference)
                : (greenDifference < blueDifference ? greenDifference : blueDifference);
            if (maximumDifference - minimumDifference >= 8)
                ++stats->chromaticChangedPixels;
            if (afterPixel[0] >= 240 && afterPixel[1] >= 240 && afterPixel[2] >= 240)
                ++stats->nearWhiteChangedPixels;
        }
        probePixels.clear();
        probeWidth = probeHeight = 0;
        return true;
#endif
    }

#if TH08_PSP_ITEM_DIRECT_GE_ENABLED
    bool ReleasePspItemDirectGeStageArena()
    {
        // No Item draw owner survives GameManager's reverse chain teardown.
        // Fence any vertices submitted by the preceding frame before making
        // their Main-RAM storage available to the following stage loader.
        if (g_PspItemDirectGeBatchActive)
        {
            pspItemDirectGeFaulted = true;
            th08::psp::BootLog(
                "ITEM_DIRECT_GE stage_release=blocked reason=active_batch\n");
            return false;
        }
        if (pspItemDirectGeVertices != NULL)
        {
            glFinish();
            const th08::psp::RenderResourceArenaFreeResult freeResult =
                th08::psp::RenderResourceArenaTryFree(
                    pspItemDirectGeVertices);
            th08::psp::BootLog(
                "ITEM_DIRECT_GE stage_release=%lu bytes=%lu "
                "next_allocation=lazy\n",
                static_cast<unsigned long>(freeResult),
                static_cast<unsigned long>(kPspItemDirectGeArenaBytes));
            if (freeResult !=
                th08::psp::RenderResourceArenaFreeResult::Freed)
            {
                pspItemDirectGeFaulted = true;
                // The pointer can only originate from the render arena. Keep
                // a NotOwned pointer visible for destructor diagnostics. A
                // quarantined pointer was consumed by the arena, but neither
                // result may rearm this optional path after an ownership fault.
                if (freeResult ==
                    th08::psp::RenderResourceArenaFreeResult::Quarantined)
                {
                    pspItemDirectGeVertices = NULL;
                }
                return false;
            }
            pspItemDirectGeVertices = NULL;
        }
        if (pspItemDirectGeFaulted)
            return false;
        pspItemDirectGeAllocationAttempted = false;
        pspItemDirectGeVertexCursor = 0U;
        pspItemDirectGeArenaPresent = ~0UL;
        return true;
    }
#endif

  private:
    HRESULT CreateSurface(UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8 **result)
    {
        if (result == NULL || width == 0 || height == 0) return E_INVALIDARG;
        if (format == D3DFMT_UNKNOWN) format = D3DFMT_A8R8G8B8;
        *result = new(std::nothrow) LinuxSurface(width, height, format, false, NULL);
#if defined(PSP)
        if (*result != NULL && static_cast<LinuxSurface *>(*result)->pixels.empty())
        {
            (*result)->Release();
            *result = NULL;
            return E_OUTOFMEMORY;
        }
#endif
        return *result != NULL ? S_OK : E_OUTOFMEMORY;
    }
    void DestroyRenderTarget()
    {
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(&pspPrepareStateCache);
#endif
        ReleaseDialogueSnapshot();
        if (dialogueSnapshotTexture != 0)
            glDeleteTextures(1, &dialogueSnapshotTexture);
        if (renderDepthBuffer != 0 && g_framebufferApi.deleteRenderbuffers != NULL)
            g_framebufferApi.deleteRenderbuffers(1, &renderDepthBuffer);
        if (renderFramebuffer != 0 && g_framebufferApi.deleteFramebuffers != NULL)
            g_framebufferApi.deleteFramebuffers(1, &renderFramebuffer);
        if (renderColorTexture != 0)
            glDeleteTextures(1, &renderColorTexture);
        renderDepthBuffer = renderFramebuffer = renderColorTexture = dialogueSnapshotTexture = 0;
        dialogueSnapshotReady = false;
        wasDialogPresent = false;
    }
    bool CreateRenderTarget(UINT width, UINT height)
    {
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(&pspPrepareStateCache);
#endif
#if defined(PSP)
        (void)width;
        (void)height;
        ApplyPspViewport();
        return true;
#else
        if (!g_framebufferApi.Initialize())
        {
            fprintf(stderr, "th08-modern: OpenGL framebuffer objects are unavailable\n");
            return false;
        }

        glGenTextures(1, &renderColorTexture);
        glBindTexture(GL_TEXTURE_2D, renderColorTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

        glGenTextures(1, &dialogueSnapshotTexture);
        glBindTexture(GL_TEXTURE_2D, dialogueSnapshotTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

        g_framebufferApi.genRenderbuffers(1, &renderDepthBuffer);
        g_framebufferApi.bindRenderbuffer(GL_RENDERBUFFER, renderDepthBuffer);
        g_framebufferApi.renderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);

        g_framebufferApi.genFramebuffers(1, &renderFramebuffer);
        g_framebufferApi.bindFramebuffer(GL_FRAMEBUFFER, renderFramebuffer);
        g_framebufferApi.framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                              GL_TEXTURE_2D, renderColorTexture, 0);
        g_framebufferApi.framebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                                 GL_RENDERBUFFER, renderDepthBuffer);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        GLenum status = g_framebufferApi.checkFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            fprintf(stderr, "th08-modern: unable to create OpenGL framebuffer (status 0x%04x)\n",
                    static_cast<unsigned int>(status));
            DestroyRenderTarget();
            return false;
        }
        return true;
#endif
    }
    void CaptureDialogueSnapshot()
    {
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(&pspPrepareStateCache);
#endif
#if defined(PSP)
        ReleaseDialogueSnapshot();
        dialogueSnapshotReady = false;
        dialogueRestoreFailureLogged = false;
        dialogueRestoresThisCapture = 0;
        ++dialogueCaptureAttempts;
        if (backbuffer == NULL)
        {
            ++dialogueCaptureFailures;
            th08::psp::BootLog(
                "DIALOGUE_CAPTURE ready=0 attempt=%lu reason=backbuffer_missing "
                "late_allocations=%lu\n",
                dialogueCaptureAttempts, dialogueLateAllocationCount);
            th08::psp::MemoryTelemetryMarkPhase("dialogue_capture_failed");
            return;
        }
        const bool scratchReserved =
            th08::psp::AnmScratchReserveTransition("dialogue snapshot");
        const bool scratchBusyAfterReserve = th08::psp::AnmScratchBusy();
        const bool scratchPoisonedAfterReserve =
            th08::psp::AnmScratchPoisoned();
        const bool useArenaFallback =
            !scratchReserved && scratchBusyAfterReserve &&
            !scratchPoisonedAfterReserve;
        const int scratchBusyAtFallback =
            useArenaFallback && scratchBusyAfterReserve ? 1 : 0;
        const unsigned long scratchActiveAtFallback =
            useArenaFallback
                ? static_cast<unsigned long>(
                      th08::psp::AnmScratchActiveBytes())
                : 0;
        const int scratchOwnerAtFallback =
            useArenaFallback ? th08::psp::AnmScratchOwnerIndex() : -1;
        if (!scratchReserved && !useArenaFallback)
        {
            ++dialogueCaptureFailures;
            th08::psp::BootLog(
                "DIALOGUE_CAPTURE ready=0 attempt=%lu "
                "reason=scratch_reserve_failed scratch_busy=%d "
                "scratch_poisoned=%d late_allocations=%lu\n",
                dialogueCaptureAttempts, scratchBusyAfterReserve ? 1 : 0,
                scratchPoisonedAfterReserve ? 1 : 0,
                dialogueLateAllocationCount);
            th08::psp::MemoryTelemetryMarkPhase("dialogue_capture_failed");
            return;
        }
        dialogueCaptureUsedArenaFallback = false;
        const th08::psp::RenderResourceArenaSnapshot before =
            th08::psp::CaptureRenderResourceArenaSnapshot();
        const struct mallinfo heapBefore = mallinfo();
        IDirect3DSurface8 *snapshot = NULL;
        bool captured = false;
        const char *captureFailureReason = NULL;
        th08::psp::RenderResourceArenaSnapshot workspaceAllocated = before;
        th08::psp::RenderResourceArenaSnapshot nested = before;
        th08::psp::RenderResourceArenaFreeResult tempFreeResult =
            th08::psp::RenderResourceArenaFreeResult::NotOwned;

#if TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE_ENABLED
        // Covers the native capture texture and the arena fallback below.
        pspSuppressStaticUploadPromotion = true;
#endif
        if (!useArenaFallback)
        {
            captured = th08_linux_surface_capture_native(
                backbuffer->width, backbuffer->height, true, &snapshot);
            if (!captured)
                captureFailureReason = "native_capture_failed";
        }
        else
        {
            // front.anm owns the retained ANM scratch during initial GUI
            // construction.  Borrow one short-lived window from the retained
            // renderer arena instead; the pixels and conversion algorithm are
            // identical and the workspace is returned before publication.
            void *workspace = th08::psp::RenderResourceArenaAllocate(
                kPspNativeCaptureWorkspaceBytes, 64,
                "dialogue capture workspace");
            workspaceAllocated =
                th08::psp::CaptureRenderResourceArenaSnapshot();
            LinuxSurface *fallbackSurface = NULL;
            if (workspace == NULL)
            {
                captureFailureReason = "fallback_workspace_failed";
            }
            else
            {
                fallbackSurface = new(std::nothrow) LinuxSurface(
                    backbuffer->width, backbuffer->height, D3DFMT_R5G6B5,
                    false, NULL, false);
                if (fallbackSurface == NULL)
                {
                    captureFailureReason = "fallback_surface_failed";
                }
                else
                {
                    captured =
                        fallbackSurface->CaptureNativeFramebufferFromWorkspace(
                            true, workspace,
                            kPspNativeCaptureWorkspaceBytes,
                            &captureFailureReason);
                }
                nested = th08::psp::CaptureRenderResourceArenaSnapshot();
                tempFreeResult =
                    th08::psp::RenderResourceArenaTryFree(workspace);
                if (tempFreeResult !=
                    th08::psp::RenderResourceArenaFreeResult::Freed)
                {
                    captureFailureReason = "fallback_workspace_free_failed";
                    captured = false;
                }

                if (!captured && fallbackSurface != NULL)
                {
                    // This is an error-only fence.  A successful capture keeps
                    // the PSPGL backing alive and never fences at the edge.
                    glFinish();
                    fallbackSurface->Release();
                    fallbackSurface = NULL;
                }
                if (captured)
                    snapshot = fallbackSurface;
            }
        }
        const struct mallinfo heapAfter = mallinfo();
        const th08::psp::RenderResourceArenaSnapshot after =
            th08::psp::CaptureRenderResourceArenaSnapshot();
        const long heapUsedDelta =
            static_cast<long>(heapAfter.uordblks) -
            static_cast<long>(heapBefore.uordblks);
        const long backingLiveDelta =
            static_cast<long>(after.liveBytes) -
            static_cast<long>(before.liveBytes);
        if (useArenaFallback)
        {
            th08::psp::BootLog(
                "DIALOGUE_FALLBACK attempt=%lu path=render_arena_fallback "
                "scratch_busy=%d scratch_active=%lu scratch_owner=%d "
                "workspace_requested=%lu "
                "before_live=%lu before_largest=%lu "
                "workspace_live=%lu workspace_largest=%lu "
                "nested_live=%lu nested_largest=%lu "
                "after_live=%lu after_largest=%lu temp_free_result=%lu "
                "backing_live_delta=%ld heap_delta=%ld "
                "scope_contention=%lu->%lu capture_ready=%d reason=%s\n",
                dialogueCaptureAttempts,
                scratchBusyAtFallback, scratchActiveAtFallback,
                scratchOwnerAtFallback,
                static_cast<unsigned long>(kPspNativeCaptureWorkspaceBytes),
                static_cast<unsigned long>(before.liveBytes),
                static_cast<unsigned long>(before.largestFreeBytes),
                static_cast<unsigned long>(workspaceAllocated.liveBytes),
                static_cast<unsigned long>(workspaceAllocated.largestFreeBytes),
                static_cast<unsigned long>(nested.liveBytes),
                static_cast<unsigned long>(nested.largestFreeBytes),
                static_cast<unsigned long>(after.liveBytes),
                static_cast<unsigned long>(after.largestFreeBytes),
                static_cast<unsigned long>(tempFreeResult),
                backingLiveDelta, heapUsedDelta,
                static_cast<unsigned long>(before.scopeContentionCount),
                static_cast<unsigned long>(after.scopeContentionCount),
                captured && snapshot != NULL ? 1 : 0,
                captureFailureReason != NULL ? captureFailureReason : "none");
        }
        const unsigned long lateAllocations =
            after.allocationCount >= before.allocationCount
                ? static_cast<unsigned long>(after.allocationCount - before.allocationCount)
                : 0;
        const unsigned long lateHeapBytes = heapUsedDelta > 0
            ? static_cast<unsigned long>(heapUsedDelta)
            : 0;
        dialogueLateAllocationCount += lateAllocations;
        dialogueLateHeapBytes += lateHeapBytes;
        if (lateAllocations != 0 || lateHeapBytes != 0 ||
            after.failureCount != before.failureCount)
        {
            th08::psp::BootLog(
                "DIALOGUE_LATE_ALLOCATION attempt=%lu allocations=%lu "
                "total=%lu heap_bytes=%lu heap_total=%lu "
                "arena_failures=%lu->%lu\n",
                dialogueCaptureAttempts, lateAllocations,
                dialogueLateAllocationCount, lateHeapBytes,
                dialogueLateHeapBytes,
                static_cast<unsigned long>(before.failureCount),
                static_cast<unsigned long>(after.failureCount));
        }
#if TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE_ENABLED
        pspSuppressStaticUploadPromotion = false;
#endif
        if (!captured || snapshot == NULL)
        {
            ++dialogueCaptureFailures;
            th08::psp::BootLog(
                "DIALOGUE_CAPTURE ready=0 attempt=%lu "
                "path=%s reason=%s late_allocations=%lu "
                "late_heap_bytes=%lu arena_live=%lu arena_largest=%lu\n",
                dialogueCaptureAttempts,
                useArenaFallback ? "render_arena_fallback" : "anm_scratch",
                captureFailureReason != NULL
                    ? captureFailureReason
                    : "native_capture_failed",
                lateAllocations, lateHeapBytes,
                static_cast<unsigned long>(after.liveBytes),
                static_cast<unsigned long>(after.largestFreeBytes));
            th08::psp::MemoryTelemetryMarkPhase("dialogue_capture_failed");
            return;
        }

        dialogueSnapshotSurface = static_cast<LinuxSurface *>(snapshot);
        dialogueSnapshotReady = true;
        dialogueCaptureUsedArenaFallback = useArenaFallback;
        dialogueCapturePresent = presentCount;
        ++dialogueCaptureSuccesses;
        th08::psp::BootLog(
            "DIALOGUE_CAPTURE ready=1 attempt=%lu success=%lu failures=%lu "
            "path=%s backing=late_lifetime refs=%lu capture_present=%lu "
            "late_allocations=%lu late_total=%lu late_heap_bytes=%lu "
            "late_heap_total=%lu "
            "arena_live=%lu arena_largest=%lu\n",
            dialogueCaptureAttempts, dialogueCaptureSuccesses,
            dialogueCaptureFailures,
            useArenaFallback ? "render_arena_fallback" : "anm_scratch",
            static_cast<unsigned long>(dialogueSnapshotSurface->refs),
            dialogueCapturePresent,
            lateAllocations, dialogueLateAllocationCount, lateHeapBytes,
            dialogueLateHeapBytes,
            static_cast<unsigned long>(after.liveBytes),
            static_cast<unsigned long>(after.largestFreeBytes));
        // Do not force the stdio-backed memory snapshot on the successful
        // dialogue edge: that sink may allocate in newlib. BootLog is a fixed
        // buffer, while the normal periodic telemetry will report the arena
        // counters without adding heap traffic to capture.
#else
        if (dialogueSnapshotTexture == 0 || backbuffer == NULL)
            return;
        glBindTexture(GL_TEXTURE_2D, dialogueSnapshotTexture);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, backbuffer->width, backbuffer->height);
        dialogueSnapshotReady = true;
#endif
    }
    void RestoreDialogueSnapshot()
    {
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(&pspPrepareStateCache);
#endif
#if defined(PSP)
        if (!dialogueSnapshotReady || dialogueSnapshotSurface == NULL || backbuffer == NULL)
            return;
        RECT sourceRect;
        sourceRect.left = 0;
        sourceRect.top = 0;
        sourceRect.right = static_cast<LONG>(dialogueSnapshotSurface->width);
        sourceRect.bottom = static_cast<LONG>(dialogueSnapshotSurface->height);
        POINT destinationPoint;
        destinationPoint.x = 0;
        destinationPoint.y = 0;
#if TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE_ENABLED
        // The snapshot is rewritten every conversation: keep it out of the
        // GE-only upper tier (R-045).
        pspSuppressStaticUploadPromotion = true;
#endif
#if TH08_PSP_DIALOGUE_SNAPSHOT_DIAG_ENABLED
        pspDialogueRestoreDiagFlash =
            dialogueRestoresThisCapture < 120U && (dialogueRestoresThisCapture % 30U) < 4U;
#endif
        const bool snapshotDrawn =
            DrawPspSurfaceCache(dialogueSnapshotSurface, sourceRect, destinationPoint);
#if TH08_PSP_DIALOGUE_SNAPSHOT_DIAG_ENABLED
        pspDialogueRestoreDiagFlash = false;
#endif
#if TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE_ENABLED
        pspSuppressStaticUploadPromotion = false;
#endif
        if (!snapshotDrawn)
        {
            ++dialogueRestoreFailures;
            if (!dialogueRestoreFailureLogged)
            {
                dialogueRestoreFailureLogged = true;
                th08::psp::BootLog(
                    "DIALOGUE_RESTORE ready=0 reason=draw_failed "
                    "capture=%lu restore_failures=%lu\n",
                    dialogueCaptureSuccesses, dialogueRestoreFailures);
                th08::psp::MemoryTelemetryMarkPhase("dialogue_restore_failed");
            }
        }
        else
        {
            ++dialogueRestoreCount;
            ++dialogueRestoresThisCapture;
        }
        return;
#else
        const UINT width = backbuffer->width;
        const UINT height = backbuffer->height;
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glDisable(GL_ALPHA_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING); glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, dialogueSnapshotTexture);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(0.0, width, height, 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glColor4ub(255, 255, 255, 255);
        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f, 0.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(static_cast<float>(width), 0.0f);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, static_cast<float>(height));
        glTexCoord2f(1.0f, 0.0f); glVertex2f(static_cast<float>(width), static_cast<float>(height));
        glEnd();
        glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW);
        glPopAttrib();
#endif
    }
    bool ResetInternal(const D3DPRESENT_PARAMETERS &parameters)
    {
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(&pspPrepareStateCache);
#endif
        UINT width = parameters.BackBufferWidth != 0 ? parameters.BackBufferWidth : 640;
        UINT height = parameters.BackBufferHeight != 0 ? parameters.BackBufferHeight : 480;
        D3DFORMAT format = parameters.BackBufferFormat;
        if (format == D3DFMT_UNKNOWN) format = D3DFMT_X8R8G8B8;
        DestroyRenderTarget();
        if (backbuffer != NULL) backbuffer->Release();
        backbuffer = new LinuxSurface(width, height, format, true, NULL);
        if (backbuffer == NULL || !CreateRenderTarget(width, height)) return false;
        viewport.X = viewport.Y = 0; viewport.Width = width; viewport.Height = height;
        viewport.MinZ = 0.0f; viewport.MaxZ = 1.0f;
#if defined(PSP)
        ApplyPspViewport();
#else
        glViewport(0, 0, width, height);
#endif
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClearDepth(1.0);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        return true;
    }
    void TransformPosition(const float *position, bool transformed, float *xOut, float *yOut,
                           float *zOut, float *fogCoordinateOut)
    {
        if (transformed)
        {
            // D3D8 pre-transformed vertices use integer pixel centers, while
            // OpenGL samples at half-integer centers.
            *xOut = position[0] + 0.5f; *yOut = position[1] + 0.5f; *zOut = position[2];
            *fogCoordinateOut = 0.0f;
            return;
        }
        float vector[4] = {position[0], position[1], position[2], 1.0f};
        const D3DMATRIX *matrices[3] = {&world, &view, &projection};
        for (int index = 0; index < 3; ++index)
        {
            const D3DMATRIX &m = *matrices[index]; float next[4];
            next[0] = vector[0] * m._11 + vector[1] * m._21 + vector[2] * m._31 + vector[3] * m._41;
            next[1] = vector[0] * m._12 + vector[1] * m._22 + vector[2] * m._32 + vector[3] * m._42;
            next[2] = vector[0] * m._13 + vector[1] * m._23 + vector[2] * m._33 + vector[3] * m._43;
            next[3] = vector[0] * m._14 + vector[1] * m._24 + vector[2] * m._34 + vector[3] * m._44;
            memcpy(vector, next, sizeof(vector));
            if (index == 1)
                *fogCoordinateOut = fabsf(vector[2]);
        }
        float reciprocal = fabsf(vector[3]) > 1.0e-8f ? 1.0f / vector[3] : 1.0f;
        *xOut = viewport.X + (vector[0] * reciprocal + 1.0f) * viewport.Width * 0.5f;
        *yOut = viewport.Y + (1.0f - vector[1] * reciprocal) * viewport.Height * 0.5f;
        *zOut = viewport.MinZ + vector[2] * reciprocal * (viewport.MaxZ - viewport.MinZ);
    }
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
    void PrepareStateCachedPsp(bool transformed)
    {
        const UINT width = backbuffer != NULL ? backbuffer->width : viewport.Width;
        const UINT height = backbuffer != NULL ? backbuffer->height : viewport.Height;
        UINT pspStateEmitted = 0;

        ApplyPspCachedCapability(GL_SCISSOR_TEST, true,
                                 &pspPrepareStateCache.scissorEnableValid,
                                 &pspPrepareStateCache.scissorEnabled,
                                 &pspStateEmitted);
        const PspPhysicalRect scissor = MakePspPhysicalRect(
            static_cast<int>(viewport.X), static_cast<int>(viewport.Y),
            static_cast<int>(viewport.X + viewport.Width),
            static_cast<int>(viewport.Y + viewport.Height), width, height);
        ApplyPspCachedScissor(&pspPrepareStateCache, scissor.left,
                              scissor.bottom, scissor.width, scissor.height,
                              &pspStateEmitted);

        if (transformed)
        {
            ApplyPspCachedViewport(&pspPrepareStateCache, kPspFitLeft, 0,
                                   kPspFitWidth, kPspScreenHeight,
                                   &pspStateEmitted);

            const bool projectionMatches =
                pspPrepareStateCache.projectionValid &&
                pspPrepareStateCache.projectionTransformed &&
                pspPrepareStateCache.projectionWidth == width &&
                pspPrepareStateCache.projectionHeight == height;
            if (!projectionMatches)
            {
                ApplyPspCachedMatrixMode(&pspPrepareStateCache, GL_PROJECTION,
                                         &pspStateEmitted);
                glLoadIdentity();
                glOrtho(0.0, width, height, 0.0, -1.0, 1.0);
                pspStateEmitted += 2;
                pspPrepareStateCache.projectionValid = true;
                pspPrepareStateCache.projectionTransformed = true;
                pspPrepareStateCache.projectionWidth = width;
                pspPrepareStateCache.projectionHeight = height;
            }

            const bool modelViewMatches =
                pspPrepareStateCache.modelViewValid &&
                pspPrepareStateCache.modelViewTransformed;
            if (!modelViewMatches)
            {
                ApplyPspCachedMatrixMode(&pspPrepareStateCache, GL_MODELVIEW,
                                         &pspStateEmitted);
                glLoadIdentity();
                ++pspStateEmitted;
                pspPrepareStateCache.modelViewValid = true;
                pspPrepareStateCache.modelViewTransformed = true;
            }
        }
        else
        {
            ApplyPspCachedViewport(&pspPrepareStateCache, scissor.left,
                                   scissor.bottom, scissor.width,
                                   scissor.height, &pspStateEmitted);

            const bool projectionMatches =
                pspPrepareStateCache.projectionValid &&
                !pspPrepareStateCache.projectionTransformed &&
                pspPrepareStateCache.projectionRawVersion ==
                    pspProjectionRawVersion;
            const bool modelViewMatches =
                pspPrepareStateCache.modelViewValid &&
                !pspPrepareStateCache.modelViewTransformed &&
                pspPrepareStateCache.worldRawVersion == pspWorldRawVersion &&
                pspPrepareStateCache.viewRawVersion == pspViewRawVersion;

            D3DMATRIX modelView;
            if (!modelViewMatches)
            {
                const float *worldValues = reinterpret_cast<const float *>(&world);
                const float *viewValues = reinterpret_cast<const float *>(&view);
                float *modelViewValues = reinterpret_cast<float *>(&modelView);
                for (int row = 0; row < 4; ++row)
                {
                    for (int column = 0; column < 4; ++column)
                    {
                        float value = 0.0f;
                        for (int inner = 0; inner < 4; ++inner)
                            value += worldValues[row * 4 + inner] *
                                     viewValues[inner * 4 + column];
                        modelViewValues[row * 4 + column] = value;
                    }
                }
                th08::psp::RenderPerfNoteMatrixRecompute();
            }

            D3DMATRIX glProjection;
            if (!projectionMatches)
            {
                glProjection = projection;
                const float *projectionValues =
                    reinterpret_cast<const float *>(&projection);
                float *glProjectionValues =
                    reinterpret_cast<float *>(&glProjection);
                for (int row = 0; row < 4; ++row)
                {
                    glProjectionValues[row * 4 + 2] =
                        projectionValues[row * 4 + 2] * 2.0f -
                        projectionValues[row * 4 + 3];
                    glProjectionValues[row * 4 + 3] =
                        projectionValues[row * 4 + 3];
                }
                th08::psp::RenderPerfNoteMatrixRecompute();
            }

            if (!projectionMatches)
            {
                ApplyPspCachedMatrixMode(&pspPrepareStateCache, GL_PROJECTION,
                                         &pspStateEmitted);
                glLoadMatrixf(reinterpret_cast<const GLfloat *>(&glProjection));
                ++pspStateEmitted;
                pspPrepareStateCache.projectionValid = true;
                pspPrepareStateCache.projectionTransformed = false;
                pspPrepareStateCache.projectionRawVersion =
                    pspProjectionRawVersion;
            }
            if (!modelViewMatches)
            {
                ApplyPspCachedMatrixMode(&pspPrepareStateCache, GL_MODELVIEW,
                                         &pspStateEmitted);
                glLoadMatrixf(reinterpret_cast<const GLfloat *>(&modelView));
                ++pspStateEmitted;
                pspPrepareStateCache.modelViewValid = true;
                pspPrepareStateCache.modelViewTransformed = false;
                pspPrepareStateCache.worldRawVersion = pspWorldRawVersion;
                pspPrepareStateCache.viewRawVersion = pspViewRawVersion;
            }
        }
        // A projection-only change leaves GL_PROJECTION current unless this
        // invariant is restored.  Raw-state boundaries invalidate the mode.
        ApplyPspCachedMatrixMode(&pspPrepareStateCache, GL_MODELVIEW,
                                 &pspStateEmitted);

        const bool blendEnabled =
            renderStates[D3DRS_ALPHABLENDENABLE] != 0;
        ApplyPspCachedCapability(GL_BLEND, blendEnabled,
                                 &pspPrepareStateCache.blendEnableValid,
                                 &pspPrepareStateCache.blendEnabled,
                                 &pspStateEmitted);
        if (blendEnabled)
        {
            const GLenum source =
                BlendFunction(renderStates[D3DRS_SRCBLEND]);
            const GLenum destination =
                BlendFunction(renderStates[D3DRS_DESTBLEND]);
            if (!pspPrepareStateCache.blendFunctionValid ||
                pspPrepareStateCache.blendSource != source ||
                pspPrepareStateCache.blendDestination != destination)
            {
                glBlendFunc(source, destination);
                pspPrepareStateCache.blendFunctionValid = true;
                pspPrepareStateCache.blendSource = source;
                pspPrepareStateCache.blendDestination = destination;
                ++pspStateEmitted;
            }
        }

        const bool alphaEnabled =
            renderStates[D3DRS_ALPHATESTENABLE] != 0;
        ApplyPspCachedCapability(GL_ALPHA_TEST, alphaEnabled,
                                 &pspPrepareStateCache.alphaEnableValid,
                                 &pspPrepareStateCache.alphaEnabled,
                                 &pspStateEmitted);
        if (alphaEnabled)
        {
            const GLenum function =
                CompareFunction(renderStates[D3DRS_ALPHAFUNC]);
            const DWORD reference = renderStates[D3DRS_ALPHAREF] & 255;
            if (!pspPrepareStateCache.alphaFunctionValid ||
                pspPrepareStateCache.alphaFunction != function ||
                pspPrepareStateCache.alphaReference != reference)
            {
                glAlphaFunc(function,
                            static_cast<float>(reference) / 255.0f);
                pspPrepareStateCache.alphaFunctionValid = true;
                pspPrepareStateCache.alphaFunction = function;
                pspPrepareStateCache.alphaReference = reference;
                ++pspStateEmitted;
            }
        }

        const bool depthEnabled = renderStates[D3DRS_ZENABLE] != 0;
        ApplyPspCachedCapability(GL_DEPTH_TEST, depthEnabled,
                                 &pspPrepareStateCache.depthEnableValid,
                                 &pspPrepareStateCache.depthEnabled,
                                 &pspStateEmitted);
        if (depthEnabled)
        {
            const GLenum function = CompareFunction(renderStates[D3DRS_ZFUNC]);
            if (!pspPrepareStateCache.depthFunctionValid ||
                pspPrepareStateCache.depthFunction != function)
            {
                glDepthFunc(function);
                pspPrepareStateCache.depthFunctionValid = true;
                pspPrepareStateCache.depthFunction = function;
                ++pspStateEmitted;
            }
        }
        const bool depthMask = renderStates[D3DRS_ZWRITEENABLE] != 0;
        if (!pspPrepareStateCache.depthMaskValid ||
            pspPrepareStateCache.depthMask != depthMask)
        {
            glDepthMask(depthMask ? GL_TRUE : GL_FALSE);
            pspPrepareStateCache.depthMaskValid = true;
            pspPrepareStateCache.depthMask = depthMask;
            ++pspStateEmitted;
        }

        GLfloat fogStart;
        GLfloat fogEnd;
        memcpy(&fogStart, &renderStates[D3DRS_FOGSTART], sizeof(fogStart));
        memcpy(&fogEnd, &renderStates[D3DRS_FOGEND], sizeof(fogEnd));
        const bool fogEnabled =
            !transformed && renderStates[D3DRS_FOGENABLE] &&
            renderStates[D3DRS_FOGVERTEXMODE] == D3DFOG_LINEAR &&
            fogEnd > fogStart;
        ApplyPspCachedCapability(GL_FOG, fogEnabled,
                                 &pspPrepareStateCache.fogEnableValid,
                                 &pspPrepareStateCache.fogEnabled,
                                 &pspStateEmitted);
        if (fogEnabled)
        {
            if (!pspPrepareStateCache.fogModeValid)
            {
                glFogi(GL_FOG_MODE, GL_LINEAR);
                pspPrepareStateCache.fogModeValid = true;
                ++pspStateEmitted;
            }
            const DWORD color = renderStates[D3DRS_FOGCOLOR];
            if (!pspPrepareStateCache.fogColorValid ||
                pspPrepareStateCache.fogColor != color)
            {
                const GLfloat fogColor[4] = {
                    static_cast<float>((color >> 16) & 255) / 255.0f,
                    static_cast<float>((color >> 8) & 255) / 255.0f,
                    static_cast<float>(color & 255) / 255.0f,
                    1.0f,
                };
                glFogfv(GL_FOG_COLOR, fogColor);
                pspPrepareStateCache.fogColorValid = true;
                pspPrepareStateCache.fogColor = color;
                ++pspStateEmitted;
            }
            if (!pspPrepareStateCache.fogStartValid ||
                pspPrepareStateCache.fogStart !=
                    renderStates[D3DRS_FOGSTART])
            {
                glFogf(GL_FOG_START, -fogStart);
                pspPrepareStateCache.fogStartValid = true;
                pspPrepareStateCache.fogStart =
                    renderStates[D3DRS_FOGSTART];
                ++pspStateEmitted;
            }
            if (!pspPrepareStateCache.fogEndValid ||
                pspPrepareStateCache.fogEnd != renderStates[D3DRS_FOGEND])
            {
                glFogf(GL_FOG_END, -fogEnd);
                pspPrepareStateCache.fogEndValid = true;
                pspPrepareStateCache.fogEnd = renderStates[D3DRS_FOGEND];
                ++pspStateEmitted;
            }
        }

        const bool colorUsesTexture = TextureOperationUsesTexture(
            textureStates[D3DTSS_COLOROP], textureStates[D3DTSS_COLORARG1],
            textureStates[D3DTSS_COLORARG2]);
        const bool alphaUsesTexture = TextureOperationUsesTexture(
            textureStates[D3DTSS_ALPHAOP], textureStates[D3DTSS_ALPHAARG1],
            textureStates[D3DTSS_ALPHAARG2]);
        const bool textureEnabled =
            texture != NULL && (colorUsesTexture || alphaUsesTexture);
        if (textureEnabled)
        {
            // Upload and bind are intentionally not cached.  PSPGL requires
            // the proven texture -> 0 -> texture edge after a redefinition.
            texture->Upload();
            ApplyPspCachedCapability(GL_TEXTURE_2D, true,
                                     &pspPrepareStateCache.textureEnableValid,
                                     &pspPrepareStateCache.textureEnabled,
                                     &pspStateEmitted);
            glBindTexture(GL_TEXTURE_2D, texture->glName);
            ++pspStateEmitted;

            if (texture->pspSamplerEpoch != pspPrepareStateCache.epoch ||
                texture->pspSamplerGlName != texture->glName)
            {
                texture->pspSamplerEpoch = pspPrepareStateCache.epoch;
                texture->pspSamplerGlName = texture->glName;
                texture->pspSamplerValidMask = 0;
            }
            const GLint minFilter =
                textureStates[D3DTSS_MINFILTER] == D3DTEXF_LINEAR
                    ? GL_LINEAR
                    : GL_NEAREST;
            const GLint magFilter =
                textureStates[D3DTSS_MAGFILTER] == D3DTEXF_LINEAR
                    ? GL_LINEAR
                    : GL_NEAREST;
            const GLint wrapS =
                textureStates[D3DTSS_ADDRESSU] == D3DTADDRESS_CLAMP
                    ? GL_CLAMP
                    : GL_REPEAT;
            const GLint wrapT =
                textureStates[D3DTSS_ADDRESSV] == D3DTADDRESS_CLAMP
                    ? GL_CLAMP
                    : GL_REPEAT;
            if (!(texture->pspSamplerValidMask & 1U) ||
                texture->pspSamplerMinFilter != minFilter)
            {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
                texture->pspSamplerValidMask |= 1U;
                texture->pspSamplerMinFilter = minFilter;
                ++pspStateEmitted;
            }
            if (!(texture->pspSamplerValidMask & 2U) ||
                texture->pspSamplerMagFilter != magFilter)
            {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
                texture->pspSamplerValidMask |= 2U;
                texture->pspSamplerMagFilter = magFilter;
                ++pspStateEmitted;
            }
            if (!(texture->pspSamplerValidMask & 4U) ||
                texture->pspSamplerWrapS != wrapS)
            {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
                texture->pspSamplerValidMask |= 4U;
                texture->pspSamplerWrapS = wrapS;
                ++pspStateEmitted;
            }
            if (!(texture->pspSamplerValidMask & 8U) ||
                texture->pspSamplerWrapT != wrapT)
            {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
                texture->pspSamplerValidMask |= 8U;
                texture->pspSamplerWrapT = wrapT;
                ++pspStateEmitted;
            }
            if (!pspPrepareStateCache.textureEnvironmentValid ||
                pspPrepareStateCache.textureEnvironment != GL_MODULATE)
            {
                glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
                pspPrepareStateCache.textureEnvironmentValid = true;
                pspPrepareStateCache.textureEnvironment = GL_MODULATE;
                ++pspStateEmitted;
            }
        }
        else
        {
            ApplyPspCachedCapability(GL_TEXTURE_2D, false,
                                     &pspPrepareStateCache.textureEnableValid,
                                     &pspPrepareStateCache.textureEnabled,
                                     &pspStateEmitted);
        }
        th08::psp::RenderPerfNoteStateEmitted(pspStateEmitted);
    }
#endif

    void PrepareState(bool transformed)
    {
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PrepareStateCachedPsp(transformed);
        return;
#endif
        const UINT width = backbuffer != NULL ? backbuffer->width : viewport.Width;
        const UINT height = backbuffer != NULL ? backbuffer->height : viewport.Height;
        const int drawableWidth = width;
        const int drawableHeight = height;
#if defined(PSP)
        // Count only GL state/matrix/client-array calls issued by the normal
        // D3D draw path.  The draw submission and texture transfers have
        // separate counters.
        std::uint32_t pspStateEmitted = 1U;
#endif
        glEnable(GL_SCISSOR_TEST);
#if defined(PSP)
        const PspPhysicalRect scissor = MakePspPhysicalRect(
            static_cast<int>(viewport.X), static_cast<int>(viewport.Y),
            static_cast<int>(viewport.X + viewport.Width),
            static_cast<int>(viewport.Y + viewport.Height), width, height);
        glScissor(scissor.left, scissor.bottom, scissor.width, scissor.height);
        ++pspStateEmitted;

        if (transformed)
        {
            ApplyPspViewport();
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0.0, width, height, 0.0, -1.0, 1.0);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            pspStateEmitted += 6U;
        }
        else
        {
            // Preserve homogeneous W and let GE perform the D3D8 3D
            // transform.  CPU projection made texture coordinates affine and
            // destroyed near-plane clipping, which tore TH08's stage walls
            // and floor into zig-zag triangles.
            glViewport(scissor.left, scissor.bottom, scissor.width, scissor.height);

            D3DMATRIX modelView;
            const float *worldValues = reinterpret_cast<const float *>(&world);
            const float *viewValues = reinterpret_cast<const float *>(&view);
            float *modelViewValues = reinterpret_cast<float *>(&modelView);
            for (int row = 0; row < 4; ++row)
            {
                for (int column = 0; column < 4; ++column)
                {
                    float value = 0.0f;
                    for (int inner = 0; inner < 4; ++inner)
                        value += worldValues[row * 4 + inner] *
                                 viewValues[inner * 4 + column];
                    modelViewValues[row * 4 + column] = value;
                }
            }

            // D3D clip Z is [0,W], OpenGL/PSPGL clip Z is [-W,W].  For TH08's
            // row-vector matrices this is projection * C, where C33=2,
            // C43=-1 and C44=1.  glLoadMatrixf interprets the row-major D3D
            // bytes as the required transposed column-vector matrix.
            D3DMATRIX glProjection = projection;
            const float *projectionValues = reinterpret_cast<const float *>(&projection);
            float *glProjectionValues = reinterpret_cast<float *>(&glProjection);
            for (int row = 0; row < 4; ++row)
            {
                glProjectionValues[row * 4 + 2] =
                    projectionValues[row * 4 + 2] * 2.0f -
                    projectionValues[row * 4 + 3];
                glProjectionValues[row * 4 + 3] = projectionValues[row * 4 + 3];
            }

            glMatrixMode(GL_PROJECTION);
            glLoadMatrixf(reinterpret_cast<const GLfloat *>(&glProjection));
            glMatrixMode(GL_MODELVIEW);
            glLoadMatrixf(reinterpret_cast<const GLfloat *>(&modelView));
            pspStateEmitted += 5U;
            th08::psp::RenderPerfNoteMatrixRecompute(2U);
        }
#else
        glScissor(static_cast<int>(viewport.X * drawableWidth / width),
                  drawableHeight - static_cast<int>((viewport.Y + viewport.Height) * drawableHeight / height),
                  static_cast<int>(viewport.Width * drawableWidth / width),
                  static_cast<int>(viewport.Height * drawableHeight / height));
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0.0, width, height, 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
#endif
        if (renderStates[D3DRS_ALPHABLENDENABLE])
        { glEnable(GL_BLEND); glBlendFunc(BlendFunction(renderStates[D3DRS_SRCBLEND]), BlendFunction(renderStates[D3DRS_DESTBLEND])); }
        else glDisable(GL_BLEND);
#if defined(PSP)
        pspStateEmitted += renderStates[D3DRS_ALPHABLENDENABLE] ? 2U : 1U;
#endif
        if (renderStates[D3DRS_ALPHATESTENABLE])
        { glEnable(GL_ALPHA_TEST); glAlphaFunc(CompareFunction(renderStates[D3DRS_ALPHAFUNC]), (renderStates[D3DRS_ALPHAREF] & 255) / 255.0f); }
        else glDisable(GL_ALPHA_TEST);
#if defined(PSP)
        pspStateEmitted += renderStates[D3DRS_ALPHATESTENABLE] ? 2U : 1U;
#endif
        if (renderStates[D3DRS_ZENABLE]) { glEnable(GL_DEPTH_TEST); glDepthFunc(CompareFunction(renderStates[D3DRS_ZFUNC])); }
        else glDisable(GL_DEPTH_TEST);
        glDepthMask(renderStates[D3DRS_ZWRITEENABLE] ? GL_TRUE : GL_FALSE);
#if defined(PSP)
        pspStateEmitted += renderStates[D3DRS_ZENABLE] ? 3U : 2U;
        // GE fog is applied after texturing, matching D3D8's final-pixel fog
        // equation.  The former SC path mixed fog into the diffuse vertex color
        // first and PSPGL multiplied the texture afterwards, which is visibly
        // wrong for every non-black fog color.  Screen-space sprites must never
        // inherit the stage fog state.
        if (!transformed && renderStates[D3DRS_FOGENABLE] &&
            renderStates[D3DRS_FOGVERTEXMODE] == D3DFOG_LINEAR)
        {
            const DWORD color = renderStates[D3DRS_FOGCOLOR];
            const GLfloat fogColor[4] = {
                ((color >> 16) & 255) / 255.0f,
                ((color >> 8) & 255) / 255.0f,
                (color & 255) / 255.0f,
                1.0f,
            };
            GLfloat fogStart;
            GLfloat fogEnd;
            memcpy(&fogStart, &renderStates[D3DRS_FOGSTART], sizeof(fogStart));
            memcpy(&fogEnd, &renderStates[D3DRS_FOGEND], sizeof(fogEnd));
            // D3D8's left-handed view space places visible geometry at +Z.
            // The GE computes fog from (viewZ + end), so its near/far values
            // must be negated to reproduce (end - viewZ) / (end - start).
            // This is also the convention used by the stable TH07 GU backend.
            if (fogEnd > fogStart)
            {
                glEnable(GL_FOG);
                glFogi(GL_FOG_MODE, GL_LINEAR);
                glFogfv(GL_FOG_COLOR, fogColor);
                glFogf(GL_FOG_START, -fogStart);
                glFogf(GL_FOG_END, -fogEnd);
                pspStateEmitted += 5U;
            }
            else
            {
                glDisable(GL_FOG);
                ++pspStateEmitted;
            }
        }
        else
        {
            glDisable(GL_FOG);
            ++pspStateEmitted;
        }
#else
        if (renderStates[D3DRS_FOGENABLE] &&
            renderStates[D3DRS_FOGVERTEXMODE] == D3DFOG_LINEAR && g_fogCoordf != NULL)
        {
            const DWORD color = renderStates[D3DRS_FOGCOLOR];
            const GLfloat fogColor[4] = {
                ((color >> 16) & 255) / 255.0f,
                ((color >> 8) & 255) / 255.0f,
                (color & 255) / 255.0f,
                1.0f
            };
            GLfloat fogStart, fogEnd;
            memcpy(&fogStart, &renderStates[D3DRS_FOGSTART], sizeof(fogStart));
            memcpy(&fogEnd, &renderStates[D3DRS_FOGEND], sizeof(fogEnd));
            glEnable(GL_FOG);
            glFogi(GL_FOG_MODE, GL_LINEAR);
            glFogi(GL_FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE);
            glFogfv(GL_FOG_COLOR, fogColor);
            glFogf(GL_FOG_START, fogStart);
            glFogf(GL_FOG_END, fogEnd);
        }
        else
        {
            glDisable(GL_FOG);
        }
#endif
        const bool colorUsesTexture = TextureOperationUsesTexture(
            textureStates[D3DTSS_COLOROP], textureStates[D3DTSS_COLORARG1],
            textureStates[D3DTSS_COLORARG2]);
        const bool alphaUsesTexture = TextureOperationUsesTexture(
            textureStates[D3DTSS_ALPHAOP], textureStates[D3DTSS_ALPHAARG1],
            textureStates[D3DTSS_ALPHAARG2]);
        if (texture != NULL && (colorUsesTexture || alphaUsesTexture))
        {
            texture->Upload(); glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, texture->glName);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, textureStates[D3DTSS_MINFILTER] == D3DTEXF_LINEAR ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, textureStates[D3DTSS_MAGFILTER] == D3DTEXF_LINEAR ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, textureStates[D3DTSS_ADDRESSU] == D3DTADDRESS_CLAMP ? GL_CLAMP : GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, textureStates[D3DTSS_ADDRESSV] == D3DTADDRESS_CLAMP ? GL_CLAMP : GL_REPEAT);
#if defined(PSP)
            // PSPGL does not implement the desktop GL_COMBINE texture-env
            // family.  Besides being ignored, every unsupported call is
            // synchronously appended to ms0:/log.txt by PSPGL.  TH08 only
            // needs SELECTARG1/MODULATE here, so fold the non-texture operand
            // into the vertex color (EffectiveColor) and let fixed-function
            // GL_MODULATE multiply it by the texture in one supported step.
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
#else
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            ConfigureTextureComponent(GL_COMBINE_RGB, GL_SOURCE0_RGB, GL_SOURCE1_RGB,
                                      GL_OPERAND0_RGB, GL_OPERAND1_RGB,
                                      textureStates[D3DTSS_COLOROP],
                                      textureStates[D3DTSS_COLORARG1],
                                      textureStates[D3DTSS_COLORARG2], GL_SRC_COLOR);
            ConfigureTextureComponent(GL_COMBINE_ALPHA, GL_SOURCE0_ALPHA, GL_SOURCE1_ALPHA,
                                      GL_OPERAND0_ALPHA, GL_OPERAND1_ALPHA,
                                      textureStates[D3DTSS_ALPHAOP],
                                      textureStates[D3DTSS_ALPHAARG1],
                                      textureStates[D3DTSS_ALPHAARG2], GL_SRC_ALPHA);
            const DWORD factor = renderStates[D3DRS_TEXTUREFACTOR];
            const GLfloat constantColor[4] = {
                ((factor >> 16) & 255) / 255.0f,
                ((factor >> 8) & 255) / 255.0f,
                (factor & 255) / 255.0f,
                ((factor >> 24) & 255) / 255.0f
            };
            glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, constantColor);
#endif
        }
        else glDisable(GL_TEXTURE_2D);
#if defined(PSP)
        pspStateEmitted += texture != NULL && (colorUsesTexture || alphaUsesTexture)
                               ? 7U
                               : 1U;
        th08::psp::RenderPerfNoteStateEmitted(pspStateEmitted);
#endif
    }
    D3DCOLOR EffectiveColor(D3DCOLOR diffuse,
                            bool forceDiffuseArg2 = false)
    {
#if defined(PSP)
        // Treat the texture operand as white.  GL_MODULATE supplies the real
        // texture sample after this value becomes the primary vertex color.
        // COLOROP and ALPHAOP are evaluated separately to preserve D3D8's
        // per-component SELECTARG1/MODULATE behavior.
        const DWORD factor = renderStates[D3DRS_TEXTUREFACTOR];
        const DWORD colorArg1 = textureStates[D3DTSS_COLORARG1] & D3DTA_SELECTMASK;
        const DWORD colorArg2 = forceDiffuseArg2
            ? D3DTA_DIFFUSE
            : textureStates[D3DTSS_COLORARG2] & D3DTA_SELECTMASK;
        const DWORD alphaArg1 = textureStates[D3DTSS_ALPHAARG1] & D3DTA_SELECTMASK;
        const DWORD alphaArg2 = forceDiffuseArg2
            ? D3DTA_DIFFUSE
            : textureStates[D3DTSS_ALPHAARG2] & D3DTA_SELECTMASK;
        const DWORD color1 = colorArg1 == D3DTA_TEXTURE ? 0xffffffffu
                             : colorArg1 == D3DTA_TFACTOR ? factor : diffuse;
        const DWORD color2 = colorArg2 == D3DTA_TEXTURE ? 0xffffffffu
                             : colorArg2 == D3DTA_TFACTOR ? factor : diffuse;
        const DWORD alpha1 = alphaArg1 == D3DTA_TEXTURE ? 0xffffffffu
                             : alphaArg1 == D3DTA_TFACTOR ? factor : diffuse;
        const DWORD alpha2 = alphaArg2 == D3DTA_TEXTURE ? 0xffffffffu
                             : alphaArg2 == D3DTA_TFACTOR ? factor : diffuse;

        DWORD rgb;
        if (textureStates[D3DTSS_COLOROP] == D3DTOP_MODULATE)
        {
            const DWORD r = (((color1 >> 16) & 255) * ((color2 >> 16) & 255) + 127) / 255;
            const DWORD g = (((color1 >> 8) & 255) * ((color2 >> 8) & 255) + 127) / 255;
            const DWORD b = ((color1 & 255) * (color2 & 255) + 127) / 255;
            rgb = (r << 16) | (g << 8) | b;
        }
        else
        {
            rgb = color1 & 0x00ffffffu;
        }

        DWORD alpha;
        if (textureStates[D3DTSS_ALPHAOP] == D3DTOP_MODULATE)
        {
            const DWORD a = (((alpha1 >> 24) & 255) * ((alpha2 >> 24) & 255) + 127) / 255;
            alpha = a << 24;
        }
        else
        {
            alpha = alpha1 & 0xff000000u;
        }
        return alpha | rgb;
#else
        (void)forceDiffuseArg2;
        if (texture != NULL &&
            (TextureOperationUsesTexture(textureStates[D3DTSS_COLOROP],
                                         textureStates[D3DTSS_COLORARG1],
                                         textureStates[D3DTSS_COLORARG2]) ||
             TextureOperationUsesTexture(textureStates[D3DTSS_ALPHAOP],
                                         textureStates[D3DTSS_ALPHAARG1],
                                         textureStates[D3DTSS_ALPHAARG2])))
            return diffuse;
        DWORD factor = renderStates[D3DRS_TEXTUREFACTOR];
        DWORD colorOp = textureStates[D3DTSS_COLOROP], alphaOp = textureStates[D3DTSS_ALPHAOP];
        DWORD colorArg1 = textureStates[D3DTSS_COLORARG1] & D3DTA_SELECTMASK;
        DWORD colorArg2 = textureStates[D3DTSS_COLORARG2] & D3DTA_SELECTMASK;
        DWORD alphaArg1 = textureStates[D3DTSS_ALPHAARG1] & D3DTA_SELECTMASK;
        DWORD alphaArg2 = textureStates[D3DTSS_ALPHAARG2] & D3DTA_SELECTMASK;
        DWORD modifier = colorArg2 == D3DTA_TFACTOR ? factor : diffuse;
        DWORD alphaModifier = alphaArg2 == D3DTA_TFACTOR ? factor : diffuse;
        DWORD rgb = modifier & 0x00ffffffu, alpha = alphaModifier & 0xff000000u;
        if (colorOp == D3DTOP_SELECTARG1 && colorArg1 == D3DTA_TEXTURE) rgb = 0x00ffffffu;
        else if (colorOp == D3DTOP_SELECTARG1 && colorArg1 == D3DTA_DIFFUSE) rgb = diffuse & 0x00ffffffu;
        if (alphaOp == D3DTOP_SELECTARG1 && alphaArg1 == D3DTA_TEXTURE) alpha = 0xff000000u;
        else if (alphaOp == D3DTOP_SELECTARG1 && alphaArg1 == D3DTA_DIFFUSE) alpha = diffuse & 0xff000000u;
        return alpha | rgb;
#endif
    }
#if TH08_PSP_BULLET_PACKED_VERTEX_AUDIT_ENABLED
    static void PackPspBulletAuditVertex(const BYTE *source,
                                         D3DCOLOR effectiveColor,
                                         PspClientVertex *candidate)
    {
        // This is the future packed-once arithmetic, kept independent from
        // TransformPosition and from the canonical arena writer below.  The
        // incoming final D3D quad already contains screen shake, axis
        // nearbyintf(-0.5), rotation/anchor and flipped UV results.  Adding the
        // D3D-to-GE half pixel here therefore covers those exact final bits.
        float position[3];
        float uv[2];
        memcpy(position, source, sizeof(position));
        memcpy(uv, source + 20U, sizeof(uv));
        candidate->u = uv[0];
        candidate->v = uv[1];
        candidate->r = static_cast<GLubyte>((effectiveColor >> 16) & 255U);
        candidate->g = static_cast<GLubyte>((effectiveColor >> 8) & 255U);
        candidate->b = static_cast<GLubyte>(effectiveColor & 255U);
        candidate->a = static_cast<GLubyte>((effectiveColor >> 24) & 255U);
        candidate->x = position[0] + 0.5f;
        candidate->y = position[1] + 0.5f;
        candidate->z = 1.0f - 2.0f * position[2];
    }

    void NotePspBulletPackedMismatchByte(UINT byteOffset)
    {
        ++pspBulletPackedVertexAudit.mismatchBytes;
        if (byteOffset < 4U)
            ++pspBulletPackedVertexAudit.mismatchUBytes;
        else if (byteOffset < 8U)
            ++pspBulletPackedVertexAudit.mismatchVBytes;
        else if (byteOffset < 12U)
            ++pspBulletPackedVertexAudit.mismatchColorBytes;
        else if (byteOffset < 16U)
            ++pspBulletPackedVertexAudit.mismatchXBytes;
        else if (byteOffset < 20U)
            ++pspBulletPackedVertexAudit.mismatchYBytes;
        else if (byteOffset < 24U)
            ++pspBulletPackedVertexAudit.mismatchZBytes;
        else
            ++pspBulletPackedVertexAudit.mismatchOtherBytes;
    }

    void AuditPspBulletPackedVertices(const BYTE *data, UINT stride,
                                      const PspClientVertex *canonical,
                                      UINT quadCount)
    {
        ++pspBulletPackedVertexAudit.eligibleBatches;
        pspBulletPackedVertexAudit.eligibleQuads += quadCount;

        for (UINT quadIndex = 0U; quadIndex < quadCount; ++quadIndex)
        {
            const BYTE *const quadSource =
                data + static_cast<size_t>(quadIndex) * 4U * stride;
            D3DCOLOR rawDiffuse[4];
            for (UINT corner = 0U; corner < 4U; ++corner)
            {
                memcpy(&rawDiffuse[corner], quadSource +
                           static_cast<size_t>(corner) * stride + 16U,
                       sizeof(rawDiffuse[corner]));
            }

            const bool uniformDiffuse =
                rawDiffuse[0] == rawDiffuse[1] &&
                rawDiffuse[0] == rawDiffuse[2] &&
                rawDiffuse[0] == rawDiffuse[3];
            D3DCOLOR effectiveDiffuse[4];
            if (uniformDiffuse)
            {
                // The intended optimization: one fixed-function evaluation
                // supplies all four corners only after exact raw-bit proof.
                const D3DCOLOR common = EffectiveColor(rawDiffuse[0]);
                effectiveDiffuse[0] = effectiveDiffuse[1] =
                    effectiveDiffuse[2] = effectiveDiffuse[3] = common;
                ++pspBulletPackedVertexAudit.uniformDiffuseQuads;
            }
            else
            {
                // Audit still covers unusual quads without assuming equality.
                // A future product must retain this per-vertex color fallback.
                for (UINT corner = 0U; corner < 4U; ++corner)
                    effectiveDiffuse[corner] = EffectiveColor(rawDiffuse[corner]);
                ++pspBulletPackedVertexAudit.perVertexDiffuseQuads;
            }

            PspClientVertex candidate[4];
            bool quadMismatch = false;
            for (UINT corner = 0U; corner < 4U; ++corner)
            {
                PackPspBulletAuditVertex(
                    quadSource + static_cast<size_t>(corner) * stride,
                    effectiveDiffuse[corner], &candidate[corner]);
                ++pspBulletPackedVertexAudit.comparedVertices;

                const BYTE *const candidateBytes =
                    reinterpret_cast<const BYTE *>(&candidate[corner]);
                const BYTE *const canonicalBytes =
                    reinterpret_cast<const BYTE *>(
                        canonical + quadIndex * 4U + corner);
                bool vertexMismatch = false;
                for (UINT byteOffset = 0U;
                     byteOffset < sizeof(PspClientVertex); ++byteOffset)
                {
                    if (candidateBytes[byteOffset] == canonicalBytes[byteOffset])
                        continue;
                    if (pspBulletPackedVertexAudit.firstMismatchValid == 0U)
                    {
                        pspBulletPackedVertexAudit.firstMismatchValid = 1U;
                        pspBulletPackedVertexAudit.firstMismatchBatch =
                            pspBulletPackedVertexAudit.attempts;
                        pspBulletPackedVertexAudit.firstMismatchQuad = quadIndex;
                        pspBulletPackedVertexAudit.firstMismatchVertex = corner;
                        pspBulletPackedVertexAudit.firstMismatchByte = byteOffset;
                    }
                    NotePspBulletPackedMismatchByte(byteOffset);
                    vertexMismatch = true;
                    quadMismatch = true;
                }
                if (vertexMismatch)
                    ++pspBulletPackedVertexAudit.mismatchVertices;
            }
            ++pspBulletPackedVertexAudit.comparedQuads;
            if (quadMismatch)
                ++pspBulletPackedVertexAudit.mismatchQuads;
            else
                ++pspBulletPackedVertexAudit.matchedQuads;
        }
    }
#endif
    void ReleaseDialogueSnapshot()
    {
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
        PspPrepareStateBoundary stateBoundary(&pspPrepareStateCache);
#endif
#if defined(PSP)
        if (dialogueSnapshotSurface != NULL)
        {
            // The last restored frame may still sample this texture from the
            // pending GE list. Dialogue teardown is the lifetime boundary, so
            // finish once before returning the late backing to the arena.
            glFinish();
            th08::psp::BootLog(
                "DIALOGUE_RESTORE complete=1 capture=%lu frames=%lu "
                "restore_total=%lu restore_failures=%lu backing_released=1 "
                "path=%s capture_present=%lu release_present=%lu "
                "late_total=%lu late_heap_total=%lu\n",
                dialogueCaptureSuccesses, dialogueRestoresThisCapture,
                dialogueRestoreCount, dialogueRestoreFailures,
                dialogueCaptureUsedArenaFallback
                    ? "render_arena_fallback"
                    : "anm_scratch",
                dialogueCapturePresent, presentCount,
                dialogueLateAllocationCount, dialogueLateHeapBytes);
            dialogueSnapshotSurface->Release();
            dialogueSnapshotSurface = NULL;
        }
        dialogueRestoresThisCapture = 0;
        dialogueRestoreFailureLogged = false;
        dialogueCaptureUsedArenaFallback = false;
        dialogueCapturePresent = 0;
#endif
        dialogueSnapshotReady = false;
    }
    HRESULT DrawIndexed(D3DPRIMITIVETYPE type, UINT minVertexIndex,
                        UINT numVertexIndices, UINT primitiveCount,
                        const void *indexData, D3DFORMAT indexFormat,
                        const BYTE *data, UINT stride)
    {
#if TH08_PSP_SWAP_NOWAIT_ENABLED
        PspWaitForPendingFlip();
#endif
#if defined(PSP) && defined(TH08_PSP_RENDER_PERF_DIAG)
        const Uint64 drawStart = SDL_GetPerformanceCounter();
#endif
        const UINT maxUint = ~static_cast<UINT>(0);
        UINT indexCount = 0;
        switch (type)
        {
        case D3DPT_POINTLIST:
            indexCount = primitiveCount;
            break;
        case D3DPT_LINELIST:
            if (primitiveCount > maxUint / 2U) return E_INVALIDARG;
            indexCount = primitiveCount * 2U;
            break;
        case D3DPT_LINESTRIP:
            if (primitiveCount > maxUint - 1U) return E_INVALIDARG;
            indexCount = primitiveCount + 1U;
            break;
        case D3DPT_TRIANGLELIST:
            if (primitiveCount > maxUint / 3U) return E_INVALIDARG;
            indexCount = primitiveCount * 3U;
            break;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN:
            if (primitiveCount > maxUint - 2U) return E_INVALIDARG;
            indexCount = primitiveCount + 2U;
            break;
        default:
            return E_INVALIDARG;
        }

        if (indexCount == 0U)
            return S_OK;
        if (numVertexIndices == 0U || indexData == NULL || data == NULL ||
            stride == 0U ||
            minVertexIndex > maxUint - numVertexIndices)
        {
            return E_INVALIDARG;
        }
        const UINT vertexRangeEnd = minVertexIndex + numVertexIndices;
        const size_t maxSize = ~static_cast<size_t>(0);
        if (static_cast<size_t>(vertexRangeEnd) > maxSize / stride)
            return E_INVALIDARG;

        const bool index16 = indexFormat == D3DFMT_INDEX16;
        const bool index32 = indexFormat == D3DFMT_INDEX32;
        if (!index16 && !index32)
            return E_INVALIDARG;

#if TH08_PSP_ANY_DIRECT_GE_ENABLED
        // Prove the one immutable shared quad-index authority before the
        // generic range walk. Its exact 0,1,2/1,2,3 prefix proves every index
        // is in [0, numVertexIndices). Item and Bullet have disjoint vertex
        // arenas and owner gates, but intentionally share this table.
        const bool directOwnerExclusive =
            g_PspBulletDirectGeBatchActive !=
            g_PspItemDirectGeBatchActive;
        const bool directShape =
            directOwnerExclusive && index16 &&
            type == D3DPT_TRIANGLELIST && minVertexIndex == 0U &&
            (primitiveCount & 1U) == 0U &&
            primitiveCount / 2U <= kPspBulletDirectGeMaxQuads &&
            numVertexIndices == primitiveCount * 2U &&
            stride == 28U &&
            fvf == (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        const unsigned short *directIndices = directShape
            ? static_cast<const unsigned short *>(indexData)
            : NULL;
        bool directIndexAuthorityValid =
            directShape && !pspBulletDirectGeIndexAuthorityRejected;

        if (directIndexAuthorityValid &&
            pspBulletDirectGeIndexAuthority == NULL)
        {
            pspBulletDirectGeIndexAuthority = directIndices;
        }
        if (directIndexAuthorityValid &&
            pspBulletDirectGeIndexAuthority != directIndices)
        {
            directIndexAuthorityValid = false;
        }
        if (directIndexAuthorityValid &&
            indexCount > pspBulletDirectGeValidatedIndexCount)
        {
            static const unsigned short kQuadCorners[6] = {
                0U, 1U, 2U, 1U, 2U, 3U,
            };
            for (UINT ordinal = pspBulletDirectGeValidatedIndexCount;
                 ordinal < indexCount; ++ordinal)
            {
                const UINT expected = (ordinal / 6U) * 4U +
                    kQuadCorners[ordinal % 6U];
                if (directIndices[ordinal] != expected)
                {
                    directIndexAuthorityValid = false;
                    pspBulletDirectGeIndexAuthorityRejected = true;
                    break;
                }
            }
            if (directIndexAuthorityValid)
                pspBulletDirectGeValidatedIndexCount = indexCount;
        }
#if TH08_PSP_BULLET_PACKED_VERTEX_AUDIT_ENABLED
        if (g_PspBulletDirectGeBatchActive)
        {
            ++pspBulletPackedVertexAudit.attempts;
            const bool auditOwnerValid =
                !g_PspItemDirectGeBatchActive;
            const bool auditStateValid =
                index16 && type == D3DPT_TRIANGLELIST &&
                minVertexIndex == 0U && (primitiveCount & 1U) == 0U &&
                primitiveCount / 2U <= kPspBulletDirectGeMaxQuads &&
                numVertexIndices == primitiveCount * 2U && stride == 28U &&
                fvf == (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            if (!auditOwnerValid)
            {
                ++pspBulletPackedVertexAudit.ownerFallbacks;
            }
            else if (!auditStateValid)
            {
                ++pspBulletPackedVertexAudit.stateFallbacks;
            }
            else
            {
                pspBulletPackedVertexAudit.attemptedQuads +=
                    primitiveCount / 2U;
                if (!directIndexAuthorityValid)
                    ++pspBulletPackedVertexAudit.indexFallbacks;
            }
        }
#endif
#endif

        const auto indexAt = [indexData, index16](UINT ordinal) -> UINT {
            if (index16)
            {
                return static_cast<UINT>(
                    static_cast<const unsigned short *>(indexData)[ordinal]);
            }
            return static_cast<const UINT *>(indexData)[ordinal];
        };

        // Generic and rejected-direct callers still receive the complete
        // range validation.  The accepted direct authority was proven above
        // by its stronger exact-prefix invariant.
#if TH08_PSP_ANY_DIRECT_GE_ENABLED
        if (!directIndexAuthorityValid)
#endif
        {
            for (UINT ordinal = 0; ordinal < indexCount; ++ordinal)
            {
                const UINT vertexIndex = indexAt(ordinal);
                if (vertexIndex < minVertexIndex ||
                    vertexIndex >= vertexRangeEnd)
                {
                    return E_INVALIDARG;
                }
            }
        }

#if TH08_PSP_BULLET_UNIFIED_QUADS_ENABLED
        // GE only has a useful 16-bit indexed path here.  Convert each unique
        // D3D vertex once, then let the immutable 0,1,2/1,2,3 stream reuse it.
#if TH08_PSP_ANY_DIRECT_GE_ENABLED
        // Native submit is deliberately narrower than the generic indexed
        // path. It accepts only an explicitly bracketed Item or enemy-bullet
        // quad stream and falls back before emitting a primitive on mismatch.
        if (directShape)
        {
            if (directIndexAuthorityValid)
            {
                const UINT quadCount = primitiveCount / 2U;
                const UINT directVertexCount = quadCount * 4U;

#if TH08_PSP_ITEM_DIRECT_GE_ENABLED
                if (g_PspItemDirectGeBatchActive &&
                    !pspItemDirectGeFaulted)
                {
                    const bool itemBatchWithinCapacity =
                        directVertexCount <=
                            kPspItemDirectGeVertexCapacity;
                    if (itemBatchWithinCapacity)
                    {
                        // Complete any deferred texture upload before carving
                        // the persistent Item workspace out of the remaining
                        // render arena.  Stage loading has only 129,664 bytes
                        // contiguous here, so reversing this order can starve
                        // a final texture surface even though total RAM is
                        // still available.
                        PrepareState(true);
                        if (pspItemDirectGeVertices == NULL &&
                            !pspItemDirectGeAllocationAttempted)
                        {
                            pspItemDirectGeAllocationAttempted = true;
                            pspItemDirectGeVertices =
                                static_cast<PspClientVertex *>(
                                    th08::psp::RenderResourceArenaAllocate(
                                        kPspItemDirectGeArenaBytes, 64U,
                                        "item direct GE vertices"));
                            th08::psp::BootLog(
                                "ITEM_DIRECT_GE lazy_allocation ready=%d "
                                "bytes=%lu alignment=64 storage=render_arena "
                                "heap_fallback=0\n",
                                pspItemDirectGeVertices != NULL ? 1 : 0,
                                static_cast<unsigned long>(
                                    kPspItemDirectGeArenaBytes));
                        }
                        const bool itemArenaHasRoom =
                            pspItemDirectGeVertices != NULL &&
                            pspItemDirectGeVertexCursor <=
                                kPspItemDirectGeVertexCapacity -
                                    directVertexCount;
                        if (itemArenaHasRoom)
                        {
                            PspClientVertex *directVertices =
                                pspItemDirectGeVertices +
                                pspItemDirectGeVertexCursor;
                            for (UINT vertexIndex = 0U;
                                 vertexIndex < directVertexCount;
                                 ++vertexIndex)
                            {
                                const BYTE *vertex = data +
                                    static_cast<size_t>(vertexIndex) * stride;
                                PspClientVertex &output =
                                    directVertices[vertexIndex];
                                const float *position =
                                    reinterpret_cast<const float *>(vertex);
                                float ignoredFogCoordinate;
                                TransformPosition(
                                    position, true, &output.x, &output.y,
                                    &output.z, &ignoredFogCoordinate);
                                output.z = 1.0f - 2.0f * output.z;

                                D3DCOLOR color =
                                    *reinterpret_cast<const D3DCOLOR *>(
                                        vertex + 16U);
                                color = EffectiveColor(color);
                                output.r = static_cast<GLubyte>(
                                    (color >> 16) & 255U);
                                output.g = static_cast<GLubyte>(
                                    (color >> 8) & 255U);
                                output.b = static_cast<GLubyte>(color & 255U);
                                output.a = static_cast<GLubyte>(
                                    (color >> 24) & 255U);

                                const float *uv =
                                    reinterpret_cast<const float *>(
                                        vertex + 20U);
                                output.u = uv[0];
                                output.v = uv[1];
                            }

                            const int submitted =
                                __pspgl_th08_draw_native_indexed_triangles(
                                    directVertices,
                                    directVertexCount *
                                        sizeof(PspClientVertex),
                                    directIndices,
                                    indexCount * sizeof(unsigned short),
                                    indexCount);
                            if (submitted != 0)
                            {
                                pspItemDirectGeVertexCursor +=
                                    directVertexCount;
                                if (pspItemDirectGeArenaHighWater <
                                    pspItemDirectGeVertexCursor)
                                {
                                    pspItemDirectGeArenaHighWater =
                                        pspItemDirectGeVertexCursor;
                                }
                                ++pspItemDirectGeSubmittedBatches;
                                pspItemDirectGeSubmittedQuads += quadCount;
                                th08::psp::RenderPerfNoteDraw(indexCount);
#if defined(TH08_PSP_RENDER_PERF_DIAG)
                                perfDrawTicks +=
                                    SDL_GetPerformanceCounter() - drawStart;
                                ++perfDrawCalls;
                                perfVertices += indexCount;
#endif
                                return S_OK;
                            }
                        }
                    }
                }
#endif
#if TH08_PSP_BULLET_DIRECT_GE_ENABLED
                if (g_PspBulletDirectGeBatchActive)
                {
                const bool arenaHasRoom =
                    pspBulletDirectGeVertices != NULL &&
                    pspBulletDirectGeVertexCursor <=
                        kPspBulletDirectGeVertexCapacity - directVertexCount;
                if (arenaHasRoom)
                {
                    PspClientVertex *directVertices =
                        pspBulletDirectGeVertices +
                        pspBulletDirectGeVertexCursor;
                    PrepareState(true);
                    for (UINT vertexIndex = 0U;
                         vertexIndex < directVertexCount; ++vertexIndex)
                    {
                        const BYTE *vertex = data +
                            static_cast<size_t>(vertexIndex) * stride;
                        PspClientVertex &output = directVertices[vertexIndex];
                        const float *position =
                            reinterpret_cast<const float *>(vertex);
                        float ignoredFogCoordinate;
                        TransformPosition(position, true, &output.x, &output.y,
                                          &output.z, &ignoredFogCoordinate);
                        output.z = 1.0f - 2.0f * output.z;

                        D3DCOLOR color = *reinterpret_cast<const D3DCOLOR *>(
                            vertex + 16U);
                        color = EffectiveColor(color);
                        output.r = static_cast<GLubyte>((color >> 16) & 255U);
                        output.g = static_cast<GLubyte>((color >> 8) & 255U);
                        output.b = static_cast<GLubyte>(color & 255U);
                        output.a = static_cast<GLubyte>((color >> 24) & 255U);

                        const float *uv = reinterpret_cast<const float *>(
                            vertex + 20U);
                        output.u = uv[0];
                        output.v = uv[1];
                    }

#if TH08_PSP_BULLET_PACKED_VERTEX_AUDIT_ENABLED
                    // The accepted directVertices remain the only submitted
                    // bytes.  The local candidate is destroyed after the
                    // comparison and cannot affect render or replay state.
                    AuditPspBulletPackedVertices(
                        data, stride, directVertices, quadCount);
#endif

                    const int submitted =
                        __pspgl_th08_draw_native_indexed_triangles(
                            directVertices,
                            directVertexCount * sizeof(PspClientVertex),
                            directIndices,
                            indexCount * sizeof(unsigned short), indexCount);
                    if (submitted != 0)
                    {
#if TH08_PSP_BULLET_PACKED_VERTEX_AUDIT_ENABLED
                        ++pspBulletPackedVertexAudit.canonicalNativeSubmits;
                        pspBulletPackedVertexAudit.canonicalNativeSubmittedQuads +=
                            quadCount;
#endif
                        pspBulletDirectGeVertexCursor += directVertexCount;
                        if (pspBulletDirectGeArenaHighWater <
                            pspBulletDirectGeVertexCursor)
                        {
                            pspBulletDirectGeArenaHighWater =
                                pspBulletDirectGeVertexCursor;
                        }
                        ++pspBulletDirectGeSubmittedBatches;
                        pspBulletDirectGeSubmittedQuads += quadCount;
                        th08::psp::RenderPerfNoteDraw(indexCount);
#if defined(TH08_PSP_RENDER_PERF_DIAG)
                        perfDrawTicks +=
                            SDL_GetPerformanceCounter() - drawStart;
                        ++perfDrawCalls;
                        perfVertices += indexCount;
#endif
                        return S_OK;
                    }
#if TH08_PSP_BULLET_PACKED_VERTEX_AUDIT_ENABLED
                    ++pspBulletPackedVertexAudit.submitFallbacks;
#endif
                }
#if TH08_PSP_BULLET_PACKED_VERTEX_AUDIT_ENABLED
                else
                {
                    ++pspBulletPackedVertexAudit.capacityFallbacks;
                }
#endif
                }
#endif
            }
#if TH08_PSP_ITEM_DIRECT_GE_ENABLED
            if (g_PspItemDirectGeBatchActive)
                ++pspItemDirectGeFallbacks;
#endif
#if TH08_PSP_BULLET_DIRECT_GE_ENABLED
            if (g_PspBulletDirectGeBatchActive)
            {
                ++pspBulletDirectGeFallbacks;
#if TH08_PSP_BULLET_PACKED_VERTEX_AUDIT_ENABLED
                ++pspBulletPackedVertexAudit.canonicalFallbacks;
#endif
            }
#endif
        }
#endif
        if (index16 &&
            static_cast<size_t>(vertexRangeEnd) <=
                maxSize / sizeof(PspClientVertex))
        {
            if (vertexRangeEnd > pspDrawVertexCapacity)
            {
                UINT newCapacity =
                    pspDrawVertexCapacity != 0U ? pspDrawVertexCapacity : 256U;
                while (newCapacity < vertexRangeEnd &&
                       newCapacity <= maxUint / 2U)
                {
                    newCapacity *= 2U;
                }
                if (newCapacity < vertexRangeEnd)
                    newCapacity = vertexRangeEnd;
                void *newStorage = realloc(
                    pspDrawVertices,
                    static_cast<size_t>(newCapacity) * sizeof(PspClientVertex));
                if (newStorage != NULL)
                {
                    pspDrawVertices =
                        static_cast<PspClientVertex *>(newStorage);
                    pspDrawVertexCapacity = newCapacity;
                }
            }

            if (vertexRangeEnd <= pspDrawVertexCapacity)
            {
                th08::psp::RenderPerfNoteDraw(indexCount);
                const bool transformed =
                    (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
                UINT offset = transformed ? 16U : 12U;
                if (fvf & D3DFVF_NORMAL) offset += 12U;
                if (fvf & D3DFVF_PSIZE) offset += 4U;
                const bool hasDiffuse = (fvf & D3DFVF_DIFFUSE) != 0;
                const UINT colorOffset = offset;
                if (hasDiffuse) offset += 4U;
                if (fvf & D3DFVF_SPECULAR) offset += 4U;
                const bool hasTexture =
                    (fvf & D3DFVF_TEXCOUNT_MASK) != 0;
                const UINT textureOffset = offset;
                PrepareState(transformed);

                for (UINT vertexIndex = minVertexIndex;
                     vertexIndex < vertexRangeEnd; ++vertexIndex)
                {
                    const BYTE *vertex = data +
                        static_cast<size_t>(vertexIndex) * stride;
                    PspClientVertex &output = pspDrawVertices[vertexIndex];
                    const float *position =
                        reinterpret_cast<const float *>(vertex);
                    if (transformed)
                    {
                        float ignoredFogCoordinate;
                        TransformPosition(position, true, &output.x, &output.y,
                                          &output.z, &ignoredFogCoordinate);
                        output.z = 1.0f - 2.0f * output.z;
                    }
                    else
                    {
                        output.x = position[0];
                        output.y = position[1];
                        output.z = position[2];
                    }
                    D3DCOLOR color = hasDiffuse
                        ? *reinterpret_cast<const D3DCOLOR *>(
                              vertex + colorOffset)
                        : 0xffffffffu;
                    color = EffectiveColor(color);
                    output.r = static_cast<GLubyte>((color >> 16) & 255U);
                    output.g = static_cast<GLubyte>((color >> 8) & 255U);
                    output.b = static_cast<GLubyte>(color & 255U);
                    output.a = static_cast<GLubyte>((color >> 24) & 255U);
                    output.u = output.v = 0.0f;
                    if (hasTexture)
                    {
                        const float *uv = reinterpret_cast<const float *>(
                            vertex + textureOffset);
                        output.u = uv[0];
                        output.v = uv[1];
                        if (!transformed)
                        {
                            output.u = uv[0] * textureTransform._11 +
                                       uv[1] * textureTransform._21 +
                                       textureTransform._31;
                            output.v = uv[0] * textureTransform._12 +
                                       uv[1] * textureTransform._22 +
                                       textureTransform._32;
                        }
                    }
                }

                glEnableClientState(GL_VERTEX_ARRAY);
                glEnableClientState(GL_COLOR_ARRAY);
                glVertexPointer(3, GL_FLOAT, sizeof(PspClientVertex),
                                &pspDrawVertices[0].x);
                glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(PspClientVertex),
                               &pspDrawVertices[0].r);
                if (hasTexture)
                {
                    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                    glTexCoordPointer(2, GL_FLOAT, sizeof(PspClientVertex),
                                      &pspDrawVertices[0].u);
                }
                else
                {
                    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
                }
#if TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED
                th08::psp::DrawPrioritySubprofileNoteDraw(static_cast<std::uint32_t>(indexCount));
#endif
                glDrawElements(PrimitiveMode(type),
                               static_cast<GLsizei>(indexCount),
                               GL_UNSIGNED_SHORT, indexData);
                glDisableClientState(GL_VERTEX_ARRAY);
                glDisableClientState(GL_COLOR_ARRAY);
                if (hasTexture)
                    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
                th08::psp::RenderPerfNoteStateEmitted(
                    hasTexture ? 9U : 7U);
#if defined(TH08_PSP_RENDER_PERF_DIAG)
                perfDrawTicks += SDL_GetPerformanceCounter() - drawStart;
                ++perfDrawCalls;
                perfVertices += indexCount;
#endif
                return S_OK;
            }
        }
#endif

        // Desktop and every rejected PSP fast condition retain a complete,
        // ordered fallback by expanding only this call's index stream into
        // the already-proven non-indexed D3D8 path.
        if (static_cast<size_t>(indexCount) <= maxSize / stride)
        {
            const size_t expandedBytes =
                static_cast<size_t>(indexCount) * stride;
            BYTE *expanded = static_cast<BYTE *>(malloc(expandedBytes));
            if (expanded != NULL)
            {
                for (UINT ordinal = 0; ordinal < indexCount; ++ordinal)
                {
                    memcpy(expanded + static_cast<size_t>(ordinal) * stride,
                           data + static_cast<size_t>(indexAt(ordinal)) * stride,
                           stride);
                }
                const HRESULT result =
                    Draw(type, primitiveCount, expanded, stride);
                free(expanded);
                return result;
            }
        }

        // The bullet caller uses independent triangle lists.  If even the
        // temporary expansion allocation fails, preserve every triangle and
        // its ordering with a bounded stack fallback rather than dropping it.
        if (type == D3DPT_TRIANGLELIST && stride <= 256U)
        {
            BYTE triangle[3U * 256U];
            for (UINT primitive = 0; primitive < primitiveCount; ++primitive)
            {
                for (UINT corner = 0; corner < 3U; ++corner)
                {
                    const UINT ordinal = primitive * 3U + corner;
                    memcpy(triangle + static_cast<size_t>(corner) * stride,
                           data + static_cast<size_t>(indexAt(ordinal)) * stride,
                           stride);
                }
                const HRESULT result =
                    Draw(D3DPT_TRIANGLELIST, 1U, triangle, stride);
                if (FAILED(result))
                    return result;
            }
            return S_OK;
        }
        return E_FAIL;
    }
#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
  public:
    PspItemNaturalQuadSubmitResult DrawPspItemNaturalQuads(
        const BYTE *canonicalVertices, UINT quadCount, UINT stride,
        const unsigned short *indices, UINT indexCount)
    {
        // All rejection paths precede PrepareState and primitive emission.
        // The caller can therefore issue its untouched canonical 6V draw once
        // without duplicating an upload, state application, or primitive.
        if (canonicalVertices == NULL || quadCount == 0U)
            return PSP_ITEM_NATURAL_REJECT_DEVICE;
#if TH08_PSP_BULLET_DIRECT_GE_ENABLED
        if (g_PspBulletDirectGeBatchActive)
            return PSP_ITEM_NATURAL_REJECT_STATE;
#endif
#if TH08_PSP_ITEM_DIRECT_GE_ENABLED
        if (g_PspItemDirectGeBatchActive)
            return PSP_ITEM_NATURAL_REJECT_STATE;
#endif
        if (quadCount > 0x600U || stride != 28U ||
            fvf != (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1) ||
            texture == NULL)
        {
            return PSP_ITEM_NATURAL_REJECT_STATE;
        }
        if (quadCount > (~static_cast<UINT>(0U)) / 6U ||
            indices == NULL ||
            (reinterpret_cast<uintptr_t>(indices) &
             (alignof(unsigned short) - 1U)) != 0U ||
            indexCount != quadCount * 6U)
        {
            return PSP_ITEM_NATURAL_REJECT_INDEX;
        }

        const UINT outputVertexCount = quadCount * 4U;
        if (outputVertexCount > pspDrawVertexCapacity)
        {
            UINT newCapacity = pspDrawVertexCapacity != 0U
                                   ? pspDrawVertexCapacity
                                   : 256U;
            while (newCapacity < outputVertexCount &&
                   newCapacity <= (~static_cast<UINT>(0U)) / 2U)
            {
                newCapacity *= 2U;
            }
            if (newCapacity < outputVertexCount)
                newCapacity = outputVertexCount;
            void *const newStorage = realloc(
                pspDrawVertices,
                static_cast<size_t>(newCapacity) *
                    sizeof(PspClientVertex));
            if (newStorage == NULL)
                return PSP_ITEM_NATURAL_REJECT_CAPACITY;
            pspDrawVertices = static_cast<PspClientVertex *>(newStorage);
            pspDrawVertexCapacity = newCapacity;
        }
        if (outputVertexCount > pspDrawVertexCapacity)
            return PSP_ITEM_NATURAL_REJECT_CAPACITY;

#if defined(TH08_PSP_RENDER_PERF_DIAG)
        const Uint64 drawStart = SDL_GetPerformanceCounter();
#endif
        // Canonical DrawPrimitiveUP accounts six submitted triangle-list
        // vertices.  Keep that logical draw workload unchanged even though
        // only four unique backend vertices are materialized.
        th08::psp::RenderPerfNoteDraw(indexCount);
        PrepareState(true);

        static const UINT kUniqueCanonicalCorners[4] = {0U, 1U, 2U, 5U};
        for (UINT quad = 0U; quad < quadCount; ++quad)
        {
            const BYTE *const canonicalQuad = canonicalVertices +
                static_cast<size_t>(quad) * 6U * stride;
            for (UINT corner = 0U; corner < 4U; ++corner)
            {
                const BYTE *const vertex = canonicalQuad +
                    static_cast<size_t>(kUniqueCanonicalCorners[corner]) *
                        stride;
                PspClientVertex &output =
                    pspDrawVertices[quad * 4U + corner];
                const float *const position =
                    reinterpret_cast<const float *>(vertex);
                float ignoredFogCoordinate;
                TransformPosition(position, true, &output.x, &output.y,
                                  &output.z, &ignoredFogCoordinate);
                output.z = 1.0f - 2.0f * output.z;

                D3DCOLOR color =
                    *reinterpret_cast<const D3DCOLOR *>(vertex + 16U);
                color = EffectiveColor(color);
                output.r = static_cast<GLubyte>((color >> 16) & 255U);
                output.g = static_cast<GLubyte>((color >> 8) & 255U);
                output.b = static_cast<GLubyte>(color & 255U);
                output.a = static_cast<GLubyte>((color >> 24) & 255U);

                const float *const uv =
                    reinterpret_cast<const float *>(vertex + 20U);
                output.u = uv[0];
                output.v = uv[1];
            }
        }

        // The Bullet no-copy indexed hook must never consume pspDrawVertices:
        // later draws may reuse it before Present.  The Item-only hook instead
        // copies both this packed stream and the already-validated immutable
        // index prefix into one PSPGL-owned buffer and pins it to the current
        // display list.  Its zero return is atomic (no PRIM), so the exact same
        // bytes can take the ordinary client path once below.
#if TH08_PSP_ITEM_NATURAL_NATIVE_COPY_ENABLED
        const UINT nativeVertexBytes =
            outputVertexCount * static_cast<UINT>(sizeof(PspClientVertex));
        const UINT nativeIndexBytes =
            indexCount * static_cast<UINT>(sizeof(*indices));
        const bool nativeCopyRejected =
            __pspgl_th08_draw_native_indexed_quads_copy(
                pspDrawVertices, nativeVertexBytes, indices,
                nativeIndexBytes, quadCount) == 0;
        if (!nativeCopyRejected)
        {
#if defined(TH08_PSP_RENDER_PERF_DIAG)
            perfDrawTicks += SDL_GetPerformanceCounter() - drawStart;
            ++perfDrawCalls;
            perfVertices += indexCount;
#endif
            return PSP_ITEM_NATURAL_SUBMIT_NATIVE;
        }
#endif

        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(PspClientVertex),
                        &pspDrawVertices[0].x);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(PspClientVertex),
                       &pspDrawVertices[0].r);
        glTexCoordPointer(2, GL_FLOAT, sizeof(PspClientVertex),
                          &pspDrawVertices[0].u);
#if TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED
        th08::psp::DrawPrioritySubprofileNoteDraw(static_cast<std::uint32_t>(indexCount));
#endif
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indexCount),
                       GL_UNSIGNED_SHORT, indices);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        th08::psp::RenderPerfNoteStateEmitted(9U);
#if defined(TH08_PSP_RENDER_PERF_DIAG)
        perfDrawTicks += SDL_GetPerformanceCounter() - drawStart;
        ++perfDrawCalls;
        perfVertices += indexCount;
#endif
#if TH08_PSP_ITEM_NATURAL_NATIVE_COPY_ENABLED
        if (nativeCopyRejected)
            return PSP_ITEM_NATURAL_SUBMIT_CLIENT_AFTER_NATIVE_REJECT;
#endif
        return PSP_ITEM_NATURAL_SUBMIT_CLIENT;
    }
#endif
#if TH08_PSP_BULLET_PACKED_VERTEX_AUDIT_ENABLED
  public:
    bool QueryPspBulletPackedVertexAuditStats(
        PspBulletPackedVertexAuditStats *stats) const
    {
        if (stats == NULL)
            return false;
        *stats = pspBulletPackedVertexAudit;
        return true;
    }
#endif
#if TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH_ENABLED
  public:
    bool BeginPspBulletPackedVertexBatch()
    {
        ++pspBulletPackedVertexFastpath.beginAttempts;
        pspBulletPackedVertexBatchActive = false;
        if (pspBulletPackedVertexRunActive)
        {
            ++pspBulletPackedVertexFastpath.contractFallbacks;
            ++pspBulletPackedVertexFastpath.abandonedRuns;
            pspBulletPackedVertexFastpath.abandonedQuads +=
                pspBulletPackedVertexRunQuads;
            pspBulletDirectGeVertexCursor = pspBulletPackedVertexRunStart;
            pspBulletPackedVertexRunActive = false;
            pspBulletPackedVertexRunQuads = 0U;
        }

        if (!g_PspBulletDirectGeBatchActive ||
            g_PspItemDirectGeBatchActive)
        {
            ++pspBulletPackedVertexFastpath.ownerFallbacks;
            ++pspBulletPackedVertexFastpath.canonicalFallbackBatches;
            return false;
        }
        if (pspBulletDirectGeVertices == NULL ||
            pspBulletDirectGeArenaPresent != presentCount)
        {
            ++pspBulletPackedVertexFastpath.stateFallbacks;
            ++pspBulletPackedVertexFastpath.canonicalFallbackBatches;
            return false;
        }
        // One BulletManager pass can emit at most its 0x600 logical slots.
        // Accept M1 only with the complete 6144-vertex arena available, so a
        // capacity failure after any packed quad is impossible by contract.
        // A repeated Bullet pass before Present remains wholly canonical.
        if (pspBulletDirectGeVertexCursor != 0U ||
            kPspBulletDirectGeVertexCapacity !=
                kPspBulletDirectGeMaxQuads * 4U)
        {
            ++pspBulletPackedVertexFastpath.capacityFallbacks;
            ++pspBulletPackedVertexFastpath.canonicalFallbackBatches;
            return false;
        }

        pspBulletPackedVertexBatchActive = true;
        ++pspBulletPackedVertexFastpath.acceptedBatches;
        return true;
    }

    static void PackPspBulletProductVertex(const BYTE *source,
                                           D3DCOLOR effectiveColor,
                                           PspClientVertex *destination)
    {
        // M0 proved this independent arithmetic byte-for-byte for 5,092,640
        // Stage-5 vertices.  The source already includes screen shake,
        // nearbyintf axis rounding, anchor/rotation and flipped UV results.
        float position[3];
        float uv[2];
        memcpy(position, source, sizeof(position));
        memcpy(uv, source + 20U, sizeof(uv));
        destination->u = uv[0];
        destination->v = uv[1];
        destination->r = static_cast<GLubyte>(
            (effectiveColor >> 16) & 255U);
        destination->g = static_cast<GLubyte>(
            (effectiveColor >> 8) & 255U);
        destination->b = static_cast<GLubyte>(effectiveColor & 255U);
        destination->a = static_cast<GLubyte>(
            (effectiveColor >> 24) & 255U);
        destination->x = position[0] + 0.5f;
        destination->y = position[1] + 0.5f;
        destination->z = 1.0f - 2.0f * position[2];
    }

    bool AppendPspBulletPackedVertexQuad(const BYTE *quad, UINT stride)
    {
        ++pspBulletPackedVertexFastpath.appendAttempts;
        if (!pspBulletPackedVertexBatchActive ||
            !g_PspBulletDirectGeBatchActive ||
            g_PspItemDirectGeBatchActive)
        {
            ++pspBulletPackedVertexFastpath.ownerFallbacks;
            return false;
        }
        if (quad == NULL || stride != 28U ||
            pspBulletDirectGeArenaPresent != presentCount)
        {
            ++pspBulletPackedVertexFastpath.stateFallbacks;
            return false;
        }
        if (pspBulletDirectGeVertices == NULL ||
            pspBulletDirectGeVertexCursor >
                kPspBulletDirectGeVertexCapacity - 4U)
        {
            ++pspBulletPackedVertexFastpath.capacityFallbacks;
            return false;
        }

        D3DCOLOR rawDiffuse[4];
        for (UINT corner = 0U; corner < 4U; ++corner)
        {
            memcpy(&rawDiffuse[corner],
                   quad + static_cast<size_t>(corner) * stride + 16U,
                   sizeof(rawDiffuse[corner]));
        }
        const bool uniformDiffuse =
            rawDiffuse[0] == rawDiffuse[1] &&
            rawDiffuse[0] == rawDiffuse[2] &&
            rawDiffuse[0] == rawDiffuse[3];
        D3DCOLOR effectiveDiffuse[4];
        if (uniformDiffuse)
        {
            // Canonical Flush forces both ARG2 operands to DIFFUSE before its
            // backend conversion.  Evaluate that final state purely here;
            // never add, reorder, or restore a SetTextureStageState request.
            const D3DCOLOR common = EffectiveColor(rawDiffuse[0], true);
            effectiveDiffuse[0] = effectiveDiffuse[1] =
                effectiveDiffuse[2] = effectiveDiffuse[3] = common;
            ++pspBulletPackedVertexFastpath.uniformDiffuseQuads;
        }
        else
        {
            for (UINT corner = 0U; corner < 4U; ++corner)
                effectiveDiffuse[corner] =
                    EffectiveColor(rawDiffuse[corner], true);
            ++pspBulletPackedVertexFastpath.perVertexDiffuseQuads;
        }

        if (!pspBulletPackedVertexRunActive)
        {
            pspBulletPackedVertexRunActive = true;
            pspBulletPackedVertexRunStart = pspBulletDirectGeVertexCursor;
            pspBulletPackedVertexRunQuads = 0U;
            pspBulletPackedVertexRunPresent = presentCount;
        }
        PspClientVertex *const destination =
            pspBulletDirectGeVertices + pspBulletDirectGeVertexCursor;
        for (UINT corner = 0U; corner < 4U; ++corner)
        {
            PackPspBulletProductVertex(
                quad + static_cast<size_t>(corner) * stride,
                effectiveDiffuse[corner], destination + corner);
        }
        pspBulletDirectGeVertexCursor += 4U;
        ++pspBulletPackedVertexRunQuads;
        ++pspBulletPackedVertexFastpath.appendedQuads;
        pspBulletPackedVertexFastpath.packedVertices += 4U;
        if (pspBulletPackedVertexFastpath.maxRunQuads <
            pspBulletPackedVertexRunQuads)
        {
            pspBulletPackedVertexFastpath.maxRunQuads =
                pspBulletPackedVertexRunQuads;
        }
        if (pspBulletDirectGeArenaHighWater <
            pspBulletDirectGeVertexCursor)
        {
            pspBulletDirectGeArenaHighWater =
                pspBulletDirectGeVertexCursor;
        }
        pspBulletPackedVertexFastpath.arenaHighWaterVertices =
            pspBulletDirectGeArenaHighWater;
        return true;
    }

    bool SubmitPspBulletPackedVertexRun(
        UINT quadCount, const unsigned short *indices, UINT indexCount)
    {
        ++pspBulletPackedVertexFastpath.submitAttempts;
        // Once append succeeds these packed bytes are the sole renderer replay
        // authority.  Validate their own bounds first; every later native or
        // contract rejection is drawn from these same bytes in this call.
        const UINT authoritativeQuads = pspBulletPackedVertexRunQuads;
        const bool safePackedRun =
            pspBulletPackedVertexRunActive &&
            pspBulletDirectGeVertices != NULL &&
            authoritativeQuads != 0U &&
            authoritativeQuads <= kPspBulletDirectGeMaxQuads &&
            pspBulletPackedVertexRunStart <=
                kPspBulletDirectGeVertexCapacity -
                    authoritativeQuads * 4U &&
            pspBulletDirectGeVertexCursor ==
                pspBulletPackedVertexRunStart + authoritativeQuads * 4U;
        if (!safePackedRun)
        {
            ++pspBulletPackedVertexFastpath.contractFallbacks;
            return false;
        }

        const bool ownerValid =
            pspBulletPackedVertexBatchActive &&
            g_PspBulletDirectGeBatchActive &&
            !g_PspItemDirectGeBatchActive;
        if (!ownerValid)
            ++pspBulletPackedVertexFastpath.ownerFallbacks;
        const bool stateValid =
            quadCount == authoritativeQuads &&
            pspBulletPackedVertexRunPresent == presentCount &&
            fvf == (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        if (!stateValid)
            ++pspBulletPackedVertexFastpath.stateFallbacks;

        bool indexValid =
            authoritativeQuads <= (~static_cast<UINT>(0U)) / 6U &&
            indexCount == authoritativeQuads * 6U && indices != NULL &&
            !pspBulletDirectGeIndexAuthorityRejected;
        static const unsigned short kQuadCorners[6] = {
            0U, 1U, 2U, 1U, 2U, 3U,
        };
        if (indexValid && pspBulletDirectGeIndexAuthority == NULL)
            pspBulletDirectGeIndexAuthority = indices;
        if (indexValid && pspBulletDirectGeIndexAuthority != indices)
            indexValid = false;
        if (indexValid &&
            indexCount > pspBulletDirectGeValidatedIndexCount)
        {
            for (UINT ordinal = pspBulletDirectGeValidatedIndexCount;
                 ordinal < indexCount; ++ordinal)
            {
                const UINT expected = (ordinal / 6U) * 4U +
                    kQuadCorners[ordinal % 6U];
                if (indices[ordinal] != expected)
                {
                    indexValid = false;
                    pspBulletDirectGeIndexAuthorityRejected = true;
                    break;
                }
            }
            if (indexValid)
                pspBulletDirectGeValidatedIndexCount = indexCount;
        }
        if (!indexValid)
            ++pspBulletPackedVertexFastpath.indexFallbacks;

#if defined(TH08_PSP_RENDER_PERF_DIAG)
        const Uint64 drawStart = SDL_GetPerformanceCounter();
#endif
        PrepareState(true);
        PspClientVertex *const vertices =
            pspBulletDirectGeVertices + pspBulletPackedVertexRunStart;
        const UINT vertexCount = authoritativeQuads * 4U;
        const UINT authoritativeIndexCount = authoritativeQuads * 6U;
        const bool nativeEligible = ownerValid && stateValid && indexValid;
        const int nativeSubmitted = nativeEligible
            ? __pspgl_th08_draw_native_indexed_triangles(
                  vertices, vertexCount * sizeof(PspClientVertex),
                  indices, authoritativeIndexCount * sizeof(unsigned short),
                  authoritativeIndexCount)
            : 0;
        if (nativeSubmitted != 0)
        {
            ++pspBulletPackedVertexFastpath.nativeSubmits;
            pspBulletPackedVertexFastpath.nativeSubmittedQuads +=
                authoritativeQuads;
            ++pspBulletDirectGeSubmittedBatches;
            pspBulletDirectGeSubmittedQuads += authoritativeQuads;
            th08::psp::RenderPerfNoteDraw(authoritativeIndexCount);
#if defined(TH08_PSP_RENDER_PERF_DIAG)
            ++perfDrawCalls;
            perfVertices += authoritativeIndexCount;
#endif
        }
        else if (indexValid)
        {
            // Native rejection is still one draw: PSPGL consumes the exact
            // same persistent 24-byte bytes through its public client arrays.
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_COLOR_ARRAY);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glVertexPointer(3, GL_FLOAT, sizeof(PspClientVertex),
                            &vertices[0].x);
            glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(PspClientVertex),
                           &vertices[0].r);
            glTexCoordPointer(2, GL_FLOAT, sizeof(PspClientVertex),
                              &vertices[0].u);
#if TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED
            th08::psp::DrawPrioritySubprofileNoteDraw(static_cast<std::uint32_t>(authoritativeIndexCount));
#endif
            glDrawElements(GL_TRIANGLES,
                           static_cast<GLsizei>(authoritativeIndexCount),
                           GL_UNSIGNED_SHORT, indices);
            glDisableClientState(GL_VERTEX_ARRAY);
            glDisableClientState(GL_COLOR_ARRAY);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            th08::psp::RenderPerfNoteStateEmitted(9U);
            ++pspBulletPackedVertexFastpath.clientFallbackSubmits;
            pspBulletPackedVertexFastpath.clientFallbackQuads +=
                authoritativeQuads;
            ++pspBulletDirectGeFallbacks;
            th08::psp::RenderPerfNoteDraw(authoritativeIndexCount);
#if defined(TH08_PSP_RENDER_PERF_DIAG)
            ++perfDrawCalls;
            perfVertices += authoritativeIndexCount;
#endif
        }
        else
        {
            // Corruption of the frontend index authority must still preserve
            // every packed quad.  This diagnostic-only recovery uses each
            // quad's identical 4V bytes as a triangle strip, records a split,
            // and is an unconditional product NO-GO.
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_COLOR_ARRAY);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            for (UINT quad = 0U; quad < authoritativeQuads; ++quad)
            {
                PspClientVertex *const strip = vertices + quad * 4U;
                glVertexPointer(3, GL_FLOAT, sizeof(PspClientVertex),
                                &strip[0].x);
                glColorPointer(4, GL_UNSIGNED_BYTE,
                               sizeof(PspClientVertex), &strip[0].r);
                glTexCoordPointer(2, GL_FLOAT,
                                  sizeof(PspClientVertex), &strip[0].u);
#if TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED
                th08::psp::DrawPrioritySubprofileNoteDraw(4U);
#endif
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                th08::psp::RenderPerfNoteDraw(4U);
            }
            glDisableClientState(GL_VERTEX_ARRAY);
            glDisableClientState(GL_COLOR_ARRAY);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            th08::psp::RenderPerfNoteStateEmitted(
                6U + authoritativeQuads * 3U);
            pspBulletPackedVertexFastpath.clientFallbackSubmits +=
                authoritativeQuads;
            pspBulletPackedVertexFastpath.clientFallbackQuads +=
                authoritativeQuads;
            ++pspBulletPackedVertexFastpath.recoverySplitRuns;
            pspBulletPackedVertexFastpath.recoverySplitQuads +=
                authoritativeQuads;
            ++pspBulletDirectGeFallbacks;
#if defined(TH08_PSP_RENDER_PERF_DIAG)
            perfDrawCalls += authoritativeQuads;
            perfVertices += vertexCount;
#endif
        }

        ++pspBulletPackedVertexFastpath.submittedRuns;
        pspBulletPackedVertexFastpath.submittedQuads += authoritativeQuads;
#if defined(TH08_PSP_RENDER_PERF_DIAG)
        perfDrawTicks += SDL_GetPerformanceCounter() - drawStart;
#endif
        pspBulletPackedVertexRunActive = false;
        pspBulletPackedVertexRunStart = pspBulletDirectGeVertexCursor;
        pspBulletPackedVertexRunQuads = 0U;
        return true;
    }

    void EndPspBulletPackedVertexBatch()
    {
        if (pspBulletPackedVertexRunActive)
        {
            ++pspBulletPackedVertexFastpath.contractFallbacks;
            ++pspBulletPackedVertexFastpath.abandonedRuns;
            pspBulletPackedVertexFastpath.abandonedQuads +=
                pspBulletPackedVertexRunQuads;
            // No primitive references an unsubmitted tail, so the append-only
            // cursor may safely forget exactly that tail without a GE fence.
            pspBulletDirectGeVertexCursor = pspBulletPackedVertexRunStart;
        }
        pspBulletPackedVertexBatchActive = false;
        pspBulletPackedVertexRunActive = false;
        pspBulletPackedVertexRunStart = pspBulletDirectGeVertexCursor;
        pspBulletPackedVertexRunQuads = 0U;
    }

    void NotePspBulletPackedVertexRecoverySplit(UINT submittedQuads)
    {
        ++pspBulletPackedVertexFastpath.recoverySplitRuns;
        pspBulletPackedVertexFastpath.recoverySplitQuads += submittedQuads;
    }

    bool QueryPspBulletPackedVertexFastpathStats(
        PspBulletPackedVertexFastpathStats *stats) const
    {
        if (stats == NULL)
            return false;
        memcpy(stats, &pspBulletPackedVertexFastpath, sizeof(*stats));
        stats->arenaHighWaterVertices = pspBulletDirectGeArenaHighWater;
        stats->arenaCapacityVertices = kPspBulletDirectGeVertexCapacity;
        return true;
    }
#endif
#if TH08_PSP_SPRITE_PAIR_BATCH_ENABLED
  public:
    bool ReservePspSpritePairs(UINT spriteCount)
    {
        const UINT maxUint = ~static_cast<UINT>(0U);
        if (spriteCount > maxUint / 2U)
            return false;
        const UINT vertexCount = spriteCount * 2U;
        const size_t maxSize = static_cast<size_t>(-1);
        if (static_cast<size_t>(vertexCount) >
            maxSize / sizeof(PspClientVertex))
        {
            return false;
        }

        if (vertexCount > pspDrawVertexCapacity)
        {
            UINT newCapacity = pspDrawVertexCapacity != 0U
                                   ? pspDrawVertexCapacity
                                   : 256U;
            while (newCapacity < vertexCount && newCapacity <= maxUint / 2U)
                newCapacity *= 2U;
            if (newCapacity < vertexCount)
                newCapacity = vertexCount;
            void *newStorage = realloc(
                pspDrawVertices,
                static_cast<size_t>(newCapacity) * sizeof(PspClientVertex));
            if (newStorage == NULL)
                return false;
            pspDrawVertices = static_cast<PspClientVertex *>(newStorage);
            pspDrawVertexCapacity = newCapacity;
        }
        return vertexCount <= pspDrawVertexCapacity;
    }

    bool DrawPspSpritePairs(const BYTE *data, UINT spriteCount,
                            UINT stride, bool allowScorePopupNativeGe)
    {
        // The caller reserves application staging before mutating the shared
        // popup VM. From this point onward there is no app-owned allocation or
        // partial-prefix fallback: one immediate pair stream owns the whole
        // validated score batch. The optional PSPGL stream copy may reject
        // before PRIM, in which case this same stream takes the client route.
        if (data == NULL || spriteCount == 0U ||
            stride != sizeof(float) * 6U + sizeof(D3DCOLOR) ||
            spriteCount > (~static_cast<UINT>(0U)) / 2U)
        {
            return false;
        }
        const UINT vertexCount = spriteCount * 2U;
        if (vertexCount > pspDrawVertexCapacity ||
            fvf != (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1) ||
            texture == NULL)
        {
            return false;
        }

#if defined(TH08_PSP_RENDER_PERF_DIAG)
        const Uint64 drawStart = SDL_GetPerformanceCounter();
#endif
        th08::psp::RenderPerfNoteDraw(vertexCount);
        PrepareState(true);
        for (UINT index = 0U; index < vertexCount; ++index)
        {
            const BYTE *vertex = data + static_cast<size_t>(index) * stride;
            PspClientVertex &output = pspDrawVertices[index];
            const float *position = reinterpret_cast<const float *>(vertex);
            float ignoredFogCoordinate;
            TransformPosition(position, true, &output.x, &output.y,
                              &output.z, &ignoredFogCoordinate);
            output.z = 1.0f - 2.0f * output.z;

            D3DCOLOR color = *reinterpret_cast<const D3DCOLOR *>(vertex + 16U);
            color = EffectiveColor(color);
            output.r = static_cast<GLubyte>((color >> 16) & 255U);
            output.g = static_cast<GLubyte>((color >> 8) & 255U);
            output.b = static_cast<GLubyte>(color & 255U);
            output.a = static_cast<GLubyte>((color >> 24) & 255U);

            const float *uv = reinterpret_cast<const float *>(vertex + 20U);
            output.u = uv[0];
            output.v = uv[1];
        }

#if TH08_PSP_SCORE_POPUP_NATIVE_GE_ENABLED
        if (allowScorePopupNativeGe)
        {
            ++pspScorePopupNativeAttempts;
            const size_t nativeVertexBytes =
                static_cast<size_t>(vertexCount) * sizeof(PspClientVertex);
            if (nativeVertexBytes <= static_cast<size_t>(~0U) &&
                __pspgl_th08_draw_native_sprite_pairs_copy(
                    pspDrawVertices,
                    static_cast<unsigned>(nativeVertexBytes)) != 0)
            {
                ++pspScorePopupNativeSubmits;
#if defined(TH08_PSP_RENDER_PERF_DIAG)
                perfDrawTicks += SDL_GetPerformanceCounter() - drawStart;
                ++perfDrawCalls;
                perfVertices += vertexCount;
#endif
                return true;
            }
            // PSPGL keeps the complete correctness fallback. A rejected
            // A rejected private score submit changes no geometry ordering;
            // the converted pair stream immediately takes the client route.
            ++pspScorePopupNativeClientFallbacks;
        }
#else
        (void)allowScorePopupNativeGe;
#endif

        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(PspClientVertex),
                        &pspDrawVertices[0].x);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(PspClientVertex),
                       &pspDrawVertices[0].r);
        glTexCoordPointer(2, GL_FLOAT, sizeof(PspClientVertex),
                          &pspDrawVertices[0].u);
        // PSPGL translates this public extension primitive to GE_SPRITES.
        // Two diagonal corners reproduce the already rounded D3D quad without
        // the canonical 0,1,2/1,2,3 duplication on SC.
#if TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED
        th08::psp::DrawPrioritySubprofileNoteDraw(static_cast<std::uint32_t>(vertexCount));
#endif
        glDrawArrays(GL_SPRITES_PSP, 0, static_cast<GLsizei>(vertexCount));
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        th08::psp::RenderPerfNoteStateEmitted(9U);
#if defined(TH08_PSP_RENDER_PERF_DIAG)
        perfDrawTicks += SDL_GetPerformanceCounter() - drawStart;
        ++perfDrawCalls;
        perfVertices += vertexCount;
#endif
        return true;
    }

#if TH08_PSP_SCORE_POPUP_NATIVE_GE_ENABLED
    bool QueryPspScorePopupNativeGeStats(
        PspScorePopupNativeGeStats *stats) const
    {
        if (stats == NULL)
            return false;
        stats->attempts = pspScorePopupNativeAttempts;
        stats->submits = pspScorePopupNativeSubmits;
        stats->clientFallbacks = pspScorePopupNativeClientFallbacks;
        return true;
    }
#endif

#if TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH_ENABLED || \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH_ENABLED
    bool DrawPspSpritePairsInPlace(BYTE *data, UINT spriteCount,
                                   UINT stride)
    {
        // Validate every D3D-side precondition before consuming the mutable
        // pair stream. PSPGL's owned transient copy can still reject after
        // conversion, but does so before PRIM; the caller's independent VM
        // tail then replays the run canonically.
        if (data == NULL || spriteCount == 0U ||
            stride != sizeof(float) * 6U + sizeof(D3DCOLOR) ||
            spriteCount > (~static_cast<UINT>(0U)) / 2U ||
            fvf != (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1) ||
            texture == NULL)
        {
            return false;
        }
        const UINT vertexCount = spriteCount * 2U;
        const size_t nativeVertexBytes =
            static_cast<size_t>(vertexCount) * sizeof(PspClientVertex);
        if (nativeVertexBytes > static_cast<size_t>(~0U))
            return false;

#if defined(TH08_PSP_RENDER_PERF_DIAG)
        const Uint64 drawStart = SDL_GetPerformanceCounter();
#endif
        PrepareState(true);

        // 24*i is never after 28*i. Read the complete current 28-byte source
        // into locals before writing its 24-byte packed replacement, so the
        // forward conversion cannot damage this or any following source.
        for (UINT index = 0U; index < vertexCount; ++index)
        {
            BYTE *const source = data + static_cast<size_t>(index) * stride;
            float position[3];
            float uv[2];
            D3DCOLOR color;
            memcpy(position, source, sizeof(position));
            memcpy(&color, source + 16U, sizeof(color));
            memcpy(uv, source + 20U, sizeof(uv));

            PspClientVertex packed;
            float ignoredFogCoordinate;
            TransformPosition(position, true, &packed.x, &packed.y,
                              &packed.z, &ignoredFogCoordinate);
            packed.z = 1.0f - 2.0f * packed.z;
            color = EffectiveColor(color);
            packed.r = static_cast<GLubyte>((color >> 16) & 255U);
            packed.g = static_cast<GLubyte>((color >> 8) & 255U);
            packed.b = static_cast<GLubyte>(color & 255U);
            packed.a = static_cast<GLubyte>((color >> 24) & 255U);
            packed.u = uv[0];
            packed.v = uv[1];
            memcpy(data + static_cast<size_t>(index) *
                              sizeof(PspClientVertex),
                   &packed, sizeof(packed));
        }

        // Do not use public glDrawArrays here: its void varray conversion can
        // hide a transient allocation failure.  The v3 PSPGL hook owns the
        // copy/list lifetime and returns zero before PRIM on every rejection.
        if (__pspgl_th08_draw_native_sprite_pairs_copy(
                data, static_cast<unsigned>(nativeVertexBytes)) == 0)
        {
            return false;
        }
        th08::psp::RenderPerfNoteDraw(vertexCount);
#if TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED
        th08::psp::DrawPrioritySubprofileNoteDraw(static_cast<std::uint32_t>(vertexCount));
#endif
#if defined(TH08_PSP_RENDER_PERF_DIAG)
        perfDrawTicks += SDL_GetPerformanceCounter() - drawStart;
        ++perfDrawCalls;
        perfVertices += vertexCount;
#endif
        return true;
    }
#endif

#if TH08_PSP_BULLET_MIXED_QUADS_FASTPATH_ENABLED || \
    TH08_PSP_ITEM_MIXED_QUADS_FASTPATH_ENABLED
    bool QueryPspMixedGeBackendStats(PspMixedGeBackendStats *stats) const
    {
        if (stats == NULL)
            return false;
        stats->bulletAttempts = pspBulletMixedGeAttempts;
        stats->bulletSubmittedBatches = pspBulletMixedGeSubmittedBatches;
        stats->bulletSubmittedQuads = pspBulletMixedGeSubmittedQuads;
        stats->bulletFallbacks = pspBulletMixedGeFallbacks;
        stats->bulletArenaExhaustions = pspBulletMixedGeArenaExhaustions;
        stats->itemAttempts = pspItemMixedGeAttempts;
        stats->itemSubmittedBatches = pspItemMixedGeSubmittedBatches;
        stats->itemSubmittedQuads = pspItemMixedGeSubmittedQuads;
        stats->itemFallbacks = pspItemMixedGeFallbacks;
        stats->itemArenaExhaustions = pspItemMixedGeArenaExhaustions;
        stats->sharedArenaHighWaterVertices =
            static_cast<unsigned long>(pspBulletDirectGeArenaHighWater);
        stats->sharedArenaCapacityVertices =
            static_cast<unsigned long>(kPspBulletDirectGeVertexCapacity);
        return true;
    }

    bool DrawPspBulletMixedQuads(
        const BYTE *pairData, UINT pairCount,
        const BYTE *quadData, UINT quadCount, UINT stride,
        const unsigned short *quadIndices, UINT quadIndexCount,
        bool itemOwner)
    {
        // The caller's final D3D vertices remain immutable replay authority.
        // Only this Present-fenced native arena is written before the PSPGL
        // hook atomically emits the proven pair prefix then indexed suffix.
        if (itemOwner)
            ++pspItemMixedGeAttempts;
        else
            ++pspBulletMixedGeAttempts;
        const auto reject = [this, itemOwner]() -> bool {
            if (itemOwner)
            {
                ++pspItemMixedGeFallbacks;
            }
            else
            {
                ++pspBulletMixedGeFallbacks;
                ++pspBulletDirectGeFallbacks;
            }
            return false;
        };
        const UINT maxUint = ~static_cast<UINT>(0U);
        const size_t maxSize = ~static_cast<size_t>(0U);
        const bool ownerTokenValid = itemOwner
            ? (g_PspItemMixedGeBatchActive &&
               !g_PspBulletDirectGeBatchActive &&
               !g_PspItemDirectGeBatchActive)
            : (g_PspBulletDirectGeBatchActive &&
               !g_PspItemMixedGeBatchActive &&
               !g_PspItemDirectGeBatchActive);
        if (!ownerTokenValid ||
            pspBulletDirectGeVertices == NULL ||
            fvf != (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1) ||
            texture == NULL || stride != 28U ||
            (pairData == NULL) != (pairCount == 0U) ||
            pairCount > maxUint / 2U || quadCount > maxUint / 4U ||
            quadCount > maxUint / 6U ||
            pairCount > maxUint - quadCount ||
            pairCount + quadCount == 0U ||
            pairCount + quadCount > kPspBulletDirectGeMaxQuads)
        {
            return reject();
        }

        const UINT pairVertexCount = pairCount * 2U;
        const UINT quadVertexCount = quadCount * 4U;
        const UINT expectedIndexCount = quadCount * 6U;
        const bool canonicalQuadAbsence =
            quadCount == 0U && quadData == NULL && quadIndices == NULL &&
            quadIndexCount == 0U;
        const bool canonicalQuadPresence =
            quadCount != 0U && quadData != NULL && quadIndices != NULL &&
            quadIndexCount == expectedIndexCount;
        if ((!canonicalQuadAbsence && !canonicalQuadPresence) ||
            (pairVertexCount != 0U &&
             ((pairVertexCount & 1U) != 0U ||
              pairVertexCount > 0xffffU)) ||
            (quadVertexCount != 0U &&
             ((quadVertexCount & 3U) != 0U ||
              quadVertexCount > 0x10000U)) ||
            (quadIndexCount != 0U &&
             (quadIndexCount > 0xffffU ||
              quadIndexCount % 3U != 0U ||
              quadIndexCount % 6U != 0U)) ||
            static_cast<size_t>(pairVertexCount) > maxSize / stride ||
            static_cast<size_t>(quadVertexCount) > maxSize / stride ||
            static_cast<size_t>(quadIndexCount) >
                maxSize / sizeof(*quadIndices) ||
            (pairData != NULL &&
             reinterpret_cast<size_t>(pairData) % alignof(float) != 0U) ||
            (quadData != NULL &&
             reinterpret_cast<size_t>(quadData) % alignof(float) != 0U) ||
            (quadIndices != NULL &&
             reinterpret_cast<size_t>(quadIndices) %
                 alignof(unsigned short) != 0U))
        {
            return reject();
        }

        // The PSPGL mixed hook deliberately trusts this application-owned
        // immutable table.  Validate its exact canonical prefix before any
        // converted vertex is written or PrepareState can submit an upload.
        if (quadCount != 0U)
        {
            if (pspBulletDirectGeIndexAuthorityRejected ||
                (pspBulletDirectGeIndexAuthority != NULL &&
                 pspBulletDirectGeIndexAuthority != quadIndices))
            {
                return reject();
            }
            static const unsigned short kQuadCorners[6] = {
                0U, 1U, 2U, 1U, 2U, 3U,
            };
            const UINT validated = pspBulletDirectGeIndexAuthority == NULL
                ? 0U : pspBulletDirectGeValidatedIndexCount;
            for (UINT ordinal = validated; ordinal < quadIndexCount;
                 ++ordinal)
            {
                const UINT expected = (ordinal / 6U) * 4U +
                    kQuadCorners[ordinal % 6U];
                if (quadIndices[ordinal] != expected)
                {
                    pspBulletDirectGeIndexAuthorityRejected = true;
                    return reject();
                }
            }
            pspBulletDirectGeIndexAuthority = quadIndices;
            if (pspBulletDirectGeValidatedIndexCount < quadIndexCount)
                pspBulletDirectGeValidatedIndexCount = quadIndexCount;
        }

        const UINT nativeVertexCount = pairVertexCount + quadVertexCount;
        if (nativeVertexCount < pairVertexCount ||
            nativeVertexCount > kPspBulletDirectGeVertexCapacity ||
            pspBulletDirectGeVertexCursor >
                kPspBulletDirectGeVertexCapacity - nativeVertexCount)
        {
            if (itemOwner)
                ++pspItemMixedGeArenaExhaustions;
            else
                ++pspBulletMixedGeArenaExhaustions;
            return reject();
        }
        const size_t pairVertexBytes =
            static_cast<size_t>(pairVertexCount) * sizeof(PspClientVertex);
        const size_t quadVertexBytes =
            static_cast<size_t>(quadVertexCount) * sizeof(PspClientVertex);
        const size_t quadIndexBytes =
            static_cast<size_t>(quadIndexCount) * sizeof(*quadIndices);
        if (pairVertexBytes > static_cast<size_t>(~0U) ||
            quadVertexBytes > static_cast<size_t>(~0U) ||
            quadIndexBytes > static_cast<size_t>(~0U))
        {
            return reject();
        }

#if defined(TH08_PSP_RENDER_PERF_DIAG)
        const Uint64 drawStart = SDL_GetPerformanceCounter();
#endif
        PrepareState(true);
        PspClientVertex *const nativeBase =
            pspBulletDirectGeVertices + pspBulletDirectGeVertexCursor;
        PspClientVertex *const nativePairs =
            pairVertexCount != 0U ? nativeBase : NULL;
        PspClientVertex *const nativeQuads =
            quadVertexCount != 0U ? nativeBase + pairVertexCount : NULL;

        const auto convertRange = [this, stride](
            const BYTE *source, UINT vertexCount,
            PspClientVertex *destination) {
            for (UINT index = 0U; index < vertexCount; ++index)
            {
                const BYTE *const vertex =
                    source + static_cast<size_t>(index) * stride;
                float position[3];
                float uv[2];
                D3DCOLOR color;
                memcpy(position, vertex, sizeof(position));
                memcpy(&color, vertex + 16U, sizeof(color));
                memcpy(uv, vertex + 20U, sizeof(uv));
                PspClientVertex &output = destination[index];
                float ignoredFogCoordinate;
                TransformPosition(position, true, &output.x, &output.y,
                                  &output.z, &ignoredFogCoordinate);
                output.z = 1.0f - 2.0f * output.z;
                color = EffectiveColor(color);
                output.r = static_cast<GLubyte>((color >> 16) & 255U);
                output.g = static_cast<GLubyte>((color >> 8) & 255U);
                output.b = static_cast<GLubyte>(color & 255U);
                output.a = static_cast<GLubyte>((color >> 24) & 255U);
                output.u = uv[0];
                output.v = uv[1];
            }
        };
        if (pairVertexCount != 0U)
            convertRange(pairData, pairVertexCount, nativePairs);
        if (quadVertexCount != 0U)
            convertRange(quadData, quadVertexCount, nativeQuads);

        if (__pspgl_th08_draw_native_mixed_quads(
                nativePairs, static_cast<unsigned>(pairVertexBytes),
                nativeQuads, static_cast<unsigned>(quadVertexBytes),
                quadCount, quadIndices,
                static_cast<unsigned>(quadIndexBytes), quadIndexCount) == 0)
        {
            return reject();
        }

        pspBulletDirectGeVertexCursor += nativeVertexCount;
        if (pspBulletDirectGeArenaHighWater <
            pspBulletDirectGeVertexCursor)
        {
            pspBulletDirectGeArenaHighWater =
                pspBulletDirectGeVertexCursor;
        }
        if (itemOwner)
        {
            ++pspItemMixedGeSubmittedBatches;
            pspItemMixedGeSubmittedQuads += pairCount + quadCount;
        }
        else
        {
            ++pspBulletMixedGeSubmittedBatches;
            pspBulletMixedGeSubmittedQuads += pairCount + quadCount;
            ++pspBulletDirectGeSubmittedBatches;
            pspBulletDirectGeSubmittedQuads += pairCount + quadCount;
        }
        const UINT submittedVertices = pairVertexCount + quadIndexCount;
        th08::psp::RenderPerfNoteDraw(submittedVertices);
#if defined(TH08_PSP_RENDER_PERF_DIAG)
        perfDrawTicks += SDL_GetPerformanceCounter() - drawStart;
        ++perfDrawCalls;
        perfVertices += submittedVertices;
#endif
        return true;
    }
#endif
  private:
#endif

    HRESULT Draw(D3DPRIMITIVETYPE type, UINT primitiveCount, const BYTE *data, UINT stride)
    {
#if TH08_PSP_SWAP_NOWAIT_ENABLED
        PspWaitForPendingFlip();
#endif
#if defined(PSP) && defined(TH08_PSP_RENDER_PERF_DIAG)
        const Uint64 drawStart = SDL_GetPerformanceCounter();
#endif
        UINT count = VertexCount(type, primitiveCount);
#if defined(PSP)
        th08::psp::RenderPerfNoteDraw(count);
#endif
        bool transformed = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
        UINT offset = transformed ? 16 : 12;
        if (fvf & D3DFVF_NORMAL) offset += 12;
        if (fvf & D3DFVF_PSIZE) offset += 4;
        bool hasDiffuse = (fvf & D3DFVF_DIFFUSE) != 0; UINT colorOffset = offset;
        if (hasDiffuse) offset += 4;
        if (fvf & D3DFVF_SPECULAR) offset += 4;
        bool hasTexture = (fvf & D3DFVF_TEXCOUNT_MASK) != 0; UINT textureOffset = offset;
        PrepareState(transformed);
#if defined(PSP)
        // PSPGL immediate mode performs several guest calls for every vertex.
        // TH07's successful renderer submits whole batches to GE; use the GL
        // client-array equivalent here so one D3D draw becomes one PSPGL/GE
        // submission.  GE now applies 3D fog after texture sampling, so XYZ
        // vertices no longer need a duplicate world/view/projection pass on SC.
        {
            if (count > pspDrawVertexCapacity)
            {
                UINT newCapacity = pspDrawVertexCapacity != 0 ? pspDrawVertexCapacity : 256;
                while (newCapacity < count && newCapacity <= 0x3fffffffu)
                    newCapacity *= 2;
                if (newCapacity < count)
                    newCapacity = count;
                void *newStorage = realloc(pspDrawVertices,
                                           static_cast<size_t>(newCapacity) *
                                               sizeof(PspClientVertex));
                if (newStorage != NULL)
                {
                    pspDrawVertices = static_cast<PspClientVertex *>(newStorage);
                    pspDrawVertexCapacity = newCapacity;
                }
            }
            if (count <= pspDrawVertexCapacity)
            {
                for (UINT index = 0; index < count; ++index)
                {
                    const BYTE *vertex = data + index * stride;
                    PspClientVertex &output = pspDrawVertices[index];
                    const float *position = reinterpret_cast<const float *>(vertex);
                    if (transformed)
                    {
                        float ignoredFogCoordinate;
                        TransformPosition(position, true, &output.x, &output.y,
                                          &output.z, &ignoredFogCoordinate);
                        output.z = 1.0f - 2.0f * output.z;
                    }
                    else
                    {
                        output.x = position[0];
                        output.y = position[1];
                        output.z = position[2];
                    }
                    D3DCOLOR color = hasDiffuse
                                         ? *reinterpret_cast<const D3DCOLOR *>(vertex + colorOffset)
                                         : 0xffffffffu;
                    color = EffectiveColor(color);
                    output.r = static_cast<GLubyte>((color >> 16) & 255);
                    output.g = static_cast<GLubyte>((color >> 8) & 255);
                    output.b = static_cast<GLubyte>(color & 255);
                    output.a = static_cast<GLubyte>((color >> 24) & 255);
                    output.u = output.v = 0.0f;
                    if (hasTexture)
                    {
                        const float *uv = reinterpret_cast<const float *>(vertex + textureOffset);
                        output.u = uv[0];
                        output.v = uv[1];
                        if (!transformed)
                        {
                            output.u = uv[0] * textureTransform._11 +
                                       uv[1] * textureTransform._21 + textureTransform._31;
                            output.v = uv[0] * textureTransform._12 +
                                       uv[1] * textureTransform._22 + textureTransform._32;
                        }
                    }
                }

                glEnableClientState(GL_VERTEX_ARRAY);
                glEnableClientState(GL_COLOR_ARRAY);
                glVertexPointer(3, GL_FLOAT, sizeof(PspClientVertex), &pspDrawVertices[0].x);
                glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(PspClientVertex),
                               &pspDrawVertices[0].r);
                if (hasTexture)
                {
                    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                    glTexCoordPointer(2, GL_FLOAT, sizeof(PspClientVertex),
                                      &pspDrawVertices[0].u);
                }
                else
                {
                    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
                }
#if TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED
                th08::psp::DrawPrioritySubprofileNoteDraw(static_cast<std::uint32_t>(count));
#endif
                glDrawArrays(PrimitiveMode(type), 0, static_cast<GLsizei>(count));
                glDisableClientState(GL_VERTEX_ARRAY);
                glDisableClientState(GL_COLOR_ARRAY);
                if (hasTexture) glDisableClientState(GL_TEXTURE_COORD_ARRAY);
                th08::psp::RenderPerfNoteStateEmitted(hasTexture ? 9U : 7U);
#if defined(TH08_PSP_RENDER_PERF_DIAG)
                perfDrawTicks += SDL_GetPerformanceCounter() - drawStart;
                ++perfDrawCalls;
                perfVertices += count;
#endif
                return S_OK;
            }
        }
#endif
        glBegin(PrimitiveMode(type));
        for (UINT index = 0; index < count; ++index)
        {
            const BYTE *vertex = data + index * stride; float x, y, z, fogCoordinate;
#if defined(PSP)
            const float *position = reinterpret_cast<const float *>(vertex);
            if (transformed)
            {
                TransformPosition(position, true, &x, &y, &z, &fogCoordinate);
                z = 1.0f - 2.0f * z;
            }
            else
            {
                x = position[0];
                y = position[1];
                z = position[2];
                fogCoordinate = 0.0f;
            }
#else
            TransformPosition(reinterpret_cast<const float *>(vertex), transformed,
                              &x, &y, &z, &fogCoordinate);
            z = 1.0f - 2.0f * z;
#endif
            D3DCOLOR color = hasDiffuse ? *reinterpret_cast<const D3DCOLOR *>(vertex + colorOffset) : 0xffffffffu;
            color = EffectiveColor(color);
#if defined(PSP)
            (void)fogCoordinate;
#endif
            if (hasTexture)
            {
                const float *uv = reinterpret_cast<const float *>(vertex + textureOffset);
                float u = uv[0], v = uv[1];
                if (!transformed)
                {
                    u = uv[0] * textureTransform._11 + uv[1] * textureTransform._21 + textureTransform._31;
                    v = uv[0] * textureTransform._12 + uv[1] * textureTransform._22 + textureTransform._32;
                }
                glTexCoord2f(u, v);
            }
            glColor4ub((color >> 16) & 255, (color >> 8) & 255, color & 255, (color >> 24) & 255);
#if !defined(PSP)
            if (g_fogCoordf != NULL)
                g_fogCoordf(fogCoordinate);
#endif
            glVertex3f(x, y, z);
        }
        glEnd();
#if defined(PSP)
#if defined(TH08_PSP_RENDER_PERF_DIAG)
        perfDrawTicks += SDL_GetPerformanceCounter() - drawStart;
        ++perfDrawCalls;
        perfVertices += count;
#endif
#endif
        return S_OK;
    }
    ULONG refs;
    SDL_Window *window;
    SDL_GLContext context;
    LinuxSurface *backbuffer;
    LinuxTexture *texture;
    LinuxVertexBuffer *vertexBuffer;
    DWORD fvf;
    UINT streamStride;
    GLuint renderFramebuffer, renderColorTexture, renderDepthBuffer, dialogueSnapshotTexture;
    LinuxSurface *dialogueSnapshotSurface;
    bool framebufferReady, dialogueSnapshotReady;
    bool dialogueRestoreFailureLogged, wasDialogPresent;
    unsigned long presentCount;
#if TH08_PSP_SWAP_NOWAIT_ENABLED
    unsigned int pspSwapVcount = 0U;
    bool pspSwapFlipPending = false;
#endif
    unsigned long dialogueCaptureAttempts, dialogueCaptureSuccesses;
    unsigned long dialogueCaptureFailures, dialogueRestoreCount;
    unsigned long dialogueRestoreFailures, dialogueRestoresThisCapture;
    unsigned long dialogueLateAllocationCount, dialogueLateHeapBytes;
    unsigned long dialogueCapturePresent;
    bool dialogueCaptureUsedArenaFallback;
    std::vector<BYTE> probePixels;
    int probeLeft, probeBottom, probeWidth, probeHeight;
#if defined(PSP)
#if defined(TH08_PSP_RENDER_PERF_DIAG)
    Uint64 perfFrequency, perfLastPresent, perfDrawTicks, perfPresentTicks;
    Uint64 perfSwapTicks, perfFrameTicks;
    unsigned long perfDrawCalls, perfVertices, perfFrames;
#endif
    PspClientVertex *pspDrawVertices;
    UINT pspDrawVertexCapacity;
#if TH08_PSP_PSPGL_STREAM_ARENA_ENABLED
    void *pspStreamArenaLease;
    unsigned long pspStreamArenaPresent;
    unsigned long pspStreamArenaLeaseFrames;
    unsigned long pspStreamArenaAllocationFailures;
    unsigned long pspStreamArenaReleaseFailures;
    unsigned long pspStreamArenaAllocs;
    unsigned long pspStreamArenaOverflows;
    unsigned long pspStreamArenaPeakBytes;
    unsigned long pspStreamArenaOrphanAddress;
    bool pspStreamArenaFaulted;
#endif
#if TH08_PSP_SCORE_POPUP_NATIVE_GE_ENABLED
    unsigned long pspScorePopupNativeAttempts = 0U;
    unsigned long pspScorePopupNativeSubmits = 0U;
    unsigned long pspScorePopupNativeClientFallbacks = 0U;
#endif
#if TH08_PSP_BULLET_DIRECT_GE_ENABLED
    PspClientVertex *pspBulletDirectGeVertices = NULL;
    PspClientVertex *pspBulletDirectGeVerticesBase = NULL;
    UINT pspBulletDirectGeVertexCursor = 0U;
    UINT pspBulletDirectGeArenaHighWater = 0U;
    unsigned long pspBulletDirectGeArenaPresent = ~0UL;
    unsigned long pspBulletDirectGeSubmittedBatches = 0U;
    unsigned long pspBulletDirectGeSubmittedQuads = 0U;
    unsigned long pspBulletDirectGeFallbacks = 0U;
#if TH08_PSP_BULLET_PACKED_VERTEX_AUDIT_ENABLED
    PspBulletPackedVertexAuditStats pspBulletPackedVertexAudit{};
#endif
    // Unconditional within direct-GE: M1 OFF/ON must not perturb the device
    // allocation size, following the accepted fixed-reservation A/B policy.
    PspBulletPackedVertexFastpathStorageStats
        pspBulletPackedVertexFastpath{};
    bool pspBulletPackedVertexBatchActive = false;
    bool pspBulletPackedVertexRunActive = false;
    UINT pspBulletPackedVertexRunStart = 0U;
    UINT pspBulletPackedVertexRunQuads = 0U;
    unsigned long pspBulletPackedVertexRunPresent = ~0UL;
#if TH08_PSP_BULLET_MIXED_QUADS_FASTPATH_ENABLED || \
    TH08_PSP_ITEM_MIXED_QUADS_FASTPATH_ENABLED
    unsigned long pspBulletMixedGeAttempts = 0U;
    unsigned long pspBulletMixedGeSubmittedBatches = 0U;
    unsigned long pspBulletMixedGeSubmittedQuads = 0U;
    unsigned long pspBulletMixedGeFallbacks = 0U;
    unsigned long pspBulletMixedGeArenaExhaustions = 0U;
    unsigned long pspItemMixedGeAttempts = 0U;
    unsigned long pspItemMixedGeSubmittedBatches = 0U;
    unsigned long pspItemMixedGeSubmittedQuads = 0U;
    unsigned long pspItemMixedGeFallbacks = 0U;
    unsigned long pspItemMixedGeArenaExhaustions = 0U;
#endif
#endif
#if TH08_PSP_ITEM_DIRECT_GE_ENABLED
    PspClientVertex *pspItemDirectGeVertices = NULL;
    bool pspItemDirectGeAllocationAttempted = false;
    bool pspItemDirectGeFaulted = false;
    UINT pspItemDirectGeVertexCursor = 0U;
    UINT pspItemDirectGeArenaHighWater = 0U;
    unsigned long pspItemDirectGeArenaPresent = ~0UL;
    unsigned long pspItemDirectGeSubmittedBatches = 0U;
    unsigned long pspItemDirectGeSubmittedQuads = 0U;
    unsigned long pspItemDirectGeFallbacks = 0U;
#endif
#if TH08_PSP_ANY_DIRECT_GE_ENABLED
    const unsigned short *pspBulletDirectGeIndexAuthority = NULL;
    UINT pspBulletDirectGeValidatedIndexCount = 0U;
    bool pspBulletDirectGeIndexAuthorityRejected = false;
#endif
#endif
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
    PspPrepareStateCache pspPrepareStateCache{};
    unsigned int pspWorldRawVersion = 1;
    unsigned int pspViewRawVersion = 1;
    unsigned int pspProjectionRawVersion = 1;
#endif
    DWORD renderStates[256], textureStates[32];
    D3DMATRIX world, view, projection, textureTransform;
    D3DVIEWPORT8 viewport;
};

class LinuxDirect3D : public IDirect3D8
{
  public:
    LinuxDirect3D() : refs(1) {}
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG value = --refs; if (value == 0) delete this; return value; }
    HRESULT GetAdapterDisplayMode(UINT, D3DDISPLAYMODE *mode)
    {
        if (mode == NULL) return E_INVALIDARG;
        mode->Width = 640; mode->Height = 480; mode->RefreshRate = 60; mode->Format = D3DFMT_X8R8G8B8; return S_OK;
    }
    HRESULT CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT) { return S_OK; }
    HRESULT CreateDevice(UINT, D3DDEVTYPE, HWND window, DWORD, D3DPRESENT_PARAMETERS *parameters,
                         IDirect3DDevice8 **result)
    {
        TH08_PSP_BOOT_CHECKPOINT("create_device", "before", 0);
        if (window == NULL || parameters == NULL || result == NULL)
        {
            TH08_PSP_BOOT_CHECKPOINT("create_device", "after", E_INVALIDARG);
            return E_INVALIDARG;
        }
        TH08_PSP_BOOT_CHECKPOINT("create_device", "before_allocate", 0);
        LinuxDevice *device = new(std::nothrow) LinuxDevice(reinterpret_cast<SDL_Window *>(window), *parameters);
        TH08_PSP_BOOT_CHECKPOINT("create_device", "after_allocate",
                                 device != NULL ? 1 : 0);
        if (device == NULL)
        {
            TH08_PSP_BOOT_CHECKPOINT("create_device", "after", E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        const bool ready = device->Ready();
        TH08_PSP_BOOT_CHECKPOINT("create_device", "ready", ready ? 1 : 0);
        if (!ready)
        {
            TH08_PSP_BOOT_CHECKPOINT("create_device", "before_failed_delete", 0);
            delete device;
            TH08_PSP_BOOT_CHECKPOINT("create_device", "after_failed_delete", 0);
            *result = NULL;
            TH08_PSP_BOOT_CHECKPOINT("create_device", "after", E_FAIL);
            return E_FAIL;
        }
        *result = device;
        TH08_PSP_BOOT_CHECKPOINT("create_device", "after", S_OK);
        return S_OK;
    }
  private: ULONG refs;
};
} // namespace

#if TH08_PSP_BULLET_DIRECT_GE_ENABLED
void th08_psp_bullet_direct_ge_set_batch(bool active)
{
    g_PspBulletDirectGeBatchActive = active;
}
#elif defined(PSP)
void th08_psp_bullet_direct_ge_set_batch(bool)
{
}
#endif

#if TH08_PSP_ITEM_MIXED_QUADS_FASTPATH_ENABLED
void th08_psp_item_mixed_ge_set_batch(bool active)
{
    g_PspItemMixedGeBatchActive = active;
}
#elif defined(PSP)
void th08_psp_item_mixed_ge_set_batch(bool)
{
}
#endif

#if TH08_PSP_ITEM_DIRECT_GE_ENABLED
void th08_psp_item_direct_ge_set_batch(bool active)
{
    g_PspItemDirectGeBatchActive = active;
}

bool th08_psp_item_direct_ge_release_stage(IDirect3DDevice8 *deviceRaw)
{
    if (deviceRaw == NULL)
        return false;
    return static_cast<LinuxDevice *>(deviceRaw)
        ->ReleasePspItemDirectGeStageArena();
}
#elif defined(PSP)
void th08_psp_item_direct_ge_set_batch(bool)
{
}

bool th08_psp_item_direct_ge_release_stage(IDirect3DDevice8 *)
{
    return true;
}
#endif

#if TH08_PSP_ASCII_POPUP_BATCH_ENABLED
bool th08_psp_reserve_ascii_popup_sprite_pairs(IDirect3DDevice8 *deviceRaw,
                                                UINT spriteCount)
{
    if (deviceRaw == NULL)
        return false;
    return static_cast<LinuxDevice *>(deviceRaw)
        ->ReservePspSpritePairs(spriteCount);
}

bool th08_psp_draw_ascii_popup_sprite_pairs(IDirect3DDevice8 *deviceRaw,
                                             const void *vertices,
                                             UINT spriteCount, UINT stride)
{
    if (deviceRaw == NULL)
        return false;
    return static_cast<LinuxDevice *>(deviceRaw)
        ->DrawPspSpritePairs(static_cast<const BYTE *>(vertices),
                            spriteCount, stride, true);
}
#endif

#if TH08_PSP_SCORE_POPUP_NATIVE_GE_ENABLED
bool th08_psp_query_score_popup_native_ge_stats(
    IDirect3DDevice8 *deviceRaw, PspScorePopupNativeGeStats *stats)
{
    if (deviceRaw == NULL || stats == NULL)
        return false;
    return static_cast<LinuxDevice *>(deviceRaw)
        ->QueryPspScorePopupNativeGeStats(stats);
}
#endif

#if TH08_PSP_BULLET_PACKED_VERTEX_AUDIT_ENABLED
bool th08_psp_query_bullet_packed_vertex_audit_stats(
    IDirect3DDevice8 *deviceRaw, PspBulletPackedVertexAuditStats *stats)
{
    if (deviceRaw == NULL || stats == NULL)
        return false;
    return static_cast<LinuxDevice *>(deviceRaw)
        ->QueryPspBulletPackedVertexAuditStats(stats);
}
#endif

#if TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH_ENABLED
bool th08_psp_bullet_packed_vertex_begin(IDirect3DDevice8 *deviceRaw)
{
    if (deviceRaw == NULL)
        return false;
    return static_cast<LinuxDevice *>(deviceRaw)
        ->BeginPspBulletPackedVertexBatch();
}

bool th08_psp_bullet_packed_vertex_append(IDirect3DDevice8 *deviceRaw,
                                          const void *quad, UINT stride)
{
    if (deviceRaw == NULL)
        return false;
    return static_cast<LinuxDevice *>(deviceRaw)
        ->AppendPspBulletPackedVertexQuad(
            static_cast<const BYTE *>(quad), stride);
}

bool th08_psp_bullet_packed_vertex_submit(
    IDirect3DDevice8 *deviceRaw, UINT quadCount,
    const unsigned short *indices, UINT indexCount)
{
    if (deviceRaw == NULL)
        return false;
    return static_cast<LinuxDevice *>(deviceRaw)
        ->SubmitPspBulletPackedVertexRun(
            quadCount, indices, indexCount);
}

void th08_psp_bullet_packed_vertex_end(IDirect3DDevice8 *deviceRaw)
{
    if (deviceRaw == NULL)
        return;
    static_cast<LinuxDevice *>(deviceRaw)
        ->EndPspBulletPackedVertexBatch();
}

void th08_psp_bullet_packed_vertex_note_recovery_split(
    IDirect3DDevice8 *deviceRaw, UINT submittedQuads)
{
    if (deviceRaw == NULL)
        return;
    static_cast<LinuxDevice *>(deviceRaw)
        ->NotePspBulletPackedVertexRecoverySplit(submittedQuads);
}

bool th08_psp_query_bullet_packed_vertex_fastpath_stats(
    IDirect3DDevice8 *deviceRaw,
    PspBulletPackedVertexFastpathStats *stats)
{
    if (deviceRaw == NULL || stats == NULL)
        return false;
    return static_cast<LinuxDevice *>(deviceRaw)
        ->QueryPspBulletPackedVertexFastpathStats(stats);
}
#endif

#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED
PspItemNaturalQuadSubmitResult th08_psp_draw_item_natural_quads(
    IDirect3DDevice8 *deviceRaw, const void *canonicalVertices,
    UINT quadCount, UINT stride, const unsigned short *indices,
    UINT indexCount)
{
    if (deviceRaw == NULL)
        return PSP_ITEM_NATURAL_REJECT_DEVICE;
    return static_cast<LinuxDevice *>(deviceRaw)->DrawPspItemNaturalQuads(
        static_cast<const BYTE *>(canonicalVertices), quadCount, stride,
        indices, indexCount);
}
#endif

#if TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH_ENABLED || \
    TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH_ENABLED
bool th08_psp_draw_sprite_pairs_in_place(IDirect3DDevice8 *deviceRaw,
                                         void *vertices,
                                         UINT spriteCount, UINT stride)
{
    if (deviceRaw == NULL)
        return false;
    return static_cast<LinuxDevice *>(deviceRaw)
        ->DrawPspSpritePairsInPlace(static_cast<BYTE *>(vertices),
                                   spriteCount, stride);
}
#endif

#if TH08_PSP_BULLET_MIXED_QUADS_FASTPATH_ENABLED
bool th08_psp_draw_bullet_mixed_quads(
    IDirect3DDevice8 *deviceRaw, const void *pairVertices, UINT pairCount,
    const void *quadVertices, UINT quadCount, UINT stride,
    const unsigned short *quadIndices, UINT quadIndexCount)
{
    if (deviceRaw == NULL)
        return false;
    return static_cast<LinuxDevice *>(deviceRaw)->DrawPspBulletMixedQuads(
        static_cast<const BYTE *>(pairVertices), pairCount,
        static_cast<const BYTE *>(quadVertices), quadCount, stride,
        quadIndices, quadIndexCount, false);
}
#endif

#if TH08_PSP_ITEM_MIXED_QUADS_FASTPATH_ENABLED
bool th08_psp_draw_item_mixed_quads(
    IDirect3DDevice8 *deviceRaw, const void *pairVertices, UINT pairCount,
    const void *quadVertices, UINT quadCount, UINT stride,
    const unsigned short *quadIndices, UINT quadIndexCount)
{
    if (deviceRaw == NULL)
        return false;
    return static_cast<LinuxDevice *>(deviceRaw)->DrawPspBulletMixedQuads(
        static_cast<const BYTE *>(pairVertices), pairCount,
        static_cast<const BYTE *>(quadVertices), quadCount, stride,
        quadIndices, quadIndexCount, true);
}
#endif

#if TH08_PSP_BULLET_MIXED_QUADS_FASTPATH_ENABLED || \
    TH08_PSP_ITEM_MIXED_QUADS_FASTPATH_ENABLED
bool th08_psp_query_mixed_ge_backend_stats(
    IDirect3DDevice8 *deviceRaw, PspMixedGeBackendStats *stats)
{
    if (deviceRaw == NULL || stats == NULL)
        return false;
    return static_cast<LinuxDevice *>(deviceRaw)
        ->QueryPspMixedGeBackendStats(stats);
}
#endif

bool th08_linux_surface_access(IDirect3DSurface8 *surfaceRaw, LinuxSurfaceAccess *access, bool readBackbuffer)
{
    if (surfaceRaw == NULL || access == NULL) return false;
    LinuxSurface *surface = static_cast<LinuxSurface *>(surfaceRaw);
#if defined(PSP)
    if (surface->backbuffer) gPspShadowReason = readBackbuffer ? "surface_access(read)" : "surface_access(dest)";
#endif
    if (!surface->EnsurePixels()) return false;
    // PSP capture releases the logical backbuffer shadow after each use to
    // avoid pinning a full frame in the fragmented game heap.  Recreate that
    // storage before ReadBackbuffer writes into it on the next transition.
    if (surface->backbuffer || readBackbuffer) surface->ReadBackbuffer();
    access->pixels = surface->pixels.empty() ? NULL : &surface->pixels[0];
    access->width = surface->width; access->height = surface->height;
    access->pitch = surface->pitch; access->format = surface->format; return true;
}

void th08_linux_surface_access_end(IDirect3DSurface8 *surfaceRaw)
{
#if defined(PSP)
    if (surfaceRaw == NULL) return;
    static_cast<LinuxSurface *>(surfaceRaw)->DropScratchShadow();
#else
    (void)surfaceRaw;
#endif
}
#if defined(PSP)
// Capture straight from the backbuffer shadow into the destination texture's
// live PSPGL image with glTexSubImage2D.  The generic D3DX copy first
// materializes the destination's full CPU copy (512 KiB for a 512x512 text
// texture) from the renderer arena; when a stage's textures fill the arena that
// allocation fails and the game aborted (R-057 stage 4B).  This path needs only
// a 16 KiB static band buffer.  Returns false to let the generic route run.
static RECT PspCaptureFullRect(UINT width, UINT height)
{
    RECT r; r.left = 0; r.top = 0; r.right = static_cast<LONG>(width); r.bottom = static_cast<LONG>(height); return r;
}
static bool PspCaptureValidRect(const RECT &r)
{
    return r.left >= 0 && r.top >= 0 && r.right > r.left && r.bottom > r.top;
}
static BYTE gPspCaptureBand[16384];
static unsigned long gPspCaptureDirectOk = 0UL;
static unsigned long gPspCaptureDirectFail = 0UL;
bool th08_linux_capture_direct_to_texture(IDirect3DSurface8 *destinationRaw, const RECT *destinationRectRaw,
                                          const LinuxSurfaceAccess &source, const RECT *sourceRectRaw,
                                          D3DCOLOR colorKey)
{
    if (destinationRaw == NULL || source.pixels == NULL) return false;
    LinuxSurface *destination = static_cast<LinuxSurface *>(destinationRaw);
    if (destination->backbuffer || destination->owner == NULL) return false;
    LinuxTexture *texture = destination->owner;
    // Only when the CPU copy is absent (static/capture texture) and the GPU
    // image exists; otherwise the generic path is allocation-free already.
    if (texture->glName == 0 || !texture->uploaded || !destination->pixels.empty()) return false;
    if (destination->width == 0 || destination->height == 0) return false;
    const UINT sourceBytes = BytesPerPixel(source.format);
    if (!(source.format == D3DFMT_R5G6B5 && sourceBytes == 2) &&
        !((source.format == D3DFMT_X8R8G8B8 || source.format == D3DFMT_A8R8G8B8) && sourceBytes == 4))
        return false;
    GLenum glFormat = GL_RGBA;
    GLenum glType = GL_UNSIGNED_BYTE;
    UINT destinationBytes = 4;
    if (texture->pspGlType != 0)
    {
        glFormat = texture->pspGlFormat;
        glType = texture->pspGlType;
        destinationBytes = glType == GL_UNSIGNED_BYTE ? (glFormat == GL_RGB ? 3 : 4) : 2;
    }
    else
    switch (destination->format)
    {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        break;
    case D3DFMT_A4R4G4B4:
        glType = GL_UNSIGNED_SHORT_4_4_4_4; destinationBytes = 2; break;
    case D3DFMT_A1R5G5B5:
    case D3DFMT_X1R5G5B5:
        glType = GL_UNSIGNED_SHORT_5_5_5_1; destinationBytes = 2; break;
    case D3DFMT_R5G6B5:
        glFormat = GL_RGB; glType = GL_UNSIGNED_SHORT_5_6_5; destinationBytes = 2; break;
    default:
        return false;
    }
    RECT sourceRect = sourceRectRaw != NULL ? *sourceRectRaw : PspCaptureFullRect(source.width, source.height);
    RECT destinationRect = destinationRectRaw != NULL ? *destinationRectRaw
                                                      : PspCaptureFullRect(destination->width, destination->height);
    if (!PspCaptureValidRect(sourceRect) || !PspCaptureValidRect(destinationRect) ||
        sourceRect.right > static_cast<LONG>(source.width) ||
        sourceRect.bottom > static_cast<LONG>(source.height))
        return false;
    if (destinationRect.right > static_cast<LONG>(destination->width))
        destinationRect.right = static_cast<LONG>(destination->width);
    if (destinationRect.bottom > static_cast<LONG>(destination->height))
        destinationRect.bottom = static_cast<LONG>(destination->height);
    if (!PspCaptureValidRect(destinationRect)) return false;
    const UINT destinationWidth = static_cast<UINT>(destinationRect.right - destinationRect.left);
    const UINT destinationHeight = static_cast<UINT>(destinationRect.bottom - destinationRect.top);
    const UINT sourceWidth = static_cast<UINT>(sourceRect.right - sourceRect.left);
    const UINT sourceHeight = static_cast<UINT>(sourceRect.bottom - sourceRect.top);
    const UINT rowBytes = destinationWidth * destinationBytes;
    if (rowBytes == 0 || rowBytes > sizeof(gPspCaptureBand)) return false;
    const UINT bandRows = static_cast<UINT>(sizeof(gPspCaptureBand) / rowBytes);
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
    PspPrepareStateBoundary stateBoundary(g_pspPrepareStateCache);
    texture->pspSamplerValidMask = 0;
#endif
    while (glGetError() != GL_NO_ERROR) {}
    glBindTexture(GL_TEXTURE_2D, texture->glName);
    glPixelStorei(GL_UNPACK_ALIGNMENT, glType == GL_UNSIGNED_BYTE ? 1 : 2);
    GLenum error = GL_NO_ERROR;
    for (UINT y0 = 0; y0 < destinationHeight && error == GL_NO_ERROR; y0 += bandRows)
    {
        const UINT rows = (destinationHeight - y0 < bandRows) ? destinationHeight - y0 : bandRows;
        for (UINT r = 0; r < rows; ++r)
        {
            const UINT y = y0 + r;
            const UINT sourceY = static_cast<UINT>(sourceRect.top) +
                static_cast<UINT>((static_cast<unsigned long long>(y) * sourceHeight) / destinationHeight);
            const BYTE *sourceRow = source.pixels + sourceY * source.pitch;
            BYTE *out = gPspCaptureBand + r * rowBytes;
            for (UINT x = 0; x < destinationWidth; ++x)
            {
                const UINT sourceX = static_cast<UINT>(sourceRect.left) +
                    static_cast<UINT>((static_cast<unsigned long long>(x) * sourceWidth) / destinationWidth);
                BYTE red, green, blue, alpha = 0xFF;
                if (sourceBytes == 2)
                {
                    const WORD v = *reinterpret_cast<const WORD *>(sourceRow + sourceX * 2);
                    red = static_cast<BYTE>(((v >> 11) & 0x1F) * 255 / 31);
                    green = static_cast<BYTE>(((v >> 5) & 0x3F) * 255 / 63);
                    blue = static_cast<BYTE>((v & 0x1F) * 255 / 31);
                }
                else
                {
                    const BYTE *p = sourceRow + sourceX * 4;
                    blue = p[0]; green = p[1]; red = p[2];
                    alpha = source.format == D3DFMT_A8R8G8B8 ? p[3] : 0xFF;
                }
                if (colorKey != 0 && (colorKey & 0x00ffffffu) ==
                    ((static_cast<DWORD>(red) << 16) | (static_cast<DWORD>(green) << 8) | blue))
                    alpha = 0;
                switch (glType)
                {
                case GL_UNSIGNED_SHORT_4_4_4_4:
                    *reinterpret_cast<WORD *>(out + x * 2) = static_cast<WORD>(
                        ((red >> 4) << 12) | ((green >> 4) << 8) | ((blue >> 4) << 4) | (alpha >> 4));
                    break;
                case GL_UNSIGNED_SHORT_5_5_5_1:
                    *reinterpret_cast<WORD *>(out + x * 2) = static_cast<WORD>(
                        ((red >> 3) << 11) | ((green >> 3) << 6) | ((blue >> 3) << 1) | (alpha >> 7));
                    break;
                case GL_UNSIGNED_SHORT_5_6_5:
                    *reinterpret_cast<WORD *>(out + x * 2) = static_cast<WORD>(
                        ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
                    break;
                default:
                    if (destinationBytes == 3)
                    {
                        out[x * 3 + 0] = red; out[x * 3 + 1] = green; out[x * 3 + 2] = blue;
                    }
                    else
                    {
                        out[x * 4 + 0] = red; out[x * 4 + 1] = green; out[x * 4 + 2] = blue; out[x * 4 + 3] = alpha;
                    }
                    break;
                }
            }
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, destinationRect.left, destinationRect.top + static_cast<GLint>(y0),
                        static_cast<GLsizei>(destinationWidth), static_cast<GLsizei>(rows), glFormat, glType,
                        gPspCaptureBand);
        error = glGetError();
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    if (error != GL_NO_ERROR)
    {
        ++gPspCaptureDirectFail;
        if (gPspCaptureDirectFail <= 8UL)
            th08::psp::BootLog("CAPTURE_DIRECT fail error=0x%04x dst=%lux%lu fmt=%lu rect=%ldx%ld\n",
                               static_cast<unsigned int>(error), static_cast<unsigned long>(destination->width),
                               static_cast<unsigned long>(destination->height),
                               static_cast<unsigned long>(destination->format),
                               static_cast<long>(destinationWidth), static_cast<long>(destinationHeight));
        return false;
    }
    destination->dirty = false;
    destination->pspBlitTextureDirty = true;
    texture->uploaded = true;
    ++gPspCaptureDirectOk;
    if (gPspCaptureDirectOk <= 4UL || (gPspCaptureDirectOk % 64UL) == 0UL)
        th08::psp::BootLog("CAPTURE_DIRECT ok count=%lu dst=%lux%lu fmt=%lu rect=%ldx%ld at=%ld,%ld src=%ld,%ld\n",
                           gPspCaptureDirectOk, static_cast<unsigned long>(destination->width),
                           static_cast<unsigned long>(destination->height),
                           static_cast<unsigned long>(destination->format), static_cast<long>(destinationWidth),
                           static_cast<long>(destinationHeight), static_cast<long>(destinationRect.left),
                           static_cast<long>(destinationRect.top), static_cast<long>(sourceRect.left),
                           static_cast<long>(sourceRect.top));
    return true;
}
#endif
void th08_linux_surface_changed(IDirect3DSurface8 *surfaceRaw)
{
    if (surfaceRaw == NULL) return;
    LinuxSurface *surface = static_cast<LinuxSurface *>(surfaceRaw);
    surface->dirty = true;
    if (surface->owner != NULL) surface->owner->uploaded = false;
    surface->FlushBackbuffer();
}

void th08_linux_texture_mark_static(IDirect3DTexture8 *textureRaw)
{
    if (textureRaw == NULL) return;
    LinuxTexture *texture = static_cast<LinuxTexture *>(textureRaw);
    texture->discardCpuCopy = true;
}

#if defined(PSP)
void th08_linux_surface_mark_static(IDirect3DSurface8 *surfaceRaw)
{
    if (surfaceRaw == NULL) return;
    LinuxSurface *surface = static_cast<LinuxSurface *>(surfaceRaw);
    if (!surface->backbuffer)
        surface->pspDiscardCpuCopy = true;
}

void th08_linux_surface_discard_readback(IDirect3DSurface8 *surfaceRaw)
{
    if (surfaceRaw == NULL) return;
    LinuxSurface *surface = static_cast<LinuxSurface *>(surfaceRaw);
    if (surface->backbuffer)
        std::vector<BYTE>().swap(surface->pixels);
}

bool th08_linux_surface_capture_native(UINT logicalWidth, UINT logicalHeight,
                                       bool readDisplayedFrame,
                                       IDirect3DSurface8 **surfaceRaw)
{
    if (surfaceRaw == NULL || logicalWidth == 0 || logicalHeight == 0)
        return false;
    *surfaceRaw = NULL;

    LinuxSurface *surface = new(std::nothrow) LinuxSurface(
        logicalWidth, logicalHeight, D3DFMT_R5G6B5, false, NULL, false);
    const char *failureReason = NULL;
    const bool captured = surface != NULL &&
        surface->CaptureNativeFramebuffer(readDisplayedFrame, &failureReason);
    // The phase reservation must not leak into the asynchronous ANM loader,
    // including every error path. The persistent PSPGL texture, if successful,
    // is owned by the renderer arena and no longer references these bytes.
    const bool released = th08::psp::AnmScratchReleaseTransition();
    if (!captured || !released)
    {
        if (surface != NULL)
            surface->Release();
        th08::psp::BootLog(
            "NATIVE_CAPTURE ready=0 reason=%s scratch_released=%d\n",
            failureReason != NULL
                ? failureReason
                : (!released ? "scratch_release_failed" : "capture_failed"),
            released ? 1 : 0);
        return false;
    }
    *surfaceRaw = surface;
    th08::psp::BootLog(
        "NATIVE_CAPTURE ready=1 backing=late_lifetime "
        "logical=%lux%lu read_displayed=%d\n",
        static_cast<unsigned long>(logicalWidth),
        static_cast<unsigned long>(logicalHeight),
        readDisplayedFrame ? 1 : 0);
    return true;
}

// Owner tag (ANM file name) recorded for renderer-arena census lines.
static char gPspTextureUploadOwner[32] = "";
extern "C" void th08_linux_set_texture_upload_owner(const char *name)
{
    if (name == NULL) { gPspTextureUploadOwner[0] = '\0'; return; }
    std::size_t i = 0;
    for (; i + 1 < sizeof(gPspTextureUploadOwner) && name[i] != '\0'; ++i)
        gPspTextureUploadOwner[i] = name[i];
    gPspTextureUploadOwner[i] = '\0';
}

static unsigned long gPspAnmTexUploads = 0UL;
static void PspNoteAnmTextureUpload(LinuxTexture *texture, LinuxSurface *surface, int converted)
{
    ++gPspAnmTexUploads;
    const unsigned long bytes = static_cast<unsigned long>(surface->width) * surface->height *
                                (texture->pspGlType == GL_UNSIGNED_BYTE ? 4UL : 2UL);
    th08::psp::BootLog("ANM_TEX owner=%s size=%lux%lu d3d=%lu gl=0x%04x bytes=%lu conv=%d\n",
                       gPspTextureUploadOwner[0] != '\0' ? gPspTextureUploadOwner : "?",
                       static_cast<unsigned long>(surface->width), static_cast<unsigned long>(surface->height),
                       static_cast<unsigned long>(surface->format), static_cast<unsigned int>(texture->pspGlType),
                       bytes, converted);
}
bool th08_linux_texture_upload_static(IDirect3DTexture8 *textureRaw, const void *sourceRaw,
                                      UINT sourcePitch, D3DFORMAT sourceFormat)
{
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
    PspPrepareStateBoundary stateBoundary(g_pspPrepareStateCache);
#endif
    th08::psp::RenderPerfNoteUploadAttempt();
    if (textureRaw == NULL || sourceRaw == NULL)
        return false;
    LinuxTexture *texture = static_cast<LinuxTexture *>(textureRaw);
    LinuxSurface *surface = texture->surface;
    if (surface == NULL || surface->width == 0 || surface->height == 0 ||
        surface->width > 512)
        return false;
#if TH08_PSP_PREPARE_STATE_CACHE_ENABLED
    texture->pspSamplerValidMask = 0;
#endif

    // Scope the PSPGL allocation, not the ANM decode buffer: sourceRaw remains
    // owned by the retained streaming scratch and is released by its lease.
    th08::psp::RenderResourceAllocationScope arenaScope(
        gPspTextureUploadOwner[0] != '\0' ? gPspTextureUploadOwner : "static ANM texture");
#if TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE_ENABLED
    PspGe4StaticUploadScope staticUpload(!pspSuppressStaticUploadPromotion);
#else
    PspGe4StaticUploadScope staticUpload(true);
#endif

    GLenum uploadFormat = GL_RGBA;
    GLenum uploadType = GL_UNSIGNED_SHORT_4_4_4_4;
    switch (surface->format)
    {
    case D3DFMT_A8R8G8B8:
        uploadType = GL_UNSIGNED_BYTE;
        break;
    case D3DFMT_A4R4G4B4:
        break;
    case D3DFMT_A1R5G5B5:
    case D3DFMT_X1R5G5B5:
        uploadType = GL_UNSIGNED_SHORT_5_5_5_1;
        break;
    case D3DFMT_R5G6B5:
        uploadFormat = GL_RGB;
        uploadType = GL_UNSIGNED_SHORT_5_6_5;
        break;
    default:
        return false;
    }

    if (texture->glName == 0)
        glGenTextures(1, &texture->glName);
    if (texture->glName == 0)
        return false;
    glBindTexture(GL_TEXTURE_2D, texture->glName);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    while (glGetError() != GL_NO_ERROR) {}

    const BYTE *source = static_cast<const BYTE *>(sourceRaw);
    const UINT sourceBytes = BytesPerPixel(sourceFormat);
#if TH08_PSP_ANM_TEXTURE_16BIT_ENABLED
    if (sourceFormat == D3DFMT_A8R8G8B8 && surface->format == D3DFMT_A8R8G8B8 && sourceBytes == 4 &&
        sourcePitch == surface->width * 4)
    {
        // Convert the writable decode scratch in place (2-byte writes never
        // overtake the 4-byte reads) and upload half the bytes.
        BYTE *packedSource = const_cast<BYTE *>(source);
        const UINT pixelCount = surface->width * surface->height;
        bool binaryAlpha = true;
        for (UINT i = 0; i < pixelCount; ++i)
        {
            const BYTE alpha = packedSource[i * 4 + 3];
            if (alpha != 0 && alpha != 255)
            {
                binaryAlpha = false;
                break;
            }
        }
        WORD *packed16 = reinterpret_cast<WORD *>(packedSource);
        if (binaryAlpha)
        {
            for (UINT i = 0; i < pixelCount; ++i)
            {
                const BYTE *p = packedSource + i * 4;
                packed16[i] = static_cast<WORD>(((p[2] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[0] >> 3) << 1) | (p[3] >> 7));
            }
            uploadType = GL_UNSIGNED_SHORT_5_5_5_1;
        }
        else
        {
            for (UINT i = 0; i < pixelCount; ++i)
            {
                const BYTE *p = packedSource + i * 4;
                packed16[i] = static_cast<WORD>(((p[2] >> 4) << 12) | ((p[1] >> 4) << 8) | ((p[0] >> 4) << 4) | (p[3] >> 4));
            }
            uploadType = GL_UNSIGNED_SHORT_4_4_4_4;
        }
        uploadFormat = GL_RGBA;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surface->width, surface->height, 0, GL_RGBA, uploadType,
                     packedSource);
        th08::psp::RenderPerfNoteActualUpload(pixelCount * 2U);
        const GLenum convertError = glGetError();
        if (convertError == GL_NO_ERROR)
            staticUpload.Finalize(texture->glName);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (convertError != GL_NO_ERROR)
        {
            th08::psp::BootLog("ANM_TEX convert_error=0x%04x owner=%s size=%lux%lu\n",
                               static_cast<unsigned int>(convertError), gPspTextureUploadOwner,
                               static_cast<unsigned long>(surface->width), static_cast<unsigned long>(surface->height));
            return false;
        }
        texture->uploaded = true;
        texture->discardCpuCopy = true;
        surface->dirty = false;
        texture->pspGlFormat = uploadFormat;
        texture->pspGlType = uploadType;
        PspNoteAnmTextureUpload(texture, surface, 1);
        return true;
    }
#endif
    if (sourceFormat == surface->format && sourceBytes == 4 &&
        sourcePitch == surface->width * 4)
    {
        // D3D A8R8G8B8 is stored as B,G,R,A bytes on this little-endian
        // target, while GL_RGBA/GL_UNSIGNED_BYTE consumes R,G,B,A.  The ANM
        // decode scratch is writable and is released immediately after all
        // entries have uploaded, so swap R/B there instead of materializing a
        // second 512x512x32 surface in the fragmented game heap.  Alpha and all
        // 8-bit colour channels remain exact.
        BYTE *packedSource = const_cast<BYTE *>(source);
        const UINT pixelCount = surface->width * surface->height;
        for (UINT i = 0; i < pixelCount; ++i)
        {
            BYTE *pixel = packedSource + i * 4;
            const BYTE blue = pixel[0];
            pixel[0] = pixel[2];
            pixel[2] = blue;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surface->width, surface->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, packedSource);
        th08::psp::RenderPerfNoteActualUpload(pixelCount * 4U);
        const GLenum directError = glGetError();
#if defined(TH08_PSP_VERBOSE_RENDER_DIAGNOSTICS)
        fprintf(stderr,
                "TH08PSP ANM_STREAM mode=direct32 size=%lux%lu srcfmt=%lu dstfmt=%lu error=0x%04x\n",
                static_cast<unsigned long>(surface->width),
                static_cast<unsigned long>(surface->height),
                static_cast<unsigned long>(sourceFormat),
                static_cast<unsigned long>(surface->format),
                static_cast<unsigned int>(directError));
#else
        if (directError != GL_NO_ERROR)
            fprintf(stderr, "TH08PSP ANM_STREAM direct32_error=0x%04x\n",
                    static_cast<unsigned int>(directError));
#endif
        if (directError == GL_NO_ERROR)
            staticUpload.Finalize(texture->glName);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (directError == GL_NO_ERROR)
        {
            texture->uploaded = true;
            texture->pspGlFormat = uploadFormat;
            texture->pspGlType = uploadType;
            PspNoteAnmTextureUpload(texture, surface, 0);
            texture->discardCpuCopy = true;
            surface->dirty = false;
            return true;
        }

        // Preserve the existing fallback contract if PSPGL rejects the direct
        // upload: restore the D3D byte order before the generic converter sees
        // the source again.
        for (UINT i = 0; i < pixelCount; ++i)
        {
            BYTE *pixel = packedSource + i * 4;
            const BYTE red = pixel[0];
            pixel[0] = pixel[2];
            pixel[2] = red;
        }
        return false;
    }

    if (sourceFormat == surface->format && sourceBytes == 2 &&
        sourcePitch == surface->width * 2)
    {
        // PSPGL's full upload path is already proven by the existing ANM
        // loader. Convert the writable archive buffer in place so no surface
        // or full-size staging allocation is required.
        WORD *packedSource = reinterpret_cast<WORD *>(const_cast<BYTE *>(source));
        const UINT pixelCount = surface->width * surface->height;
        if (surface->format == D3DFMT_A4R4G4B4)
        {
            for (UINT i = 0; i < pixelCount; ++i)
                packedSource[i] = static_cast<WORD>((packedSource[i] << 4) |
                                                    (packedSource[i] >> 12));
        }
        else if (surface->format == D3DFMT_A1R5G5B5)
        {
            for (UINT i = 0; i < pixelCount; ++i)
                packedSource[i] = static_cast<WORD>((packedSource[i] << 1) |
                                                    (packedSource[i] >> 15));
        }
        else if (surface->format == D3DFMT_X1R5G5B5)
        {
            for (UINT i = 0; i < pixelCount; ++i)
                packedSource[i] = static_cast<WORD>((packedSource[i] << 1) | 1U);
        }
        glTexImage2D(GL_TEXTURE_2D, 0, uploadFormat, surface->width, surface->height, 0,
                     uploadFormat, uploadType, packedSource);
        th08::psp::RenderPerfNoteActualUpload(pixelCount * 2U);
        const GLenum directError = glGetError();
#if defined(TH08_PSP_VERBOSE_RENDER_DIAGNOSTICS)
        fprintf(stderr,
                "TH08PSP ANM_STREAM mode=direct size=%lux%lu srcfmt=%lu dstfmt=%lu error=0x%04x\n",
                static_cast<unsigned long>(surface->width),
                static_cast<unsigned long>(surface->height),
                static_cast<unsigned long>(sourceFormat),
                static_cast<unsigned long>(surface->format),
                static_cast<unsigned int>(directError));
#else
        if (directError != GL_NO_ERROR)
            fprintf(stderr, "TH08PSP ANM_STREAM direct_error=0x%04x\n",
                    static_cast<unsigned int>(directError));
#endif
        if (directError == GL_NO_ERROR)
            staticUpload.Finalize(texture->glName);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (directError == GL_NO_ERROR)
        {
            texture->uploaded = true;
            texture->pspGlFormat = uploadFormat;
            texture->pspGlType = uploadType;
            PspNoteAnmTextureUpload(texture, surface, 0);
            texture->discardCpuCopy = true;
            surface->dirty = false;
            return true;
        }
        return false;
    }

    glTexImage2D(GL_TEXTURE_2D, 0, uploadFormat, surface->width, surface->height, 0,
                 uploadFormat, uploadType, NULL);
    th08::psp::RenderPerfNoteActualUpload(0);

    constexpr UINT stripHeight = 8;
    WORD converted[512 * stripHeight];
    for (UINT top = 0; top < surface->height; top += stripHeight)
    {
        const UINT rows = top + stripHeight <= surface->height
                              ? stripHeight
                              : surface->height - top;
        for (UINT row = 0; row < rows; ++row)
        {
            const BYTE *sourceRow = source + (top + row) * sourcePitch;
            for (UINT x = 0; x < surface->width; ++x)
            {
                BYTE rgba[4];
                DecodePixel(sourceRow + x * sourceBytes, sourceFormat, rgba);
                WORD packed;
                switch (surface->format)
                {
                case D3DFMT_A4R4G4B4:
                    packed = static_cast<WORD>(((rgba[0] >> 4) << 12) |
                                               ((rgba[1] >> 4) << 8) |
                                               ((rgba[2] >> 4) << 4) |
                                               (rgba[3] >> 4));
                    break;
                case D3DFMT_A1R5G5B5:
                case D3DFMT_X1R5G5B5:
                    packed = static_cast<WORD>(((rgba[0] >> 3) << 11) |
                                               ((rgba[1] >> 3) << 6) |
                                               ((rgba[2] >> 3) << 1) |
                                               (surface->format == D3DFMT_X1R5G5B5 ||
                                                        rgba[3] >= 128
                                                    ? 1
                                                    : 0));
                    break;
                default:
                    packed = static_cast<WORD>(((rgba[0] >> 3) << 11) |
                                               ((rgba[1] >> 2) << 5) |
                                               (rgba[2] >> 3));
                    break;
                }
                converted[row * surface->width + x] = packed;
            }
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, top, surface->width, rows,
                        uploadFormat, uploadType, converted);
        th08::psp::RenderPerfNoteActualUpload(surface->width * rows * 2U);
    }
    const GLenum stripError = glGetError();
#if defined(TH08_PSP_VERBOSE_RENDER_DIAGNOSTICS)
    fprintf(stderr,
            "TH08PSP ANM_STREAM mode=strip size=%lux%lu srcfmt=%lu dstfmt=%lu error=0x%04x\n",
            static_cast<unsigned long>(surface->width),
            static_cast<unsigned long>(surface->height),
            static_cast<unsigned long>(sourceFormat),
            static_cast<unsigned long>(surface->format),
            static_cast<unsigned int>(stripError));
#else
    if (stripError != GL_NO_ERROR)
        fprintf(stderr, "TH08PSP ANM_STREAM strip_error=0x%04x\n",
                static_cast<unsigned int>(stripError));
#endif
    if (stripError == GL_NO_ERROR)
        staticUpload.Finalize(texture->glName);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (stripError != GL_NO_ERROR)
        return false;

    texture->uploaded = true;

    texture->pspGlFormat = uploadFormat;

    texture->pspGlType = uploadType;

    PspNoteAnmTextureUpload(texture, surface, 0);
    texture->discardCpuCopy = true;
    surface->dirty = false;
    std::vector<BYTE>().swap(surface->pixels);
    return true;
}
#endif

bool th08_linux_texture_region_stats(IDirect3DTexture8 *textureRaw, float u0, float v0,
                                     float u1, float v1, D3DCOLOR diffuse,
                                     LinuxTextureRegionStats *stats)
{
    if (textureRaw == NULL || stats == NULL)
        return false;
    LinuxTexture *texture = static_cast<LinuxTexture *>(textureRaw);
    LinuxSurface *surface = texture->surface;
    if (surface == NULL || surface->pixels.empty() || surface->width == 0 || surface->height == 0)
        return false;
    if (u0 != u0 || v0 != v0 || u1 != u1 || v1 != v1 ||
        fabsf(u0) > 16.0f || fabsf(v0) > 16.0f ||
        fabsf(u1) > 16.0f || fabsf(v1) > 16.0f)
        return false;

    if (u0 > u1) { const float swap = u0; u0 = u1; u1 = swap; }
    if (v0 > v1) { const float swap = v0; v0 = v1; v1 = swap; }
    int left = static_cast<int>(floorf(u0 * surface->width));
    int top = static_cast<int>(floorf(v0 * surface->height));
    int right = static_cast<int>(ceilf(u1 * surface->width));
    int bottom = static_cast<int>(ceilf(v1 * surface->height));
    if (left >= right || top >= bottom || right - left > static_cast<int>(surface->width) * 4 ||
        bottom - top > static_cast<int>(surface->height) * 4)
        return false;

    memset(stats, 0, sizeof(*stats));
    const UINT bytes = BytesPerPixel(surface->format);
    const BYTE diffuseAlpha = (diffuse >> 24) & 0xff;
    const BYTE diffuseRed = (diffuse >> 16) & 0xff;
    const BYTE diffuseGreen = (diffuse >> 8) & 0xff;
    const BYTE diffuseBlue = diffuse & 0xff;
    for (int y = top; y < bottom; ++y)
    {
        for (int x = left; x < right; ++x)
        {
            const int textureWidth = static_cast<int>(surface->width);
            const int textureHeight = static_cast<int>(surface->height);
            const int wrappedX = ((x % textureWidth) + textureWidth) % textureWidth;
            const int wrappedY = ((y % textureHeight) + textureHeight) % textureHeight;
            BYTE rgba[4];
            DecodePixel(&surface->pixels[wrappedY * surface->pitch + wrappedX * bytes],
                        surface->format, rgba);
            ++stats->sampledPixels;
            if (rgba[3] <= 8)
                continue;
            ++stats->visiblePixels;
            const BYTE maximum = rgba[0] > rgba[1]
                ? (rgba[0] > rgba[2] ? rgba[0] : rgba[2])
                : (rgba[1] > rgba[2] ? rgba[1] : rgba[2]);
            const BYTE minimum = rgba[0] < rgba[1]
                ? (rgba[0] < rgba[2] ? rgba[0] : rgba[2])
                : (rgba[1] < rgba[2] ? rgba[1] : rgba[2]);
            if (maximum - minimum >= 32)
                ++stats->colorfulPixels;
            if (rgba[0] >= 240 && rgba[1] >= 240 && rgba[2] >= 240)
                ++stats->nearWhitePixels;
            if (x == left || x == right - 1 || y == top || y == bottom - 1)
                ++stats->visibleEdgePixels;

            const BYTE modulatedAlpha = static_cast<BYTE>(rgba[3] * diffuseAlpha / 255U);
            if (modulatedAlpha <= 8)
                continue;
            ++stats->modulatedVisiblePixels;
            const BYTE modulatedRed = static_cast<BYTE>(rgba[0] * diffuseRed / 255U);
            const BYTE modulatedGreen = static_cast<BYTE>(rgba[1] * diffuseGreen / 255U);
            const BYTE modulatedBlue = static_cast<BYTE>(rgba[2] * diffuseBlue / 255U);
            const BYTE contributionRed =
                static_cast<BYTE>(modulatedRed * modulatedAlpha / 255U);
            const BYTE contributionGreen =
                static_cast<BYTE>(modulatedGreen * modulatedAlpha / 255U);
            const BYTE contributionBlue =
                static_cast<BYTE>(modulatedBlue * modulatedAlpha / 255U);
            const BYTE contributionMaximum = contributionRed > contributionGreen
                ? (contributionRed > contributionBlue ? contributionRed : contributionBlue)
                : (contributionGreen > contributionBlue ? contributionGreen : contributionBlue);
            const BYTE contributionMinimum = contributionRed < contributionGreen
                ? (contributionRed < contributionBlue ? contributionRed : contributionBlue)
                : (contributionGreen < contributionBlue ? contributionGreen : contributionBlue);
            if (contributionMaximum - contributionMinimum >= 8)
                ++stats->modulatedColorfulPixels;
            if (modulatedRed >= 240 && modulatedGreen >= 240 && modulatedBlue >= 240)
                ++stats->modulatedNearWhitePixels;
        }
    }
    return true;
}

void th08_linux_dialogue_snapshot_restore(IDirect3DDevice8 *deviceRaw)
{
#if TH08_PSP_DIALOGUE_SNAPSHOT_AT_BACKGROUND_ENABLED
#if TH08_PSP_DIALOGUE_SNAPSHOT_DIAG_ENABLED
    static unsigned long diagCalls = 0;
    if (diagCalls < 3UL)
    {
        ++diagCalls;
        th08::psp::BootLog("DIALOGUE_BG_HOOK called=%lu device=%p\n", diagCalls,
                           static_cast<void *>(deviceRaw));
    }
#endif
    if (deviceRaw != NULL)
        static_cast<LinuxDevice *>(deviceRaw)->RestoreDialogueSnapshotExternal();
#else
    (void)deviceRaw;
#endif
}

bool th08_linux_begin_framebuffer_probe(IDirect3DDevice8 *deviceRaw, int left, int top,
                                        int right, int bottom)
{
    if (deviceRaw == NULL)
        return false;
    return static_cast<LinuxDevice *>(deviceRaw)->BeginFramebufferProbe(left, top, right, bottom);
}

bool th08_linux_end_framebuffer_probe(IDirect3DDevice8 *deviceRaw,
                                      LinuxFramebufferDeltaStats *stats)
{
    if (deviceRaw == NULL)
        return false;
    return static_cast<LinuxDevice *>(deviceRaw)->EndFramebufferProbe(stats);
}

extern "C" IDirect3D8 *Direct3DCreate8(UINT sdkVersion)
{ return sdkVersion == D3D_SDK_VERSION ? new(std::nothrow) LinuxDirect3D() : NULL; }
