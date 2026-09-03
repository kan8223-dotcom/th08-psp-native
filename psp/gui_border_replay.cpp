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
