#pragma once
// TH08_PSP_ME_POPUP: the score/time popups (AsciiManager::scorePopups) are
// turned into GE quads on the Media Engine from a capture taken at the end
// of the calc chain, using the draw-time state recorded by the previous
// AsciiManager high-priority draw.  The draw submits the quads directly when
// that state still matches; otherwise the SC batch runs as before.
#include <cstdint>

#define TH08_ME_POPUP_MAX_RECORDS 723U
#define TH08_ME_POPUP_MAX_QUADS 2048U
#define TH08_ME_POPUP_SPRITES 32U

struct PspMePopupSprite
{
    float widthPx, u0, u1, v0, v1;
    std::uint32_t pad[3];
}; // 32 bytes

struct PspMePopupRecord
{
    float x, y;
    std::uint32_t color;
    std::int32_t timerCurrent;
    unsigned char count;
    unsigned char digits[12];
    unsigned char pad[3];
}; // 32 bytes

struct PspMePopupVertex
{
    float u, v;
    unsigned char r, g, b, a;
    float x, y, z;
}; // 24 bytes (PspClientVertex layout)

struct __attribute__((aligned(64))) PspMePopupJob
{
    std::uint32_t count;               // records
    std::uint32_t recordsAddr, spritesAddr, outAddr;
    std::uint32_t outCapacity;         // quads
    std::uint32_t outCount;            // quads written (ME)
    std::uint32_t digits;              // digits seen (ME)
    std::uint32_t addrMask;            // 0x80000000 on the ME
    float playerX, playerY, scaleX, scaleY;
    float spriteSizeY, posZ, scrollX, scrollY;
    float shakeX, shakeY, vpX, vpY, vpW, vpH;
    std::uint32_t anchor, flag17, color2, useMixColor, mixColor;
    std::uint32_t onMe;                // 1: running on the ME (ME cache maintenance)
    std::uint32_t pad[4];
};

extern "C"
{
void th08_me_popup_kernel(PspMePopupJob *job);
// SC side (psp/me_popup.cpp).
void th08_psp_me_popup_capture(void);   // end of the calc chain
// Draw (AsciiManager high-priority): 1 = popups were submitted from the ME
// output (nothing else to draw), 0 = run the SC batch.
int th08_psp_me_popup_draw(void);
}
