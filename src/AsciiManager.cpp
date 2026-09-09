#include "th_pch.h"
#if defined(PSP)
#include "fileio.hpp"
#if defined(PSP) && ((defined(TH08_PSP_ME_POPUP) && TH08_PSP_ME_POPUP) || (defined(TH08_PSP_ME_POPUP_AUDIT) && TH08_PSP_ME_POPUP_AUDIT))
#include "psp/me_popup.hpp"
#define TH08_PSP_ME_POPUP_ENABLED 1
#endif
#endif

#if defined(PSP)
#include "render_perf_telemetry.hpp"
#endif

#include <stdarg.h>
#include <stdio.h>

#include "Player.hpp"
#include "GameManager.hpp"
#include "EclManager.hpp"
#include "ScreenEffect.hpp"
#include "Spellcard.hpp"
#include "ZunMath.hpp"
#include "Gui.hpp"
#include "ResultScreen.hpp"

namespace th08
{

DIFFABLE_STATIC(ChainElem, g_AsciiManagerDrawChainLowPrio);
DIFFABLE_STATIC(AsciiManager, g_AsciiManager);
DIFFABLE_STATIC(ChainElem, g_AsciiManagerCalcChain);
DIFFABLE_STATIC(ChainElem, g_AsciiManagerDrawChainHighPrio);

#if defined(PSP)
namespace
{
struct PspAsciiRenderOwnerSidecar
{
    AsciiManager *owner;
    PspAsciiRenderOwner strings[ASCII_MAX_STRINGS];
};

PspAsciiRenderOwner g_PspAsciiCurrentRenderOwner =
    PspAsciiRenderOwner::Game;
PspAsciiRenderOwnerSidecar g_PspAsciiRenderOwners{};

void PspResetAsciiRenderOwners(AsciiManager *owner)
{
    memset(&g_PspAsciiRenderOwners, 0, sizeof(g_PspAsciiRenderOwners));
    g_PspAsciiRenderOwners.owner = owner;
}

void PspEnsureAsciiRenderOwners(AsciiManager *owner)
{
    if (g_PspAsciiRenderOwners.owner != owner)
        PspResetAsciiRenderOwners(owner);
}

PspAsciiRenderOwner PspAsciiStringRenderOwner(AsciiManager *ascii,
                                               i32 stringIndex)
{
    if (g_PspAsciiRenderOwners.owner != ascii || stringIndex < 0 ||
        stringIndex >= ASCII_MAX_STRINGS)
    {
        return PspAsciiRenderOwner::Game;
    }
    return g_PspAsciiRenderOwners.strings[stringIndex];
}

void PspDrawFpsOverlayGlyph(AnmVm *vm)
{
    const u32 spritesBefore = g_AnmManager->spritesToDraw;
    const u32 flushesBefore = g_AnmManager->flushesThisFrame;
    const ZunResult result = g_AnmManager->DrawNoRotation(vm);
    if (result != ZUN_SUCCESS)
        return;

    const u32 spritesAfter = g_AnmManager->spritesToDraw;
    const u32 flushesAfter = g_AnmManager->flushesThisFrame;
    const bool appendedWithoutFlush =
        flushesAfter == flushesBefore && spritesAfter == spritesBefore + 1U;
    const bool appendedAfterFlush =
        flushesAfter > flushesBefore && spritesAfter == 1U;
    if (appendedWithoutFlush || appendedAfterFlush)
    {
        // The ordinary AsciiManager path appends one canonical quad.  Its
        // eventual backend workload is six logical triangle-list vertices.
        th08::psp::RenderPerfQueueFpsOverlayVertices(6U);
    }
}
} // namespace

PspAsciiRenderOwnerScope::PspAsciiRenderOwnerScope(
    PspAsciiRenderOwner owner)
    : previousOwner_(g_PspAsciiCurrentRenderOwner)
{
    g_PspAsciiCurrentRenderOwner = owner;
}

PspAsciiRenderOwnerScope::~PspAsciiRenderOwnerScope()
{
    g_PspAsciiCurrentRenderOwner = previousOwner_;
}
#endif
#if defined(PSP) && defined(TH08_PSP_DRAW_PRIORITY_SUBPROFILE) && TH08_PSP_DRAW_PRIORITY_SUBPROFILE
#if defined(TH08_PSP_HUD_TEXT_NATIVE) && TH08_PSP_HUD_TEXT_NATIVE
#define TH08_PSP_HUD_TEXT_NATIVE_ENABLED 1
extern "C" void th08_psp_ascii_popup_build_quad(AnmVm *vm, const AnmLoadedSprite *sprite, float widthPx, float scaleX,
                                                float scaleY, float *xyz12, float *uv8);
extern "C" void *th08_psp_bullet_me_reserve(void *deviceRaw, unsigned int quadCount);
extern "C" void th08_psp_bullet_me_reserve_commit(void *deviceRaw);
extern "C" int th08_psp_bullet_me_submit(void *deviceRaw, const void *vertices, unsigned int quadCount,
                                         const unsigned short *indices);
extern "C" int th08_psp_bullet_me_color_identity(void *deviceRaw);
#if defined(TH08_PSP_HUD_TEXT_NATIVE_AUDIT) && TH08_PSP_HUD_TEXT_NATIVE_AUDIT
extern "C" void th08_psp_ascii_last_quad(float *xyz12, float *uv8);
#endif
#else
#define TH08_PSP_HUD_TEXT_NATIVE_ENABLED 0
#endif
namespace
{
struct PspAsciiSubStats
{
    unsigned long calls, popupsUs, restUs;
    unsigned long hudStrings, hudGlyphs, hudFallbacks, hudAuditGlyphs, hudAuditMismatch;
    unsigned long hudSkipVm, hudSkipIndices, hudSkipAlpha, hudSkipTexture, hudSkipIdentity;
    unsigned long hudDrawCalls, hudDrawUs, hudSubmits, hudMergedStrings;
} g_PspAsciiSub;
extern "C" unsigned int sceKernelGetSystemTimeLow(void);
struct PspHudTextTimer
{
    unsigned int start;
    ~PspHudTextTimer()
    {
        g_PspAsciiSub.hudDrawUs += sceKernelGetSystemTimeLow() - start;
        if ((++g_PspAsciiSub.hudDrawCalls % 600UL) == 0UL)
            th08::psp::BootLog("HUD_TEXT stats calls=%lu us=%lu strings=%lu glyphs=%lu submits=%lu merged=%lu fallback=%lu\n",
                g_PspAsciiSub.hudDrawCalls, g_PspAsciiSub.hudDrawUs, g_PspAsciiSub.hudStrings,
                g_PspAsciiSub.hudGlyphs, g_PspAsciiSub.hudSubmits, g_PspAsciiSub.hudMergedStrings,
                g_PspAsciiSub.hudFallbacks);
    }
};
#if TH08_PSP_HUD_TEXT_NATIVE_ENABLED
struct PspHudGlyphVertex
{
    float u, v;
    unsigned char r, g, b, a;
    float x, y, z;
}; // PspClientVertex layout (24 bytes)
#if defined(TH08_PSP_HUD_TEXT_STREAM_AUDIT) && TH08_PSP_HUD_TEXT_STREAM_AUDIT
unsigned int g_PspHudStreamHash = 2166136261U;
unsigned int g_PspHudStreamGlyphs = 0U;
void PspHudStreamBytes(const void *data, unsigned int bytes)
{
    const unsigned char *p = static_cast<const unsigned char *>(data);
    while (bytes-- != 0U)
        g_PspHudStreamHash = (g_PspHudStreamHash ^ *p++) * 16777619U;
}
#endif
inline unsigned int PspHudMix(unsigned int c1, unsigned int c2)
{
    const unsigned int c = (c1 * c2) / 128U;
    return c >= 256U ? 255U : c;
}
// One AsciiManager string as a single indexed quad batch.  Mirrors the
// canonical per-glyph loop below (sprite choice, advance, newline) and the
// quad math of DrawNoRotation (BuildPspAsciiPopupQuad).  Returns false to let
// the canonical loop draw the string.
bool PspHudTextDrawNative(AsciiManager *am, const AsciiManagerString *s, AnmVm *vm, float spaceWidth,
                          PspHudGlyphVertex *batchOut = NULL, unsigned int *batchQuads = NULL)
{
    if (am->asciiAnm == NULL || g_Supervisor.d3dDevice == NULL || !vm->IsVisible() || !vm->flag1)
    {
        ++g_PspAsciiSub.hudSkipVm;
        return false;
    }
    const u16 *const indices = g_AnmManager->PspBulletUnifiedQuadIndices();
    if (indices == NULL)
    {
        ++g_PspAsciiSub.hudSkipIndices;
        return false;
    }
    const int base = s->isSelected ? (170 - ' ') : (31 - ' ');
    const u32 color1 = s->isSelected ? 0xffffffffU : s->color;
    if ((color1 >> 24) == 0U)
    {
        ++g_PspAsciiSub.hudSkipAlpha;
        return false; // DrawNoRotation draws nothing: keep the canonical walk
    }
    IDirect3DTexture8 *texture = NULL;
    const AnmLoadedSprite *firstSprite = NULL;
    unsigned int glyphs = 0U;
    for (const u8 *p = reinterpret_cast<const u8 *>(s->text); *p; ++p)
    {
        if (*p == '\n' || *p == ' ')
            continue;
        const AnmLoadedSprite *const sprite = am->asciiAnm->GetSprite(*p + base);
        if (sprite == NULL || sprite->texture == NULL || (texture != NULL && sprite->texture != texture))
        {
            ++g_PspAsciiSub.hudSkipTexture;
            return false;
        }
        texture = sprite->texture;
        if (firstSprite == NULL)
            firstSprite = sprite;
        ++glyphs;
    }
    if (glyphs == 0U || glyphs > 0x600U)
    {
        ++g_PspAsciiSub.hudSkipTexture;
        return false;
    }
    vm->loadedSprite = const_cast<AnmLoadedSprite *>(firstSprite);
    vm->color1.d3dColor = color1;
    vm->pos = s->position;
    // The render state DrawNoRotation would set for the first glyph; only then
    // does the device's colour path tell whether the vertex colour passes through.
    if (batchQuads == NULL || *batchQuads == 0U)
    {
        g_AnmManager->FlushVertexBuffer();
        g_AnmManager->PspMeApplySpriteState(vm);
        if (!th08_psp_bullet_me_color_identity(g_Supervisor.d3dDevice))
        {
            ++g_PspAsciiSub.hudSkipIdentity;
            return false;
        }
    }
    u32 color = vm->flag17 ? vm->color2.d3dColor : color1;
    if (g_AnmManager->useMixColor)
    {
        const u32 m = g_AnmManager->color.d3dColor;
        color = (PspHudMix(color >> 24, m >> 24) << 24) | (PspHudMix((color >> 16) & 255U, (m >> 16) & 255U) << 16) |
                (PspHudMix((color >> 8) & 255U, (m >> 8) & 255U) << 8) | PspHudMix(color & 255U, m & 255U);
    }
    const unsigned char cr = static_cast<unsigned char>((color >> 16) & 255U);
    const unsigned char cg = static_cast<unsigned char>((color >> 8) & 255U);
    const unsigned char cb = static_cast<unsigned char>(color & 255U);
    const unsigned char ca = static_cast<unsigned char>(color >> 24);
#if defined(TH08_PSP_HUD_TEXT_NATIVE_AUDIT) && TH08_PSP_HUD_TEXT_NATIVE_AUDIT
    // Audit: draw canonically and compare our quad with the one DrawNoRotation built.
    (void)cr; (void)cg; (void)cb; (void)ca;
    for (const u8 *p = reinterpret_cast<const u8 *>(s->text); *p; ++p)
    {
        if (*p == '\n')
        {
            vm->pos.y += 16.0f * s->scaleY;
            vm->pos.x = s->position.x;
            continue;
        }
        if (*p == ' ')
        {
            vm->pos.x += spaceWidth;
            continue;
        }
        const AnmLoadedSprite *const sprite = am->asciiAnm->GetSprite(*p + base);
        float xyz[12], uv[8], cxyz[12], cuv[8];
        th08_psp_ascii_popup_build_quad(vm, sprite, vm->spriteSize.x, vm->scale.x, vm->scale.y, xyz, uv);
        vm->loadedSprite = const_cast<AnmLoadedSprite *>(sprite);
        vm->color1.d3dColor = color1;
        g_AnmManager->DrawNoRotation(vm);
        th08_psp_ascii_last_quad(cxyz, cuv);
        ++g_PspAsciiSub.hudAuditGlyphs;
        bool same = true;
        for (int i = 0; i < 12 && same; ++i)
            same = xyz[i] == cxyz[i];
        for (int i = 0; i < 8 && same; ++i)
            same = uv[i] == cuv[i];
        if (!same)
        {
            ++g_PspAsciiSub.hudAuditMismatch;
            static unsigned samples = 0U;
            if (samples < 6U)
            {
                ++samples;
                th08::psp::BootLog("HUD_TEXT_AUDIT ch=%u want %f,%f,%f uv %f,%f got %f,%f,%f uv %f,%f\n", *p, cxyz[0], cxyz[1],
                                   cxyz[2], cuv[0], cuv[1], xyz[0], xyz[1], xyz[2], uv[0], uv[1]);
            }
        }
        vm->pos.x += spaceWidth;
    }
    ++g_PspAsciiSub.hudStrings;
    g_PspAsciiSub.hudGlyphs += glyphs;
    return true;
#else
    // Keep whatever was reserved before us in this present (the bullet batch
    // the ME wrote earlier this frame): a fresh reservation must not rewind
    // over it (r221/r222: some enemy bullets flickered).
    if (batchOut == NULL)
        th08_psp_bullet_me_reserve_commit(g_Supervisor.d3dDevice);
    PspHudGlyphVertex *const out =
        batchOut != NULL ? batchOut + *batchQuads * 4U :
        static_cast<PspHudGlyphVertex *>(th08_psp_bullet_me_reserve(g_Supervisor.d3dDevice, glyphs));
    if (out == NULL)
    {
        ++g_PspAsciiSub.hudFallbacks;
        return false;
    }
    unsigned int q = 0U;
    for (const u8 *p = reinterpret_cast<const u8 *>(s->text); *p; ++p)
    {
        if (*p == '\n')
        {
            vm->pos.y += 16.0f * s->scaleY;
            vm->pos.x = s->position.x;
            continue;
        }
        if (*p == ' ')
        {
            vm->pos.x += spaceWidth;
            continue;
        }
        const AnmLoadedSprite *const sprite = am->asciiAnm->GetSprite(*p + base);
        float xyz[12], uv[8];
        th08_psp_ascii_popup_build_quad(vm, sprite, vm->spriteSize.x, vm->scale.x, vm->scale.y, xyz, uv);
        PspHudGlyphVertex *const v = out + q * 4U;
        for (int i = 0; i < 4; ++i)
        {
            v[i].u = uv[i * 2];
            v[i].v = uv[i * 2 + 1];
            v[i].r = cr; v[i].g = cg; v[i].b = cb; v[i].a = ca;
            v[i].x = xyz[i * 3] + 0.5f;
            v[i].y = xyz[i * 3 + 1] + 0.5f;
            v[i].z = 1.0f - 2.0f * xyz[i * 3 + 2];
        }
        ++q;
        vm->pos.x += spaceWidth;
    }
    if (batchQuads != NULL)
    {
        *batchQuads += q;
    }
    else if (!th08_psp_bullet_me_submit(g_Supervisor.d3dDevice, out, q, indices))
    {
        ++g_PspAsciiSub.hudFallbacks;
        vm->pos = s->position;
        return false;
    }
#if defined(TH08_PSP_HUD_TEXT_STREAM_AUDIT) && TH08_PSP_HUD_TEXT_STREAM_AUDIT
    PspHudStreamBytes(&s->isGui, sizeof(s->isGui));
    PspHudStreamBytes(&s->isSelected, sizeof(s->isSelected));
    PspHudStreamBytes(s->text, strlen(s->text));
    PspHudStreamBytes(out, q * 4U * sizeof(*out));
    PspHudStreamBytes(&vm->pos, sizeof(vm->pos));
    PspHudStreamBytes(&vm->scale, sizeof(vm->scale));
    PspHudStreamBytes(&vm->color1, sizeof(vm->color1));
    g_PspHudStreamGlyphs += q;
#endif
    // The GE reads these vertices later: the next reservation (next string,
    // same present) must not rewind over them (r221: HUD digits flickered
    // under load).
    if (batchOut == NULL)
    {
        th08_psp_bullet_me_reserve_commit(g_Supervisor.d3dDevice);
        ++g_PspAsciiSub.hudSubmits;
    }
    ++g_PspAsciiSub.hudStrings;
    g_PspAsciiSub.hudGlyphs += q;
    return true;
#endif
}
#if defined(TH08_PSP_HUD_TEXT_BATCH) && TH08_PSP_HUD_TEXT_BATCH && \
    !(defined(TH08_PSP_HUD_TEXT_NATIVE_AUDIT) && TH08_PSP_HUD_TEXT_NATIVE_AUDIT)
// Only adjacent game-owned strings using this same VM and viewport may join.
// Position, scale, selected atlas and colour are baked per string by the r229
// builder; blend/depth/UV-scroll/mix/shake are invariant during this walk.
int PspHudTextDrawRun(AsciiManager *am, int first)
{
    AnmVm *const vm = &am->largeText;
    if (am->asciiAnm == NULL || g_Supervisor.d3dDevice == NULL || !vm->IsVisible() || !vm->flag1)
        return 0;
    const u16 *const indices = g_AnmManager->PspBulletUnifiedQuadIndices();
    if (indices == NULL)
        return 0;
    IDirect3DTexture8 *texture = NULL;
    unsigned int glyphs = 0U;
    int end = first;
    for (; end < am->numStrings; ++end)
    {
        const AsciiManagerString &s = am->strings[end];
        if (s.isGui != am->strings[first].isGui ||
            PspAsciiStringRenderOwner(am, end) != PspAsciiRenderOwner::Game ||
            (!s.isSelected && (s.color >> 24) == 0U))
            break;
        const int base = s.isSelected ? (170 - ' ') : (31 - ' ');
        IDirect3DTexture8 *candidateTexture = texture;
        unsigned int count = 0U;
        const u8 *p = reinterpret_cast<const u8 *>(s.text);
        for (; *p; ++p)
        {
            if (*p == '\n' || *p == ' ')
                continue;
            const AnmLoadedSprite *const sprite = am->asciiAnm->GetSprite(*p + base);
            if (sprite == NULL || sprite->texture == NULL ||
                (candidateTexture != NULL && candidateTexture != sprite->texture))
                break;
            candidateTexture = sprite->texture;
            ++count;
        }
        // Split BEFORE the string. Both index-table and arena limits still
        // apply; an allocation failure retries the old per-string path.
        if (*p != 0 || count == 0U || glyphs + count > 0x600U)
            break;
        texture = candidateTexture;
        glyphs += count;
    }
    if (end - first < 2)
        return 0;
    // Flush before reserving: queued canonical vertices own earlier storage.
    g_AnmManager->FlushVertexBuffer();
    th08_psp_bullet_me_reserve_commit(g_Supervisor.d3dDevice);
    PspHudGlyphVertex *const out = static_cast<PspHudGlyphVertex *>(
        th08_psp_bullet_me_reserve(g_Supervisor.d3dDevice, glyphs));
    if (out == NULL)
        return 0;
    unsigned int quads = 0U;
    const unsigned long stringsBefore = g_PspAsciiSub.hudStrings;
    const unsigned long glyphsBefore = g_PspAsciiSub.hudGlyphs;
#if defined(TH08_PSP_HUD_TEXT_STREAM_AUDIT) && TH08_PSP_HUD_TEXT_STREAM_AUDIT
    const unsigned int hashBefore = g_PspHudStreamHash;
    const unsigned int auditGlyphsBefore = g_PspHudStreamGlyphs;
#endif
    bool built = true;
    for (int index = first; index < end; ++index)
    {
        const AsciiManagerString &s = am->strings[index];
        vm->scale.x = s.scaleX;
        vm->scale.y = s.scaleY;
        if (!PspHudTextDrawNative(am, &s, vm, am->spaceWidth * s.scaleX, out, &quads))
        {
            built = false;
            break;
        }
    }
    if (!built || !th08_psp_bullet_me_submit(g_Supervisor.d3dDevice, out, quads, indices))
    {
        // No text primitives were submitted. Restore the first string's
        // inputs before its normal helper/canonical fallback retries it.
        g_PspAsciiSub.hudStrings = stringsBefore;
        g_PspAsciiSub.hudGlyphs = glyphsBefore;
#if defined(TH08_PSP_HUD_TEXT_STREAM_AUDIT) && TH08_PSP_HUD_TEXT_STREAM_AUDIT
        g_PspHudStreamHash = hashBefore;
        g_PspHudStreamGlyphs = auditGlyphsBefore;
#endif
        ++g_PspAsciiSub.hudFallbacks;
        vm->pos = am->strings[first].position;
        vm->scale.x = am->strings[first].scaleX;
        vm->scale.y = am->strings[first].scaleY;
        return 0;
    }
    th08_psp_bullet_me_reserve_commit(g_Supervisor.d3dDevice);
    ++g_PspAsciiSub.hudSubmits;
    g_PspAsciiSub.hudMergedStrings += end - first - 1;
    return end - first;
}
#define TH08_PSP_HUD_TEXT_BATCH_ENABLED 1
#endif
#endif
} // namespace
#else
#define TH08_PSP_HUD_TEXT_NATIVE_ENABLED 0
#endif

// Menu script indices and interrupts used by the original pause/retry state machines.
#define ASCII_SCRIPT_PAUSE 12
#define ASCII_SCRIPT_RETURN_TO_GAME 13
#define ASCII_SCRIPT_QUIT 14
#define ASCII_SCRIPT_RESTART 15
#define ASCII_SCRIPT_CONFIRM 16
#define ASCII_SCRIPT_YES 17
#define ASCII_SCRIPT_NO 18
#define ASCII_SCRIPT_DIFFICULTY 19
#define ASCII_SCRIPT_PRACTICE 20
#define ASCII_SCRIPT_SLOW_MODE 21
#define ASCII_SCRIPT_RETRY 22
#define ASCII_SCRIPT_RETRY_YES 23
#define ASCII_SCRIPT_RETRY_NO 24

#define CAPTURE_SCRIPT_MENU_BACKGROUND 0

#define ASCII_INTERRUPT_SHOW 1
#define ASCII_INTERRUPT_HIDE 2
#define ASCII_INTERRUPT_BACKGROUND_HIDE 1
#define ASCII_INTERRUPT_CLOCKTIME_FLIP 1

#define PAUSE_SPRITE(i) (i - ASCII_SCRIPT_PAUSE)
#define PAUSE_SPRITE_PAUSED PAUSE_SPRITE(ASCII_SCRIPT_PAUSE)
#define PAUSE_SPRITE_RETURN_TO_GAME PAUSE_SPRITE(ASCII_SCRIPT_RETURN_TO_GAME)
#define PAUSE_SPRITE_QUIT PAUSE_SPRITE(ASCII_SCRIPT_QUIT)
#define PAUSE_SPRITE_RESTART PAUSE_SPRITE(ASCII_SCRIPT_RESTART)
#define PAUSE_SPRITE_CONFIRM PAUSE_SPRITE(ASCII_SCRIPT_CONFIRM)
#define PAUSE_SPRITE_YES PAUSE_SPRITE(ASCII_SCRIPT_YES)
#define PAUSE_SPRITE_NO PAUSE_SPRITE(ASCII_SCRIPT_NO)
#define PAUSE_SPRITE_DIFFICULTY PAUSE_SPRITE(ASCII_SCRIPT_DIFFICULTY)
#define PAUSE_SPRITE_PRACTICE_MODE PAUSE_SPRITE(ASCII_SCRIPT_PRACTICE)
#define PAUSE_SPRITE_SLOW_MODE PAUSE_SPRITE(ASCII_SCRIPT_SLOW_MODE)

#define RETRY_SPRITE(i) (i - ASCII_SCRIPT_RETRY)
#define RETRY_SPRITE_RETRY RETRY_SPRITE(ASCII_SCRIPT_RETRY)
#define RETRY_SPRITE_YES RETRY_SPRITE(ASCII_SCRIPT_RETRY_YES)
#define RETRY_SPRITE_NO RETRY_SPRITE(ASCII_SCRIPT_RETRY_NO)
#define RETRY_SPRITE_CLOCKTIME 3

#define COLOR_MENU_ITEM_SELECTED 0xffff8080
#define COLOR_MENU_ITEM_NORMAL 0xff505050

#define ARCADE_LEFT 32
#define ARCADE_TOP 16
#define ARCADE_WIDTH 384
#define ARCADE_HEIGHT 448

enum
{
    PAUSE_MENU_STATE_INIT = 0,
    PAUSE_MENU_STATE_RETURN_TO_GAME_SELECTED = 1,
    PAUSE_MENU_STATE_RETURN_TO_TITLE_SELECTED = 2,
    PAUSE_MENU_STATE_RETRY_SELECTED = 3,
    PAUSE_MENU_STATE_CLOSING = 4,
    PAUSE_MENU_STATE_RETURN_TO_GAME_YES_SELECTED = 5,
    PAUSE_MENU_STATE_RETURN_TO_GAME_NO_SELECTED = 6,
    PAUSE_MENU_STATE_RETURN_TO_TITLE_YES_SELECTED = 7,
    PAUSE_MENU_STATE_RETURN_TO_TITLE_NO_SELECTED = 8,
    PAUSE_MENU_STATE_EXIT_TO_TITLE = 9,
    PAUSE_MENU_STATE_RESTART_GAME = 10
};

enum
{
    RETRY_MENU_STATE_INIT = 0,
    RETRY_MENU_STATE_YES_SELECTED = 1,
    RETRY_MENU_STATE_NO_SELECTED = 2,
    RETRY_MENU_STATE_RETRY = 3,
    RETRY_MENU_STATE_EXIT_TO_TITLE = 4,
};

#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
namespace
{
constexpr i32 kPspScorePopupSlots =
    ASCII_MAX_SCORE_POPUPS + ASCII_MAX_PLAYER_POPUPS;
constexpr i32 kPspTimePopupSlots = ASCII_MAX_TIME_POPUPS;
constexpr i32 kPspPopupBitsPerWord = 32;
constexpr i32 kPspScorePopupWords =
    (kPspScorePopupSlots + kPspPopupBitsPerWord - 1) /
    kPspPopupBitsPerWord;
constexpr i32 kPspTimePopupWords =
    (kPspTimePopupSlots + kPspPopupBitsPerWord - 1) /
    kPspPopupBitsPerWord;

// This is presentation-only derived state.  Keeping it outside AsciiManager
// preserves the original object layout and every replay/gameplay owner.  The
// authoritative inUse flags remain in the retail popup arrays.
struct PspAsciiPopupOccupancy
{
    AsciiManager *owner;
    u32 scoreWords[kPspScorePopupWords];
    u32 timeWords[kPspTimePopupWords];
    i32 scoreLastActiveWord;
    i32 timeLastActiveWord;
};

PspAsciiPopupOccupancy g_PspAsciiPopupOccupancy;

void PspSetPopupBit(u32 *words, i32 *lastActiveWord, i32 slot)
{
    const i32 word = slot / kPspPopupBitsPerWord;
    const i32 bit = slot % kPspPopupBitsPerWord;
    words[word] |= 1U << bit;
    if (*lastActiveWord < word)
        *lastActiveWord = word;
}

void PspClearPopupBit(u32 *words, i32 *lastActiveWord, i32 slot)
{
    const i32 word = slot / kPspPopupBitsPerWord;
    const i32 bit = slot % kPspPopupBitsPerWord;
    words[word] &= ~(1U << bit);

    if (word != *lastActiveWord || words[word] != 0)
        return;
    while (*lastActiveWord >= 0 && words[*lastActiveWord] == 0)
        --*lastActiveWord;
}

void PspResetAsciiPopupOccupancy(AsciiManager *owner)
{
    memset(&g_PspAsciiPopupOccupancy, 0,
           sizeof(g_PspAsciiPopupOccupancy));
    g_PspAsciiPopupOccupancy.owner = owner;
    g_PspAsciiPopupOccupancy.scoreLastActiveWord = -1;
    g_PspAsciiPopupOccupancy.timeLastActiveWord = -1;
}

void PspRebuildAsciiPopupOccupancy(AsciiManager *ascii)
{
    PspResetAsciiPopupOccupancy(ascii);
    for (i32 slot = 0; slot < kPspScorePopupSlots; ++slot)
    {
        if (ascii->scorePopups[slot].inUse)
        {
            PspSetPopupBit(g_PspAsciiPopupOccupancy.scoreWords,
                           &g_PspAsciiPopupOccupancy.scoreLastActiveWord,
                           slot);
        }
    }
    for (i32 slot = 0; slot < kPspTimePopupSlots; ++slot)
    {
        if (ascii->timePopups[slot].inUse)
        {
            PspSetPopupBit(g_PspAsciiPopupOccupancy.timeWords,
                           &g_PspAsciiPopupOccupancy.timeLastActiveWord,
                           slot);
        }
    }
}

void PspEnsureAsciiPopupOccupancy(AsciiManager *ascii)
{
    if (g_PspAsciiPopupOccupancy.owner != ascii)
        PspRebuildAsciiPopupOccupancy(ascii);
}

void PspMarkScorePopupActive(AsciiManager *ascii, i32 slot)
{
    PspEnsureAsciiPopupOccupancy(ascii);
    PspSetPopupBit(g_PspAsciiPopupOccupancy.scoreWords,
                   &g_PspAsciiPopupOccupancy.scoreLastActiveWord, slot);
}

void PspMarkTimePopupActive(AsciiManager *ascii, i32 slot)
{
    PspEnsureAsciiPopupOccupancy(ascii);
    PspSetPopupBit(g_PspAsciiPopupOccupancy.timeWords,
                   &g_PspAsciiPopupOccupancy.timeLastActiveWord, slot);
}

void PspMarkScorePopupInactive(i32 slot)
{
    PspClearPopupBit(g_PspAsciiPopupOccupancy.scoreWords,
                     &g_PspAsciiPopupOccupancy.scoreLastActiveWord, slot);
}

void PspMarkTimePopupInactive(i32 slot)
{
    PspClearPopupBit(g_PspAsciiPopupOccupancy.timeWords,
                     &g_PspAsciiPopupOccupancy.timeLastActiveWord, slot);
}

// The iterator snapshots one occupancy word at a time.  Clearing the current
// bit during expiry or stale-positive repair is safe, and ctz preserves the
// retail ascending slot order exactly.
struct PspPopupBitIterator
{
    PspPopupBitIterator(const u32 *sourceWords, i32 sourceWordCount,
                        i32 sourceSlotCount)
        : words(sourceWords), wordCount(sourceWordCount),
          slotCount(sourceSlotCount), nextWord(0), activeWord(0), pending(0)
    {
    }

    bool Next(i32 *slot)
    {
        for (;;)
        {
            if (pending != 0)
            {
                const u32 bit = static_cast<u32>(__builtin_ctz(pending));
                pending &= pending - 1U;
                const i32 candidate =
                    activeWord * kPspPopupBitsPerWord + static_cast<i32>(bit);
                if (candidate < slotCount)
                {
                    *slot = candidate;
                    return true;
                }
                continue;
            }

            if (nextWord >= wordCount)
                return false;
            activeWord = nextWord;
            pending = words[nextWord++];
        }
    }

    const u32 *words;
    i32 wordCount;
    i32 slotCount;
    i32 nextWord;
    i32 activeWord;
    u32 pending;
};
} // namespace
#endif

// FUNCTION: th08 0x402000
AsciiManager::AsciiManager()
{
}

// FUNCTION: th08 0x402130
AsciiManagerString::AsciiManagerString()
{
}

// FUNCTION: th08 0x402150
PauseMenu::PauseMenu()
{
}

// FUNCTION: th08 0x402190
RetryMenu::RetryMenu()
{
}

// FUNCTION: th08 0x4021d0
AsciiManagerPopup::AsciiManagerPopup()
{
}

// FUNCTION: th08 0x402200
#pragma var_order(i, popup, ascii)
ChainCallbackResult AsciiManager::OnUpdate(AsciiManager *ascii)
{
    AsciiManagerPopup *popup;
    i32 i;

    if (g_GameManager.isInGameMenu == 0 && g_GameManager.showRetryMenu == 0)
    {
        popup = &ascii->scorePopups[0];
        if (((*(u32 *)&g_GameManager.flags >> 10) & 1) == 0)
        {
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
            PspEnsureAsciiPopupOccupancy(ascii);
            PspPopupBitIterator scoreIterator(
                g_PspAsciiPopupOccupancy.scoreWords,
                g_PspAsciiPopupOccupancy.scoreLastActiveWord + 1,
                kPspScorePopupSlots);
            while (scoreIterator.Next(&i))
            {
                popup = &ascii->scorePopups[i];
#else
            for (i = 0; i < ASCII_MAX_SCORE_POPUPS + ASCII_MAX_PLAYER_POPUPS; i++, popup++)
            {
#endif
                if (!popup->inUse)
                {
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
                    // A stale positive only costs this visit; repair it
                    // without changing the authoritative popup array.
                    PspMarkScorePopupInactive(i);
#endif
                    continue;
                }
                popup->position.y -= 0.5f * g_Supervisor.framerateMultiplier;
                popup->timer++;
                if (popup->timer > 60)
                {
                    popup->inUse = false;
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
                    PspMarkScorePopupInactive(i);
#endif
                }
            }

            popup = &ascii->timePopups[0];
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
            PspPopupBitIterator timeIterator(
                g_PspAsciiPopupOccupancy.timeWords,
                g_PspAsciiPopupOccupancy.timeLastActiveWord + 1,
                kPspTimePopupSlots);
            while (timeIterator.Next(&i))
            {
                popup = &ascii->timePopups[i];
#else
            for (i = 0; i < ASCII_MAX_TIME_POPUPS; i++, popup++)
            {
#endif
                if (!popup->inUse)
                {
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
                    PspMarkTimePopupInactive(i);
#endif
                    continue;
                }
                popup->timer++;
                if (popup->timer > 90)
                {
                    popup->inUse = false;
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
                    PspMarkTimePopupInactive(i);
#endif
                }
            }
        }
    }
    else if (g_GameManager.isInGameMenu != 0)
    {
        ascii->pauseMenu.OnUpdate();
    }

    if (g_GameManager.showRetryMenu != 0)
    {
        ascii->retryMenu.OnUpdate();
    }

    ascii->UpdateVms();
    if (g_GameManager.IsDemoMode())
    {
        if (ascii->demoIcon.scriptIndex == 0)
        {
            ascii->asciiAnm->SetAndExecuteScriptIdx(&ascii->demoIcon, 11);
        }
        g_AnmManager->ExecuteScript(&ascii->demoIcon);
    }
    else
    {
        ascii->demoIcon.scriptIndex = 0;
    }
    ascii->frameTimer++;

    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

ChainCallbackResult AsciiManager::OnDrawLowPrio(AsciiManager *ascii)
{
    ascii->OnDrawLowPrioImpl();
    ascii->ResetStrings();
    ascii->pauseMenu.OnDraw();
    ascii->retryMenu.OnDraw();
    if (ascii->demoIcon.scriptIndex != 0)
    {
        g_AnmManager->DrawNoRotation(&ascii->demoIcon);
    }

    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

// FUNCTION: th08 0x402430
ChainCallbackResult AsciiManager::OnDrawHighPrio(AsciiManager *ascii)
{
    ascii->OnDrawHighPrioImpl();

    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

void AsciiManager::Reset()
{
    memset(&this->smallScoreText, 0, sizeof(AnmVm));
    memset(&this->popupText, 0, sizeof(AnmVm));
    memset(&this->largeText, 0, sizeof(AnmVm));
    memset(&this->strings, 0, sizeof(this->strings));
    memset(&this->pauseMenu, 0, sizeof(PauseMenu));
    memset(&this->retryMenu, 0, sizeof(RetryMenu));
    memset(&this->scorePopups, 0, sizeof(this->scorePopups));
    memset(&this->timePopups, 0, sizeof(this->timePopups));
#if defined(PSP)
    PspResetAsciiRenderOwners(this);
#endif
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
    PspResetAsciiPopupOccupancy(this);
#endif

    this->numStrings = 0;
    this->isGui = FALSE;
    this->isSelected = FALSE;
    this->nextScorePopupIndex = 0;
    this->nextPlayerPointPopupIndex = 0;
    /* nextTimePopupIndex is not set to 0?  */
    this->resetOnlyState829C = 0;
    this->color.d3dColor = 0xffffffff;
    this->scaleX = 1.0f;
    this->scaleY = 1.0f;
    this->smallScoreText.anchor = 3;
    this->popupText.anchor = 3;
    this->asciiAnm->InitializeAndSetSprite(&this->smallScoreText, 0);
    this->asciiAnm->InitializeAndSetSprite(&this->popupText, 136);
    this->asciiAnm->InitializeAndSetSprite(&this->largeText, 32);
    this->smallScoreText.pos.z = 0.1f;
    /* This was already set to FALSE ? */
    this->isSelected = FALSE;
    this->SetSpaceWidth(13);
}

void AsciiManager::InitializeVms()
{
    this->asciiAnm->SetAndExecuteScriptIdx(&this->youkaiGauge, 5);
    this->asciiAnm->SetAndExecuteScriptIdx(&this->youkaiGaugeYoukaiIcon, 7);
    this->asciiAnm->SetAndExecuteScriptIdx(&this->youkaiGaugeHumanIcon, 6);
    this->asciiAnm->SetAndExecuteScriptIdx(&this->youkaiGaugeCursor, 8);
    this->asciiAnm->SetAndExecuteScriptIdx(&this->percentageText, 4);
    this->asciiAnm->SetAndExecuteScriptIdx(&this->auxiliaryGaugeVm, 9);
    this->asciiAnm->SetAndExecuteScriptIdx(&this->bossMarkers[0], 10);
    this->asciiAnm->SetAndExecuteScriptIdx(&this->bossMarkers[1], 10);
    this->asciiAnm->SetAndExecuteScriptIdx(&this->bossMarkers[2], 10);
    this->asciiAnm->SetAndExecuteScriptIdx(&this->bossMarkers[3], 10);

    this->youkaiGaugeHumanIcon.pos.x -= (g_GameManager.youkaiGaugeHumanLimit * 56.0f) / -10000.0f;
    this->youkaiGaugeYoukaiIcon.pos.x += (g_GameManager.youkaiGaugeYoukaiLimit * 56.0f) / 10000.0f;

    this->SetGaugeInterrupt(this->GetGaugeInterrupt());
}

ZunResult AsciiManager::RegisterChain()
{
    AsciiManager *ascii = &g_AsciiManager;

    g_AsciiManagerCalcChain.SetCallback((ChainCallback)AsciiManager::OnUpdate);
    g_AsciiManagerCalcChain.addedCallback = (ChainLifetimeCallback)AsciiManager::AddedCallback;
    g_AsciiManagerCalcChain.deletedCallback = (ChainLifetimeCallback)AsciiManager::DeletedCallback;
    g_AsciiManagerCalcChain.arg = ascii;
    if (g_Chain.AddToCalcChain(&g_AsciiManagerCalcChain, CHAIN_PRIO_CALC_ASCIIMANAGER) != ZUN_SUCCESS)
    {
        return ZUN_ERROR;
    }

    g_AsciiManagerDrawChainLowPrio.SetCallback((ChainCallback)AsciiManager::OnDrawLowPrio);
    g_AsciiManagerDrawChainLowPrio.arg = ascii;
    g_Chain.AddToDrawChain(&g_AsciiManagerDrawChainLowPrio, CHAIN_PRIO_DRAW_ASCIIMANAGER_LOW_PRIO);

    g_AsciiManagerDrawChainHighPrio.SetCallback((ChainCallback)AsciiManager::OnDrawHighPrio);
    g_AsciiManagerDrawChainHighPrio.arg = ascii;
    g_Chain.AddToDrawChain(&g_AsciiManagerDrawChainHighPrio, CHAIN_PRIO_DRAW_ASCIIMANAGER_HIGH_PRIO);

    return ZUN_SUCCESS;
}

ZunResult AsciiManager::AddedCallback(AsciiManager *ascii)
{
    memset(ascii, 0, sizeof(AsciiManager));
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
    PspResetAsciiPopupOccupancy(ascii);
#endif

    ascii->asciiAnm = g_AnmManager->PreloadAnm(1, "ascii.anm");
    if (ascii->asciiAnm == NULL)
    {
        return ZUN_ERROR;
    }

    ascii->captureAnm = g_AnmManager->PreloadAnm(3, "capture.anm");
    if (ascii->captureAnm == NULL)
    {
        return ZUN_ERROR;
    }

    ascii->Reset();
    ascii->InitializeVms();

    return ZUN_SUCCESS;
}

// FUNCTION: th08 0x4028c0
ZunResult AsciiManager::DeletedCallback(AsciiManager *ascii)
{
#if defined(PSP)
    PspResetAsciiRenderOwners(NULL);
#endif
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
    // Invalidate rather than claiming that the still-populated retail arrays
    // are empty.  An unexpected callback before the next AddedCallback will
    // therefore rebuild, never skip an authoritative inUse popup.
    PspResetAsciiPopupOccupancy(NULL);
#endif
    g_AnmManager->ReleaseAnm(1);
    g_AnmManager->ReleaseAnm(3);

    return ZUN_SUCCESS;
}

// FUNCTION: th08 0x4028f0
void AsciiManager::CutChain()
{
    g_Chain.Cut(&g_AsciiManagerCalcChain);
    g_Chain.Cut(&g_AsciiManagerDrawChainLowPrio);
    /* ZUN seemingly forgot this: g_Chain.Cut(&g_AsciiManagerDrawChainHighPrio); */
}

#pragma var_order(nextString)
void AsciiManager::AddString(Float3 *position, const char *string)
{
    AsciiManagerString *nextString;

    if (this->numStrings >= ARRAY_SIZE_SIGNED(this->strings))
    {
        return;
    }

#if defined(PSP)
    const i32 stringIndex = this->numStrings;
    nextString = &this->strings[stringIndex];
#else
    nextString = &this->strings[this->numStrings];
#endif
    this->numStrings++;

#if defined(PSP)
    // The capacity rejection above must not create a phantom owner.  Every
    // accepted slot is overwritten, so ResetStrings needs no retail-layout or
    // per-frame memset change.
    PspEnsureAsciiRenderOwners(this);
    g_PspAsciiRenderOwners.strings[stringIndex] =
        g_PspAsciiCurrentRenderOwner;
#endif

    strcpy(nextString->text, string);

    nextString->position = *position;

    nextString->color = this->color.d3dColor;
    nextString->scaleX = this->scaleX;
    nextString->scaleY = this->scaleY;
    nextString->isGui = this->isGui;

    if (g_Supervisor.IsSoftwareTexturing())
    {
        nextString->isSelected = this->isSelected;
    }
    else
    {
        nextString->isSelected = FALSE;
    }
}

void AsciiManager::AddFormatText(Float3 *position, const char *fmt, ...)
{
    char buf[512];
    va_list va;

    va_start(va, fmt);
    vsprintf(buf, fmt, va);
    this->AddString(position, buf);
    va_end(va);
}

int AsciiManager::AddFormatText2(Float3 *position, const char *fmt, ...)
{
    char buf[512];
    va_list args;

    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    this->AddString(position, buf);
    va_end(args);

    /* Did you know that vsprintf returns the number of characters added to the
     * buffer? So ZUN did not have to call strlen here.
     */
    return strlen(buf);
}

// FUNCTION: th08 0x402b20
#pragma var_order(spaceWidth, i, curString, text, isGui, vector)
void AsciiManager::OnDrawLowPrioImpl()
{
    Float3 vector;
    ZunBool isGui = TRUE;
    int i;
    AsciiManagerString *curString = &this->strings[0];
    u8 *text;
    float spaceWidth;

    this->largeText.visible = true;
    this->largeText.anchor = 3;

#if TH08_PSP_HUD_TEXT_NATIVE_ENABLED
    {
    PspHudTextTimer hudTimer = {sceKernelGetSystemTimeLow()};
#if defined(TH08_PSP_HUD_TEXT_STREAM_AUDIT) && TH08_PSP_HUD_TEXT_STREAM_AUDIT
    g_PspHudStreamHash = 2166136261U;
    g_PspHudStreamGlyphs = 0U;
#endif
#endif
    for (i = 0; i < this->numStrings; i++, curString++)
    {
#if defined(PSP)
        const PspAsciiRenderOwner renderOwner =
            PspAsciiStringRenderOwner(this, i);
        const bool isFpsOverlay = renderOwner != PspAsciiRenderOwner::Game;
#endif
        this->largeText.pos = curString->position;

        text = (u8 *)curString->text;

        this->largeText.scale.x = curString->scaleX;
        this->largeText.scale.y = curString->scaleY;
        spaceWidth = this->spaceWidth * curString->scaleX;

        if (isGui != curString->isGui)
        {
            isGui = curString->isGui;

            g_AnmManager->FlushVertexBuffer();

            if (isGui)
            {
                g_Supervisor.viewport.X = g_GameManager.arcadeRegionTopLeftPos.x;
                g_Supervisor.viewport.Y = g_GameManager.arcadeRegionTopLeftPos.y;
                g_Supervisor.viewport.Width = g_GameManager.arcadeRegionSize.x;
                g_Supervisor.viewport.Height = g_GameManager.arcadeRegionSize.y;
                g_Supervisor.d3dDevice->SetViewport(&g_Supervisor.viewport);
            }
            else
            {
                g_Supervisor.viewport.X = 0;
                g_Supervisor.viewport.Y = 0;
                g_Supervisor.viewport.Width = 640;
                g_Supervisor.viewport.Height = 480;
                g_Supervisor.d3dDevice->SetViewport(&g_Supervisor.viewport);
            }
        }

#if TH08_PSP_HUD_TEXT_NATIVE_ENABLED
#if defined(TH08_PSP_HUD_TEXT_BATCH_ENABLED) && TH08_PSP_HUD_TEXT_BATCH_ENABLED
        if (!isFpsOverlay)
        {
            const int consumed = PspHudTextDrawRun(this, i);
            if (consumed != 0)
            {
                i += consumed - 1;
                curString += consumed - 1;
                continue;
            }
        }
#endif
        if (!isFpsOverlay && PspHudTextDrawNative(this, curString, &this->largeText, spaceWidth))
            continue;
#endif
        while (*text)
        {
            if (*text == '\n')
            {
                this->largeText.pos.y += 16.0f * curString->scaleY;
                this->largeText.pos.x = curString->position.x;
            }
            else if (*text == ' ')
            {
                this->largeText.pos.x += spaceWidth;
            }
            else
            {
                if (!curString->isSelected)
                {
                    this->largeText.loadedSprite = this->asciiAnm->GetSprite(*text + (31 - ' '));
                    this->largeText.color1.d3dColor = curString->color;
                }
                else
                {
                    this->largeText.loadedSprite = this->asciiAnm->GetSprite(*text + (170 - ' '));
                    this->largeText.color1.d3dColor = 0xffffffff;
                }

#if defined(PSP)
                if (isFpsOverlay)
                    PspDrawFpsOverlayGlyph(&this->largeText);
                else
#endif
                    g_AnmManager->DrawNoRotation(&this->largeText);
                this->largeText.pos.x += spaceWidth;
            }

            text++;
        }
    }

#if TH08_PSP_HUD_TEXT_NATIVE_ENABLED
#if defined(TH08_PSP_HUD_TEXT_STREAM_AUDIT) && TH08_PSP_HUD_TEXT_STREAM_AUDIT
    // Packed UV/RGBA/XYZ plus string/viewport order, independent of grouping.
    th08::psp::BootLog("HUD_STREAM st=%d f=%lu call=%lu glyphs=%u hash=%08x\n",
        g_GameManager.currentStage, static_cast<unsigned long>(g_GameManager.gameplayFrameCounter),
        g_PspAsciiSub.hudDrawCalls + 1UL, g_PspHudStreamGlyphs, g_PspHudStreamHash);
#endif
    }
#endif
    if (isGui)
    {
        g_AnmManager->FlushVertexBuffer();
        g_Supervisor.viewport.X = 0;
        g_Supervisor.viewport.Y = 0;
        g_Supervisor.viewport.Width = 640;
        g_Supervisor.viewport.Height = 480;
        g_Supervisor.d3dDevice->SetViewport(&g_Supervisor.viewport);
    }

    for (i = 0; i < ARRAY_SIZE_SIGNED(this->bossMarkers); i++)
    {
        if (this->bossMarkers[i].pos.x >= 56.0f && this->bossMarkers[i].pos.x <= 392.0f)
        {
            spaceWidth = fabsf(this->bossMarkers[i].pos.x - 32.0f - g_Player.position.x);

            this->bossMarkers[i].loadedSprite = this->asciiAnm->GetSprite(157);

            switch (this->bossMarkerStates[i])
            {
            case 0:
            no_flicker:
                this->bossMarkers[i].color1.r = 255;
                this->bossMarkers[i].color1.g = 255;
                this->bossMarkers[i].color1.b = 255;
                if (spaceWidth < 64.0f)
                {
                    this->bossMarkers[i].color1.a = (spaceWidth * 64.0f) / 64.0f + 96.0f;
                }
                else
                {
                    this->bossMarkers[i].color1.a = 160;
                }
                break;
            case 1:
                this->bossMarkers[i].color1.a = 128;
                this->bossMarkers[i].color1.r = 255;
                this->bossMarkers[i].color1.g = 64;
                this->bossMarkers[i].color1.b = 64;
                break;
            case 2:
                if (this->frameTimer % 8 == 0)
                {
                    this->bossMarkers[i].loadedSprite = this->asciiAnm->GetSprite(158);
                    this->bossMarkers[i].color1.a = 255;
                    this->bossMarkers[i].color1.r = 255;
                    this->bossMarkers[i].color1.g = 255;
                    this->bossMarkers[i].color1.b = 255;
                }
                else
                {
                    goto no_flicker;
                }
                break;
            case 3:
                if (this->frameTimer % 4 == 0)
                {
                    this->bossMarkers[i].loadedSprite = this->asciiAnm->GetSprite(158);
                    this->bossMarkers[i].color1.a = 255;
                    this->bossMarkers[i].color1.r = 255;
                    this->bossMarkers[i].color1.g = 255;
                    this->bossMarkers[i].color1.b = 255;
                }
                else
                {
                    goto no_flicker;
                }
                break;
            case 4:
                if (this->frameTimer % 2 == 0)
                {
                    this->bossMarkers[i].loadedSprite = this->asciiAnm->GetSprite(158);
                    this->bossMarkers[i].color1.a = 255;
                    this->bossMarkers[i].color1.r = 255;
                    this->bossMarkers[i].color1.g = 255;
                    this->bossMarkers[i].color1.b = 255;
                }
                else
                {
                    goto no_flicker;
                }
                break;
            }

            g_AnmManager->DrawNoRotation(&this->bossMarkers[i]);
        }
    }
}

void AsciiManager::CreateScorePopup(Float3 *position, i32 number, D3DCOLOR color)
{
    AsciiManagerPopup *popup;
    int characterCount;

    if (this->nextScorePopupIndex >= ASCII_MAX_SCORE_POPUPS)
    {
        this->nextScorePopupIndex = 0;
    }
    popup = &this->scorePopups[nextScorePopupIndex];
    popup->inUse = true;

    characterCount = 0;
    if (number >= 0)
    {
        while (number != 0)
        {
            popup->text[characterCount] = number % 10;
            characterCount++;
            number /= 10;
        }
    }
    else
    {
        popup->text[characterCount] = 10;
        characterCount++;
    }

    if (characterCount == 0)
    {
        popup->text[characterCount] = 0;
        characterCount++;
    }

    popup->characterCount = characterCount;
    popup->color = color;
    popup->timer = 0;
    popup->position = *position;
    popup->position.x += g_GameManager.arcadeRegionTopLeftPos.x;
    popup->position.y += g_GameManager.arcadeRegionTopLeftPos.y;
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
    PspMarkScorePopupActive(this, nextScorePopupIndex);
#endif
    this->nextScorePopupIndex++;
}

void AsciiManager::CreatePlayerPointPopup(Float3 *position, i32 number, D3DCOLOR color)
{
    AsciiManagerPopup *popup;
    int characterCount;

    if (this->nextPlayerPointPopupIndex >= ASCII_MAX_PLAYER_POPUPS)
    {
        this->nextPlayerPointPopupIndex = 0;
    }
    popup = &this->scorePopups[ASCII_MAX_SCORE_POPUPS + nextPlayerPointPopupIndex];
    popup->inUse = true;

    characterCount = 0;
    if (number >= 0)
    {
        while (number != 0)
        {
            popup->text[characterCount] = number % 10;
            characterCount++;
            number /= 10;
        }
    }
    else
    {
        popup->text[characterCount] = 10;
        characterCount++;
    }

    if (characterCount == 0)
    {
        popup->text[characterCount] = 0;
        characterCount++;
    }

    popup->characterCount = characterCount;
    popup->color = color;
    popup->timer = 0;
    popup->position = *position;
    popup->position.x += g_GameManager.arcadeRegionTopLeftPos.x;
    popup->position.y += g_GameManager.arcadeRegionTopLeftPos.y;
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
    PspMarkScorePopupActive(
        this, ASCII_MAX_SCORE_POPUPS + nextPlayerPointPopupIndex);
#endif
    this->nextPlayerPointPopupIndex++;
}

void AsciiManager::CreateTimePopup(Float3 *position, i32 primaryNumber, i32 secondaryNumber, D3DCOLOR color)
{
    AsciiManagerPopup *popup;
    int characterCount;

    if (this->nextTimePopupIndex >= ASCII_MAX_TIME_POPUPS)
    {
        this->nextTimePopupIndex = 0;
    }
    popup = &this->timePopups[nextTimePopupIndex];
    popup->inUse = true;

    characterCount = 0;
    if (secondaryNumber > 0)
    {
        popup->text[characterCount] = 15;
        characterCount++;
        while (secondaryNumber != 0)
        {
            popup->text[characterCount] = secondaryNumber % 10;
            characterCount++;
            secondaryNumber /= 10;
        }
        popup->text[characterCount] = 14;
        characterCount++;
    }

    if (primaryNumber > 0)
    {
        while (primaryNumber != 0)
        {
            popup->text[characterCount] = primaryNumber % 10;
            characterCount++;
            primaryNumber /= 10;
        }
    }
    else
    {
        popup->text[characterCount] = 0;
        characterCount++;
    }

    popup->text[characterCount] = 13;
    characterCount++;

    popup->characterCount = characterCount;
    popup->color = color;
    popup->timer = 0;
    popup->position = *position;
    popup->position.x += g_GameManager.arcadeRegionTopLeftPos.x;
    popup->position.y += g_GameManager.arcadeRegionTopLeftPos.y;
    popup->scale.x = this->scaleX;
    popup->scale.y = this->scaleY;
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
    PspMarkTimePopupActive(this, nextTimePopupIndex);
#endif
    this->nextTimePopupIndex++;
}

void AsciiManager::CreateFamiliarPopup(Float3 *position, i32 primaryNumber, i32 secondaryNumber, D3DCOLOR color)
{
    AsciiManagerPopup *popup;
    int characterCount;

    if (this->nextTimePopupIndex >= ASCII_MAX_TIME_POPUPS)
    {
        this->nextTimePopupIndex = 0;
    }
    popup = &this->timePopups[nextTimePopupIndex];
    popup->inUse = true;

    characterCount = 0;
    if (secondaryNumber > 0)
    {
        popup->text[characterCount] = 15;
        characterCount++;
        while (secondaryNumber != 0)
        {
            popup->text[characterCount] = secondaryNumber % 10;
            characterCount++;
            secondaryNumber /= 10;
        }
        popup->text[characterCount] = 14;
        characterCount++;
    }

    if (primaryNumber > 0)
    {
        while (primaryNumber != 0)
        {
            popup->text[characterCount] = primaryNumber % 10;
            characterCount++;
            primaryNumber /= 10;
        }
    }
    else
    {
        popup->text[characterCount] = 0;
        characterCount++;
    }

    popup->text[characterCount] = 13;
    characterCount++;

    popup->characterCount = characterCount;
    popup->color = color;
    popup->timer = 88;
    popup->position = *position;
    popup->position.x += g_GameManager.arcadeRegionTopLeftPos.x + 3.5f * characterCount;
    popup->position.y += g_GameManager.arcadeRegionTopLeftPos.y;
    popup->scale.x = this->scaleX;
    popup->scale.y = this->scaleY;
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
    PspMarkTimePopupActive(this, nextTimePopupIndex);
#endif
    this->nextTimePopupIndex++;
}

// FUNCTION: th08 0x4037b0
i32 PauseMenu::OnUpdate()
{
    i32 i;

    if (WAS_PRESSED(TH_BUTTON_MENU) && this->curState != PAUSE_MENU_STATE_CLOSING)
    {
        g_SoundPlayer.PlaySoundByIdx(SOUND_SELECT, 0);

        this->curState = PAUSE_MENU_STATE_CLOSING;

        for (i = 0; i < ARRAY_SIZE(this->menuSprites); i++)
        {
            if (this->menuSprites[i].IsVisible())
            {
                this->menuSprites[i].pendingInterrupt = ASCII_INTERRUPT_HIDE;
            }
        }

        this->numFrames = 0;
        this->menuBackground.pendingInterrupt = ASCII_INTERRUPT_BACKGROUND_HIDE;
    }

    if (WAS_PRESSED(TH_BUTTON_Q) && this->curState != PAUSE_MENU_STATE_EXIT_TO_TITLE)
    {
        g_SoundPlayer.PlaySoundByIdx(SOUND_SELECT, 0);

        this->curState = PAUSE_MENU_STATE_EXIT_TO_TITLE;

        for (i = 0; i < ARRAY_SIZE(this->menuSprites); i++)
        {
            if (this->menuSprites[i].IsVisible())
            {
                this->menuSprites[i].pendingInterrupt = ASCII_INTERRUPT_HIDE;
            }
        }

        this->numFrames = 0;
    }

    if (!g_GameManager.IsReplay() && WAS_PRESSED(TH_BUTTON_RESET) && this->curState != PAUSE_MENU_STATE_EXIT_TO_TITLE)
    {
        g_SoundPlayer.PlaySoundByIdx(SOUND_SELECT, 0);

        this->curState = PAUSE_MENU_STATE_RESTART_GAME;

        for (i = 0; i < ARRAY_SIZE(this->menuSprites); i++)
        {
            if (this->menuSprites[i].IsVisible())
            {
                this->menuSprites[i].pendingInterrupt = ASCII_INTERRUPT_HIDE;
            }
        }

        this->numFrames = 0;
    }

    switch (this->curState)
    {
    case PAUSE_MENU_STATE_INIT:
        for (i = 0; i < ARRAY_SIZE(this->menuSprites); i++)
        {
            g_AsciiManager.asciiAnm->SetAndExecuteScriptIdx(&this->menuSprites[i], i + ASCII_SCRIPT_PAUSE);
        }

        for (i = 0; i < 4; i++)
        {
            this->menuSprites[i].pendingInterrupt = ASCII_INTERRUPT_SHOW;
        }

        if (g_GameManager.IsSpellPractice() && g_GameManager.currentSpellCardNumber >= SPELLCARD_LAST_WORD_START)
        {
            g_AsciiManager.asciiAnm->SetSprite(&this->menuSprites[PAUSE_SPRITE_DIFFICULTY], 288);
        }
        else
        {
            g_AsciiManager.asciiAnm->SetSprite(&this->menuSprites[PAUSE_SPRITE_DIFFICULTY], g_GameManager.difficulty + 283);
        }

        if (!g_GameManager.IsPracticeMode())
        {
            this->menuSprites[PAUSE_SPRITE_PRACTICE_MODE].SetInvisible();
        }

        if (!g_GameManager.cfg->slowMode)
        {
            this->menuSprites[PAUSE_SPRITE_SLOW_MODE].SetInvisible();
        }

        if (g_GameManager.IsReplay())
        {
            this->menuSprites[PAUSE_SPRITE_RESTART].currentInstruction = NULL;
        }

        this->curState++;
        this->numFrames = 0;

        if (g_Supervisor.flags.lockableBackbuffer)
        {
            g_AsciiManager.captureAnm->SetAndExecuteScriptIdx(&this->menuBackground, CAPTURE_SCRIPT_MENU_BACKGROUND);

            // Seemingly intentionally the width and height are switched?
            if (g_AnmManager->SetTextureCaptureParams(3,
                                                      ARCADE_LEFT,
                                                      ARCADE_TOP,
                                                      ARCADE_WIDTH,
                                                      ARCADE_HEIGHT,
                                                      this->menuBackground.loadedSprite->startPixelInclusive.x,
                                                      this->menuBackground.loadedSprite->startPixelInclusive.y,
                                                      this->menuBackground.loadedSprite->heightPx,
                                                      this->menuBackground.loadedSprite->widthPx) != ZUN_SUCCESS)
            {
                // ZUN landmine: if the screen capture never works, the pause
                // menu gets stuck and only the Escape, Q and R keys work.
                this->curState = PAUSE_MENU_STATE_INIT;
                return 0;
            }
            else
            {
                this->menuBackground.pos.x = ARCADE_LEFT;
                this->menuBackground.pos.y = ARCADE_TOP;
                this->menuBackground.pos.z = 0.0f;
            }
        }
        // fallthrough
    case PAUSE_MENU_STATE_RETURN_TO_GAME_SELECTED:
        this->menuSprites[PAUSE_SPRITE_RETURN_TO_GAME].color1.d3dColor = COLOR_WHITE;
        this->menuSprites[PAUSE_SPRITE_QUIT].color1.d3dColor = this->menuSprites[PAUSE_SPRITE_RESTART].color1.d3dColor = COLOR_MENU_ITEM_NORMAL;

        this->menuSprites[PAUSE_SPRITE_RETURN_TO_GAME].pos2 = Float3(-4.0f, -4.0f, 0.0f);
        this->menuSprites[PAUSE_SPRITE_QUIT].pos2 = this->menuSprites[PAUSE_SPRITE_RESTART].pos2 = Float3(0.0f, 0.0f, 0.0f);

        if (this->numFrames >= 4)
        {
            if (!g_GameManager.IsReplay())
            {
                if (WAS_PRESSED(TH_BUTTON_UP))
                {
                    this->curState = PAUSE_MENU_STATE_RETRY_SELECTED;
                    g_SoundPlayer.PlaySoundByIdx(SOUND_SHOOT, 0);
                }
            }
            else if (WAS_PRESSED(TH_BUTTON_UP))
            {
                this->curState = PAUSE_MENU_STATE_RETURN_TO_TITLE_SELECTED;
                g_SoundPlayer.PlaySoundByIdx(SOUND_SHOOT, 0);
            }

            if (WAS_PRESSED(TH_BUTTON_DOWN))
            {
                this->curState = PAUSE_MENU_STATE_RETURN_TO_TITLE_SELECTED;
                g_SoundPlayer.PlaySoundByIdx(SOUND_SHOOT, 0);
            }

            if (WAS_PRESSED(TH_BUTTON_SELECTMENU))
            {
                g_SoundPlayer.PlaySoundByIdx(SOUND_SELECT, 0);

                for (i = PAUSE_SPRITE_PAUSED; i < PAUSE_SPRITE_CONFIRM; i++)
                {
                    this->menuSprites[i].pendingInterrupt = ASCII_INTERRUPT_HIDE;
                }

                this->curState = PAUSE_MENU_STATE_CLOSING;
                this->numFrames = 0;
                this->menuBackground.pendingInterrupt = ASCII_INTERRUPT_BACKGROUND_HIDE;
            }
        }
        break;
    case PAUSE_MENU_STATE_RETURN_TO_TITLE_SELECTED:
        this->menuSprites[PAUSE_SPRITE_RETURN_TO_GAME].color1.d3dColor = this->menuSprites[PAUSE_SPRITE_RESTART].color1.d3dColor = COLOR_MENU_ITEM_NORMAL;
        this->menuSprites[PAUSE_SPRITE_QUIT].color1.d3dColor = COLOR_WHITE;

        this->menuSprites[PAUSE_SPRITE_RETURN_TO_GAME].pos2 = this->menuSprites[PAUSE_SPRITE_RESTART].pos2 = Float3(0.0f, 0.0f, 0.0f);
        this->menuSprites[PAUSE_SPRITE_QUIT].pos2 = Float3(-4.0f, -4.0f, 0.0f);

        if (this->numFrames >= 4)
        {
            if (WAS_PRESSED(TH_BUTTON_UP))
            {
                this->curState = PAUSE_MENU_STATE_RETURN_TO_GAME_SELECTED;
                g_SoundPlayer.PlaySoundByIdx(SOUND_SHOOT, 0);
            }
            if (g_GameManager.IsReplay())
            {
                if (WAS_PRESSED(TH_BUTTON_DOWN))
                {
                    this->curState = PAUSE_MENU_STATE_RETURN_TO_GAME_SELECTED;
                    g_SoundPlayer.PlaySoundByIdx(SOUND_SHOOT, 0);
                }
            }
            else if (WAS_PRESSED(TH_BUTTON_DOWN))
            {
                this->curState = PAUSE_MENU_STATE_RETRY_SELECTED;
                g_SoundPlayer.PlaySoundByIdx(SOUND_SHOOT, 0);
            }

            if (WAS_PRESSED(TH_BUTTON_SELECTMENU))
            {
                g_SoundPlayer.PlaySoundByIdx(SOUND_SELECT, 0);

                for (i = PAUSE_SPRITE_PAUSED; i < PAUSE_SPRITE_CONFIRM; i++)
                {
                    this->menuSprites[i].pendingInterrupt = ASCII_INTERRUPT_HIDE;
                }

                for (; i < PAUSE_SPRITE_DIFFICULTY; i++)
                {
                    this->menuSprites[i].pendingInterrupt = ASCII_INTERRUPT_SHOW;
                }

                this->curState = PAUSE_MENU_STATE_RETURN_TO_GAME_NO_SELECTED;
                this->numFrames = 0;
            }
        }
        break;
    case PAUSE_MENU_STATE_RETRY_SELECTED:
        this->menuSprites[PAUSE_SPRITE_RETURN_TO_GAME].color1.d3dColor = this->menuSprites[PAUSE_SPRITE_QUIT].color1.d3dColor = COLOR_MENU_ITEM_NORMAL;
        this->menuSprites[PAUSE_SPRITE_RESTART].color1.d3dColor = COLOR_WHITE;

        this->menuSprites[PAUSE_SPRITE_RETURN_TO_GAME].pos2 = this->menuSprites[PAUSE_SPRITE_QUIT].pos2 = Float3(0.0f, 0.0f, 0.0f);
        this->menuSprites[PAUSE_SPRITE_RESTART].pos2 = Float3(-4.0f, -4.0f, 0.0f);

        if (this->numFrames >= 4)
        {
            if (WAS_PRESSED(TH_BUTTON_UP))
            {
                this->curState = PAUSE_MENU_STATE_RETURN_TO_TITLE_SELECTED;
                g_SoundPlayer.PlaySoundByIdx(SOUND_SHOOT, 0);
            }
            if (WAS_PRESSED(TH_BUTTON_DOWN))
            {
                this->curState = PAUSE_MENU_STATE_RETURN_TO_GAME_SELECTED;
                g_SoundPlayer.PlaySoundByIdx(SOUND_SHOOT, 0);
            }

            if (WAS_PRESSED(TH_BUTTON_SELECTMENU))
            {
                g_SoundPlayer.PlaySoundByIdx(SOUND_SELECT, 0);

                for (i = PAUSE_SPRITE_PAUSED; i < PAUSE_SPRITE_CONFIRM; i++)
                {
                    this->menuSprites[i].pendingInterrupt = ASCII_INTERRUPT_HIDE;
                }

                for (; i < PAUSE_SPRITE_DIFFICULTY; i++)
                {
                    this->menuSprites[i].pendingInterrupt = ASCII_INTERRUPT_SHOW;
                }

                this->curState = PAUSE_MENU_STATE_RETURN_TO_TITLE_NO_SELECTED;
                this->numFrames = 0;
            }
        }
        break;
        break;
    case PAUSE_MENU_STATE_CLOSING:
        if (this->numFrames >= 20)
        {
            this->curState = 0;

            g_GameManager.isInGameMenu = FALSE;

            for (i = 0; i < ARRAY_SIZE(this->menuSprites); i++)
            {
                this->menuSprites[i].SetInvisible();
            }

            g_SoundPlayer.UnPause();
            g_Supervisor.systemTime = timeGetTime();
        }
        break;
    case PAUSE_MENU_STATE_RETURN_TO_GAME_YES_SELECTED:
    case PAUSE_MENU_STATE_RETURN_TO_TITLE_YES_SELECTED:
        this->menuSprites[PAUSE_SPRITE_YES].color1.d3dColor = COLOR_MENU_ITEM_SELECTED;
        this->menuSprites[PAUSE_SPRITE_NO].color1.d3dColor = COLOR_MENU_ITEM_NORMAL;

        this->menuSprites[PAUSE_SPRITE_YES].pos2 = Float3(-4.0f, -4.0f, 0.0f);
        this->menuSprites[PAUSE_SPRITE_NO].pos2 = Float3(0.0f, 0.0f, 0.0f);

        if (this->numFrames >= 4)
        {
            if (WAS_PRESSED(TH_BUTTON_UP) || WAS_PRESSED(TH_BUTTON_DOWN))
            {
                if (this->curState == PAUSE_MENU_STATE_RETURN_TO_GAME_YES_SELECTED)
                {
                    this->curState = PAUSE_MENU_STATE_RETURN_TO_GAME_NO_SELECTED;
                }
                else
                {
                    this->curState = PAUSE_MENU_STATE_RETURN_TO_TITLE_NO_SELECTED;
                }

                g_SoundPlayer.PlaySoundByIdx(SOUND_SHOOT, 0);
            }

            if (WAS_PRESSED(TH_BUTTON_SELECTMENU))
            {
                g_SoundPlayer.PlaySoundByIdx(SOUND_SELECT, 0);

                for (i = PAUSE_SPRITE_CONFIRM; i < PAUSE_SPRITE_DIFFICULTY; i++)
                {
                    this->menuSprites[i].pendingInterrupt = ASCII_INTERRUPT_HIDE;
                }

                if (this->curState == PAUSE_MENU_STATE_RETURN_TO_GAME_YES_SELECTED)
                {
                    this->curState = PAUSE_MENU_STATE_EXIT_TO_TITLE;
                }
                else
                {
                    this->curState = PAUSE_MENU_STATE_RESTART_GAME;
                }

                this->numFrames = 0;
            }
        }
        break;
    case PAUSE_MENU_STATE_RETURN_TO_GAME_NO_SELECTED:
    case PAUSE_MENU_STATE_RETURN_TO_TITLE_NO_SELECTED:
        this->menuSprites[PAUSE_SPRITE_YES].color1.d3dColor = COLOR_MENU_ITEM_NORMAL;
        this->menuSprites[PAUSE_SPRITE_NO].color1.d3dColor = COLOR_MENU_ITEM_SELECTED;

        this->menuSprites[PAUSE_SPRITE_YES].pos2 = Float3(0.0f, 0.0f, 0.0f);
        this->menuSprites[PAUSE_SPRITE_NO].pos2 = Float3(-4.0f, -4.0f, 0.0f);

        if (this->numFrames >= 4)
        {
            if (WAS_PRESSED(TH_BUTTON_UP) || WAS_PRESSED(TH_BUTTON_DOWN))
            {
                if (this->curState == PAUSE_MENU_STATE_RETURN_TO_GAME_NO_SELECTED)
                {
                    this->curState = PAUSE_MENU_STATE_RETURN_TO_GAME_YES_SELECTED;
                }
                else
                {
                    this->curState = PAUSE_MENU_STATE_RETURN_TO_TITLE_YES_SELECTED;
                }

                g_SoundPlayer.PlaySoundByIdx(SOUND_SHOOT, 0);
            }

            if (WAS_PRESSED(TH_BUTTON_SELECTMENU))
            {
                g_SoundPlayer.PlaySoundByIdx(SOUND_SELECT, 0);

                for (i = PAUSE_SPRITE_PAUSED; i < PAUSE_SPRITE_CONFIRM; i++)
                {
                    this->menuSprites[i].pendingInterrupt = ASCII_INTERRUPT_SHOW;
                }

                for (; i < PAUSE_SPRITE_DIFFICULTY; i++)
                {
                    this->menuSprites[i].pendingInterrupt = ASCII_INTERRUPT_HIDE;
                }

                if (this->curState == PAUSE_MENU_STATE_RETURN_TO_GAME_NO_SELECTED)
                {
                    this->curState = PAUSE_MENU_STATE_RETURN_TO_TITLE_SELECTED;
                }
                else
                {
                    this->curState = PAUSE_MENU_STATE_RETRY_SELECTED;
                }

                this->numFrames = 0;
            }
        }
        break;
    case PAUSE_MENU_STATE_EXIT_TO_TITLE:
        if (this->numFrames >= 20)
        {
            this->curState = 0;

            g_Supervisor.curState = SupervisorState_TitleScreen;
            g_GameManager.isInGameMenu = FALSE;
            g_Supervisor.systemTime = timeGetTime();

            ResultScreen::RegisterChain(RESULT_SCREEN_REGISTER_SAVE_DATA);
        }
        break;
    case PAUSE_MENU_STATE_RESTART_GAME:
        if (this->numFrames >= 20)
        {
            if (!g_GameManager.IsSpellPractice() && !g_GameManager.IsPracticeMode() && g_GameManager.difficulty != EXTRA)
            {
                this->curState = PAUSE_MENU_STATE_INIT;
                g_Supervisor.curState = SupervisorState_GameManagerRestartFromBeginning;
                g_GameManager.isInGameMenu = FALSE;
                g_Supervisor.systemTime = timeGetTime();
            }
            else
            {
                if (g_GameManager.IsSpellPractice() &&
                    !GameManager::ShouldPauseMusicInSpellPractice(g_GameManager.currentSpellCardNumber))
                {
                    g_SoundPlayer.UnPause();
                    g_SoundPlayer.FadeIn(2.0f);
                }
                else
                {
                    g_Supervisor.StopAudio();
                }

                g_Supervisor.curState = SupervisorState_SpellcardPracticeRestart;

                g_Gui.CaptureArcade();

                g_GameManager.isInGameMenu = FALSE;
                g_Supervisor.systemTime = timeGetTime();

                return 0;
            }
        }
        break;
    }

    for (i = 0; i < ARRAY_SIZE(this->menuSprites); i++)
    {
        g_AnmManager->ExecuteScript(&this->menuSprites[i]);
    }

    if (g_Supervisor.flags.lockableBackbuffer)
    {
        g_AnmManager->ExecuteScript(&this->menuBackground);
    }

    this->numFrames++;

    return 0;
}

// FUNCTION: th08 0x404720
Float3::Float3(float x, float y, float z)
{
    this->x = x;
    this->y = y;
    this->z = z;
}

#pragma var_order(menuBackground, vmIdx)
// FUNCTION: th08 0x404750
void PauseMenu::OnDraw()
{
    u32 vmIdx;
    if (g_GameManager.isInGameMenu)
    {
        g_AnmManager->FlushVertexBuffer();
        g_Supervisor.viewport.X = g_GameManager.arcadeRegionTopLeftPos.x;
        g_Supervisor.viewport.Y = g_GameManager.arcadeRegionTopLeftPos.y;
        g_Supervisor.viewport.Width = g_GameManager.arcadeRegionSize.x;
        g_Supervisor.viewport.Height = g_GameManager.arcadeRegionSize.y;
        g_Supervisor.d3dDevice->SetViewport(&g_Supervisor.viewport);
        if (((g_EclGameTimeScaleFlags >> 1) & 1) != 0 && this->curState != 0)
        {
            AnmVm menuBackground = this->menuBackground;
            menuBackground.zWriteDisabled = TRUE;
            g_AnmManager->DrawNoRotation(&menuBackground);
        }
        for (vmIdx = 0; vmIdx < 10; vmIdx++)
        {
            if (this->menuSprites[vmIdx].IsVisible())
                g_AnmManager->DrawNoRotation(&this->menuSprites[vmIdx]);
        }
    }
}

// FUNCTION: th08 0x404890
i32 RetryMenu::OnUpdate()
{
    i32 i;

    if (g_GameManager.IsPracticeMode() && !g_GameManager.flags.isSpellPractice)
    {
        g_GameManager.showRetryMenu = FALSE;
        g_GameManager.globals->displayScore = g_GameManager.globals->score;
        g_Supervisor.curState = SupervisorState_ResultScreenFromGame;
        return 1;
    }

    if (g_GameManager.IsReplay())
    {
        g_GameManager.showRetryMenu = FALSE;
        g_Supervisor.curState = SupervisorState_FinishReplay;
        g_GameManager.globals->displayScore = g_GameManager.globals->score;
        return 1;
    }

    switch (this->curState)
    {
    case RETRY_MENU_STATE_INIT:
        if (this->numFrames == 0)
        {
            if (!g_GameManager.IsSpellPractice() && g_GameManager.difficulty < EXTRA
                && ((i8)g_GameManager.GetClockTime() >= 11 || g_GameManager.currentStage == STAGE6B))
            {
                g_GameManager.showRetryMenu = FALSE;
                g_GameManager.globals->displayScore = g_GameManager.globals->score;

                if (g_GameManager.difficulty >= EXTRA)
                {
                    g_Supervisor.curState = SupervisorState_ResultScreenFromGame;
                }
                else
                {
                    g_GameManager.flags.gameCleared = FALSE;
                    g_Supervisor.curState = SupervisorState_Ending;
                }

                return 1;
            }

            if (g_GameManager.IsSpellPractice() &&
                !GameManager::ShouldPauseMusicInSpellPractice(g_GameManager.currentSpellCardNumber))
            {
                g_SoundPlayer.PartialFadeOut(1.0f);
            }
            else
            {
                g_SoundPlayer.Pause();
            }

            for (i = 0; i < RETRY_SPRITE_CLOCKTIME; i++)
            {
                g_AsciiManager.asciiAnm->SetAndExecuteScriptIdx(&this->menuSprites[i], i + ASCII_SCRIPT_RETRY);
                this->menuSprites[i].pendingInterrupt = ASCII_INTERRUPT_SHOW;
            }

            g_Gui.timesAnm->SetAndExecuteScriptIdx(&this->menuSprites[RETRY_SPRITE_CLOCKTIME], 1);
            g_Gui.timesAnm->SetSprite(&this->menuSprites[RETRY_SPRITE_CLOCKTIME], (i8)g_GameManager.GetClockTime());

            if (g_Supervisor.flags.lockableBackbuffer)
            {
                g_AsciiManager.captureAnm->SetAndExecuteScriptIdx(&this->menuBackground, 0);

                // Seemingly intentionally the width and height are switched?
                if (g_AnmManager->SetTextureCaptureParams(3,
                                                          ARCADE_LEFT,
                                                          ARCADE_TOP,
                                                          ARCADE_WIDTH,
                                                          ARCADE_HEIGHT,
                                                          this->menuBackground.loadedSprite->startPixelInclusive.x,
                                                          this->menuBackground.loadedSprite->startPixelInclusive.y,
                                                          this->menuBackground.loadedSprite->heightPx,
                                                          this->menuBackground.loadedSprite->widthPx) != ZUN_SUCCESS)
                {
                    // ZUN landmine: if the screen capture never works, the pause
                    // menu gets stuck and only the Escape, Q and R keys work.
                    this->curState = PAUSE_MENU_STATE_INIT;
                    return 0;
                }
                else
                {
                    this->menuBackground.pos.x = ARCADE_LEFT;
                    this->menuBackground.pos.y = ARCADE_TOP;
                    this->menuBackground.pos.z = 0.0f;
                }
            }

            g_Supervisor.UpdateGameTime();
        }

        if (this->numFrames > 8)
        {
            break;
        }

        // Why +=? Why not =?
        if (!g_GameManager.IsSpellPractice() && g_GameManager.difficulty < EXTRA)
        {
            this->curState += RETRY_MENU_STATE_NO_SELECTED;
        }
        else
        {
            this->curState += !g_Spellcard.IsCaptured() && g_GameManager.IsSpellPractice()
                              ? RETRY_MENU_STATE_YES_SELECTED
                              : RETRY_MENU_STATE_NO_SELECTED;
        }

        this->numFrames = 0;

        if (this->curState == RETRY_MENU_STATE_NO_SELECTED)
        {
            goto selected_no;
        }
        // fallthrough
    case RETRY_MENU_STATE_YES_SELECTED:
        this->menuSprites[RETRY_SPRITE_YES].color1.d3dColor = COLOR_MENU_ITEM_SELECTED;
        this->menuSprites[RETRY_SPRITE_NO].color1.d3dColor = COLOR_MENU_ITEM_NORMAL;
        this->menuSprites[RETRY_SPRITE_YES].pos2 = Float3(-4.0f, -4.0f, 0.0f);
        this->menuSprites[RETRY_SPRITE_NO].pos2 = Float3(0.0f, 0.0f, 0.0f);

        if (this->numFrames >= 4)
        {
            if (WAS_PRESSED(TH_BUTTON_UP) || WAS_PRESSED(TH_BUTTON_DOWN))
            {
                this->curState = RETRY_MENU_STATE_NO_SELECTED;
                g_SoundPlayer.PlaySoundByIdx(SOUND_SHOOT, 0);
            }

            if (WAS_PRESSED(TH_BUTTON_SELECTMENU))
            {
                g_SoundPlayer.PlaySoundByIdx(SOUND_SELECT, 0);

                if (!g_GameManager.IsSpellPractice() && g_GameManager.difficulty < EXTRA)
                {
                    this->menuSprites[RETRY_SPRITE_CLOCKTIME].pendingInterrupt = ASCII_INTERRUPT_CLOCKTIME_FLIP;
                    this->curState = RETRY_MENU_STATE_RETRY;
                    this->numFrames = 0;
                }
                else
                {
                    if (g_GameManager.IsSpellPractice() &&
                        !GameManager::ShouldPauseMusicInSpellPractice(g_GameManager.currentSpellCardNumber))
                    {
                        g_GameManager.showRetryMenu = FALSE;
                        g_SoundPlayer.UnPause();
                        g_SoundPlayer.PartialFadeIn(1.0f);
                    }
                    else
                    {
                        g_Supervisor.StopAudio();
                    }

                    g_Supervisor.curState = SupervisorState_SpellcardPracticeRestart;
                    g_Gui.CaptureArcade();
                    g_GameManager.showRetryMenu = FALSE;
                    g_Supervisor.systemTime = timeGetTime();

                    return 0;
                }
            }
        }
        break;
    case RETRY_MENU_STATE_NO_SELECTED:
selected_no:
        this->menuSprites[RETRY_SPRITE_NO].color1.d3dColor = COLOR_MENU_ITEM_SELECTED;
        this->menuSprites[RETRY_SPRITE_YES].color1.d3dColor = COLOR_MENU_ITEM_NORMAL;
        this->menuSprites[RETRY_SPRITE_NO].pos2 = Float3(-4.0f, -4.0f, 0.0f);
        this->menuSprites[RETRY_SPRITE_YES].pos2 = Float3(0.0f, 0.0f, 0.0f);

        if (this->numFrames >= 30)
        {
            if (WAS_PRESSED(TH_BUTTON_UP) || WAS_PRESSED(TH_BUTTON_DOWN))
            {
                this->curState = RETRY_MENU_STATE_YES_SELECTED;
                g_SoundPlayer.PlaySoundByIdx(SOUND_SHOOT, 0);
            }
            if (WAS_PRESSED(TH_BUTTON_SELECTMENU))
            {
                g_SoundPlayer.PlaySoundByIdx(SOUND_SELECT, 0);

                for (i = 0; i < 4; i++)
                {
                    this->menuSprites[i].pendingInterrupt = ASCII_INTERRUPT_HIDE;
                }

                this->curState = RETRY_MENU_STATE_EXIT_TO_TITLE;
                this->numFrames = 0;
            }
        }
        break;
    case RETRY_MENU_STATE_EXIT_TO_TITLE:
        if (this->numFrames >= 20)
        {
            this->curState = RETRY_MENU_STATE_INIT;
            this->numFrames = 0;

            g_GameManager.showRetryMenu = FALSE;

            g_Supervisor.curState = SupervisorState_ResultScreenFromGame;

            for (i = 0; i < 4; i++)
            {
                this->menuSprites[i].SetInvisible();
            }

            g_GameManager.globals->displayScore = g_GameManager.globals->score;
            g_Supervisor.systemTime = timeGetTime();
            return 0;
        }

        break;
    case RETRY_MENU_STATE_RETRY:
        if (this->numFrames == 15)
        {
            g_GameManager.AddToClockTime(1);
            g_Gui.timesAnm->SetSprite(&this->menuSprites[RETRY_SPRITE_CLOCKTIME], (i8)g_GameManager.GetClockTime());
        }
        if (this->numFrames == 60)
        {
            this->menuBackground.pendingInterrupt = ASCII_INTERRUPT_BACKGROUND_HIDE;

            for (i = 0; i < 4; i++)
            {
                // This doesn't do anything? Could this be an interrupt that
                // was removed from the scripts later in development?
                this->menuSprites[i].pendingInterrupt = 3;
            }
        }
        if (this->numFrames >= 90)
        {
            this->curState = RETRY_MENU_STATE_INIT;
            this->numFrames = 0;

            g_GameManager.showRetryMenu = FALSE;

            for (i = 0; i < 4; i++)
            {
                this->menuSprites[i].SetInvisible();
            }

            g_GameManager.globals->numRetries++;

            // Set the score to the number of retry. Each increment is a
            // multiple of 10, so the last digit of your score is the number
            // of continues/retries used.
            g_GameManager.globals->displayScore = g_GameManager.globals->numRetries;
            g_GameManager.globals->scoreDisplayStep = 0;
            g_GameManager.globals->score = g_GameManager.globals->displayScore;

            g_GameManager.SetLives(g_GameManager.cfg->lifeCount);

            g_GameManager.SetBombCount(g_Player.primaryShtFile->initialBombCount);

            g_GameManager.globals->grazeInStage = 0;
            g_GameManager.globals->pointItemsCollectedInStage = 0;
            g_GameManager.globals->pointItemsCollected = 0;

            g_GameManager.SetPower(0);

            g_GameManager.globals->pointItemExtendsSoFar = 0;
            g_GameManager.globals->nextPointItemExtendThreshold = 100;

            g_Supervisor.screenTransitionCountdown = 8;

            IncrementIfBelow(&g_GameManager.plst.playData[g_GameManager.difficulty].attemptsTotal, 999999);
            IncrementIfBelow(&g_GameManager.plst.playData[MAX_DIFFICULTIES + 1].attemptsTotal, 999999);
            IncrementIfBelow(&g_GameManager.plst.playData[g_GameManager.difficulty].attemptsPerCharacter[g_GameManager.shotType], 999999);
            IncrementIfBelow(&g_GameManager.plst.playData[MAX_DIFFICULTIES + 1].attemptsPerCharacter[g_GameManager.shotType], 999999);
            IncrementIfBelow(&g_GameManager.plst.playData[g_GameManager.difficulty].continues, 999999);
            IncrementIfBelow(&g_GameManager.plst.playData[MAX_DIFFICULTIES + 1].continues, 999999);

            g_SoundPlayer.UnPause();

            g_Supervisor.systemTime = timeGetTime();

            return 0;
        }
        break;
    }

    for (i = 0; i < 4; i++)
    {
        g_AnmManager->ExecuteScript(&this->menuSprites[i]);
    }

    if (g_Supervisor.flags.lockableBackbuffer)
    {
        g_AnmManager->ExecuteScript(&this->menuBackground);
    }

    this->numFrames++;

    return 0;
}

#pragma var_order(vmIdx)
// FUNCTION: th08 0x4052b0
void RetryMenu::OnDraw()
{
    i32 vmIdx;
    if (g_GameManager.showRetryMenu)
    {
        g_AnmManager->FlushVertexBuffer();
        g_Supervisor.viewport.X = g_GameManager.arcadeRegionTopLeftPos.x;
        g_Supervisor.viewport.Y = g_GameManager.arcadeRegionTopLeftPos.y;
        g_Supervisor.viewport.Width = g_GameManager.arcadeRegionSize.x;
        g_Supervisor.viewport.Height = g_GameManager.arcadeRegionSize.y;
        g_Supervisor.d3dDevice->SetViewport(&g_Supervisor.viewport);
        if (((g_EclGameTimeScaleFlags >> 1) & 1) != 0 && (this->curState != 0 || this->numFrames > 2))
            g_AnmManager->DrawNoRotation(&this->menuBackground);

        if (!g_GameManager.IsPracticeMode() && g_GameManager.difficulty < EXTRA)
        {
            for (vmIdx = 0; vmIdx < 4; vmIdx++)
                if (this->menuSprites[vmIdx].IsVisible())
                    g_AnmManager->DrawNoRotation(&this->menuSprites[vmIdx]);
        }
        else
        {
            for (vmIdx = 0; vmIdx < 3; vmIdx++)
                if (this->menuSprites[vmIdx].IsVisible())
                    g_AnmManager->DrawNoRotation(&this->menuSprites[vmIdx]);
        }
    }
}

#pragma var_order(popup, alpha, dy, dx, i, j, charPtr, unused, rect, alphaColor, divisor)
// FUNCTION: th08 0x405420
#if defined(PSP) && defined(TH08_PSP_DRAW_PRIORITY_SUBPROFILE) && TH08_PSP_DRAW_PRIORITY_SUBPROFILE
extern "C" unsigned int sceKernelGetSystemTimeLow(void);
namespace
{
struct PspAsciiSubTimer
{
    unsigned int start;
    unsigned int mid;
    ~PspAsciiSubTimer()
    {
        const unsigned int end = sceKernelGetSystemTimeLow();
        g_PspAsciiSub.popupsUs += mid - start;
        g_PspAsciiSub.restUs += end - mid;
        if ((++g_PspAsciiSub.calls % 600UL) == 0UL)
        {
            th08::psp::BootLog("ASCII_SUB stats calls=%lu popups_us=%lu rest_us=%lu hud_strings=%lu hud_glyphs=%lu "
                               "hud_fallbacks=%lu hud_audit_glyphs=%lu hud_audit_mismatch=%lu "
                               "skip_vm=%lu skip_idx=%lu skip_alpha=%lu skip_tex=%lu skip_ident=%lu\n",
                               g_PspAsciiSub.calls, g_PspAsciiSub.popupsUs, g_PspAsciiSub.restUs, g_PspAsciiSub.hudStrings,
                               g_PspAsciiSub.hudGlyphs, g_PspAsciiSub.hudFallbacks, g_PspAsciiSub.hudAuditGlyphs,
                               g_PspAsciiSub.hudAuditMismatch, g_PspAsciiSub.hudSkipVm, g_PspAsciiSub.hudSkipIndices,
                               g_PspAsciiSub.hudSkipAlpha, g_PspAsciiSub.hudSkipTexture, g_PspAsciiSub.hudSkipIdentity);
            th08::psp::FlushBootLog();
        }
    }
};
} // namespace
#define TH08_PSP_ASCII_SUB_ENABLED 1
#else
#define TH08_PSP_ASCII_SUB_ENABLED 0
#endif

void AsciiManager::OnDrawHighPrioImpl()
{
#if TH08_PSP_ASCII_SUB_ENABLED
    PspAsciiSubTimer pspAsciiSubTimer = {sceKernelGetSystemTimeLow(), 0U};
#endif
    AsciiManagerPopup *popup;
    u8 *charPtr;
    i32 alpha;
    f32 dx, dy;
    i32 i, j;
    ZunRect rect;
    ZunColor alphaColor;
    i32 divisor;

    popup = this->scorePopups;
    Float3 unused;

    if (!g_Supervisor.IsFogDisabled())
    {
        g_Supervisor.DisableFog();
    }
    g_Supervisor.SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);

#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_BATCH) && \
    TH08_PSP_ASCII_POPUP_BATCH
    const bool scorePopupsDrawnByBatch =
#if defined(TH08_PSP_ME_POPUP_ENABLED)
        th08_psp_me_popup_draw() != 0 ||
#endif
        g_AnmManager->DrawPspAsciiPopupBatch(
            &this->smallScoreText, this->asciiAnm, this->scorePopups,
            ASCII_MAX_SCORE_POPUPS + ASCII_MAX_PLAYER_POPUPS,
            g_Player.position.x, g_Player.position.y,
            this->scaleX, this->scaleY) == ZUN_SUCCESS;
#else
    const bool scorePopupsDrawnByBatch = false;
#endif

    if (!scorePopupsDrawnByBatch)
    {

#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
    PspEnsureAsciiPopupOccupancy(this);
    PspPopupBitIterator scoreDrawIterator(
        g_PspAsciiPopupOccupancy.scoreWords,
        g_PspAsciiPopupOccupancy.scoreLastActiveWord + 1,
        kPspScorePopupSlots);
    while (scoreDrawIterator.Next(&j))
    {
        popup = &this->scorePopups[j];
#else
    for (j = 0; j < ASCII_MAX_SCORE_POPUPS + ASCII_MAX_PLAYER_POPUPS; j++, popup++)
    {
#endif
        if (!popup->inUse)
        {
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
            PspMarkScorePopupInactive(j);
#endif
            continue;
        }
#if defined(PSP)
        th08::psp::RenderPerfNotePopup(popup->characterCount);
#endif

        this->smallScoreText.pos.x = popup->position.x - (f32)(popup->characterCount * 4);
        this->smallScoreText.pos.y = popup->position.y;
        this->smallScoreText.color1.d3dColor = popup->color;

        dx = g_Player.position.x - popup->position.x;
        dy = g_Player.position.y - popup->position.y;
        alpha = (i32)(dx * dx + dy * dy);
        if (alpha > 4096)
        {
            alpha = 208;
        }
        else if (alpha > 1024)
        {
            alpha = ((alpha - 1024) << 7) / 3072 + 80;
        }
        else
        {
            alpha = 80;
        }

        this->smallScoreText.scale.x = this->scaleX;
        this->smallScoreText.scale.y = this->scaleY;

        charPtr = (u8 *)&popup->text[popup->characterCount - 1];
        for (i = popup->characterCount; i > 0; i--)
        {
            if (popup->timer < 52)
            {
                this->smallScoreText.loadedSprite = this->asciiAnm->GetSprite(*charPtr);
                this->smallScoreText.color1.a = alpha;
            }
            else if (popup->timer < 56)
            {
                this->smallScoreText.loadedSprite = this->asciiAnm->GetSprite(*charPtr + 11);
                this->smallScoreText.color1.a = alpha;
            }
            else
            {
                this->smallScoreText.loadedSprite = this->asciiAnm->GetSprite(*charPtr + 21);
                this->smallScoreText.color1.a = alpha;
            }
            this->smallScoreText.spriteSize.x = this->smallScoreText.loadedSprite->widthPx;
            g_AnmManager->DrawNoRotation(&this->smallScoreText);
            this->smallScoreText.pos.x += 8.0f;
            charPtr--;
        }
    }
    }

    if (this->nightBlindnessAlpha > 0)
    {
        alphaColor.a = this->nightBlindnessAlpha;
        alphaColor.r = 0;
        alphaColor.g = 0;
        alphaColor.b = 0;

        rect.left = 32.0f;
        rect.top = 16.0f;
        rect.right = g_Player.position.x + 32.0f - this->nightBlindnessRadius +
                     g_AnmManager->screenShakeOffset.x;
        rect.bottom = 464.0f;
        if (rect.right > rect.left)
        {
            ScreenEffect::DrawSquare(&rect, alphaColor.d3dColor);
        }

        rect.left = g_Player.position.x + 32.0f + this->nightBlindnessRadius +
                    g_AnmManager->screenShakeOffset.x;
        rect.top = 16.0f;
        rect.right = 416.0f;
        rect.bottom = 464.0f;
        if (rect.right > rect.left)
        {
            ScreenEffect::DrawSquare(&rect, alphaColor.d3dColor);
        }

        rect.left = g_Player.position.x + 32.0f - this->nightBlindnessRadius +
                    g_AnmManager->screenShakeOffset.x;
        if (rect.left < 32.0f)
        {
            rect.left = 32.0f;
        }
        rect.top = 16.0f;
        rect.right = g_Player.position.x + 32.0f + this->nightBlindnessRadius +
                     g_AnmManager->screenShakeOffset.x;
        if (rect.right > 416.0f)
        {
            rect.right = 416.0f;
        }
        rect.bottom = g_Player.position.y + 16.0f - this->nightBlindnessRadius +
                      g_AnmManager->screenShakeOffset.y;
        if (rect.bottom > rect.top)
        {
            ScreenEffect::DrawSquare(&rect, alphaColor.d3dColor);
        }

        rect.top = g_Player.position.y + 16.0f + this->nightBlindnessRadius +
                   g_AnmManager->screenShakeOffset.y;
        rect.bottom = 464.0f;
        if (rect.bottom > rect.top)
        {
            ScreenEffect::DrawSquare(&rect, alphaColor.d3dColor);
        }

        g_EffectManager.effectAnm->SetAndExecuteScriptIdx(&this->nightBlindnessVm, 105);
        this->nightBlindnessVm.scale.y = this->nightBlindnessRadius / 63.0f;
        this->nightBlindnessVm.scale.x = this->nightBlindnessVm.scale.y;
        this->nightBlindnessVm.pos = g_Player.position;
        this->nightBlindnessVm.pos.x += 32.0f;
        this->nightBlindnessVm.pos.y += 16.0f;
        this->nightBlindnessVm.color1.a = this->nightBlindnessAlpha;
        g_AnmManager->DrawNoRotation(&this->nightBlindnessVm);
    }

#if TH08_PSP_ASCII_SUB_ENABLED
    pspAsciiSubTimer.mid = sceKernelGetSystemTimeLow();
#endif
    popup = this->timePopups;
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
    PspPopupBitIterator timeDrawIterator(
        g_PspAsciiPopupOccupancy.timeWords,
        g_PspAsciiPopupOccupancy.timeLastActiveWord + 1,
        kPspTimePopupSlots);
    while (timeDrawIterator.Next(&j))
    {
        popup = &this->timePopups[j];
#else
    for (j = 0; j < ASCII_MAX_TIME_POPUPS; j++, popup++)
    {
#endif
        if (!popup->inUse)
        {
#if defined(PSP) && defined(TH08_PSP_ASCII_POPUP_OCCUPANCY) && \
    TH08_PSP_ASCII_POPUP_OCCUPANCY
            PspMarkTimePopupInactive(j);
#endif
            continue;
        }
#if defined(PSP)
        th08::psp::RenderPerfNotePopup(popup->characterCount);
#endif

        this->popupText.pos.x = popup->position.x - 3.5f * popup->characterCount;
        this->popupText.pos.y = popup->position.y;
        this->popupText.color1.d3dColor = popup->color;

        dx = g_Player.position.x - popup->position.x;
        dy = g_Player.position.y - popup->position.y;
        alpha = (i32)(dx * dx + dy * dy);
        if (alpha > 4096)
        {
            alpha = 208;
        }
        else if (alpha > 1024)
        {
            alpha = ((alpha - 1024) << 7) / 3072 + 80;
        }
        else
        {
            alpha = 80;
        }

        this->popupText.scale.x = popup->scale.x;
        this->popupText.scale.y = popup->scale.y;

        charPtr = (u8 *)&popup->text[popup->characterCount - 1];
        for (i = popup->characterCount; i > 0; i--)
        {
            this->popupText.loadedSprite = this->asciiAnm->GetSprite(*charPtr + 136);
            this->popupText.color1.a = alpha;
            this->popupText.spriteSize.x = this->popupText.loadedSprite->widthPx;
            g_AnmManager->DrawNoRotation(&this->popupText);
            this->popupText.pos.x += 7.0f * popup->scale.x;
            charPtr--;
        }
    }

    g_AnmManager->screenShakeOffset.y = 0.0f;
    g_AnmManager->screenShakeOffset.x = 0.0f;

    if (this->youkaiGauge.IsVisible())
    {
        this->youkaiGaugeCursor.pos.x =
            (f32)g_GameManager.GetYoukaiGauge() * 112.0f / 2.0f / 10000.0f + this->youkaiGauge.pos.x + 64.0f;
        g_AnmManager->Draw2DRotatedOrAxisAligned(&this->youkaiGaugeCursor);

        this->percentageText.pos.x =
            (f32)g_GameManager.GetYoukaiGauge() * 80.0f / 2.0f / 10000.0f + this->youkaiGauge.pos.x + 64.0f;
        this->percentageText.pos.y = this->youkaiGaugeCursor.pos.y - 7.0f;
        this->percentageText.pos.z = this->youkaiGaugeCursor.pos.z;
        this->percentageText.color1.a = this->youkaiGauge.color1.a;

        if (g_GameManager.GaugeIsExtremelyHuman())
        {
            this->percentageText.color1.r = 112;
            this->percentageText.color1.g = 112;
            this->percentageText.color1.b = 255;
        }
        else if (g_GameManager.GaugeIsModeratelyHuman())
        {
            this->percentageText.color1.r = 176;
            this->percentageText.color1.g = 176;
            this->percentageText.color1.b = 255;
        }
        else if (g_GameManager.GaugeIsExtremelyYoukai())
        {
            this->percentageText.color1.r = 255;
            this->percentageText.color1.g = 112;
            this->percentageText.color1.b = 112;
        }
        else if (g_GameManager.GaugeIsModeratelyYoukai())
        {
            this->percentageText.color1.r = 255;
            this->percentageText.color1.g = 176;
            this->percentageText.color1.b = 176;
        }
        else
        {
            this->percentageText.color1.r = 255;
            this->percentageText.color1.g = 255;
            this->percentageText.color1.b = 255;
        }

        this->youkaiGauge.color1.d3dColor = this->percentageText.color1.d3dColor;

        g_AnmManager->DrawNoRotation(&this->youkaiGauge);
        g_AnmManager->DrawNoRotation(&this->youkaiGaugeHumanIcon);
        g_AnmManager->DrawNoRotation(&this->youkaiGaugeYoukaiIcon);

        this->DrawPercentage(&this->percentageText.pos, g_GameManager.GetYoukaiGauge(),
                             this->percentageText.color1.d3dColor);

        divisor = 10000000;
        i = g_GameManager.globals->pointItemValue;
        alpha = 0;
        this->percentageText.pos.x = this->youkaiGauge.pos.x + 62.0f - 14.0f;
        this->percentageText.pos.y = this->youkaiGauge.pos.y + 3.0f + 8.0f;

        for (j = 0; j < 8; j++)
        {
            alpha += i / divisor;
            if (alpha != 0)
            {
                this->asciiAnm->SetSprite(&this->percentageText, i / divisor + 136);
                g_AnmManager->DrawNoRotation(&this->percentageText);
                this->percentageText.pos.x += 7.0f;
            }
            i %= divisor;
            divisor /= 10;
        }
    }
}

#pragma var_order(xOffset, numDigits, absPercentage)
// FUNCTION: th08 0x405e10
void AsciiManager::DrawPercentage(Float3 *position, i32 percentage, D3DCOLOR color)
{
    f32 xOffset;
    i32 absPercentage;
    i32 numDigits;

    numDigits = 4;
    if (percentage < 0)
    {
        numDigits++;
    }

    absPercentage = abs(percentage);
    if (absPercentage >= 10000)
    {
        numDigits += 3;
    }
    else
    {
        if (absPercentage >= 1000)
        {
            numDigits += 2;
        }
        else
        {
            numDigits++;
        }
    }

    xOffset = (f32)numDigits * 3.5f - 3.5f - 4.0f;

    this->percentageText.pos = *position;
    this->percentageText.pos.x -= xOffset;
    this->percentageText.color1.d3dColor = color;

    if (percentage < 0)
    {
        this->asciiAnm->SetSprite(&this->percentageText, 148);
        g_AnmManager->DrawNoRotation(&this->percentageText);
        this->percentageText.pos.x += 7.0f;
    }

    if (absPercentage >= 10000)
    {
        this->asciiAnm->SetSprite(&this->percentageText, 137);
        g_AnmManager->DrawNoRotation(&this->percentageText);
        this->percentageText.pos.x += 7.0f;

        this->asciiAnm->SetSprite(&this->percentageText, 136);
        g_AnmManager->DrawNoRotation(&this->percentageText);
        this->percentageText.pos.x += 7.0f;

        this->asciiAnm->SetSprite(&this->percentageText, 136);
        g_AnmManager->DrawNoRotation(&this->percentageText);
        this->percentageText.pos.x += 7.0f;

        this->asciiAnm->SetSprite(&this->percentageText, 147);
        g_AnmManager->DrawNoRotation(&this->percentageText);
        this->percentageText.pos.x += 5.0f;
        this->percentageText.scale.y = 0.8f;
        this->percentageText.scale.x = 0.8f;
        this->percentageText.pos.y += 2.0f;

        this->asciiAnm->SetSprite(&this->percentageText, 136);
        g_AnmManager->DrawNoRotation(&this->percentageText);
        this->percentageText.pos.x += 5.0f;

        this->asciiAnm->SetSprite(&this->percentageText, 136);
        g_AnmManager->DrawNoRotation(&this->percentageText);
        this->percentageText.pos.x += 7.0f;
    }
    else if (absPercentage >= 1000)
    {
        numDigits = absPercentage;
        this->asciiAnm->SetSprite(&this->percentageText, numDigits / 1000 + 136);
        numDigits = numDigits % 1000;
        g_AnmManager->DrawNoRotation(&this->percentageText);
        this->percentageText.pos.x += 7.0f;

        this->asciiAnm->SetSprite(&this->percentageText, numDigits / 100 + 136);
        numDigits = numDigits % 100;
        g_AnmManager->DrawNoRotation(&this->percentageText);
        this->percentageText.pos.x += 7.0f;

        this->asciiAnm->SetSprite(&this->percentageText, 147);
        g_AnmManager->DrawNoRotation(&this->percentageText);
        this->percentageText.pos.x += 5.0f;
        this->percentageText.scale.y = 0.8f;
        this->percentageText.scale.x = 0.8f;
        this->percentageText.pos.y += 2.0f;

        this->asciiAnm->SetSprite(&this->percentageText, numDigits / 10 + 136);
        numDigits = numDigits % 10;
        g_AnmManager->DrawNoRotation(&this->percentageText);
        this->percentageText.pos.x += 5.0f;

        this->asciiAnm->SetSprite(&this->percentageText, numDigits + 136);
        g_AnmManager->DrawNoRotation(&this->percentageText);
        this->percentageText.pos.x += 7.0f;
    }
    else
    {
        numDigits = absPercentage;
        this->asciiAnm->SetSprite(&this->percentageText, numDigits / 100 + 136);
        numDigits = numDigits % 100;
        g_AnmManager->DrawNoRotation(&this->percentageText);
        this->percentageText.pos.x += 7.0f;

        this->asciiAnm->SetSprite(&this->percentageText, 147);
        g_AnmManager->DrawNoRotation(&this->percentageText);
        this->percentageText.pos.x += 5.0f;
        this->percentageText.scale.y = 0.8f;
        this->percentageText.scale.x = 0.8f;
        this->percentageText.pos.y += 2.0f;

        this->asciiAnm->SetSprite(&this->percentageText, numDigits / 10 + 136);
        numDigits = numDigits % 10;
        g_AnmManager->DrawNoRotation(&this->percentageText);
        this->percentageText.pos.x += 5.0f;

        this->asciiAnm->SetSprite(&this->percentageText, numDigits + 136);
        g_AnmManager->DrawNoRotation(&this->percentageText);
        this->percentageText.pos.x += 7.0f;
    }

    this->percentageText.scale.y = 1.0f;
    this->percentageText.scale.x = 1.0f;
    this->percentageText.pos.y -= 2.0f;
    this->asciiAnm->SetSprite(&this->percentageText, 146);
    g_AnmManager->DrawNoRotation(&this->percentageText);
}

// FUNCTION: th08 0x4068e0
inline void AnmVmBase::Initialize()
{
    memset(this, 0, sizeof(AnmVmBase));

    this->scale.x = 1.0f;
    this->scale.y = 1.0f;
    this->color1.d3dColor = COLOR_WHITE;
    D3DXMatrixIdentity(&this->matrix1);
    this->flags = 7;
    this->currentTimeInScript.Initialize();
}

} /* namespace th08 */
