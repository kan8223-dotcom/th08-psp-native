#pragma once

#include <cstdint>

// TH08_PSP_GUI_BORDER_REPLAY: Gui::DrawGameScene draws the 123 static HUD
// border tiles (frontVms[13] x 113, frontVms[14] x 10) through
// DrawNoRotation every presented frame.  Their six-vertex records only
// depend on the two sprites' state, the AnmManager mix/shake state and the
// viewport, so the product switch replays the last canonical append
// (one memcpy plus the first-tile texture/blend state calls) whenever that
// key is unchanged, and re-runs the canonical loop otherwise.  The audit
// switch keeps the canonical loop and compares its output with the replay
// candidate.  Pixels, order and draw-call boundaries are the canonical ones.
#if defined(PSP) && defined(TH08_PSP_GUI_BORDER_REPLAY_AUDIT) && \
    TH08_PSP_GUI_BORDER_REPLAY_AUDIT
#define TH08_PSP_GUI_BORDER_REPLAY_AUDIT_ENABLED 1
#else
#define TH08_PSP_GUI_BORDER_REPLAY_AUDIT_ENABLED 0
#endif
#if defined(PSP) && defined(TH08_PSP_GUI_BORDER_REPLAY) && \
    TH08_PSP_GUI_BORDER_REPLAY
#define TH08_PSP_GUI_BORDER_REPLAY_ENABLED 1
#else
#define TH08_PSP_GUI_BORDER_REPLAY_ENABLED 0
#endif
#if TH08_PSP_GUI_BORDER_REPLAY_AUDIT_ENABLED && TH08_PSP_GUI_BORDER_REPLAY_ENABLED
#error "GUI_BORDER_REPLAY audit and product switches are mutually exclusive"
#endif
#if TH08_PSP_GUI_BORDER_REPLAY_AUDIT_ENABLED || TH08_PSP_GUI_BORDER_REPLAY_ENABLED
#define TH08_PSP_GUI_BORDER_STATS_ENABLED 1
#else
#define TH08_PSP_GUI_BORDER_STATS_ENABLED 0
#endif

#if TH08_PSP_GUI_BORDER_STATS_ENABLED

namespace th08
{
struct AnmVm;
struct AnmManager;
} // namespace th08

namespace th08::psp
{
// Defined in src/AnmManager.cpp (shared sprite batch internals).
bool GuiBorderBatchActive();
bool GuiBorderBatchCanAppend(const AnmManager *manager, unsigned int vertexCount);

// Replaces the three canonical tile loops of Gui::DrawGameScene (same calls,
// same order); vm13/vm14 are &frontVms[13] / &frontVms[14].
void GuiBorderDrawTiles(AnmVm *vm13, AnmVm *vm14);

// Front sprites (frame pieces, difficulty, life/bomb stars, bottom strip):
// a generic "sprite run" recorder.  The caller keys the run, tries a native
// replay, and otherwise draws canonically between Begin/End so the appended
// vertices become the record.
#if defined(TH08_PSP_GUI_FRONT_NATIVE) && TH08_PSP_GUI_FRONT_NATIVE && defined(TH08_PSP_GUI_BORDER_NATIVE) && \
    TH08_PSP_GUI_BORDER_NATIVE
#define TH08_PSP_GUI_FRONT_NATIVE_ENABLED 1
#else
#define TH08_PSP_GUI_FRONT_NATIVE_ENABLED 0
#endif
constexpr unsigned int kGuiSpriteRunMaxVms = 16U;
constexpr unsigned int kGuiSpriteRunMaxSprites = 32U;
struct GuiSpriteKey
{
    const void *loadedSprite;
    const void *texture;
    float uvStartX, uvStartY, uvEndX, uvEndY;
    float scaleX, scaleY;
    float spriteSizeX, spriteSizeY;
    float uvScrollX, uvScrollY;
    float posX, posY, posZ, rotZ;
    std::uint32_t color1, color2;
    std::uint32_t flags;
};
struct GuiSpriteRunKey
{
    GuiSpriteKey vm[kGuiSpriteRunMaxVms];
    std::uint32_t vmCount, spriteCount, aux;
    float shakeX, shakeY;
    std::uint32_t mixColor, useMixColor;
    std::uint32_t viewportX, viewportY, viewportW, viewportH;
    std::uint32_t depthTestDisabled;
};
void GuiSpriteKeyFill(GuiSpriteKey *key, const AnmVm *vm);
void GuiSpriteRunKeyFinish(GuiSpriteRunKey *key, unsigned int vmCount, unsigned int spriteCount, unsigned int aux);
bool GuiSpriteRunTryReplay(const GuiSpriteRunKey *key);
void GuiSpriteRunBegin();
// Canonical draws routed through the recorder (each appended sprite is copied
// out of the batch right away, so texture/blend flushes inside the run are fine).
int GuiSpriteRunDraw(AnmVm *vm);   // returns the ZunResult of the canonical draw
int GuiSpriteRunDraw2D(AnmVm *vm);
void GuiSpriteRunEnd(const GuiSpriteRunKey *key);

void GuiBorderStatsResetWindow(bool active);
void GuiBorderStatsCancelWindow();
void GuiBorderStatsEmitWindow(std::int32_t stage, std::uint32_t baselineStageFrame,
                              std::uint32_t stageFrame);
} // namespace th08::psp

#endif
