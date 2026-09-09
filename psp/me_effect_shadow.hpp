#pragma once
// TH08_PSP_ME_EFFECT_SHADOW: shadow validation of an ME effect-quad job.
// While EffectManager::DrawBackgroundEffects runs, every Draw2D /
// DrawNoRotation sprite is recorded (inputs) together with the quad the SC
// produced (outputs).  The same quad math (th08_me_effect_kernel) is then
// executed on the Media Engine (or on the SC when no ME is available, e.g.
// PPSSPP) and compared bit for bit.  Drawing is unchanged.  Default OFF.
#if defined(PSP) && defined(TH08_PSP_ME_EFFECT_SHADOW) && TH08_PSP_ME_EFFECT_SHADOW
#define TH08_PSP_ME_EFFECT_SHADOW_ENABLED 1
#else
#define TH08_PSP_ME_EFFECT_SHADOW_ENABLED 0
#endif

#define TH08_ME_EFFECT_MAX_RECORDS 704U

// TH08_PSP_ME_EFFECT_ADOPT: build the background-effect quads on the ME from a
// capture taken after the calc chain and draw them without Draw2D.  _AUDIT
// keeps the SC path and compares every quad/cull decision.  Default OFF.
#if defined(PSP) && defined(TH08_PSP_ME_EFFECT_ADOPT) && TH08_PSP_ME_EFFECT_ADOPT
#define TH08_PSP_ME_EFFECT_ADOPT_ENABLED 1
#else
#define TH08_PSP_ME_EFFECT_ADOPT_ENABLED 0
#endif
#if defined(PSP) && defined(TH08_PSP_ME_EFFECT_ADOPT_AUDIT) && TH08_PSP_ME_EFFECT_ADOPT_AUDIT
#define TH08_PSP_ME_EFFECT_ADOPT_AUDIT_ENABLED 1
#else
#define TH08_PSP_ME_EFFECT_ADOPT_AUDIT_ENABLED 0
#endif
#define TH08_ME_EFFECT_ADOPT_MAX_ENTRIES 704U
enum
{
    TH08_ME_ADOPT_STAT_FRAMES = 0, TH08_ME_ADOPT_STAT_CAPTURED, TH08_ME_ADOPT_STAT_SUBMITTED, TH08_ME_ADOPT_STAT_ON_ME,
    TH08_ME_ADOPT_STAT_ADOPTED, TH08_ME_ADOPT_STAT_LATE, TH08_ME_ADOPT_STAT_FB_STATE, TH08_ME_ADOPT_STAT_FB_ORDER,
    TH08_ME_ADOPT_STAT_AUDIT_CULL, TH08_ME_ADOPT_STAT_AUDIT_QUAD, TH08_ME_ADOPT_STAT_ENTRIES, TH08_ME_ADOPT_STAT_RECORDS,
    TH08_ME_ADOPT_STAT_OVERFLOW, TH08_ME_ADOPT_STAT_BUSY, TH08_ME_ADOPT_STAT_CAM, TH08_ME_ADOPT_STAT_FB_CAM,
    TH08_ME_ADOPT_STAT_ROT, TH08_ME_ADOPT_STAT_G0, TH08_ME_ADOPT_STAT_ACQ_NOTLIVE, TH08_ME_ADOPT_STAT_ACQ_HELD,
    TH08_ME_ADOPT_STAT_COUNT
};

// Plain C layouts shared by the SC and the ME (no game headers).
struct PspMeEffectRecord
{
    float posX, posY, posZ;
    float scaleX, scaleY;
    float sizeX, sizeY;
    float sine, cosine;
    float uvStartX, uvEndX, uvStartY, uvEndY;
    float scrollX, scrollY;
    unsigned int color1, color2;
    unsigned char anchor, rotated, flag17, kind; // kind: 0 axis 2D, 1 rotated 2D, 2 camera-facing, 3 projected 3D, 4 camera-facing + stage adjust
    float matrix[16];                             // kind 3: vm->matrix2 (row-major, as D3DXMATRIX); kind 4: pos2 xyz, posFinal xyz, posInitial.x
};

struct PspMeEffectQuad
{
    float x[4], y[4], z[4];
    float u[4], v[4];
    unsigned int color;
    unsigned int culled;
    // kind 4 (stage particle with AdjustStageEffectDrawPosition): the vm state
    // the callback would leave behind (posFinal xyz, posInitial.x); adjApplied
    // is 0 when the projection was rejected before the callback ran.
    float adjX, adjY, adjZ, adjW;
    unsigned int adjApplied;
    unsigned int pad[1];
};

struct PspMeEffectJob
{
    unsigned int count;
    unsigned int useMixColor;
    unsigned int mixColor;
    unsigned int pad0;
    float shakeX, shakeY;
    float vpX, vpY, vpW, vpH;
    unsigned int recordsAddr;
    unsigned int outAddr;
    float playerX, playerY, playerZ;   // kind 4: g_Player.position
    unsigned int adjustEnabled;        // kind 4: !isInGameMenu && !showRetryMenu
    float view[16];
    float proj[16];
    float camRight[4];
};

extern "C"
{
void th08_me_effect_capture_arm(int armed);
void th08_me_effect_capture_begin(float posX, float posY, float posZ, float scaleX, float scaleY, float sizeX,
                                  float sizeY, float sine, float cosine, float uvStartX, float uvEndX,
                                  float uvStartY, float uvEndY, float scrollX, float scrollY,
                                  unsigned int color1, unsigned int color2, unsigned int anchor,
                                  unsigned int rotated, unsigned int flag17, unsigned int kind,
                                  const float *matrix16);
// quadVertices: 4 x VertexTex1DiffuseXyzrhw (28 bytes each) or NULL when culled.
// Optional, before capture_end(NULL) from the viewport cull: logs the first few culled quads.
void th08_me_effect_capture_cull_note(const void *quadVertices);
void th08_me_effect_capture_end(const void *quadVertices);
// Per Present: compare finished jobs, submit the new one, log stats.
void th08_me_effect_shadow_frame(void);
void th08_me_effect_set_frame_state(float shakeX, float shakeY, float vpX, float vpY, float vpW, float vpH,
                                    unsigned int useMixColor, unsigned int mixColor, const float *view16,
                                    const float *proj16, const float *camRight3);
// ---- adoption (psp/me_effect_adopt.cpp) ----
// Capture (end of calc chain): begin with the draw-time state recorded by the
// previous DrawBackgroundEffects, one entry per list node in list order
// (kind 0 = ME record, 1 = invisible/alpha 0, 2 = culled early, 3 = not a
// Draw2D node), then submit.  Draw: state_matches -> acquire -> per node
// entry_tag/kind/quad -> release.
int th08_me_effect_adopt_begin(float shakeX, float shakeY, float vpX, float vpY, float vpW, float vpH,
                               unsigned int useMixColor, unsigned int mixColor);
int th08_me_effect_adopt_add_entry(const void *tag, unsigned int kind);
int th08_me_effect_adopt_add_record(const void *tag, float posX, float posY, float posZ, float scaleX, float scaleY,
                                    float sizeX, float sizeY, float uvStartX, float uvEndX, float uvStartY,
                                    float uvEndY, float scrollX, float scrollY, unsigned int color1,
                                    unsigned int color2, unsigned int anchor, unsigned int flag17);
// Camera-facing (group 1) records: predicted SetCamera2 view/projection and
// camera right vector for this frame; matrices_match compares the acquired
// job's copy with the draw-time values (mismatch: those entries draw on the SC).
void th08_me_effect_adopt_set_matrices(const float *view16, const float *proj16, const float *camRight3,
                                       float playerX, float playerY, float playerZ, unsigned int adjustEnabled);
int th08_me_effect_adopt_add_record_cam(const void *tag, float posX, float posY, float posZ, float scaleX,
                                        float scaleY, float sizeX, float sizeY, float sine, float cosine,
                                        float uvStartX, float uvEndX, float uvStartY, float uvEndY, float scrollX,
                                        float scrollY, unsigned int color1, unsigned int color2, unsigned int anchor,
                                        unsigned int flag17);
int th08_me_effect_adopt_matrices_match(const float *view16, const float *proj16, const float *camRight3);
// Channels: 0 = draw list 1 (DrawBackgroundEffects), 1 = draw list 0 (OnDraw,
// Draw2D with the arcade offset).  Every call below acts on the selected one.
void th08_me_effect_adopt_select_channel(unsigned int ch);
// Rotated 2D record (Draw2D with rotation.z != 0): kernel kind 1.
int th08_me_effect_adopt_add_record_rot(const void *tag, float posX, float posY, float posZ, float scaleX,
                                        float scaleY, float sizeX, float sizeY, float sine, float cosine,
                                        float uvStartX, float uvEndX, float uvStartY, float uvEndY, float scrollX,
                                        float scrollY, unsigned int color1, unsigned int color2, unsigned int anchor,
                                        unsigned int flag17);
// Same as add_record_cam for effects drawn through AdjustStageEffectDrawPosition
// (ids 0x33/0x3F): the ME also evaluates the callback (kind 4).
int th08_me_effect_adopt_add_record_cam_adjust(const void *tag, float posX, float posY, float posZ, float scaleX,
                                               float scaleY, float sizeX, float sizeY, float sine, float cosine,
                                               float uvStartX, float uvEndX, float uvStartY, float uvEndY,
                                               float scrollX, float scrollY, unsigned int color1, unsigned int color2,
                                               unsigned int anchor, unsigned int flag17, const float *pos2,
                                               const float *posFinal, float posInitialX);
void th08_me_effect_adopt_abort(void);
void th08_me_effect_adopt_submit(void);
int th08_me_effect_adopt_state_matches(float shakeX, float shakeY, float vpX, float vpY, float vpW, float vpH,
                                       unsigned int useMixColor, unsigned int mixColor);
int th08_me_effect_adopt_acquire(unsigned int waitUs, unsigned int *entryCount);
const void *th08_me_effect_adopt_entry_tag(unsigned int i);
unsigned int th08_me_effect_adopt_entry_kind(unsigned int i);
const PspMeEffectQuad *th08_me_effect_adopt_entry_quad(unsigned int i);
void th08_me_effect_adopt_release(void);
void th08_me_effect_adopt_note(unsigned int what);
// Pure quad math (runs on the ME or the SC).  Reads records and writes out
// through the addresses in the job; both must be uncached-safe.
void th08_me_effect_kernel(const PspMeEffectJob *job, const PspMeEffectRecord *records, PspMeEffectQuad *out);
}
