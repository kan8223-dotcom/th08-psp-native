#include "gui_border_replay.hpp"

#if TH08_PSP_GUI_BORDER_STATS_ENABLED

#include "AnmManager.hpp"
#include "Supervisor.hpp"
#include "ZunMath.hpp"
#include "fileio.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#if TH08_PSP_GUI_BORDER_REPLAY_ENABLED && defined(TH08_PSP_GUI_BORDER_NATIVE) && TH08_PSP_GUI_BORDER_NATIVE && \
    defined(TH08_PSP_ME_BULLET_ADOPT) && TH08_PSP_ME_BULLET_ADOPT
#define TH08_PSP_GUI_BORDER_NATIVE_ENABLED 1
extern "C" void sceKernelDcacheWritebackRange(const void *p, unsigned int size);
extern "C" int th08_psp_bullet_me_submit(void *deviceRaw, const void *vertices, unsigned int quadCount,
                                         const unsigned short *indices);
extern "C" int th08_psp_bullet_me_color_identity(void *deviceRaw);
#else
#define TH08_PSP_GUI_BORDER_NATIVE_ENABLED 0
#endif


namespace th08::psp
{
namespace
{
constexpr std::uint32_t kLeftColumnTiles = 15U;   // yPos 0..432 step 32
constexpr std::uint32_t kRightPanelTiles = 98U;   // 7 columns x 14 rows
constexpr std::uint32_t kStripTiles = 10U;        // 5 columns x top/bottom
constexpr std::uint32_t kTileCount = kLeftColumnTiles + kRightPanelTiles + kStripTiles;
constexpr std::uint32_t kVerticesPerSprite = 6U;
constexpr std::uint32_t kVertexCount = kTileCount * kVerticesPerSprite;

struct SpriteKey
{
    const AnmLoadedSprite *loadedSprite;
    const IDirect3DTexture8 *texture;
    float uvStartX, uvStartY, uvEndX, uvEndY;
    float scaleX, scaleY;
    float spriteSizeX, spriteSizeY;
    float uvScrollX, uvScrollY;
    std::uint32_t color1, color2;
    std::uint32_t flags; // visible, flag1, anchor, blendMode, zWriteDisabled, flag15, flag17
};

struct Key
{
    SpriteKey sprite[2];
    float shakeX, shakeY;
    std::uint32_t mixColor;
    std::uint32_t useMixColor;
    std::uint32_t viewportX, viewportY, viewportW, viewportH;
    std::uint32_t depthTestDisabled;
};

struct Record
{
    bool valid;
    Key key;
    VertexTex1DiffuseXyzrhw vertices[kVertexCount];
};

Record gRecord;
Record gScratch; // audit: canonical output of the current frame
bool gWindowActive = false;
std::uint32_t gRecordGen = 0U; // bumped whenever gRecord.vertices are rewritten
#if TH08_PSP_GUI_BORDER_NATIVE_ENABLED
// GE-ready copy of the record (u,v,rgba,x,y,z; the quad batch's transformed
// conversion), rebuilt when the record changes.  Two copies so a rebuild never
// touches the buffer the previous frame's lists may still read.
struct NativeVertex
{
    float u, v;
    unsigned char r, g, b, a;
    float x, y, z;
};
struct NativeBuffer
{
    NativeVertex v[kTileCount * 4U] __attribute__((aligned(64)));
    std::uint32_t gen;
    bool valid;
};
NativeBuffer gNative[2] __attribute__((aligned(64)));
unsigned gNativeCur = 0U;
std::uint32_t gNativeReplays = 0U, gNativeBuilds = 0U, gNativeFallbacks = 0U, gNativePresents = 0U;
#endif
std::uint32_t gPresents = 0U;
std::uint32_t gKeyHits = 0U;
std::uint32_t gReplays = 0U;
std::uint32_t gRecaptures = 0U;
std::uint32_t gRejected = 0U;
std::uint32_t gMismatches = 0U;
std::uint32_t gFallbacks = 0U;

void Inc(std::uint32_t &value)
{
    if (value != std::numeric_limits<std::uint32_t>::max())
        ++value;
}

void FillSpriteKey(SpriteKey *key, const AnmVm *vm)
{
    std::memset(key, 0, sizeof(*key));
    key->loadedSprite = vm->loadedSprite;
    if (vm->loadedSprite != NULL)
    {
        key->texture = vm->loadedSprite->texture;
        key->uvStartX = vm->loadedSprite->uvStart.x;
        key->uvStartY = vm->loadedSprite->uvStart.y;
        key->uvEndX = vm->loadedSprite->uvEnd.x;
        key->uvEndY = vm->loadedSprite->uvEnd.y;
    }
    key->scaleX = vm->scale.x;
    key->scaleY = vm->scale.y;
    key->spriteSizeX = vm->spriteSize.x;
    key->spriteSizeY = vm->spriteSize.y;
    key->uvScrollX = vm->uvScrollPos.x;
    key->uvScrollY = vm->uvScrollPos.y;
    key->color1 = vm->color1.d3dColor;
    key->color2 = vm->color2.d3dColor;
    key->flags = (vm->visible ? 1U : 0U) | (vm->flag1 ? 2U : 0U) |
                 (static_cast<std::uint32_t>(vm->anchor) << 2U) |
                 (static_cast<std::uint32_t>(vm->blendMode) << 4U) |
                 (vm->zWriteDisabled ? 64U : 0U) | (vm->flag15 ? 128U : 0U) |
                 (vm->flag17 ? 256U : 0U);
}

void FillKey(Key *key, const AnmVm *vm13, const AnmVm *vm14)
{
    std::memset(key, 0, sizeof(*key));
    FillSpriteKey(&key->sprite[0], vm13);
    FillSpriteKey(&key->sprite[1], vm14);
    key->shakeX = g_AnmManager->screenShakeOffset.x;
    key->shakeY = g_AnmManager->screenShakeOffset.y;
    key->mixColor = g_AnmManager->color.d3dColor;
    key->useMixColor = g_AnmManager->useMixColor ? 1U : 0U;
    key->viewportX = g_Supervisor.viewport.X;
    key->viewportY = g_Supervisor.viewport.Y;
    key->viewportW = g_Supervisor.viewport.Width;
    key->viewportH = g_Supervisor.viewport.Height;
    key->depthTestDisabled = g_Supervisor.IsDepthTestDisabled() ? 1U : 0U;
}

// The canonical loops, verbatim from Gui::DrawGameScene.  Returns true when
// the 123 tiles were appended contiguously to the shared sprite batch (so
// their vertices are a faithful record); false otherwise.
bool RunCanonical(AnmVm *vm13, AnmVm *vm14, Record *record)
{
    AnmManager *const manager = g_AnmManager;
    AnmVm *vm;
    f32 xPos;
    f32 yPos;
    const VertexTex1DiffuseXyzrhw *const endBefore = manager->vertexBufferEndPtr;
    const u32 spritesBefore = manager->spritesToDraw;
    const VertexTex1DiffuseXyzrhw *startAfterFirst = NULL;
    u32 spritesAfterFirst = 0U;

    vm = vm13;
    for (yPos = 0.0f; yPos < 464.0f; yPos += 32.0f)
    {
        vm->pos = Float3(0.0f, yPos, 0.49f);
        g_AnmManager->DrawNoRotation(vm);
        if (yPos == 0.0f)
        {
            // A first-tile texture/state flush is canonical; any later flush
            // would split the run and disqualify the record.
            startAfterFirst = manager->vertexBufferStartPtr;
            spritesAfterFirst = manager->spritesToDraw;
        }
    }
    for (xPos = 416.0f; xPos < 624.0f; xPos += 32.0f)
    {
        for (yPos = 16.0f; yPos < 464.0f; yPos += 32.0f)
        {
            vm->pos = Float3(xPos, yPos, 0.49f);
            g_AnmManager->DrawNoRotation(vm);
        }
    }
    vm = vm14;
    for (xPos = 0.0f; xPos < 624.0f; xPos += 128.0f)
    {
        vm->pos = Float3(xPos, 0.0f, 0.49f);
        g_AnmManager->DrawNoRotation(vm);
        vm->pos = Float3(xPos, 464.0f, 0.49f);
        g_AnmManager->DrawNoRotation(vm);
    }

    const VertexTex1DiffuseXyzrhw *const endAfter = manager->vertexBufferEndPtr;
    const bool contiguous =
        endAfter == endBefore + kVertexCount &&
        manager->vertexBufferStartPtr == startAfterFirst &&
        manager->spritesToDraw == spritesAfterFirst + (kTileCount - 1U) &&
        spritesAfterFirst >= 1U &&
        manager->spritesToDraw >= spritesBefore &&
        !GuiBorderBatchActive();
    if (!contiguous)
    {
        record->valid = false;
        return false;
    }
    std::memcpy(record->vertices, endBefore, sizeof(record->vertices));
    record->valid = true;
    return true;
}

// Product replay: the first-tile state calls of DrawInner for each sprite,
// then the recorded vertices in one append.  Returns false (nothing done)
// when the shared batch cannot take the run as one contiguous append.
bool Replay(AnmVm *vm13, AnmVm *vm14)
{
    AnmManager *const manager = g_AnmManager;
    if (GuiBorderBatchActive() ||
        !GuiBorderBatchCanAppend(manager, kVertexCount))
        return false;
    AnmVm *const vms[2] = {vm13, vm14};
    for (std::size_t i = 0U; i < 2U; ++i)
    {
        AnmVm *const vm = vms[i];
        if (manager->currentTexture != vm->loadedSprite->texture)
        {
            manager->currentTexture = vm->loadedSprite->texture;
            manager->FlushVertexBuffer();
            g_Supervisor.d3dDevice->SetTexture(0, manager->currentTexture);
        }
        if (manager->currentVertexShader != 1)
        {
            manager->FlushVertexBuffer();
            manager->currentVertexShader = 1;
        }
        manager->SetRenderStateForVm(vm);
    }
    // The canonical loop reaches SetRenderStateForVm once per tile.
    manager->renderStateChangesThisFrame += kTileCount - 2U;
    if (!GuiBorderBatchCanAppend(manager, kVertexCount))
        return false;
    std::memcpy(manager->vertexBufferEndPtr, gRecord.vertices,
                sizeof(gRecord.vertices));
    manager->vertexBufferEndPtr += kVertexCount;
    manager->spritesToDraw += kTileCount;
    return true;
}
#if TH08_PSP_GUI_BORDER_NATIVE_ENABLED
// Product replay through a persistent native buffer: the first-tile state
// calls per sprite, then one indexed submit per sprite (113 + 10 quads).
bool NativeReplay(AnmVm *vm13, AnmVm *vm14)
{
    AnmManager *const manager = g_AnmManager;
    if (GuiBorderBatchActive())
        return false;
    const u16 *const indices = manager->PspBulletUnifiedQuadIndices();
    if (indices == NULL || !th08_psp_bullet_me_color_identity(g_Supervisor.d3dDevice))
        return false;
    NativeBuffer *buf = &gNative[gNativeCur];
    if (!buf->valid || buf->gen != gRecordGen)
    {
        buf = &gNative[gNativeCur ^ 1U];
        // Six-vertex sprites (0,1,2,1,2,3): corners are entries 0, 1, 2, 5.
        static const unsigned corner[4] = {0U, 1U, 2U, 5U};
        for (std::uint32_t t = 0U; t < kTileCount; ++t)
        {
            const VertexTex1DiffuseXyzrhw *const six = gRecord.vertices + t * kVerticesPerSprite;
            for (unsigned k = 0U; k < 4U; ++k)
            {
                const VertexTex1DiffuseXyzrhw &in = six[corner[k]];
                NativeVertex &out = buf->v[t * 4U + k];
                out.u = in.textureUV.x;
                out.v = in.textureUV.y;
                out.r = static_cast<unsigned char>((in.diffuse >> 16) & 255U);
                out.g = static_cast<unsigned char>((in.diffuse >> 8) & 255U);
                out.b = static_cast<unsigned char>(in.diffuse & 255U);
                out.a = static_cast<unsigned char>((in.diffuse >> 24) & 255U);
                out.x = in.pos.x + 0.5f;
                out.y = in.pos.y + 0.5f;
                out.z = 1.0f - 2.0f * in.pos.z;
            }
        }
        buf->gen = gRecordGen;
        buf->valid = true;
        sceKernelDcacheWritebackRange(buf->v, sizeof(buf->v));
        gNativeCur ^= 1U;
        ++gNativeBuilds;
    }
    // Everything queued before the border must draw first (the canonical path
    // appends to the same sprite batch).
    manager->FlushVertexBuffer();
    AnmVm *const vms[2] = {vm13, vm14};
    const std::uint32_t firstQuad[2] = {0U, kLeftColumnTiles + kRightPanelTiles};
    const std::uint32_t quadCount[2] = {kLeftColumnTiles + kRightPanelTiles, kStripTiles};
    for (std::size_t i = 0U; i < 2U; ++i)
    {
        AnmVm *const vm = vms[i];
        if (manager->currentTexture != vm->loadedSprite->texture)
        {
            manager->currentTexture = vm->loadedSprite->texture;
            manager->FlushVertexBuffer();
            g_Supervisor.d3dDevice->SetTexture(0, manager->currentTexture);
        }
        if (manager->currentVertexShader != 1)
        {
            manager->FlushVertexBuffer();
            manager->currentVertexShader = 1;
        }
        manager->SetRenderStateForVm(vm);
        if (!th08_psp_bullet_me_submit(g_Supervisor.d3dDevice, buf->v + firstQuad[i] * 4U, quadCount[i], indices))
            return false; // only on a device fault; the canonical path then redraws
    }
    manager->renderStateChangesThisFrame += kTileCount - 2U;
    ++gNativeReplays;
    return true;
}
#endif
} // namespace

void GuiBorderDrawTiles(AnmVm *vm13, AnmVm *vm14)
{
    Key key;
    FillKey(&key, vm13, vm14);
    const bool keyHit =
        gRecord.valid && std::memcmp(&key, &gRecord.key, sizeof(key)) == 0;
    if (gWindowActive)
    {
        Inc(gPresents);
        if (keyHit)
            Inc(gKeyHits);
    }
#if TH08_PSP_GUI_BORDER_REPLAY_ENABLED
#if TH08_PSP_GUI_BORDER_NATIVE_ENABLED
    if ((++gNativePresents % 600U) == 0U)
        BootLog("GUI_BORDER_NATIVE presents=%lu replays=%lu builds=%lu fallbacks=%lu\n",
                static_cast<unsigned long>(gNativePresents), static_cast<unsigned long>(gNativeReplays),
                static_cast<unsigned long>(gNativeBuilds), static_cast<unsigned long>(gNativeFallbacks));
    if (keyHit && NativeReplay(vm13, vm14))
    {
        if (gWindowActive)
            Inc(gReplays);
        return;
    }
    if (keyHit)
        ++gNativeFallbacks;
#endif
    if (keyHit)
    {
        if (Replay(vm13, vm14))
        {
            if (gWindowActive)
                Inc(gReplays);
            return;
        }
        if (gWindowActive)
            Inc(gFallbacks);
        gRecord.valid = false;
    }
    if (RunCanonical(vm13, vm14, &gRecord))
    {
        gRecord.key = key;
        ++gRecordGen;
        if (gWindowActive)
            Inc(gRecaptures);
    }
    else if (gWindowActive)
    {
        Inc(gRejected);
    }
#else
    // Audit: canonical every frame; compare with the stored record when the
    // key says the replay would have been used.
    if (RunCanonical(vm13, vm14, &gScratch))
    {
        if (keyHit && std::memcmp(gScratch.vertices, gRecord.vertices,
                                  sizeof(gScratch.vertices)) != 0)
        {
            if (gWindowActive)
                Inc(gMismatches);
        }
        std::memcpy(gRecord.vertices, gScratch.vertices, sizeof(gRecord.vertices));
        gRecord.key = key;
        ++gRecordGen;
        gRecord.valid = true;
        if (gWindowActive && !keyHit)
            Inc(gRecaptures);
    }
    else
    {
        gRecord.valid = false;
        if (gWindowActive)
            Inc(gRejected);
    }
#endif
}

#if TH08_PSP_GUI_FRONT_NATIVE_ENABLED
namespace
{
struct SpriteRunEntry
{
    AnmVm *vm;
    const void *texture;
    std::uint32_t state;
    VertexTex1DiffuseXyzrhw v[kVerticesPerSprite];
};
struct SpriteRunRecord
{
    bool valid;
    GuiSpriteRunKey key;
    std::uint32_t count;
    SpriteRunEntry e[kGuiSpriteRunMaxSprites];
};
SpriteRunRecord gRun;
SpriteRunEntry gRunTmp[kGuiSpriteRunMaxSprites];
std::uint32_t gRunTmpCount = 0U;
bool gRunCapturing = false, gRunBad = false;
std::uint32_t gRunGen = 0U;
struct RunNativeBuffer
{
    NativeVertex v[kGuiSpriteRunMaxSprites * 4U] __attribute__((aligned(64)));
    std::uint32_t gen;
    bool valid;
};
RunNativeBuffer gRunNative[2] __attribute__((aligned(64)));
unsigned gRunNativeCur = 0U;
std::uint32_t gRunPresents = 0U, gRunReplays = 0U, gRunBuilds = 0U, gRunFallbacks = 0U, gRunRejected = 0U,
              gRunRecaptures = 0U, gRunEmpty = 0U, gRunGroups = 0U;

std::uint32_t SpriteRunState(const AnmVm *vm)
{
    return (static_cast<std::uint32_t>(vm->blendMode) & 255U) | (vm->zWriteDisabled ? 256U : 0U) |
           (vm->flag15 ? 512U : 0U);
}

ZunResult SpriteRunCapture(AnmVm *vm, bool draw2d)
{
    AnmManager *const manager = g_AnmManager;
    const u32 spritesBefore = manager->spritesToDraw;
    const u32 flushesBefore = manager->flushesThisFrame;
    const VertexTex1DiffuseXyzrhw *const endBefore = manager->vertexBufferEndPtr;
    const ZunResult result = draw2d ? manager->Draw2D(vm) : manager->DrawNoRotation(vm);
    if (!gRunCapturing || gRunBad)
        return result;
    // Either one sprite was appended in place, or a flush inside the draw
    // restarted the batch and the sprite is its first entry, or nothing was
    // drawn (hidden / culled).
    const u32 after = manager->spritesToDraw;
    u32 appended;
    if (after == spritesBefore + 1U && manager->vertexBufferEndPtr == endBefore + kVerticesPerSprite)
        appended = 1U;
    else if (after == 1U && manager->vertexBufferEndPtr == manager->vertexBufferStartPtr + kVerticesPerSprite)
        appended = 1U;
    else if (after == spritesBefore && manager->vertexBufferEndPtr == endBefore)
        appended = 0U;
    else
        appended = 0xFFFFFFFFU;
    if (appended == 0U)
        return result;
    if (appended != 1U || gRunTmpCount >= kGuiSpriteRunMaxSprites || GuiBorderBatchActive() || vm->loadedSprite == NULL)
    {
        static unsigned badLogs = 0U;
        if (badLogs < 5U)
        {
            ++badLogs;
            BootLog("GUI_FRONT_BAD idx=%lu before=%lu after=%lu flushes=%lu endDelta=%ld batch=%d sprite=%d\n",
                    static_cast<unsigned long>(gRunTmpCount), static_cast<unsigned long>(spritesBefore),
                    static_cast<unsigned long>(after), static_cast<unsigned long>(manager->flushesThisFrame - flushesBefore),
                    static_cast<long>(manager->vertexBufferEndPtr - endBefore), GuiBorderBatchActive() ? 1 : 0,
                    vm->loadedSprite != NULL ? 1 : 0);
        }
        gRunBad = true;
        return result;
    }
    SpriteRunEntry &e = gRunTmp[gRunTmpCount++];
    e.vm = vm;
    e.texture = vm->loadedSprite->texture;
    e.state = SpriteRunState(vm);
    std::memcpy(e.v, manager->vertexBufferEndPtr - kVerticesPerSprite, sizeof(e.v));
    return result;
}
} // namespace

void GuiSpriteKeyFill(GuiSpriteKey *key, const AnmVm *vm)
{
    std::memset(key, 0, sizeof(*key));
    key->loadedSprite = vm->loadedSprite;
    if (vm->loadedSprite != NULL)
    {
        key->texture = vm->loadedSprite->texture;
        key->uvStartX = vm->loadedSprite->uvStart.x;
        key->uvStartY = vm->loadedSprite->uvStart.y;
        key->uvEndX = vm->loadedSprite->uvEnd.x;
        key->uvEndY = vm->loadedSprite->uvEnd.y;
    }
    key->scaleX = vm->scale.x;
    key->scaleY = vm->scale.y;
    key->spriteSizeX = vm->spriteSize.x;
    key->spriteSizeY = vm->spriteSize.y;
    key->uvScrollX = vm->uvScrollPos.x;
    key->uvScrollY = vm->uvScrollPos.y;
    key->posX = vm->pos.x;
    key->posY = vm->pos.y;
    key->posZ = vm->pos.z;
    key->rotZ = vm->rotation.z;
    key->color1 = vm->color1.d3dColor;
    key->color2 = vm->color2.d3dColor;
    key->flags = (vm->visible ? 1U : 0U) | (vm->flag1 ? 2U : 0U) |
                 (static_cast<std::uint32_t>(vm->anchor) << 2U) |
                 (static_cast<std::uint32_t>(vm->blendMode) << 4U) |
                 (vm->zWriteDisabled ? 64U : 0U) | (vm->flag15 ? 128U : 0U) |
                 (vm->flag17 ? 256U : 0U);
}

void GuiSpriteRunKeyFinish(GuiSpriteRunKey *key, unsigned int vmCount, unsigned int spriteCount, unsigned int aux)
{
    for (unsigned int i = vmCount; i < kGuiSpriteRunMaxVms; ++i)
        std::memset(&key->vm[i], 0, sizeof(key->vm[i]));
    key->vmCount = vmCount;
    key->spriteCount = spriteCount;
    key->aux = aux;
    key->shakeX = g_AnmManager->screenShakeOffset.x;
    key->shakeY = g_AnmManager->screenShakeOffset.y;
    key->mixColor = g_AnmManager->color.d3dColor;
    key->useMixColor = g_AnmManager->useMixColor ? 1U : 0U;
    key->viewportX = g_Supervisor.viewport.X;
    key->viewportY = g_Supervisor.viewport.Y;
    key->viewportW = g_Supervisor.viewport.Width;
    key->viewportH = g_Supervisor.viewport.Height;
    key->depthTestDisabled = g_Supervisor.IsDepthTestDisabled() ? 1U : 0U;
}

bool GuiSpriteRunTryReplay(const GuiSpriteRunKey *key)
{
    if ((++gRunPresents % 600U) == 0U)
        BootLog("GUI_FRONT_NATIVE presents=%lu replays=%lu builds=%lu fallbacks=%lu rejected=%lu recaptures=%lu empty=%lu groups=%lu\n",
                static_cast<unsigned long>(gRunPresents), static_cast<unsigned long>(gRunReplays),
                static_cast<unsigned long>(gRunBuilds), static_cast<unsigned long>(gRunFallbacks),
                static_cast<unsigned long>(gRunRejected), static_cast<unsigned long>(gRunRecaptures),
                static_cast<unsigned long>(gRunEmpty), static_cast<unsigned long>(gRunGroups));
    if (!gRun.valid || std::memcmp(key, &gRun.key, sizeof(*key)) != 0)
        return false;
    if (gRun.count == 0U)
    {
        ++gRunEmpty;
        return true;
    }
    AnmManager *const manager = g_AnmManager;
    if (GuiBorderBatchActive())
        return false;
    const u16 *const indices = manager->PspBulletUnifiedQuadIndices();
    if (indices == NULL || !th08_psp_bullet_me_color_identity(g_Supervisor.d3dDevice))
    {
        ++gRunFallbacks;
        return false;
    }
    RunNativeBuffer *buf = &gRunNative[gRunNativeCur];
    if (!buf->valid || buf->gen != gRunGen)
    {
        buf = &gRunNative[gRunNativeCur ^ 1U];
        static const unsigned corner[4] = {0U, 1U, 2U, 5U};
        for (std::uint32_t t = 0U; t < gRun.count; ++t)
        {
            const VertexTex1DiffuseXyzrhw *const six = gRun.e[t].v;
            for (unsigned k = 0U; k < 4U; ++k)
            {
                const VertexTex1DiffuseXyzrhw &in = six[corner[k]];
                NativeVertex &out = buf->v[t * 4U + k];
                out.u = in.textureUV.x;
                out.v = in.textureUV.y;
                out.r = static_cast<unsigned char>((in.diffuse >> 16) & 255U);
                out.g = static_cast<unsigned char>((in.diffuse >> 8) & 255U);
                out.b = static_cast<unsigned char>(in.diffuse & 255U);
                out.a = static_cast<unsigned char>((in.diffuse >> 24) & 255U);
                out.x = in.pos.x + 0.5f;
                out.y = in.pos.y + 0.5f;
                out.z = 1.0f - 2.0f * in.pos.z;
            }
        }
        buf->gen = gRunGen;
        buf->valid = true;
        sceKernelDcacheWritebackRange(buf->v, sizeof(buf->v));
        gRunNativeCur ^= 1U;
        ++gRunBuilds;
    }
    manager->FlushVertexBuffer();
    std::uint32_t first = 0U;
    while (first < gRun.count)
    {
        std::uint32_t last = first + 1U;
        while (last < gRun.count && gRun.e[last].texture == gRun.e[first].texture && gRun.e[last].state == gRun.e[first].state)
            ++last;
        AnmVm *const vm = gRun.e[first].vm;
        if (manager->currentTexture != vm->loadedSprite->texture)
        {
            manager->currentTexture = vm->loadedSprite->texture;
            manager->FlushVertexBuffer();
            g_Supervisor.d3dDevice->SetTexture(0, manager->currentTexture);
        }
        if (manager->currentVertexShader != 1)
        {
            manager->FlushVertexBuffer();
            manager->currentVertexShader = 1;
        }
        manager->SetRenderStateForVm(vm);
        if (!th08_psp_bullet_me_submit(g_Supervisor.d3dDevice, buf->v + first * 4U, last - first, indices))
        {
            ++gRunFallbacks;
            return false; // device fault: the canonical path redraws (partial overdraw is identical pixels)
        }
        ++gRunGroups;
        first = last;
    }
    manager->renderStateChangesThisFrame += gRun.count - 1U;
    ++gRunReplays;
    return true;
}

void GuiSpriteRunBegin()
{
    gRunCapturing = true;
    gRunBad = false;
    gRunTmpCount = 0U;
}

int GuiSpriteRunDraw(AnmVm *vm) { return static_cast<int>(SpriteRunCapture(vm, false)); }
int GuiSpriteRunDraw2D(AnmVm *vm) { return static_cast<int>(SpriteRunCapture(vm, true)); }

void GuiSpriteRunEnd(const GuiSpriteRunKey *key)
{
    if (!gRunCapturing)
        return;
    gRunCapturing = false;
    if (gRunBad)
    {
        gRun.valid = false;
        ++gRunRejected;
        return;
    }
    gRun.count = gRunTmpCount;
    if (gRunTmpCount != 0U)
        std::memcpy(gRun.e, gRunTmp, sizeof(SpriteRunEntry) * gRunTmpCount);
    gRun.key = *key;
    gRun.valid = true;
    ++gRunGen;
    ++gRunRecaptures;
}
#endif // TH08_PSP_GUI_FRONT_NATIVE_ENABLED

void GuiBorderStatsResetWindow(bool active)
{
    gPresents = gKeyHits = gReplays = gRecaptures = gRejected = gMismatches = gFallbacks = 0U;
    gWindowActive = active;
}

void GuiBorderStatsCancelWindow()
{
    gWindowActive = false;
}

void GuiBorderStatsEmitWindow(std::int32_t stage, std::uint32_t baselineStageFrame,
                              std::uint32_t stageFrame)
{
    if (!gWindowActive)
        return;
    BootLog("GUI_BORDER V1 st=%ld sf=%lu-%lu mode=%s tiles=%lu presents=%lu "
            "key_hits=%lu replays=%lu recaptures=%lu rejected=%lu fallbacks=%lu "
            "mismatches=%lu\n",
            static_cast<long>(stage),
            static_cast<unsigned long>(baselineStageFrame),
            static_cast<unsigned long>(stageFrame),
            TH08_PSP_GUI_BORDER_REPLAY_AUDIT_ENABLED ? "audit" : "product",
            static_cast<unsigned long>(kTileCount),
            static_cast<unsigned long>(gPresents),
            static_cast<unsigned long>(gKeyHits),
            static_cast<unsigned long>(gReplays),
            static_cast<unsigned long>(gRecaptures),
            static_cast<unsigned long>(gRejected),
            static_cast<unsigned long>(gFallbacks),
            static_cast<unsigned long>(gMismatches));
}
} // namespace th08::psp

#endif // TH08_PSP_GUI_BORDER_STATS_ENABLED
