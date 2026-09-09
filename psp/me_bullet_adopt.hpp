#pragma once
// TH08_PSP_ME_BULLET_ADOPT: bullet quads (BulletManager draw buckets) are
// built on the Media Engine from a capture taken after the calc chain and
// written straight into the direct-GE vertex arena as native 24-byte
// vertices; BulletManager::OnDraw submits the compacted runs per render
// state.  _AUDIT keeps the SC path and compares each quad.  Default OFF.
#if defined(PSP) && defined(TH08_PSP_ME_BULLET_ADOPT) && TH08_PSP_ME_BULLET_ADOPT
#define TH08_PSP_ME_BULLET_ADOPT_ENABLED 1
#else
#define TH08_PSP_ME_BULLET_ADOPT_ENABLED 0
#endif
#if defined(PSP) && defined(TH08_PSP_ME_BULLET_ADOPT_AUDIT) && TH08_PSP_ME_BULLET_ADOPT_AUDIT
#define TH08_PSP_ME_BULLET_ADOPT_AUDIT_ENABLED 1
#else
#define TH08_PSP_ME_BULLET_ADOPT_AUDIT_ENABLED 0
#endif
#define TH08_PSP_ME_BULLET_ANY_ENABLED (TH08_PSP_ME_BULLET_ADOPT_ENABLED || TH08_PSP_ME_BULLET_ADOPT_AUDIT_ENABLED)

#define TH08_ME_BULLET_MAX_RECORDS 2048U
#define TH08_ME_BULLET_STATUS_CULLED 0xFFFFU
#define TH08_ME_BULLET_STATUS_SKIP 0xFFFEU

// Plain C layouts shared by the SC and the ME.
struct PspMeBulletRecord
{
    float posX, posY, posZ;
    float scaleX, scaleY;
    float sizeX, sizeY;
    float sine, cosine;
    float uvStartX, uvEndX, uvStartY, uvEndY;
    float scrollX, scrollY;
    unsigned int color1, color2;
    unsigned char anchor, flag17, kind, pad; // kind: 0 axis-aligned (rounded), 1 rotated, 0xFF skip (no draw)
    unsigned int pad2;                       // 80 bytes
};

// Matches d3d8_compat's PspClientVertex (u v | rgba | x y z).
struct PspMeClientVertex
{
    float u, v;
    unsigned char r, g, b, a;
    float x, y, z;
};

struct PspMeBulletJob
{
    unsigned int count;
    unsigned int useMixColor;
    unsigned int mixColor;
    unsigned int outCapacity; // quads
    float shakeX, shakeY;
    float vpX, vpY, vpW, vpH;
    unsigned int recordsAddr;
    unsigned int outAddr;    // PspMeClientVertex[outCapacity * 4]
    unsigned int statusAddr; // unsigned short[count]
    unsigned int drawn;      // output: quads written
    unsigned int pad[2];     // 64 bytes
};

extern "C"
{
// Pure quad math + native vertex packing (runs on the ME or the SC).
void th08_me_bullet_kernel(PspMeBulletJob *job, const PspMeBulletRecord *records, PspMeClientVertex *out,
                           unsigned short *status);

// Capture (end of calc chain): begin with the draw-time state recorded by
// the previous BulletManager::OnDraw, one record per bucket bullet in draw
// order, then submit with the arena reservation.  Draw: state_matches ->
// acquire -> per bullet status -> release.
int th08_me_bullet_adopt_begin(float shakeX, float shakeY, float vpX, float vpY, float vpW, float vpH,
                               unsigned int useMixColor, unsigned int mixColor);
int th08_me_bullet_adopt_add(const void *tag, const PspMeBulletRecord *record); // 0 on overflow
void th08_me_bullet_adopt_abort(void);
// out: reserved arena vertices (NULL -> kernel output dropped, draw falls back).
void th08_me_bullet_adopt_submit(void *outVertices, unsigned int outCapacityQuads);
// 1 match, 0 mismatch, 2 no job submitted this frame.
int th08_me_bullet_adopt_state_matches(float shakeX, float shakeY, float vpX, float vpY, float vpW, float vpH,
                                       unsigned int useMixColor, unsigned int mixColor);
int th08_me_bullet_adopt_acquire(unsigned int waitUs, unsigned int *recordCount, unsigned int *drawnQuads);
const void *th08_me_bullet_adopt_tag(unsigned int i);
unsigned int th08_me_bullet_adopt_status(unsigned int i);
const PspMeClientVertex *th08_me_bullet_adopt_vertices(void);
void th08_me_bullet_adopt_release(void);
enum
{
    TH08_ME_BULLET_STAT_FRAMES = 0, TH08_ME_BULLET_STAT_CAPTURED, TH08_ME_BULLET_STAT_SUBMITTED, TH08_ME_BULLET_STAT_ON_ME,
    TH08_ME_BULLET_STAT_ADOPTED, TH08_ME_BULLET_STAT_LATE, TH08_ME_BULLET_STAT_FB_STATE, TH08_ME_BULLET_STAT_FB_ORDER,
    TH08_ME_BULLET_STAT_FB_COLOR, TH08_ME_BULLET_STAT_AUDIT_CULL, TH08_ME_BULLET_STAT_AUDIT_QUAD,
    TH08_ME_BULLET_STAT_RECORDS, TH08_ME_BULLET_STAT_DRAWN, TH08_ME_BULLET_STAT_GROUPS, TH08_ME_BULLET_STAT_OVERFLOW,
    TH08_ME_BULLET_STAT_BUSY, TH08_ME_BULLET_STAT_NO_ARENA, TH08_ME_BULLET_STAT_COUNT
};
void th08_me_bullet_adopt_note(unsigned int what);
// d3d8_compat (direct-GE arena): reserve quads for the ME writer, submit a
// compacted run, and check that the vertex color passes through unchanged.
void *th08_psp_bullet_me_reserve(void *deviceRaw, unsigned int quadCount);
void th08_psp_bullet_me_reserve_commit(void *deviceRaw);
int th08_psp_bullet_me_submit(void *deviceRaw, const void *vertices, unsigned int quadCount,
                              const unsigned short *indices);
int th08_psp_bullet_me_color_identity(void *deviceRaw);
// Items share the bullet job (captured first, drawn first).  ItemManager::OnDraw
// calls take() per item after the original VM mutations; 1 = drawn (or culled)
// by the ME path, 0 = draw it canonically.  pass_end() flushes the last run.
int th08_psp_me_item_take(void *item, void *vm);
void th08_psp_me_item_pass_end(void);
}
