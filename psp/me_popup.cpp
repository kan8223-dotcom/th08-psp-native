#include "me_popup.hpp"

#if defined(PSP) && ((defined(TH08_PSP_ME_POPUP) && TH08_PSP_ME_POPUP) || \
                     (defined(TH08_PSP_ME_POPUP_AUDIT) && TH08_PSP_ME_POPUP_AUDIT))

#include "AnmManager.hpp"
#include "AsciiManager.hpp"
#include "GameManager.hpp"
#include "Player.hpp"
#include "Supervisor.hpp"
#include "fileio.hpp"
#include "me_core.hpp"
#include "swap_triple.hpp"

#include <cmath>
#include <cstring>

extern "C" void sceKernelDcacheWritebackRange(const void *p, unsigned int size);
extern "C" void sceKernelDcacheInvalidateRange(const void *p, unsigned int size);
extern "C" int sceKernelDelayThread(unsigned int us);
extern "C" void meLibDcacheInvalidateRange(unsigned int addr, unsigned int size);
extern "C" void meLibDcacheWritebackRange(unsigned int addr, unsigned int size);
extern "C" int th08_psp_bullet_me_submit(void *deviceRaw, const void *vertices, unsigned int quadCount,
                                         const unsigned short *indices);
extern "C" int th08_psp_bullet_me_color_identity(void *deviceRaw);
#if defined(TH08_PSP_ME_POPUP_AUDIT) && TH08_PSP_ME_POPUP_AUDIT
extern "C" void th08_psp_ascii_popup_build_quad(th08::AnmVm *vm, const th08::AnmLoadedSprite *sprite, float widthPx,
                                                float scaleX, float scaleY, float *xyz12, float *uv8);
#endif

using namespace th08;

namespace
{
inline unsigned int MeMix(unsigned int c1, unsigned int c2)
{
    unsigned int color = (c1 * c2) / 128U;
    if (color >= 256U)
        color = 255U;
    return color;
}
} // namespace

// Mirrors AnmManager::DrawPspAsciiPopupBatch + BuildPspAsciiPopupQuad.
extern "C" void th08_me_popup_kernel(PspMePopupJob *job)
{
    const std::uint32_t mask = job->addrMask;
    const bool onMe = job->onMe != 0U;
    const PspMePopupRecord *records = reinterpret_cast<const PspMePopupRecord *>(job->recordsAddr | mask);
    const PspMePopupSprite *sprites = reinterpret_cast<const PspMePopupSprite *>(job->spritesAddr | mask);
    // Output goes through the uncached alias on both CPUs: the GE reads it
    // right after the job is done, so nothing may sit in a CPU cache.
    PspMePopupVertex *out = reinterpret_cast<PspMePopupVertex *>(job->outAddr | 0x40000000U);
    if (onMe)
    {
        meLibDcacheInvalidateRange((job->recordsAddr | mask) & ~63U, (job->count * sizeof(PspMePopupRecord) + 127U) & ~63U);
        meLibDcacheInvalidateRange((job->spritesAddr | mask) & ~63U, (TH08_ME_POPUP_SPRITES * sizeof(PspMePopupSprite) + 127U) & ~63U);
    }
    const float halfHeight = (job->spriteSizeY * job->scaleY) / 2.0f;
    const float zOut = 1.0f - 2.0f * job->posZ;
    const float vpL = job->vpX, vpT = job->vpY, vpR = job->vpX + job->vpW, vpB = job->vpY + job->vpH;
    std::uint32_t written = 0U, digits = 0U;
    for (std::uint32_t p = 0U; p < job->count; ++p)
    {
        const PspMePopupRecord &r = records[p];
        const float dx = job->playerX - r.x;
        const float dy = job->playerY - r.y;
        std::int32_t alpha = static_cast<std::int32_t>(dx * dx + dy * dy);
        if (alpha > 4096)
            alpha = 208;
        else if (alpha > 1024)
            alpha = ((alpha - 1024) << 7) / 3072 + 80;
        else
            alpha = 80;
        float posX = r.x - static_cast<float>(r.count * 4);
        const float posY = r.y;
        float rawTop, rawBottom;
        if ((job->anchor & 2U) == 0U)
        {
            rawTop = posY - halfHeight;
            rawBottom = halfHeight + posY;
        }
        else
        {
            rawTop = posY;
            rawBottom = halfHeight + posY + halfHeight;
        }
        const float top = nearbyintf(rawTop + job->shakeY) - 0.5f;
        const float bottom = nearbyintf(rawBottom + job->shakeY) - 0.5f;
        std::uint32_t color1 = (r.color & 0x00FFFFFFU) | (static_cast<std::uint32_t>(alpha) << 24);
        std::uint32_t color = job->flag17 ? job->color2 : color1;
        if (job->useMixColor)
        {
            const std::uint32_t m = job->mixColor;
            const std::uint32_t b = MeMix(color & 255U, m & 255U);
            const std::uint32_t g = MeMix((color >> 8) & 255U, (m >> 8) & 255U);
            const std::uint32_t rr = MeMix((color >> 16) & 255U, (m >> 16) & 255U);
            const std::uint32_t a = MeMix((color >> 24) & 255U, (m >> 24) & 255U);
            color = (a << 24) | (rr << 16) | (g << 8) | b;
        }
        const unsigned char cr = static_cast<unsigned char>((color >> 16) & 255U);
        const unsigned char cg = static_cast<unsigned char>((color >> 8) & 255U);
        const unsigned char cb = static_cast<unsigned char>(color & 255U);
        const unsigned char ca = static_cast<unsigned char>((color >> 24) & 255U);
        const std::int32_t offset = r.timerCurrent >= 52 ? (r.timerCurrent < 56 ? 11 : 21) : 0;
        for (std::int32_t k = static_cast<std::int32_t>(r.count) - 1; k >= 0; --k)
        {
            ++digits;
            const std::uint32_t spriteIndex = static_cast<std::uint32_t>(r.digits[k] + offset);
            const PspMePopupSprite &s = sprites[spriteIndex < TH08_ME_POPUP_SPRITES ? spriteIndex : 0U];
            const float halfWidth = (s.widthPx * job->scaleX) / 2.0f;
            float x0, x1;
            if ((job->anchor & 1U) == 0U)
            {
                x0 = posX - halfWidth;
                x1 = halfWidth + posX;
            }
            else
            {
                x0 = posX;
                x1 = halfWidth + posX + halfWidth;
            }
            x0 += job->shakeX;
            x1 += job->shakeX;
            const float rx0 = nearbyintf(x0) - 0.5f;
            const float rx1 = nearbyintf(x1) - 0.5f;
            const float maxX = rx0 > rx1 ? rx0 : rx1, minX = rx0 < rx1 ? rx0 : rx1;
            const float maxY = top > bottom ? top : bottom, minY = top < bottom ? top : bottom;
            const bool visible = !(maxX < vpL || maxY < vpT || minX > vpR || minY > vpB);
            if (visible && written < job->outCapacity)
            {
                PspMePopupVertex *v = out + written * 4U;
                const float xs[4] = {rx0, rx1, rx0, rx1};
                const float ys[4] = {top, top, bottom, bottom};
                const float us[4] = {s.u0, s.u1, s.u0, s.u1};
                const float vs[4] = {s.v0, s.v0, s.v1, s.v1};
                // Six word stores per vertex (uncached target): rgba packed once.
                const std::uint32_t rgba = static_cast<std::uint32_t>(cr) | (static_cast<std::uint32_t>(cg) << 8) |
                                           (static_cast<std::uint32_t>(cb) << 16) | (static_cast<std::uint32_t>(ca) << 24);
                for (int i = 0; i < 4; ++i)
                {
                    v[i].u = us[i];
                    v[i].v = vs[i];
                    *reinterpret_cast<std::uint32_t *>(&v[i].r) = rgba;
                    v[i].x = xs[i] + 0.5f;
                    v[i].y = ys[i] + 0.5f;
                    v[i].z = zOut;
                }
                ++written;
            }
            posX += 8.0f;
        }
    }
    job->outCount = written;
    job->digits = digits;
    if (onMe)
        __asm__ volatile("sync");
}

// ---------------------------------------------------------------- SC side --
namespace
{
struct DrawState
{
    bool valid;
    float playerX, playerY, scaleX, scaleY, spriteSizeY, posZ, scrollX, scrollY, shakeX, shakeY, vpX, vpY, vpW, vpH;
    std::uint32_t anchor, flag17, color2, useMixColor, mixColor;
    IDirect3DTexture8 *texture;
} gDraw;
PspMePopupJob gJob __attribute__((aligned(64)));
PspMePopupRecord gRecords[TH08_ME_POPUP_MAX_RECORDS] __attribute__((aligned(64)));
PspMePopupSprite gSprites[TH08_ME_POPUP_SPRITES] __attribute__((aligned(64)));
PspMePopupVertex gOut[2][TH08_ME_POPUP_MAX_QUADS * 4U] __attribute__((aligned(64)));
unsigned gOutSlot = 0U;
bool gLive = false, gOnMe = false;
unsigned long gFrames = 0UL, gCaptured = 0UL, gOnMeCount = 0UL, gAdopted = 0UL, gFbState = 0UL, gFbBusy = 0UL, gLate = 0UL,
              gQuads = 0UL, gPopups = 0UL, gAuditMismatch = 0UL, gAuditQuads = 0UL, gNoTexture = 0UL,
              gZeroSeen = 0UL, gZeroHealed = 0UL, gZeroFallback = 0UL, gZeroVerts = 0UL, gBigBatches = 0UL;

void RecordDrawState()
{
    AsciiManager *const am = &g_AsciiManager;
    const AnmVm *const vm = &am->smallScoreText;
    gDraw.valid = true;
    gDraw.playerX = g_Player.position.x;
    gDraw.playerY = g_Player.position.y;
    gDraw.scaleX = am->scaleX;
    gDraw.scaleY = am->scaleY;
    gDraw.spriteSizeY = vm->spriteSize.y;
    gDraw.posZ = vm->pos.z;
    gDraw.scrollX = vm->uvScrollPos.x;
    gDraw.scrollY = vm->uvScrollPos.y;
    gDraw.shakeX = g_AnmManager->screenShakeOffset.x;
    gDraw.shakeY = g_AnmManager->screenShakeOffset.y;
    gDraw.vpX = static_cast<float>(g_Supervisor.viewport.X);
    gDraw.vpY = static_cast<float>(g_Supervisor.viewport.Y);
    gDraw.vpW = static_cast<float>(g_Supervisor.viewport.Width);
    gDraw.vpH = static_cast<float>(g_Supervisor.viewport.Height);
    gDraw.anchor = vm->anchor;
    gDraw.flag17 = vm->flag17 ? 1U : 0U;
    gDraw.color2 = vm->color2.d3dColor;
    gDraw.useMixColor = g_AnmManager->useMixColor ? 1U : 0U;
    gDraw.mixColor = g_AnmManager->color.d3dColor;
}
bool StateMatchesJob()
{
    const PspMePopupJob &j = gJob;
    return j.playerX == g_Player.position.x && j.playerY == g_Player.position.y && j.scaleX == g_AsciiManager.scaleX &&
           j.scaleY == g_AsciiManager.scaleY && j.spriteSizeY == g_AsciiManager.smallScoreText.spriteSize.y &&
           j.posZ == g_AsciiManager.smallScoreText.pos.z && j.scrollX == g_AsciiManager.smallScoreText.uvScrollPos.x &&
           j.scrollY == g_AsciiManager.smallScoreText.uvScrollPos.y && j.shakeX == g_AnmManager->screenShakeOffset.x &&
           j.shakeY == g_AnmManager->screenShakeOffset.y && j.vpX == static_cast<float>(g_Supervisor.viewport.X) &&
           j.vpY == static_cast<float>(g_Supervisor.viewport.Y) && j.vpW == static_cast<float>(g_Supervisor.viewport.Width) &&
           j.vpH == static_cast<float>(g_Supervisor.viewport.Height) && j.anchor == g_AsciiManager.smallScoreText.anchor &&
           j.flag17 == (g_AsciiManager.smallScoreText.flag17 ? 1U : 0U) &&
           j.color2 == g_AsciiManager.smallScoreText.color2.d3dColor &&
           j.useMixColor == (g_AnmManager->useMixColor ? 1U : 0U) && j.mixColor == g_AnmManager->color.d3dColor;
}
} // namespace

extern "C" void th08_psp_me_popup_capture(void)
{
    ++gFrames;
    if ((gFrames % 600UL) == 0UL)
    {
        th08::psp::BootLog("ME_POPUP stats frames=%lu captured=%lu on_me=%lu adopted=%lu fb_state=%lu fb_busy=%lu late=%lu "
                           "popups=%lu quads=%lu no_texture=%lu audit_quads=%lu audit_mismatch=%lu "
                           "zero_seen=%lu zero_healed=%lu zero_fallback=%lu zero_verts=%lu big=%lu\n",
                           gFrames, gCaptured, gOnMeCount, gAdopted, gFbState, gFbBusy, gLate, gPopups, gQuads, gNoTexture,
                           gAuditQuads, gAuditMismatch, gZeroSeen, gZeroHealed, gZeroFallback, gZeroVerts, gBigBatches);
        th08::psp::FlushBootLog();
    }
    // Draw eligibility can be cleared by state fallback/timeout while the ME
    // still owns these inputs. Completion, not gLive, governs their lifetime.
    if (gOnMe && th08_me_core_job_done(&gJob) == 0)
    {
        ++gFbBusy;
        return; // still running from a skipped draw: keep it, the draw will pick it up
    }
    gLive = false;
    AsciiManager *const am = &g_AsciiManager;
    if (!gDraw.valid || am->asciiAnm == NULL || am->asciiAnm->sprites == NULL)
        return;
    // Sprite table 0..31 (digits 0-10 plus the timer offsets 11 / 21).
    IDirect3DTexture8 *texture = NULL;
    for (unsigned i = 0U; i < TH08_ME_POPUP_SPRITES; ++i)
    {
        const AnmLoadedSprite *const s = am->asciiAnm->GetSprite(static_cast<i32>(i));
        if (s == NULL || s->texture == NULL || (texture != NULL && texture != s->texture))
        {
            ++gNoTexture;
            return;
        }
        texture = s->texture;
        gSprites[i].widthPx = s->widthPx;
        gSprites[i].u0 = s->uvStart.x + gDraw.scrollX;
        gSprites[i].u1 = s->uvEnd.x + gDraw.scrollX;
        gSprites[i].v0 = s->uvStart.y + gDraw.scrollY;
        gSprites[i].v1 = s->uvEnd.y + gDraw.scrollY;
    }
    std::uint32_t n = 0U, totalDigits = 0U;
    for (i32 p = 0; p < ASCII_MAX_SCORE_POPUPS + ASCII_MAX_PLAYER_POPUPS; ++p)
    {
        const AsciiManagerPopup &popup = am->scorePopups[p];
        if (!popup.inUse)
            continue;
        if (popup.characterCount == 0U || popup.characterCount > sizeof(popup.text) || n >= TH08_ME_POPUP_MAX_RECORDS)
            return; // the SC batch handles the odd cases
        PspMePopupRecord &r = gRecords[n++];
        r.x = popup.position.x;
        r.y = popup.position.y;
        r.color = popup.color;
        r.timerCurrent = popup.timer.current;
        r.count = popup.characterCount;
        for (unsigned k = 0U; k < 12U; ++k)
        {
            const unsigned char d = k < popup.characterCount ? static_cast<unsigned char>(popup.text[k]) : 0U;
            if (k < popup.characterCount && d > 10U)
                return;
            r.digits[k] = d;
        }
        totalDigits += popup.characterCount;
    }
    if (n == 0U || totalDigits > TH08_ME_POPUP_MAX_QUADS)
        return;
    // A saved raw GE QID can be recycled during a popup-free interval and
    // identify the current open list. Waiting on it here self-deadlocks before
    // this thread can flush that list. Only the live Present owner may fence.
    // Retiring the previous Present also retires every older use of either slot.
#if TH08_PSP_SWAP_TRIPLE_ENABLED
    th08::psp::SwapTripleWaitPendingDone();
#endif
    gOutSlot ^= 1U;
    std::memset(&gJob, 0, sizeof(gJob));
    gJob.count = n;
    gJob.recordsAddr = reinterpret_cast<std::uintptr_t>(gRecords);
    gJob.spritesAddr = reinterpret_cast<std::uintptr_t>(gSprites);
    gJob.outAddr = reinterpret_cast<std::uintptr_t>(gOut[gOutSlot]);
    gJob.outCapacity = TH08_ME_POPUP_MAX_QUADS;
    gJob.playerX = gDraw.playerX; gJob.playerY = gDraw.playerY;
    gJob.scaleX = gDraw.scaleX; gJob.scaleY = gDraw.scaleY;
    gJob.spriteSizeY = gDraw.spriteSizeY; gJob.posZ = gDraw.posZ;
    gJob.scrollX = gDraw.scrollX; gJob.scrollY = gDraw.scrollY;
    gJob.shakeX = gDraw.shakeX; gJob.shakeY = gDraw.shakeY;
    gJob.vpX = gDraw.vpX; gJob.vpY = gDraw.vpY; gJob.vpW = gDraw.vpW; gJob.vpH = gDraw.vpH;
    gJob.anchor = gDraw.anchor; gJob.flag17 = gDraw.flag17; gJob.color2 = gDraw.color2;
    gJob.useMixColor = gDraw.useMixColor; gJob.mixColor = gDraw.mixColor;
    // The player position of this frame is what the draw will use.
    gJob.playerX = g_Player.position.x;
    gJob.playerY = g_Player.position.y;
    gDraw.texture = texture;
    sceKernelDcacheWritebackRange(gRecords, sizeof(PspMePopupRecord) * n);
    sceKernelDcacheWritebackRange(gSprites, sizeof(gSprites));
    sceKernelDcacheWritebackRange(&gJob, sizeof(gJob));
    ++gCaptured;
    gPopups += n;
    if (th08_me_core_ready() && th08_me_core_submit_job_kind(&gJob, 6U))
    {
        gOnMe = true;
        ++gOnMeCount;
    }
    else
    {
        // SC fallback: write the output through the uncached alias so no
        // dirty lines of this buffer linger in the SC cache (they would be
        // written back over a later ME result: stretched digits).
        gJob.addrMask = 0x40000000U;
        gJob.onMe = 0U;
        th08_me_popup_kernel(&gJob);
        gJob.addrMask = 0U;
        gOnMe = false;
    }
    gLive = true;
}

extern "C" int th08_psp_me_popup_draw(void)
{
    // Always refresh the draw-time state for the next capture.
    RecordDrawState();
    if (!gLive)
        return 0;
    gLive = false;
    if (!StateMatchesJob() || gDraw.texture == NULL || !th08_psp_bullet_me_color_identity(g_Supervisor.d3dDevice))
    {
        ++gFbState;
        return 0;
    }
    if (gOnMe)
    {
        unsigned waited = 0U;
        while (th08_me_core_job_done(&gJob) == 0)
        {
            if (waited >= 3000U)
            {
                ++gLate;
                return 0;
            }
            sceKernelDelayThread(20);
            waited += 20U;
        }
        sceKernelDcacheInvalidateRange(&gJob, sizeof(gJob));
        sceKernelDcacheInvalidateRange(gOut[gOutSlot], sizeof(PspMePopupVertex) * 4U * TH08_ME_POPUP_MAX_QUADS);
        // Self-check: a written vertex is never all zero (alpha >= 80).  A zero
        // vertex means the ME output has not reached RAM: wait for it, and
        // if it never comes let the SC batch draw (no stretched digits).
        const std::uint32_t verts = gJob.outCount * 4U;
        const volatile std::uint32_t *const uw =
            reinterpret_cast<const volatile std::uint32_t *>(reinterpret_cast<std::uintptr_t>(gOut[gOutSlot]) | 0x40000000U);
        std::uint32_t zeros = 0U, first = 0xFFFFFFFFU, last = 0U;
        for (std::uint32_t i = 0U; i < verts; ++i)
        {
            const std::uint32_t *w = reinterpret_cast<const std::uint32_t *>(gOut[gOutSlot] + i);
            if ((w[0] | w[1] | w[2] | w[3] | w[4] | w[5]) == 0U)
            {
                ++zeros;
                if (first == 0xFFFFFFFFU)
                    first = i;
                last = i;
            }
        }
        if (zeros != 0U)
        {
            ++gZeroSeen;
            gZeroVerts += zeros;
            unsigned spins = 0U;
            bool healed = false;
            while (spins < 50U && !healed)
            {
                sceKernelDelayThread(20);
                ++spins;
                healed = true;
                for (std::uint32_t i = first; i <= last && healed; ++i)
                {
                    const volatile std::uint32_t *w = uw + i * 6U;
                    if ((w[0] | w[1] | w[2] | w[3] | w[4] | w[5]) == 0U)
                        healed = false;
                }
            }
            static unsigned zeroLogs = 0U;
            if (zeroLogs < 12U)
            {
                ++zeroLogs;
                th08::psp::BootLog("ME_POPUP_ZERO quads=%lu verts=%lu zeros=%lu first=%lu last=%lu slot=%u healed=%d spins=%u\n",
                                   static_cast<unsigned long>(gJob.outCount), static_cast<unsigned long>(verts),
                                   static_cast<unsigned long>(zeros), static_cast<unsigned long>(first),
                                   static_cast<unsigned long>(last), gOutSlot, healed ? 1 : 0, spins);
                th08::psp::FlushBootLog();
            }
            if (!healed)
            {
                ++gZeroFallback;
                return 0;
            }
            ++gZeroHealed;
            sceKernelDcacheInvalidateRange(gOut[gOutSlot], sizeof(PspMePopupVertex) * 4U * TH08_ME_POPUP_MAX_QUADS);
        }
    }
    const std::uint32_t quads = gJob.outCount;
#if defined(TH08_PSP_ME_POPUP_AUDIT) && TH08_PSP_ME_POPUP_AUDIT
    {
        // Recompute every visible digit quad on the SC with the batch's own
        // math and compare the converted vertices.
        AsciiManager *const am = &g_AsciiManager;
        AnmVm vm = am->smallScoreText;
        vm.scale.x = gJob.scaleX;
        vm.scale.y = gJob.scaleY;
        std::uint32_t q = 0U;
        const PspMePopupVertex *const out = gOut[gOutSlot];
        for (std::uint32_t p = 0U; p < gJob.count && q <= quads; ++p)
        {
            const PspMePopupRecord &r = gRecords[p];
            const float dx = gJob.playerX - r.x, dy = gJob.playerY - r.y;
            std::int32_t alpha = static_cast<std::int32_t>(dx * dx + dy * dy);
            if (alpha > 4096) alpha = 208; else if (alpha > 1024) alpha = ((alpha - 1024) << 7) / 3072 + 80; else alpha = 80;
            vm.pos.x = r.x - static_cast<float>(r.count * 4);
            vm.pos.y = r.y;
            vm.color1.d3dColor = (r.color & 0x00FFFFFFU) | (static_cast<std::uint32_t>(alpha) << 24);
            const std::int32_t offset = r.timerCurrent >= 52 ? (r.timerCurrent < 56 ? 11 : 21) : 0;
            for (std::int32_t k = static_cast<std::int32_t>(r.count) - 1; k >= 0; --k)
            {
                const AnmLoadedSprite *const sprite = am->asciiAnm->GetSprite(r.digits[k] + offset);
                float xyz[12], uv[8];
                vm.loadedSprite = const_cast<AnmLoadedSprite *>(sprite);
                vm.spriteSize.x = sprite->widthPx;
                th08_psp_ascii_popup_build_quad(&vm, sprite, sprite->widthPx, gJob.scaleX, gJob.scaleY, xyz, uv);
                float maxX = xyz[0], minX = xyz[0], maxY = xyz[1], minY = xyz[1];
                for (int i = 1; i < 4; ++i)
                {
                    if (xyz[i * 3] > maxX) maxX = xyz[i * 3];
                    if (xyz[i * 3] < minX) minX = xyz[i * 3];
                    if (xyz[i * 3 + 1] > maxY) maxY = xyz[i * 3 + 1];
                    if (xyz[i * 3 + 1] < minY) minY = xyz[i * 3 + 1];
                }
                const bool visible = !(maxX < gJob.vpX || maxY < gJob.vpY || minX > gJob.vpX + gJob.vpW || minY > gJob.vpY + gJob.vpH);
                if (!visible)
                {
                    vm.pos.x += 8.0f;
                    continue;
                }
                ++gAuditQuads;
                bool same = q < quads;
                std::uint32_t color = vm.flag17 ? vm.color2.d3dColor : vm.color1.d3dColor;
                if (gJob.useMixColor)
                {
                    const std::uint32_t m = gJob.mixColor;
                    color = (MeMix(color >> 24, m >> 24) << 24) | (MeMix((color >> 16) & 255U, (m >> 16) & 255U) << 16) |
                            (MeMix((color >> 8) & 255U, (m >> 8) & 255U) << 8) | MeMix(color & 255U, m & 255U);
                }
                for (int i = 0; i < 4 && same; ++i)
                {
                    const PspMePopupVertex &v = out[q * 4U + i];
                    same = v.x == xyz[i * 3] + 0.5f && v.y == xyz[i * 3 + 1] + 0.5f && v.z == 1.0f - 2.0f * xyz[i * 3 + 2] &&
                           v.u == uv[i * 2] && v.v == uv[i * 2 + 1] && v.r == ((color >> 16) & 255U) &&
                           v.g == ((color >> 8) & 255U) && v.b == (color & 255U) && v.a == (color >> 24);
                }
                if (!same)
                {
                    ++gAuditMismatch;
                    static unsigned samples = 0U;
                    if (samples < 6U)
                    {
                        ++samples;
                        th08::psp::BootLog("ME_POPUP_AUDIT q=%lu/%lu want %f,%f,%f uv %f,%f c=%08lx got %f,%f,%f uv %f,%f c=%02x%02x%02x%02x\n",
                                           static_cast<unsigned long>(q), static_cast<unsigned long>(quads), xyz[0] + 0.5f,
                                           xyz[1] + 0.5f, 1.0f - 2.0f * xyz[2], uv[0], uv[1], static_cast<unsigned long>(color),
                                           q < quads ? out[q * 4U].x : 0.0f, q < quads ? out[q * 4U].y : 0.0f,
                                           q < quads ? out[q * 4U].z : 0.0f, q < quads ? out[q * 4U].u : 0.0f,
                                           q < quads ? out[q * 4U].v : 0.0f, q < quads ? out[q * 4U].a : 0,
                                           q < quads ? out[q * 4U].r : 0, q < quads ? out[q * 4U].g : 0, q < quads ? out[q * 4U].b : 0);
                    }
                }
                ++q;
                vm.pos.x += 8.0f;
            }
        }
        if (q != quads)
            ++gAuditMismatch;
    }
    return 0; // audit: the SC batch still draws
#else
    ++gAdopted;
    gQuads += quads;
    if (quads == 0U)
        return 1;
    AsciiManager *const am = &g_AsciiManager;
    AnmVm *const vm = &am->smallScoreText;
    vm->loadedSprite = am->asciiAnm->GetSprite(0);
    vm->scale.x = gJob.scaleX;
    vm->scale.y = gJob.scaleY;
    g_AnmManager->FlushVertexBuffer();
    g_AnmManager->PspMeApplySpriteState(vm);
    const u16 *const indices = g_AnmManager->PspBulletUnifiedQuadIndices();
    if (indices == NULL)
        return 0;
    // The shared unified index table covers 0x600 quads (AnmManager
    // kPspBulletUnifiedQuadCapacity).  A larger chunk makes the GE read past
    // the table: garbage indices, vertices from the never-written tail,
    // digits stretched to the origin for one frame.
    if (quads > 0x600U)
    {
        ++gBigBatches;
        static unsigned bigLogs = 0U;
        if (bigLogs < 8U)
        {
            ++bigLogs;
            th08::psp::BootLog("ME_POPUP_BIG frame=%lu quads=%lu popups=%lu\n", gFrames,
                               static_cast<unsigned long>(quads), static_cast<unsigned long>(gJob.count));
        }
    }
    std::uint32_t done = 0U;
    while (done < quads)
    {
        const std::uint32_t chunk = quads - done > 0x600U ? 0x600U : quads - done;
        if (!th08_psp_bullet_me_submit(g_Supervisor.d3dDevice, gOut[gOutSlot] + done * 4U, chunk, indices))
            return 0;
        done += chunk;
    }
    return 1;
#endif
}

#else
extern "C" void th08_me_popup_kernel(PspMePopupJob *) {}
extern "C" void th08_psp_me_popup_capture(void) {}
extern "C" int th08_psp_me_popup_draw(void) { return 0; }
#endif
