// TH08_PSP_ME_EFFECT_ADOPT: the background-effect quads are built on the Media
// Engine from a capture taken at the end of the calc chain, and
// DrawBackgroundEffects emits them instead of running Draw2D.  See
// me_effect_shadow.hpp for the shared layouts and th08_me_effect_kernel.
#include "me_effect_shadow.hpp"

#include "fileio.hpp"
#include "me_core.hpp"

#include <cstdint>
#include <cstring>
#if defined(PSP)
#include <pspkernel.h>
#endif

#if TH08_PSP_ME_EFFECT_ADOPT_ENABLED || TH08_PSP_ME_EFFECT_ADOPT_AUDIT_ENABLED
namespace
{
constexpr unsigned kSlots = 2U;
constexpr unsigned kChannels = 2U; // 0: draw list 1 (DrawBackgroundEffects), 1: draw list 0 (OnDraw)
struct Channel
{
    PspMeEffectRecord records[kSlots][TH08_ME_EFFECT_MAX_RECORDS] __attribute__((aligned(64)));
    PspMeEffectQuad out[kSlots][TH08_ME_EFFECT_MAX_RECORDS] __attribute__((aligned(64)));
    PspMeEffectJob job[kSlots] __attribute__((aligned(64)));
    const void *tag[kSlots][TH08_ME_EFFECT_ADOPT_MAX_ENTRIES];
    unsigned short ref[kSlots][TH08_ME_EFFECT_ADOPT_MAX_ENTRIES]; // kind << 12 | record index
    unsigned entryCount[kSlots], recordCount[kSlots];
    unsigned slot;         // last submitted slot
    unsigned captureSlot;  // slot being captured
    bool live[kSlots];     // submitted and not yet released
    bool onMe[kSlots];
    bool capturing;
    bool acquired;
};
Channel gChannels[kChannels] __attribute__((aligned(64)));
Channel *C = &gChannels[0];
unsigned long gStats[TH08_ME_ADOPT_STAT_COUNT];

template <typename T> inline volatile T *Uncached(T *p)
{
    return reinterpret_cast<volatile T *>(0x40000000u | reinterpret_cast<std::uintptr_t>(p));
}

inline unsigned RoundLines(unsigned bytes) { return (bytes + 63U) & ~63U; }
} // namespace

extern "C" void th08_me_effect_adopt_select_channel(unsigned int ch)
{
    C = &gChannels[ch < kChannels ? ch : 0U];
}

extern "C" void th08_me_effect_adopt_note(unsigned int what)
{
    if (what < TH08_ME_ADOPT_STAT_COUNT)
        ++gStats[what];
}

extern "C" int th08_me_effect_adopt_begin(float shakeX, float shakeY, float vpX, float vpY, float vpW, float vpH,
                                          unsigned int useMixColor, unsigned int mixColor)
{
    ++gStats[TH08_ME_ADOPT_STAT_FRAMES];
    if ((gStats[TH08_ME_ADOPT_STAT_FRAMES] % 600UL) == 0UL)
    {
        th08::psp::BootLog("ME_ADOPT stats frames=%lu captured=%lu submitted=%lu on_me=%lu adopted=%lu late=%lu "
                           "fb_state=%lu fb_order=%lu audit_cull=%lu audit_quad=%lu entries=%lu records=%lu "
                           "overflow=%lu busy=%lu cam=%lu fb_cam=%lu rot=%lu g0=%lu acq_notlive=%lu acq_held=%lu\n",
                           gStats[TH08_ME_ADOPT_STAT_FRAMES], gStats[TH08_ME_ADOPT_STAT_CAPTURED],
                           gStats[TH08_ME_ADOPT_STAT_SUBMITTED], gStats[TH08_ME_ADOPT_STAT_ON_ME],
                           gStats[TH08_ME_ADOPT_STAT_ADOPTED], gStats[TH08_ME_ADOPT_STAT_LATE],
                           gStats[TH08_ME_ADOPT_STAT_FB_STATE], gStats[TH08_ME_ADOPT_STAT_FB_ORDER],
                           gStats[TH08_ME_ADOPT_STAT_AUDIT_CULL], gStats[TH08_ME_ADOPT_STAT_AUDIT_QUAD],
                           gStats[TH08_ME_ADOPT_STAT_ENTRIES], gStats[TH08_ME_ADOPT_STAT_RECORDS],
                           gStats[TH08_ME_ADOPT_STAT_OVERFLOW], gStats[TH08_ME_ADOPT_STAT_BUSY],
                           gStats[TH08_ME_ADOPT_STAT_CAM], gStats[TH08_ME_ADOPT_STAT_FB_CAM],
                           gStats[TH08_ME_ADOPT_STAT_ROT], gStats[TH08_ME_ADOPT_STAT_G0],
                           gStats[TH08_ME_ADOPT_STAT_ACQ_NOTLIVE], gStats[TH08_ME_ADOPT_STAT_ACQ_HELD]);
        th08::psp::FlushBootLog();
    }
    if (C->acquired)
        return 0; // draw side still consuming (should not happen: capture runs between draws)
    const unsigned slot = C->slot ^ 1U;
    if (C->live[slot])
    {
        // Superseded job (draw skipped a frame): make sure the ME is done with it.
        if (C->onMe[slot] && th08_me_core_job_done(&C->job[slot]) == 0)
        {
            ++gStats[TH08_ME_ADOPT_STAT_BUSY];
            return 0;
        }
        C->live[slot] = false;
    }
    C->captureSlot = slot;
    C->entryCount[slot] = 0U;
    C->recordCount[slot] = 0U;
    PspMeEffectJob &j = C->job[slot];
    std::memset(&j, 0, sizeof(j));
    j.shakeX = shakeX; j.shakeY = shakeY;
    j.vpX = vpX; j.vpY = vpY; j.vpW = vpW; j.vpH = vpH;
    j.useMixColor = useMixColor; j.mixColor = mixColor;
    C->capturing = true;
    ++gStats[TH08_ME_ADOPT_STAT_CAPTURED];
    return 1;
}

extern "C" int th08_me_effect_adopt_add_entry(const void *tag, unsigned int kind)
{
    if (!C->capturing)
        return 0;
    const unsigned slot = C->captureSlot;
    const unsigned n = C->entryCount[slot];
    if (n >= TH08_ME_EFFECT_ADOPT_MAX_ENTRIES)
        return 0;
    C->tag[slot][n] = tag;
    C->ref[slot][n] = static_cast<unsigned short>(kind << 12);
    C->entryCount[slot] = n + 1U;
    return 1;
}

extern "C" int th08_me_effect_adopt_add_record(const void *tag, float posX, float posY, float posZ, float scaleX,
                                               float scaleY, float sizeX, float sizeY, float uvStartX, float uvEndX,
                                               float uvStartY, float uvEndY, float scrollX, float scrollY,
                                               unsigned int color1, unsigned int color2, unsigned int anchor,
                                               unsigned int flag17)
{
    if (!C->capturing)
        return 0;
    const unsigned slot = C->captureSlot;
    const unsigned n = C->entryCount[slot];
    const unsigned r = C->recordCount[slot];
    if (n >= TH08_ME_EFFECT_ADOPT_MAX_ENTRIES || r >= TH08_ME_EFFECT_MAX_RECORDS)
        return 0;
    PspMeEffectRecord &rec = C->records[slot][r];
    rec.posX = posX; rec.posY = posY; rec.posZ = posZ;
    rec.scaleX = scaleX; rec.scaleY = scaleY;
    rec.sizeX = sizeX; rec.sizeY = sizeY;
    rec.sine = 0.0f; rec.cosine = 1.0f;
    rec.uvStartX = uvStartX; rec.uvEndX = uvEndX; rec.uvStartY = uvStartY; rec.uvEndY = uvEndY;
    rec.scrollX = scrollX; rec.scrollY = scrollY;
    rec.color1 = color1; rec.color2 = color2;
    rec.anchor = static_cast<unsigned char>(anchor);
    rec.rotated = 0U;
    rec.flag17 = static_cast<unsigned char>(flag17);
    rec.kind = 0U;
    std::memset(rec.matrix, 0, sizeof(rec.matrix));
    C->tag[slot][n] = tag;
    C->ref[slot][n] = static_cast<unsigned short>(r);
    C->entryCount[slot] = n + 1U;
    C->recordCount[slot] = r + 1U;
    return 1;
}

extern "C" void th08_me_effect_adopt_set_matrices(const float *view16, const float *proj16, const float *camRight3,
                                                  float playerX, float playerY, float playerZ,
                                                  unsigned int adjustEnabled)
{
    if (!C->capturing)
        return;
    PspMeEffectJob &j = C->job[C->captureSlot];
    std::memcpy(j.view, view16, sizeof(j.view));
    std::memcpy(j.proj, proj16, sizeof(j.proj));
    j.camRight[0] = camRight3[0]; j.camRight[1] = camRight3[1]; j.camRight[2] = camRight3[2]; j.camRight[3] = 0.0f;
    j.playerX = playerX; j.playerY = playerY; j.playerZ = playerZ;
    j.adjustEnabled = adjustEnabled;
}

extern "C" int th08_me_effect_adopt_add_record_cam_adjust(const void *tag, float posX, float posY, float posZ,
                                                          float scaleX, float scaleY, float sizeX, float sizeY,
                                                          float sine, float cosine, float uvStartX, float uvEndX,
                                                          float uvStartY, float uvEndY, float scrollX, float scrollY,
                                                          unsigned int color1, unsigned int color2, unsigned int anchor,
                                                          unsigned int flag17, const float *pos2, const float *posFinal,
                                                          float posInitialX)
{
    if (!th08_me_effect_adopt_add_record_cam(tag, posX, posY, posZ, scaleX, scaleY, sizeX, sizeY, sine, cosine,
                                             uvStartX, uvEndX, uvStartY, uvEndY, scrollX, scrollY, color1, color2,
                                             anchor, flag17))
        return 0;
    PspMeEffectRecord &rec = C->records[C->captureSlot][C->recordCount[C->captureSlot] - 1U];
    rec.kind = 4U;
    rec.matrix[0] = pos2[0]; rec.matrix[1] = pos2[1]; rec.matrix[2] = pos2[2];
    rec.matrix[3] = posFinal[0]; rec.matrix[4] = posFinal[1]; rec.matrix[5] = posFinal[2];
    rec.matrix[6] = posInitialX;
    return 1;
}

extern "C" int th08_me_effect_adopt_add_record_cam(const void *tag, float posX, float posY, float posZ, float scaleX,
                                                   float scaleY, float sizeX, float sizeY, float sine, float cosine,
                                                   float uvStartX, float uvEndX, float uvStartY, float uvEndY,
                                                   float scrollX, float scrollY, unsigned int color1,
                                                   unsigned int color2, unsigned int anchor, unsigned int flag17)
{
    if (!th08_me_effect_adopt_add_record(tag, posX, posY, posZ, scaleX, scaleY, sizeX, sizeY, uvStartX, uvEndX,
                                         uvStartY, uvEndY, scrollX, scrollY, color1, color2, anchor, flag17))
        return 0;
    PspMeEffectRecord &rec = C->records[C->captureSlot][C->recordCount[C->captureSlot] - 1U];
    rec.sine = sine;
    rec.cosine = cosine;
    rec.rotated = 1U;
    rec.kind = 2U;
    ++gStats[TH08_ME_ADOPT_STAT_CAM];
    return 1;
}

extern "C" int th08_me_effect_adopt_add_record_rot(const void *tag, float posX, float posY, float posZ, float scaleX,
                                                   float scaleY, float sizeX, float sizeY, float sine, float cosine,
                                                   float uvStartX, float uvEndX, float uvStartY, float uvEndY,
                                                   float scrollX, float scrollY, unsigned int color1,
                                                   unsigned int color2, unsigned int anchor, unsigned int flag17)
{
    if (!th08_me_effect_adopt_add_record(tag, posX, posY, posZ, scaleX, scaleY, sizeX, sizeY, uvStartX, uvEndX,
                                         uvStartY, uvEndY, scrollX, scrollY, color1, color2, anchor, flag17))
        return 0;
    PspMeEffectRecord &rec = C->records[C->captureSlot][C->recordCount[C->captureSlot] - 1U];
    rec.sine = sine;
    rec.cosine = cosine;
    rec.rotated = 1U;
    rec.kind = 1U;
    ++gStats[TH08_ME_ADOPT_STAT_ROT];
    return 1;
}

extern "C" int th08_me_effect_adopt_matrices_match(const float *view16, const float *proj16, const float *camRight3)
{
    const unsigned slot = C->slot;
    if (!C->live[slot])
        return 0;
    const PspMeEffectJob &j = C->job[slot];
    return std::memcmp(j.view, view16, sizeof(j.view)) == 0 && std::memcmp(j.proj, proj16, sizeof(j.proj)) == 0 &&
                   j.camRight[0] == camRight3[0] && j.camRight[1] == camRight3[1] && j.camRight[2] == camRight3[2]
               ? 1
               : 0;
}

extern "C" void th08_me_effect_adopt_abort(void)
{
    if (!C->capturing)
        return;
    C->capturing = false;
    ++gStats[TH08_ME_ADOPT_STAT_OVERFLOW];
}

extern "C" void th08_me_effect_adopt_submit(void)
{
    if (!C->capturing)
        return;
    C->capturing = false;
    const unsigned slot = C->captureSlot;
    const unsigned n = C->recordCount[slot];
    gStats[TH08_ME_ADOPT_STAT_ENTRIES] += C->entryCount[slot];
    gStats[TH08_ME_ADOPT_STAT_RECORDS] += n;
    PspMeEffectJob &j = C->job[slot];
    j.count = n;
    j.recordsAddr = reinterpret_cast<std::uintptr_t>(C->records[slot]);
    j.outAddr = reinterpret_cast<std::uintptr_t>(C->out[slot]);
    bool onMe = false;
    if (n != 0U)
    {
        sceKernelDcacheWritebackRange(C->records[slot], RoundLines(sizeof(PspMeEffectRecord) * n));
        sceKernelDcacheWritebackRange(&j, sizeof(j));
        if (th08_me_core_ready())
            onMe = th08_me_core_submit_job(&j) != 0;
        if (!onMe)
            th08_me_effect_kernel(&j, C->records[slot],
                                  const_cast<PspMeEffectQuad *>(reinterpret_cast<volatile PspMeEffectQuad *>(Uncached(C->out[slot]))));
        else
            ++gStats[TH08_ME_ADOPT_STAT_ON_ME];
    }
    C->onMe[slot] = onMe;
    C->live[slot] = true;
    C->slot = slot;
    ++gStats[TH08_ME_ADOPT_STAT_SUBMITTED];
}

extern "C" int th08_me_effect_adopt_state_matches(float shakeX, float shakeY, float vpX, float vpY, float vpW,
                                                  float vpH, unsigned int useMixColor, unsigned int mixColor)
{
    const unsigned slot = C->slot;
    if (!C->live[slot])
        return 0;
    const PspMeEffectJob &j = C->job[slot];
    return j.shakeX == shakeX && j.shakeY == shakeY && j.vpX == vpX && j.vpY == vpY && j.vpW == vpW && j.vpH == vpH &&
                   j.useMixColor == useMixColor && j.mixColor == mixColor
               ? 1
               : 0;
}

extern "C" int th08_me_effect_adopt_acquire(unsigned int waitUs, unsigned int *entryCount)
{
    const unsigned slot = C->slot;
    if (!C->live[slot])
    {
        ++gStats[TH08_ME_ADOPT_STAT_ACQ_NOTLIVE];
        return 0;
    }
    if (C->acquired)
    {
        ++gStats[TH08_ME_ADOPT_STAT_ACQ_HELD];
        return 0;
    }
    if (C->onMe[slot])
    {
        unsigned waited = 0U;
        while (th08_me_core_job_done(&C->job[slot]) == 0)
        {
            if (waited >= waitUs)
            {
                ++gStats[TH08_ME_ADOPT_STAT_LATE];
                return 0;
            }
            sceKernelDelayThread(20);
            waited += 20U;
        }
        // The ME wrote the quads back to RAM; drop any stale lines before cached reads.
        sceKernelDcacheInvalidateRange(C->out[slot], RoundLines(sizeof(PspMeEffectQuad) * C->recordCount[slot]));
    }
    else
    {
        // Kernel ran on the SC through the uncached alias: nothing cached.
        sceKernelDcacheInvalidateRange(C->out[slot], RoundLines(sizeof(PspMeEffectQuad) * C->recordCount[slot]));
    }
    C->acquired = true;
    *entryCount = C->entryCount[slot];
    ++gStats[TH08_ME_ADOPT_STAT_ADOPTED];
    return 1;
}

extern "C" const void *th08_me_effect_adopt_entry_tag(unsigned int i) { return C->tag[C->slot][i]; }
extern "C" unsigned int th08_me_effect_adopt_entry_kind(unsigned int i) { return C->ref[C->slot][i] >> 12; }
extern "C" const PspMeEffectQuad *th08_me_effect_adopt_entry_quad(unsigned int i)
{
    return &C->out[C->slot][C->ref[C->slot][i] & 0x0FFFU];
}

extern "C" void th08_me_effect_adopt_release(void)
{
    if (!C->acquired)
        return;
    C->acquired = false;
    C->live[C->slot] = false;
}
#endif
