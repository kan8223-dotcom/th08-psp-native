#pragma once
// TH08_PSP_ME_BG_ADOPT: Background::RenderObjects billboards (vm type 2) and
// type-1 strips are projected, fog-coloured and quad-built on the Media
// Engine from a capture taken after the calc chain; the draw walk submits
// the compacted 2D runs through the bullet direct-GE arena.  3D quads
// (Draw3D) stay on the SC.  _AUDIT keeps the SC path and compares.
#if defined(PSP) && defined(TH08_PSP_ME_BG_ADOPT) && TH08_PSP_ME_BG_ADOPT
#define TH08_PSP_ME_BG_ADOPT_ENABLED 1
#else
#define TH08_PSP_ME_BG_ADOPT_ENABLED 0
#endif
#if defined(PSP) && defined(TH08_PSP_ME_BG_ADOPT_AUDIT) && TH08_PSP_ME_BG_ADOPT_AUDIT
#define TH08_PSP_ME_BG_ADOPT_AUDIT_ENABLED 1
#else
#define TH08_PSP_ME_BG_ADOPT_AUDIT_ENABLED 0
#endif
#define TH08_PSP_ME_BG_ANY_ENABLED (TH08_PSP_ME_BG_ADOPT_ENABLED || TH08_PSP_ME_BG_ADOPT_AUDIT_ENABLED)

#define TH08_ME_BG_MAX_RECORDS 512U
#define TH08_ME_BG_STATUS_CULLED 0xFFFFU
#define TH08_ME_BG_STATUS_SKIP 0xFFFEU

#include "me_bullet_adopt.hpp" // PspMeClientVertex, status constants

// kind 5: billboard (RenderObjects type 0 with (vm->type & 0xF) == 2 ->
// DrawNoRotationNoRound).  kind 7: type-1 strip (QueueSpriteQuad).
struct PspMeBgRecord
{
    float posX, posY, posZ;    // world position (kind 5) / position1 (kind 7)
    float pos2X, pos2Y, pos2Z; // kind 7: position2
    float quadWidth;           // kind 5: size.x or sprite px; kind 7: width or sprite px
    float scaleX0, scaleY0;    // kind 5: scale before projection (size rule applied)
    float sizeX, sizeY;        // sprite px
    float uvStartX, uvEndX, uvStartY, uvEndY;
    float scrollX, scrollY;
    unsigned int color1, color2;
    unsigned char anchor, flag17, kind, mode; // mode: RenderObjects(mode) the quad belongs to
    unsigned int pad2;         // 88 bytes
};

struct PspMeBgJob
{
    unsigned int count;
    unsigned int useMixColor;
    unsigned int mixColor;
    unsigned int outCapacity;
    float shakeX, shakeY;
    float vpX, vpY, vpW, vpH;
    unsigned int recordsAddr, outAddr, statusAddr;
    unsigned int drawn;
    float camX, camY, camZ;       // cameraCurrent.position + positionOffset
    float camVecX, camVecY, camVecZ; // normalized view row 0
    float fogNear, fogFar;
    unsigned int fogColor;
    unsigned int pad[3];
    float view[16];
    float proj[16];
};

extern "C"
{
void th08_me_bg_kernel(PspMeBgJob *job, const PspMeBgRecord *records, PspMeClientVertex *out,
                       unsigned short *status);
int th08_me_bg_adopt_begin(const PspMeBgJob *header); // copies header (matrices, camera, fog, state)
int th08_me_bg_adopt_add(const void *tag, const PspMeBgRecord *record);
void th08_me_bg_adopt_abort(void);
void th08_me_bg_adopt_submit(void *outVertices, unsigned int outCapacityQuads);
// 1 match, 0 mismatch, 2 no job this frame.
int th08_me_bg_adopt_state_matches(float shakeX, float shakeY, float vpX, float vpY, float vpW, float vpH,
                                   unsigned int useMixColor, unsigned int mixColor, const float *view16,
                                   const float *proj16);
int th08_me_bg_adopt_acquire(unsigned int waitUs, unsigned int *recordCount, unsigned int *drawnQuads);
const void *th08_me_bg_adopt_tag(unsigned int i);
unsigned int th08_me_bg_adopt_status(unsigned int i);
const PspMeClientVertex *th08_me_bg_adopt_vertices(void);
void th08_me_bg_adopt_release(void);
enum
{
    TH08_ME_BG_STAT_FRAMES = 0, TH08_ME_BG_STAT_CAPTURED, TH08_ME_BG_STAT_SUBMITTED, TH08_ME_BG_STAT_ON_ME,
    TH08_ME_BG_STAT_ADOPTED, TH08_ME_BG_STAT_LATE, TH08_ME_BG_STAT_FB_STATE, TH08_ME_BG_STAT_FB_ORDER,
    TH08_ME_BG_STAT_AUDIT_CULL, TH08_ME_BG_STAT_AUDIT_QUAD, TH08_ME_BG_STAT_RECORDS, TH08_ME_BG_STAT_DRAWN,
    TH08_ME_BG_STAT_GROUPS, TH08_ME_BG_STAT_OVERFLOW, TH08_ME_BG_STAT_BUSY, TH08_ME_BG_STAT_NO_ARENA,
    TH08_ME_BG_STAT_AUDIT_ULP,
    TH08_ME_BG_STAT_COUNT
};
void th08_me_bg_adopt_note(unsigned int what);
}
