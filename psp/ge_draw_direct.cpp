#include "ge_draw_direct.hpp"

#if TH08_PSP_GE_DRAW_DIRECT_ENABLED
#include <GLES/egl.h>
#include <pspkernel.h>
#include <cstdint>
#include <cstring>
extern "C"
{
#include "pspgl_internal.h"
#include "pspgl_buffers.h"
#include "pspgl_texobj.h"
#include "pspgl_dlist.h"
void __pspgl_matrix_sync(struct pspgl_context *, const struct pspgl_matrix_stack *);
}

// Register conventions are pinned to PSPGL de4260ad (pspgl_context.c,
// glBlendFunc/glAlphaFunc/glDepthFunc/glFog). See licenses/PSPGL-LICENSE.txt.
namespace
{
unsigned long gSubmits, gFallbacks, gPalettes, gStates;

inline void Set(pspgl_context *c, unsigned reg, unsigned value)
{
    const std::uint32_t word = (reg << 24) | (value & 0x00ffffffU);
    if (c->hw.ge_reg[reg] != word)
    {
        c->hw.ge_reg[reg] = word;
        c->hw.ge_reg_touched[reg / 32U] |= 1U << (reg % 32U);
    }
}

inline unsigned Float24(float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits >> 8;
}

bool ContextReady(const pspgl_context *c)
{
    return c != NULL && c->draw != NULL && c->draw->draw != NULL &&
           *c->draw->draw != NULL && __pspgl_actuallist == NULL;
}

int BlendFactor(unsigned value, bool source)
{
    switch (value)
    {
    case GL_ZERO: case GL_ONE: return 10;
    case GL_SRC_ALPHA: return 2;
    case GL_ONE_MINUS_SRC_ALPHA: return 3;
    case GL_DST_ALPHA: return 4;
    case GL_ONE_MINUS_DST_ALPHA: return 5;
    case GL_DST_COLOR: return source ? 0 : -1;
    case GL_ONE_MINUS_DST_COLOR: return source ? 1 : -1;
    case GL_SRC_COLOR: return source ? -1 : 0;
    case GL_ONE_MINUS_SRC_COLOR: return source ? -1 : 1;
    default: return -1;
    }
}

void FlushMatrix(pspgl_context *c, pspgl_matrix_stack &stack,
                 unsigned trigger, int skipRow, unsigned index = 0U,
                 bool flipTexture = false)
{
    if (!(stack.flags & MF_DIRTY))
        return;
    const float *matrix = __pspgl_identity;
    if (!(stack.flags & MF_DISABLED))
    {
        // A current GL matrix can live only in VMAT7, not in stack[].mat.
        __pspgl_matrix_sync(c, &stack);
        matrix = stack.stack[stack.depth].mat;
    }
    const unsigned elements = skipRow < 0 ? 16U : 12U;
    __pspgl_context_writereg_uncached(c, trigger, index * elements);
    for (unsigned column = 0U; column < 4U; ++column)
        for (unsigned row = 0U; row < 4U; ++row)
            if (static_cast<int>(row) != skipRow)
            {
                float value = matrix[column * 4U + row];
                // The D3D frontend folds texture transforms into UVs, leaving
                // an identity texture stack. Its only GE adjustment is V flip.
                if (flipTexture)
                {
                    if (column == 1U && row == 1U) value = -1.0f;
                    if (column == 3U && row == 1U) value = 1.0f;
                }
                __pspgl_dlist_enqueue_cmd(((trigger + 1U) << 24) | Float24(value));
            }
    stack.flags &= ~MF_DIRTY;
}

bool MatricesSupported(pspgl_context *c, bool textured)
{
    if ((c->projection_stack.flags | c->view_stack.flags) & MF_ADJUST)
        return false;
    for (unsigned i = 0U; i < NBONES; ++i)
        if (c->bone_stacks[i].flags & MF_ADJUST)
            return false;
    // Other consumers' projective texture matrices use the canonical path.
    const pspgl_matrix_stack &texture = c->texture_stack;
    if (textured && !(texture.flags & MF_DISABLED) &&
        !(texture.stack[texture.depth].flags & MF_IDENTITY))
        return false;
    return textured || !(texture.flags & MF_ADJUST);
}
} // namespace

extern "C" int th08_ge_draw_direct_state(const Th08GeDrawState *s)
{
    pspgl_context *c = pspgl_curctx;
    if (!ContextReady(c) || s == NULL)
        return 0;
    const int source = BlendFactor(s->blendSource, true);
    const int destination = BlendFactor(s->blendDestination, false);
    if ((s->blend && (source < 0 || destination < 0)) ||
        (s->alpha && (s->alphaFunction < GL_NEVER || s->alphaFunction > GL_ALWAYS)) ||
        (s->depth && (s->depthFunction < GL_NEVER || s->depthFunction > GL_ALWAYS)))
        return 0;
    Set(c, CMD_ENA_BLEND, s->blend != 0U);
    if (s->blend)
    {
        if (source == 10) Set(c, CMD_FIXEDCOL_SRC, s->blendSource == GL_ONE ? 0xffffffU : 0U);
        if (destination == 10) Set(c, CMD_FIXEDCOL_DST, s->blendDestination == GL_ONE ? 0xffffffU : 0U);
        Set(c, CMD_BLEND_FUNC, (c->hw.ge_reg[CMD_BLEND_FUNC] & ~0xffU) |
                              (static_cast<unsigned>(destination) << 4) | static_cast<unsigned>(source));
    }
    Set(c, CMD_ENA_ALPHA_TEST, s->alpha != 0U);
    if (s->alpha)
    {
        static const unsigned char functions[] = {0, 4, 2, 5, 6, 3, 7, 1};
        // Preserve the existing float conversion/rounding at the GL boundary.
        const unsigned reference = static_cast<unsigned char>(
            255.0f * (static_cast<float>(s->alphaReference & 255U) / 255.0f));
        Set(c, CMD_ALPHA_FUNC, 0xff0000U | (reference << 8) | functions[s->alphaFunction - GL_NEVER]);
    }
    Set(c, CMD_ENA_DEPTH_TEST, s->depth && c->draw->depth_buffer != NULL);
    if (s->depth)
    {
        static const unsigned char functions[] = {
            GE_NEVER, GE_GEQUAL, GE_EQUAL, GE_GREATER,
            GE_LEQUAL, GE_NOTEQUAL, GE_LESS, GE_ALWAYS};
        Set(c, CMD_DEPTH_FUNC, functions[s->depthFunction - GL_NEVER]);
    }
    Set(c, CMD_DEPTH_MASK, s->depthWrite ? 0U : 0xffffffU);
    Set(c, CMD_ENA_FOG, s->fog != 0U);
    if (s->fog)
    {
        c->fog.near = -s->fogStart;
        c->fog.far = -s->fogEnd;
        Set(c, CMD_FOG_NEAR, Float24(1.0f / (c->fog.far - c->fog.near)));
        Set(c, CMD_FOG_FAR, Float24(c->fog.far));
        Set(c, CMD_FOG_COLOR, ((s->fogColor & 255U) << 16) |
                              (s->fogColor & 0xff00U) | ((s->fogColor >> 16) & 255U));
    }
    ++gStates;
    return 1;
}

extern "C" void th08_ge_draw_direct_flush_state()
{
    pspgl_context *c = pspgl_curctx;
    unsigned paletteBlocks = 0U;
    if ((c->hw.ge_reg[CMD_ENA_TEXTURE] & 1U) && c->texture.bound != NULL &&
        (c->hw.dirty & HWD_CLUT))
    {
        const pspgl_teximg *palette = __pspgl_texobj_cmap(c->texture.bound);
        if (palette != NULL)
        {
            const unsigned address = reinterpret_cast<unsigned>(palette->image->base) + palette->offset;
            Set(c, CMD_SET_CLUT, address);
            Set(c, CMD_SET_CLUT_MSB, (address >> 8) & 0xf0000U);
            Set(c, CMD_CLUT_MODE, palette->texfmt->hwformat | (0xffU << 8));
            paletteBlocks = palette->width / 8U;
        }
        c->hw.dirty &= ~HWD_CLUT;
    }
    __pspgl_context_flush_pending_state_changes(c, 0, 255);
    if (paletteBlocks != 0U)
    {
        __pspgl_context_writereg_uncached(c, CMD_CLUT_LOAD, paletteBlocks);
        ++gPalettes;
    }
}

extern "C" int th08_ge_draw_direct_triangles(const void *vertices, unsigned vertexBytes,
                                              const unsigned short *indices, unsigned indexBytes,
                                              unsigned indexCount)
{
    pspgl_context *c = pspgl_curctx;
    const std::uintptr_t v = reinterpret_cast<std::uintptr_t>(vertices);
    const std::uintptr_t i = reinterpret_cast<std::uintptr_t>(indices);
    const bool textured = c != NULL && (c->hw.ge_reg[CMD_ENA_TEXTURE] & 1U);
    if (!ContextReady(c) || !v || !i || (v & 3U) || (i & 1U) ||
        vertexBytes == 0U || vertexBytes % 24U || vertexBytes / 24U > 0x10000U ||
        indexCount == 0U || indexCount > 0xffffU || indexCount % 3U ||
        indexBytes != indexCount * sizeof(unsigned short) ||
        v + vertexBytes < v || i + indexBytes < i ||
        (textured && (c->texture.bound == NULL || c->texture.bound->images[0] == NULL)) ||
        !MatricesSupported(c, textured))
    {
        ++gFallbacks;
        return 0;
    }
    sceKernelDcacheWritebackRange(vertices, vertexBytes);
    sceKernelDcacheWritebackRange(indices, indexBytes);
    bool flipped = false;
    if (textured)
    {
        Set(c, CMD_TEXMAPMODE, (GE_UV << 8) | GE_TEXTURE_MATRIX);
        flipped = (c->texture.bound->flags & TOF_FLIPPED) != 0U;
        pspgl_matrix_stack &texture = c->texture_stack;
        const bool wasFlipped = (texture.flags & MF_ADJUST) != 0U;
        if (wasFlipped != flipped) texture.flags |= MF_DIRTY;
        texture.flags &= ~MF_ADJUST;
        if (flipped)
        {
            texture.flags |= MF_ADJUST;
            texture.scale[0] = 1.0f; texture.scale[1] = -1.0f; texture.scale[2] = 1.0f;
            texture.trans[0] = 0.0f; texture.trans[1] = 1.0f; texture.trans[2] = 0.0f;
        }
    }
    c->modelview_stack.flags &= ~MF_ADJUST; // This API always uses float XYZ.
    FlushMatrix(c, c->projection_stack, CMD_MAT_PROJ_TRIGGER, -1);
    FlushMatrix(c, c->modelview_stack, CMD_MAT_MODEL_TRIGGER, 3);
    FlushMatrix(c, c->texture_stack, CMD_MAT_TEXTURE_TRIGGER, 2, 0U, flipped);
    FlushMatrix(c, c->view_stack, CMD_MAT_VIEW_TRIGGER, 3);
    for (unsigned bone = 0U; bone < NBONES; ++bone)
        FlushMatrix(c, c->bone_stacks[bone], CMD_MAT_BONE_TRIGGER, 3, bone);
    const unsigned format = GE_TEXTURE_32BITF | GE_COLOR_8888 | GE_VERTEX_32BITF |
                            GE_TRANSFORM_3D | GE_VINDEX_16BIT;
    Set(c, CMD_VERTEXTYPE, format);
    th08_ge_draw_direct_flush_state();
    __pspgl_context_writereg_uncached(c, CMD_BASE, (v >> 8) & 0xf0000U);
    __pspgl_context_writereg_uncached(c, CMD_VERTEXPTR, v & 0xffffffU);
    __pspgl_context_writereg_uncached(c, CMD_BASE, (i >> 8) & 0xf0000U);
    __pspgl_context_writereg_uncached(c, CMD_INDEXPTR, i & 0xffffffU);
    __pspgl_context_writereg_uncached(c, CMD_PRIM, (GE_TRIANGLES << 16) | indexCount);
    // Any command can roll over the list: pin after PRIM, including the CLUT.
    __pspgl_context_pin_buffers(c);
    ++gSubmits;
    return 1;
}

extern "C" void th08_ge_draw_direct_stats(unsigned long *submits, unsigned long *fallbacks,
                                           unsigned long *palettes, unsigned long *states)
{
    *submits = gSubmits; *fallbacks = gFallbacks;
    *palettes = gPalettes; *states = gStates;
}
#endif
